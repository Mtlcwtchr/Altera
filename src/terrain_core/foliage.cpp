#include "terrain_core/foliage.hpp"
#include "terrain_core/clearings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace terrain_core {
namespace {

// Must match cbuffer Spawn in assets/shaders/terrain_core/foliage_spawn.hlsl.
struct SpawnUniforms {
    float planes[6][4];
    std::int32_t camCell[2];
    float camCellFrac[2];
    std::int32_t camTexel[2];
    float camTexelFrac[2];
    float eyeZ, spacing0, cellSize, range;
    std::uint32_t gridSide, clipSize, levels, seed;
    std::uint32_t firstModel, modelCount;
    float scaleMin, scaleMax;
    float minNormalZ, pixelScale, minPixels, shoreHeight;
    float biomeSize, worldSize;
    std::int32_t tileLog2;
    float heightLow;
    float heightHigh, waterMapSize, held, pad;   // held: AL-78, the layer is taken over inside holdRect
    float holdRect[4];
    float biomeRules[kMaxBiomes][4];
};
static_assert(sizeof(SpawnUniforms) == 752);

// Must match cbuffer Hold in assets/shaders/terrain_core/foliage_hold.hlsl.
struct HoldUniforms {
    float planes[6][4];
    float eye[4];
    std::uint32_t count;
    float pixelScale, minPixels, pad;
};
static_assert(sizeof(HoldUniforms) == 128);

// ---- AL-78: the GPU's scattering on the CPU (foliage_spawn.hlsl, biome_pick.hlsli,
// ground_noise.hlsli, clearings.hlsli), for the trees the game holds ----
std::uint32_t hashCell(std::int32_t x, std::int32_t y, std::uint32_t s) {
    std::uint32_t h = std::uint32_t(x) * 0x8da6b343u ^ std::uint32_t(y) * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float unitOf(std::uint32_t h) { return float(h & 0xffffu) / 65536.0f; }
float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
struct Point { std::int32_t bx, by; float sx, sy; };
float cellNoiseCpu(const Point& p, int log2Cell, std::uint32_t seed) {
    log2Cell = std::max(0, log2Cell);
    const int size = 1 << log2Cell;
    const float ix = (float(p.bx & (size - 1)) + p.sx) / float(size), iy = (float(p.by & (size - 1)) + p.sy) / float(size);
    const float wx = std::floor(ix), wy = std::floor(iy);
    const std::int32_t cx = (p.bx >> log2Cell) + std::int32_t(wx), cy = (p.by >> log2Cell) + std::int32_t(wy);
    const float fx = ix - wx, fy = iy - wy;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    const float a = float(hashCell(cx, cy, seed) & 0xffffu), c = float(hashCell(cx + 1, cy, seed) & 0xffffu);
    const float d = float(hashCell(cx, cy + 1, seed) & 0xffffu), e = float(hashCell(cx + 1, cy + 1, seed) & 0xffffu);
    const float top = a + (c - a) * ux, bottom = d + (e - d) * ux;
    return (top + (bottom - top) * uy) / 65535.0f;   // footprint 0: no fade
}
std::uint32_t dominantBiomeCpu(const BiomeMap& map, float worldSize, float spacing, int tileLog2, const Point& p) {
    const float size = float(map.size());
    if (size <= 0) return 0;
    const float texelMetres = worldSize / size;
    const float mx = (float(p.bx) + p.sx) * spacing, my = (float(p.by) + p.sy) * spacing;
    const int warpCell = int(std::round(std::log2(std::max(texelMetres * 0.35f / spacing, 1.0f))));
    const float wx = cellNoiseCpu(p, warpCell, 31u) - 0.5f, wy = cellNoiseCpu(p, warpCell, 37u) - 0.5f;
    const float whx = mx / texelMetres - 0.5f + wx * 0.9f, why = my / texelMetres - 0.5f + wy * 0.9f;
    const float bx = std::floor(whx), by = std::floor(why), fx = whx - bx, fy = why - by;
    float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    std::uint32_t ids[4];
    for (int k = 0; k < 4; ++k) {
        const int tx = int(std::clamp(bx + float(k & 1), 0.0f, size - 1)), ty = int(std::clamp(by + float(k >> 1), 0.0f, size - 1));
        ids[k] = map.ids()[std::size_t(ty) * map.size() + std::size_t(tx)];
    }
    for (int a = 0; a < 4; ++a)
        for (int c = 0; c < a; ++c)
            if (ids[c] == ids[a] && w[c] >= 0) {
                w[c] += w[a];
                w[a] = -1;
            }
    float best = -1;
    for (int c = 0; c < 4; ++c) {
        if (w[c] < 0) continue;
        w[c] += (cellNoiseCpu(p, tileLog2 + 4, 61u + ids[c]) - 0.5f) * 0.35f;
        best = std::max(best, w[c]);
    }
    std::uint32_t id = ids[0];
    for (int k = 0; k < 4; ++k)
        if (w[k] == best) id = ids[k];
    return id;
}
float clearingKeepCpu(float x, float y) {
    const ClearingUniforms& c = clearings();
    float keep = 1.0f;
    const int n = std::min(int(c.info[0]), 32);
    for (int i = 0; i < n; ++i)
        keep = std::min(keep, smooth(c.discs[i][2], c.discs[i][3], std::hypot(x - c.discs[i][0], y - c.discs[i][1])));
    return keep;
}

struct Vertex { float position[3], normal[3], uv[2]; };

bool readMesh(const std::filesystem::path& file, std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices,
              float lo[3], float hi[3]) {
    std::ifstream in(file, std::ios::binary);
    char magic[4];
    std::uint32_t version = 0, vertexCount = 0, indexCount = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&vertexCount), 4);
    in.read(reinterpret_cast<char*>(&indexCount), 4);
    in.read(reinterpret_cast<char*>(lo), 12);
    in.read(reinterpret_cast<char*>(hi), 12);
    if (!in || std::memcmp(magic, "FOLM", 4) != 0 || version != 1) return false;
    vertices.resize(vertexCount);
    indices.resize(indexCount);
    in.read(reinterpret_cast<char*>(vertices.data()), std::streamsize(vertexCount * sizeof(Vertex)));
    in.read(reinterpret_cast<char*>(indices.data()), std::streamsize(indexCount * 4));
    return bool(in);
}

} // namespace

Foliage::~Foliage() {
    if (device_)
        for (auto* b : readback_)
            if (b) SDL_ReleaseGPUTransferBuffer(device_->handle(), b);
}

bool Foliage::open(engine::Device& device, const Params& params, const std::filesystem::path& configFile,
                   const GroundUniforms& ground, std::string* why) {
    device_ = &device;
    params_ = params;
    tileLog2_ = ground.tileLog2;
    auto fail = [&](const std::string& what) { if (why) *why = what; return false; };
    std::ifstream in(configFile);
    if (!in) return fail("cannot read " + configFile.string());
    nlohmann::json config;
    try { config = nlohmann::json::parse(in); } catch (const std::exception& e) { return fail(e.what()); }
    const std::filesystem::path root = configFile.parent_path().parent_path().parent_path().parent_path();
    const std::filesystem::path dir = root / "assets" / "terrain_core" / "foliage";

    // Models, in the order the layers name them: a layer's models are one run.
    std::vector<Vertex> allVertices;
    std::vector<std::uint32_t> allIndices;
    std::vector<std::filesystem::path> textureFiles;
    std::map<std::string, std::uint32_t> modelIndex;
    std::uint32_t offset = 0;
    for (const auto& l : config["layers"]) {
        FoliageLayer layer;
        layer.name = l.value("name", std::string("layer"));
        layer.spacing = l.value("spacing_m", 1.0);
        layer.range = l.value("range_m", 50.0);
        layer.scaleMin = l["scale"][0].get<float>();
        layer.scaleMax = l["scale"][1].get<float>();
        layer.minNormalZ = l.value("min_normal_z", 0.7f);
        layer.maxCount = l.value("max_count", 50000u);
        layer.held = l.value("held", false);   // AL-78
        layer.firstModel = std::uint32_t(models_.size());
        const auto& names = l["models"];
        const std::uint32_t perModel = (layer.maxCount + std::uint32_t(names.size()) - 1) / std::uint32_t(names.size());
        for (const auto& n : names) {
            const std::string name = n.get<std::string>();
            std::vector<Vertex> v;
            std::vector<std::uint32_t> idx;
            float lo[3], hi[3];
            // AL-68: a dev stand-in from Skyrim (tools/game_assets/skyrim/foliage.py, git-ignored) wins when baked.
            const auto standIn = dir / "skyrim" / (name + ".mesh");
            const auto from = std::filesystem::exists(standIn) ? dir / "skyrim" : dir;
            if (!readMesh(from / (name + ".mesh"), v, idx, lo, hi))
                return fail("cannot read model " + name + " (run tools/terrain_lab/build_foliage.py)");
            Model m;
            m.name = name;
            m.firstIndex = std::uint32_t(allIndices.size());
            m.indexCount = std::uint32_t(idx.size());
            m.offset = offset;
            m.capacity = perModel;
            offset += perModel;
            const float w = hi[0] - lo[0], d = hi[1] - lo[1];
            m.height = hi[2] - lo[2];
            m.radius = 0.5f * std::sqrt(w * w + d * d + m.height * m.height);
            const auto base = std::uint32_t(allVertices.size());
            for (auto i : idx) allIndices.push_back(base + i);
            allVertices.insert(allVertices.end(), v.begin(), v.end());
            textureFiles.push_back(from / (name + ".png"));
            modelIndex[name] = std::uint32_t(models_.size());
            models_.push_back(m);
        }
        layer.modelCount = std::uint32_t(models_.size()) - layer.firstModel;
        const auto& biomes = config["biomes"];
        for (const auto& [id, perLayer] : biomes.items()) {
            const auto b = std::uint32_t(std::stoul(id));
            if (b >= kMaxBiomes || !perLayer.contains(layer.name)) continue;
            const auto& r = perLayer[layer.name];
            const float cluster = r[1].get<float>();
            layer.rules[b] = {r[0].get<float>(),
                              cluster > 0 ? float(std::round(std::log2(cluster / params.spacing))) : 0.0f,
                              r[2].get<float>(), 0.0f};
        }
        layers_.push_back(layer);
    }

    // GPU side.
    {
        engine::Device::Uploader upload(device);
        vertices_ = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX, allVertices.data(), allVertices.size() * sizeof(Vertex));
        indices_ = upload.add(SDL_GPU_BUFFERUSAGE_INDEX, allIndices.data(), allIndices.size() * 4);
        std::vector<float> info;
        std::vector<std::uint32_t> slots;
        for (std::size_t i = 0; i < models_.size(); ++i) {
            const Model& m = models_[i];
            info.insert(info.end(), {m.radius, m.height, float(i), 0.0f});
            slots.insert(slots.end(), {m.offset, m.capacity, m.firstIndex, m.indexCount});
        }
        modelInfo_ = upload.add(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, info.data(), info.size() * 4);
        modelSlots_ = upload.add(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, slots.data(), slots.size() * 4);
        if (!upload.finish()) return fail("foliage upload: " + device.error());
    }
    instances_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                   std::size_t(offset) * 32);
    counters_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                  models_.size() * 4);
    args_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                              models_.size() * 20);
    std::vector<std::vector<std::filesystem::path>> layers;
    for (const auto& f : textureFiles) layers.push_back({f});
    textures_ = device.loadArrayMipped(layers, true);
    gpuBytes_ = allVertices.size() * sizeof(Vertex) + allIndices.size() * 4 + std::uint64_t(offset) * 32 +
                textureFiles.size() * 256 * 256 * 4 * 4 / 3;

    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampling.max_lod = 1000.0f;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = device.makeSampler(sampling);
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampling.address_mode_u = sampling.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    linear_ = device.makeSampler(sampling);

    spawn_ = device.makeCompute({"terrain_core/foliage_spawn.hlsl", "spawnCs"});
    hold_ = device.makeCompute({"terrain_core/foliage_hold.hlsl", "holdCs"});   // AL-78
    reset_ = device.makeCompute({"terrain_core/foliage_args.hlsl", "resetCs"});
    argsPass_ = device.makeCompute({"terrain_core/foliage_args.hlsl", "argsCs"});
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/foliage_draw.hlsl";
    wanted.vertexEntry = "foliageVs";
    wanted.fragmentEntry = "foliagePs";
    wanted.depthClip = true;
    wanted.cull = SDL_GPU_CULLMODE_NONE;   // cards are seen from both sides
    wanted.buffers = {{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                      {1, 32, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                         {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
                         {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                         {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16}};
    draw_ = device.makePipeline(wanted);
    wanted.fragmentEntry = "foliageShadowPs";
    wanted.depthOnly = true;
    wanted.depthBiasConstant = -2.0f;
    wanted.depthBiasSlope = -2.5f;
    shadow_ = device.makePipeline(wanted);
    if (!shadow_ || !vertices_ || !indices_ || !instances_ || !counters_ || !args_ || !textures_ || !spawn_ || !reset_ ||
        !argsPass_ || !draw_ || !nearest_ || !linear_)
        return fail("foliage GPU objects: " + device.error());

    SDL_GPUTransferBufferCreateInfo download{};
    download.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    download.size = std::uint32_t(models_.size() * 4);
    for (auto& b : readback_)
        if (!(b = SDL_CreateGPUTransferBuffer(device.handle(), &download))) return fail("foliage readback");
    lastCounts_.assign(models_.size(), 0);
    forestHold().foliage = this;   // AL-78: the game builds its forest with this one
    return true;
}

std::vector<HeldTree> Foliage::heldTrees(const double rect[4], const BiomeMap& biomes,
                                         const std::function<float(double, double)>& height,
                                         const std::function<bool(double, double)>& keep) const {
    std::vector<HeldTree> out;
    const float spacing0 = float(params_.spacing), worldSize = float(params_.worldSize);
    for (std::size_t li = 0; li < layers_.size(); ++li) {
        const FoliageLayer& layer = layers_[li];
        if (!layer.held || layer.modelCount == 0) continue;
        const std::uint32_t seed = params_.seed * 977u + std::uint32_t(li) * 7919u;
        const double s = layer.spacing;
        const auto c0x = std::int32_t(std::floor(rect[0] / s)), c1x = std::int32_t(std::floor(rect[2] / s));
        const auto c0y = std::int32_t(std::floor(rect[1] / s)), c1y = std::int32_t(std::floor(rect[3] / s));
        for (std::int32_t cy = c0y; cy <= c1y; ++cy)
            for (std::int32_t cx = c0x; cx <= c1x; ++cx) {
                const std::uint32_t h = hashCell(cx, cy, seed);
                const double x = (double(cx) + double(h & 0xffffu) / 65536.0) * s;
                const double y = (double(cy) + double(h >> 16) / 65536.0) * s;
                if (x < rect[0] || y < rect[1] || x >= rect[2] || y >= rect[3]) continue;
                const float z = height(x, y);
                if (z < 1.5f) continue;   // the sea's shore (shoreHeight)
                const double e = spacing0;
                const float gx = float((height(x + e, y) - height(x - e, y)) / (2 * e));
                const float gy = float((height(x, y + e) - height(x, y - e)) / (2 * e));
                if (1.0f / std::sqrt(1.0f + gx * gx + gy * gy) < layer.minNormalZ) continue;
                if (!keep(x, y)) continue;
                Point p;
                const double tx = x / spacing0, ty = y / spacing0;
                p.bx = std::int32_t(std::floor(tx));
                p.by = std::int32_t(std::floor(ty));
                p.sx = float(tx - std::floor(tx));
                p.sy = float(ty - std::floor(ty));
                const std::uint32_t biome = std::min(dominantBiomeCpu(biomes, worldSize, spacing0, int(tileLog2_), p), 31u);
                const auto& rule = layer.rules[biome];
                if (rule[0] <= 0) continue;
                float chance = rule[0];
                if (rule[1] > 0) {
                    const float n = cellNoiseCpu(p, int(rule[1]), seed + 7u) * 0.7f + cellNoiseCpu(p, int(rule[1]) - 2, seed + 11u) * 0.3f;
                    chance *= smooth(1.0f - rule[2] - 0.08f, 1.0f - rule[2] + 0.08f, n);
                }
                chance *= clearingKeepCpu(float(x), float(y));
                const std::uint32_t h2 = hashCell(cx, cy, seed + 101u);
                if (unitOf(h2) >= chance) continue;
                const std::uint32_t h3 = hashCell(cx, cy, seed + 202u);
                HeldTree t;
                t.x = float(x);
                t.y = float(y);
                t.z = z;
                t.yaw = unitOf(h2 >> 16) * 6.2831853f;
                t.model = layer.firstModel + h3 % layer.modelCount;
                t.scale = layer.scaleMin + (layer.scaleMax - layer.scaleMin) * unitOf(h3 >> 8);
                t.range = float(layer.range);
                t.layer = std::uint32_t(li);
                out.push_back(t);
            }
    }
    return out;
}

void Foliage::spawn(SDL_GPUCommandBuffer* commands, const FoliageFrame& frame, SDL_GPUTexture* clipmap,
                    SDL_GPUTexture* biomeMap, std::uint32_t biomeSize, SDL_GPUTexture* water,
                    std::uint32_t waterSize, const Rivers& rivers) {
    // Counts from two frames ago (the ring slot about to be reused).
    if (frame_ >= 3) {
        auto* b = readback_[frame_ % 3];
        if (const auto* counts = static_cast<const std::uint32_t*>(SDL_MapGPUTransferBuffer(device_->handle(), b, false))) {
            std::memcpy(lastCounts_.data(), counts, lastCounts_.size() * 4);
            SDL_UnmapGPUTransferBuffer(device_->handle(), b);
        }
        triangles_ = 0;
        for (auto& layer : layers_) {
            layer.drawn = 0;
            for (std::uint32_t m = layer.firstModel; m < layer.firstModel + layer.modelCount; ++m) {
                const std::uint32_t n = std::min(lastCounts_[m], models_[m].capacity);
                layer.drawn += n;
                triangles_ += std::uint64_t(n) * models_[m].indexCount / 3;
            }
        }
    }
    const std::uint32_t models = std::uint32_t(models_.size());
    const std::uint32_t count[4] = {models, 0, 0, 0};
    {
        SDL_GPUStorageBufferReadWriteBinding rw{counters_.get(), false, {}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, reset_.get());
        SDL_PushGPUComputeUniformData(commands, 0, count, sizeof(count));
        SDL_DispatchGPUCompute(pass, (models + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    {
        SDL_GPUStorageBufferReadWriteBinding rw[2] = {{instances_.get(), false, {}}, {counters_.get(), false, {}}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2);
        SDL_BindGPUComputePipeline(pass, spawn_.get());
        const SDL_GPUTextureSamplerBinding samplers[3] = {
                {clipmap, nearest_.get()}, {biomeMap, nearest_.get()}, {water, nearest_.get()}};
        SDL_BindGPUComputeSamplers(pass, 0, samplers, 3);
        const auto river = rivers.buffers();
        SDL_GPUBuffer* reads[5] = {modelInfo_.get(), modelSlots_.get(), river[0], river[1], river[2]};
        SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 5);
        const double spacing0 = params_.spacing;
        const double range = params_.macroHeightRange, sea = params_.macroSeaLevel;
        for (std::size_t li = 0; li < layers_.size(); ++li) {
            FoliageLayer& layer = layers_[li];
            if (!layer.enabled) { layer.candidates = 0; continue; }
            SpawnUniforms u{};
            for (int k = 0; k < 6; ++k)
                for (int j = 0; j < 4; ++j) u.planes[k][j] = frame.frustum[k][j];
            const double cx = frame.eye.x / layer.spacing, cy = frame.eye.y / layer.spacing;
            u.camCell[0] = std::int32_t(std::floor(cx));
            u.camCell[1] = std::int32_t(std::floor(cy));
            u.camCellFrac[0] = float(cx - std::floor(cx));
            u.camCellFrac[1] = float(cy - std::floor(cy));
            const double tx = frame.eye.x / spacing0, ty = frame.eye.y / spacing0;
            u.camTexel[0] = std::int32_t(std::floor(tx));
            u.camTexel[1] = std::int32_t(std::floor(ty));
            u.camTexelFrac[0] = float(tx - std::floor(tx));
            u.camTexelFrac[1] = float(ty - std::floor(ty));
            u.eyeZ = float(frame.eye.z);
            u.spacing0 = float(spacing0);
            u.cellSize = float(layer.spacing);
            u.range = float(layer.range);
            u.gridSide = std::uint32_t(std::ceil(2.0 * layer.range / layer.spacing)) + 2;
            u.clipSize = params_.clipSize;
            u.levels = params_.levels;
            u.seed = params_.seed * 977u + std::uint32_t(li) * 7919u;
            u.firstModel = layer.firstModel;
            u.modelCount = layer.modelCount;
            u.scaleMin = layer.scaleMin;
            u.scaleMax = layer.scaleMax;
            u.minNormalZ = layer.minNormalZ;
            u.pixelScale = frame.pixelScale;
            u.minPixels = frame.minPixels;
            u.shoreHeight = 1.5f;
            u.biomeSize = float(biomeSize);
            u.worldSize = float(params_.worldSize);
            u.waterMapSize = float(waterSize);
            u.tileLog2 = std::int32_t(tileLog2_);
            u.heightLow = float(-sea * range - params_.detailBound() - frame.eye.z);
            u.heightHigh = float((1.0 - sea) * range + params_.detailBound() - frame.eye.z);
            const ForestHold& fh = forestHold();
            u.held = fh.active && layer.held ? 1.0f : 0.0f;
            for (int k = 0; k < 4; ++k) u.holdRect[k] = float(fh.rect[k]);
            for (std::uint32_t b = 0; b < kMaxBiomes; ++b)
                for (int j = 0; j < 4; ++j) u.biomeRules[b][j] = layer.rules[b][j];
            layer.candidates = u.gridSide * u.gridSide;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_PushGPUComputeUniformData(commands, 1, &clearings(), sizeof(ClearingUniforms));
            SDL_DispatchGPUCompute(pass, (u.gridSide + 7) / 8, (u.gridSide + 7) / 8, 1);
        }
        SDL_EndGPUComputePass(pass);
    }
    // AL-78: the trees the game holds, appended to the same instances (frustum,
    // range and size on screen tested as the scattering tests its own)
    {
        const ForestHold& fh = forestHold();
        if (fh.version != heldVersion_) {
            heldVersion_ = fh.version;
            std::vector<float> data;
            data.reserve(fh.trees.size() * 8);
            for (const HeldTree& t : fh.trees)
                data.insert(data.end(), {t.x, t.y, t.z, t.yaw, float(t.model), t.scale, t.range, 0.0f});
            heldCount_ = fh.active ? std::uint32_t(fh.trees.size()) : 0;
            held_ = heldCount_ ? device_->uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, data.data(), data.size() * 4)
                               : engine::Buffer{};
        }
        bool layerOn = false;
        for (const auto& l : layers_) layerOn = layerOn || (l.held && l.enabled);
        if (heldCount_ && held_ && hold_ && layerOn) {
            SDL_GPUStorageBufferReadWriteBinding rw[2] = {{instances_.get(), false, {}}, {counters_.get(), false, {}}};
            auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2);
            SDL_BindGPUComputePipeline(pass, hold_.get());
            SDL_GPUBuffer* reads[3] = {held_.get(), modelInfo_.get(), modelSlots_.get()};
            SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 3);
            HoldUniforms u{};
            for (int k = 0; k < 6; ++k)
                for (int j = 0; j < 4; ++j) u.planes[k][j] = frame.frustum[k][j];
            u.eye[0] = float(frame.eye.x);
            u.eye[1] = float(frame.eye.y);
            u.eye[2] = float(frame.eye.z);
            u.count = heldCount_;
            u.pixelScale = frame.pixelScale;
            u.minPixels = frame.minPixels;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_DispatchGPUCompute(pass, (heldCount_ + 63) / 64, 1, 1);
            SDL_EndGPUComputePass(pass);
        }
    }
    {
        SDL_GPUStorageBufferReadWriteBinding rw[2] = {{counters_.get(), false, {}}, {args_.get(), false, {}}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2);
        SDL_BindGPUComputePipeline(pass, argsPass_.get());
        SDL_GPUBuffer* reads[1] = {modelSlots_.get()};
        SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 1);
        SDL_PushGPUComputeUniformData(commands, 0, count, sizeof(count));
        SDL_DispatchGPUCompute(pass, (models + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    {
        auto* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUBufferRegion from{counters_.get(), 0, models * 4};
        const SDL_GPUTransferBufferLocation to{readback_[frame_ % 3], 0};
        SDL_DownloadFromGPUBuffer(copy, &from, &to);
        SDL_EndGPUCopyPass(copy);
    }
    ++frame_;
}

void Foliage::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view,
                   const LightUniforms& light, SDL_GPUTexture* shadowAtlas, SDL_GPUSampler* shadowSampler) {
    bool any = false;
    for (const auto& l : layers_) any = any || l.enabled;
    if (!any) return;
    SDL_BindGPUGraphicsPipeline(pass, draw_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    const SDL_GPUBufferBinding buffers[2] = {{vertices_.get(), 0}, {instances_.get(), 0}};
    SDL_BindGPUVertexBuffers(pass, 0, buffers, 2);
    const SDL_GPUBufferBinding index{indices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const SDL_GPUTextureSamplerBinding textures[2] = {{textures_.get(), linear_.get()}, {shadowAtlas, shadowSampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, args_.get(), 0, std::uint32_t(models_.size()));
}

void Foliage::drawShadow(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view) {
    bool any = false;
    for (const auto& l : layers_) any = any || l.enabled;
    if (!any || !shadow_) return;
    SDL_BindGPUGraphicsPipeline(pass, shadow_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    const SDL_GPUBufferBinding buffers[2] = {{vertices_.get(), 0}, {instances_.get(), 0}};
    SDL_BindGPUVertexBuffers(pass, 0, buffers, 2);
    const SDL_GPUBufferBinding index{indices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const SDL_GPUTextureSamplerBinding texture{textures_.get(), linear_.get()};
    SDL_BindGPUFragmentSamplers(pass, 0, &texture, 1);
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, args_.get(), 0, std::uint32_t(models_.size()));
}

std::uint64_t Foliage::drawnInstances() const {
    std::uint64_t n = 0;
    for (const auto& l : layers_) n += l.drawn;
    return n;
}

std::uint64_t Foliage::candidates() const {
    std::uint64_t n = 0;
    for (const auto& l : layers_) n += l.candidates;
    return n;
}

} // namespace terrain_core
