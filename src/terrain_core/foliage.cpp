#include "terrain_core/foliage.hpp"

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
    float heightHigh, waterMapSize, pad[2];
    float biomeRules[kMaxBiomes][4];
};
static_assert(sizeof(SpawnUniforms) == 736);

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
        layer.firstModel = std::uint32_t(models_.size());
        const auto& names = l["models"];
        const std::uint32_t perModel = (layer.maxCount + std::uint32_t(names.size()) - 1) / std::uint32_t(names.size());
        for (const auto& n : names) {
            const std::string name = n.get<std::string>();
            std::vector<Vertex> v;
            std::vector<std::uint32_t> idx;
            float lo[3], hi[3];
            if (!readMesh(dir / (name + ".mesh"), v, idx, lo, hi))
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
            textureFiles.push_back(dir / (name + ".png"));
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
    return true;
}

void Foliage::spawn(SDL_GPUCommandBuffer* commands, const FoliageFrame& frame, SDL_GPUTexture* clipmap,
                    SDL_GPUTexture* biomeMap, std::uint32_t biomeSize, SDL_GPUTexture* water,
                    std::uint32_t waterSize) {
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
        SDL_GPUBuffer* reads[2] = {modelInfo_.get(), modelSlots_.get()};
        SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);
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
            for (std::uint32_t b = 0; b < kMaxBiomes; ++b)
                for (int j = 0; j < 4; ++j) u.biomeRules[b][j] = layer.rules[b][j];
            layer.candidates = u.gridSide * u.gridSide;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_DispatchGPUCompute(pass, (u.gridSide + 7) / 8, (u.gridSide + 7) / 8, 1);
        }
        SDL_EndGPUComputePass(pass);
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
