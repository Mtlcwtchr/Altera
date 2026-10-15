#include "terrain_core/props.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace terrain_core {
namespace {

// `blend`: a cliff tile's vertex, how far it wears the tile's second biome
// (version 2 of the file; version 1 has none, 0).
struct Vertex { float position[3], normal[3], uv[2], blend; };
constexpr int kFloats = sizeof(Vertex) / sizeof(float);

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
    if (!in || std::memcmp(magic, "FOLM", 4) != 0 || (version != 1 && version != 2)) return false;
    vertices.resize(vertexCount);
    indices.resize(indexCount);
    if (version == 2) {
        in.read(reinterpret_cast<char*>(vertices.data()), std::streamsize(vertexCount * sizeof(Vertex)));
    } else {
        for (auto& v : vertices) {
            in.read(reinterpret_cast<char*>(&v), 8 * sizeof(float));
            v.blend = 0;
        }
    }
    in.read(reinterpret_cast<char*>(indices.data()), std::streamsize(indexCount * 4));
    return bool(in);
}

// Pixels a model must cover on screen for each level (radius in pixels).
constexpr float kLevelPixels[2] = {260.0f, 70.0f};

} // namespace

const char* Props::categoryName(int c) {
    static const char* names[kCategories] = {"terrain", "megaliths", "buildings"};
    return c >= 0 && c < kCategories ? names[c] : "?";
}

Props::~Props() {
    for (auto& [i, f] : pending_)
        if (f.valid()) f.wait();
    if (device_) {
        for (auto& [i, m] : detail_)
            if (m.transfer) SDL_ReleaseGPUTransferBuffer(device_->handle(), m.transfer);
        for (auto& m : spare_)
            if (m.transfer) SDL_ReleaseGPUTransferBuffer(device_->handle(), m.transfer);
    }
    if (device_ && transfer_) SDL_ReleaseGPUTransferBuffer(device_->handle(), transfer_);
}

bool Props::open(engine::Device& device, const std::filesystem::path& featuresFile,
                 const std::filesystem::path& modelDir, const std::filesystem::path& materialsJson,
                 const std::function<double(double, double)>& ground, std::string* why) {
    device_ = &device;
    auto fail = [&](const std::string& what) { if (why) *why = what; return false; };
    nlohmann::json features, table;
    try {
        std::ifstream in(featuresFile);
        if (!in) return fail("cannot read " + featuresFile.string() + " (run tools/terrain_lab/world_features.py)");
        features = nlohmann::json::parse(in);
        std::ifstream t(materialsJson);
        if (!t) return fail("cannot read " + materialsJson.string());
        table = nlohmann::json::parse(t);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    // Layers of the ground's material array: its materials in the order the
    // biome table loads them (nlohmann's object order, as BiomeTable::load).
    std::map<std::string, float> layerOf;
    for (const auto& [name, spec] : table["materials"].items()) {
        (void)spec;
        const float layer = float(layerOf.size());
        layerOf[name] = layer;
    }
    const float fallback = layerOf.count("rocky_grey") ? layerOf["rocky_grey"] : 0.0f;
    {
        std::ifstream rules(materialsJson.parent_path() / "props.json");
        std::string moss = "grass_dark", cover = "grass_dry";
        if (rules) {
            try {
                const auto j = nlohmann::json::parse(rules);
                moss = j.value("moss_material", moss);
                cover = j.value("cover_material", cover);
            } catch (const std::exception&) {}
        }
        mossLayer_ = layerOf.count(moss) ? layerOf[moss] : fallback;
        coverLayer_ = layerOf.count(cover) ? layerOf[cover] : fallback;
    }
    {
        // What a cliff wears in each biome: the biome's slope material on its
        // walls (or props.json cliffs.materials by biome name), its ground on
        // what faces up, its tint; cliffs.moss by biome name.
        nlohmann::json cliffs;
        try {
            std::ifstream rules(materialsJson.parent_path() / "props.json");
            if (rules) cliffs = nlohmann::json::parse(rules).value("cliffs", nlohmann::json::object());
        } catch (const std::exception&) {}
        const auto rockOf = cliffs.value("materials", nlohmann::json::object());
        const auto mossOf = cliffs.value("moss", nlohmann::json::object());
        auto layer = [&](const std::string& name, float otherwise) {
            return layerOf.count(name) ? layerOf[name] : otherwise;
        };
        for (auto& b : cliffBiomes_.stone) b[0] = b[1] = fallback, b[2] = 0.85f, b[3] = 0.7f;
        for (auto& t : cliffBiomes_.tint) t[0] = t[1] = t[2] = 1.0f, t[3] = 0.0f;
        for (const auto& b : table.value("biomes", nlohmann::json::array())) {
            const int id = b.value("id", -1);
            if (id < 0 || id >= int(CliffBiomes::kBiomes)) continue;
            const std::string name = b.value("name", std::string());
            const std::string base = b.value("base", std::string());
            const auto slope = b.value("slope", nlohmann::json::object());
            auto& stone = cliffBiomes_.stone[std::size_t(id)];
            stone[1] = layer(base, fallback);
            stone[0] = layer(rockOf.value(name, slope.value("material", base)), stone[1]);
            stone[2] = slope.value("from", 0.85f);
            stone[3] = slope.value("to", 0.7f);
            auto& tint = cliffBiomes_.tint[std::size_t(id)];
            if (b.contains("tint"))
                for (int i = 0; i < 3; ++i) tint[i] = b["tint"][i].get<float>();
            tint[3] = std::clamp(mossOf.value(name, 0.0f), 0.0f, 1.0f);
        }
    }

    std::vector<Vertex> allVertices;
    std::vector<std::uint32_t> allIndices;
    std::map<std::string, std::uint32_t> modelIndex;
    // A model is found by name in the models' folder, or - the world's own,
    // its cliff tiles - in a folder beside the features file.
    auto modelFor = [&](const std::string& name, const std::filesystem::path& dir, bool keepCpu) -> int {
        const std::string key = (dir / name).string();
        if (auto it = modelIndex.find(key); it != modelIndex.end()) return int(it->second);
        Model m;
        m.name = name;
        for (int k = 0; k < kLevels; ++k) {
            std::vector<Vertex> v;
            std::vector<std::uint32_t> idx;
            float lo[3], hi[3];
            const auto file = dir / (name + "_lod" + std::to_string(k) + ".mesh");
            if (!readMesh(file, v, idx, lo, hi)) return -1;
            if (k == 0) {
                std::memcpy(m.lo, lo, sizeof(lo));
                std::memcpy(m.hi, hi, sizeof(hi));
                if (keepCpu) {
                    CpuMesh& c = cpu_[std::uint32_t(models_.size())];
                    c.vertices.assign(reinterpret_cast<const float*>(v.data()),
                                      reinterpret_cast<const float*>(v.data()) + v.size() * kFloats);
                    c.indices = idx;
                }
            }
            m.levels[k].firstIndex = std::uint32_t(allIndices.size());
            m.levels[k].indexCount = std::uint32_t(idx.size());
            const auto base = std::uint32_t(allVertices.size());
            for (auto i : idx) allIndices.push_back(base + i);
            allVertices.insert(allVertices.end(), v.begin(), v.end());
        }
        const float dx = m.hi[0] - m.lo[0], dy = m.hi[1] - m.lo[1], dz = m.hi[2] - m.lo[2];
        m.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        modelIndex[key] = std::uint32_t(models_.size());
        models_.push_back(m);
        return int(models_.size()) - 1;
    };

    for (const auto& o : features["instances"]) {
        const std::string name = o.value("m", std::string());
        const std::string own = o.value("dir", std::string());
        const std::filesystem::path dir = own.empty() ? modelDir : featuresFile.parent_path() / own;
        const bool detail = o.value("detail", false);
        const int model = modelFor(name, dir, detail);
        if (model < 0) return fail("cannot read model " + name + " in " + dir.string() +
                                   (own.empty() ? " (run tools/blender/terrain_props.py --all)"
                                                : " (run tools/terrain_lab/world_features.py)"));
        const Model& m = models_[std::size_t(model)];
        Instance in;
        in.model = std::uint32_t(model);
        in.detail = detail;
        const std::string cat = o.value("cat", std::string("terrain"));
        in.category = cat == "megaliths" ? kMegaliths : cat == "buildings" ? kBuildings : kTerrain;
        ++counts_[in.category];
        in.x = o.value("x", 0.0);
        in.y = o.value("y", 0.0);
        in.yaw = o.value("yaw", 0.0f);
        in.sx = o["s"][0].get<float>();
        in.sy = o["s"][1].get<float>();
        in.sz = o["s"][2].get<float>();
        const std::string material = o.value("material", std::string());
        in.layer = layerOf.count(material) ? layerOf[material] : fallback;
        in.moss = std::clamp(o.value("moss", 0.0f), 0.0f, 1.0f);
        in.cover = std::clamp(o.value("cover", 0.0f), 0.0f, 1.0f);
        in.mirror = std::clamp(o.value("mirror", 0.0f), 0.0f, 1.0f);
        in.slab = o.value("look", std::string()) == "a1_monolith";
        // A cliff tile: the two biomes it wears (world_cliffs.py), by id.
        if (o.contains("biomes") && o["biomes"].size() == 2)
            for (int k = 0; k < 2; ++k)
                in.biomes[k] = std::clamp(o["biomes"][k].get<int>(), 0, int(CliffBiomes::kBiomes) - 1);
        if (o.contains("z")) {
            in.z = o["z"].get<double>();
        } else {
            // Stood on the lowest ground under its footprint, so no edge of
            // its foot hangs over a slope.
            const double r = 0.5 * std::max(double(m.hi[0] - m.lo[0]) * in.sx, double(m.hi[1] - m.lo[1]) * in.sy);
            double z = ground(in.x, in.y);
            for (int k = 0; k < 8; ++k) {
                const double a = k * 0.7853981633974483;
                z = std::min(z, ground(in.x + std::cos(a) * r * 0.7, in.y + std::sin(a) * r * 0.7));
            }
            // Buried: a tower in the dunes, a wall to its arches.
            in.z = z - o.value("sink", 0.0);
            // A spire is stretched from that ground up to the height it was
            // given: always reaching the high ground round it.
            if (o.contains("top") && m.hi[2] > 1.0f) in.sz = float((o["top"].get<double>() - in.z) / m.hi[2]);
        }
        // Bounding sphere (yaw ignored: the radius covers any turn).
        const double c = std::cos(in.yaw), s = std::sin(in.yaw);
        const double mx = 0.5 * (m.lo[0] + m.hi[0]) * in.sx, my = 0.5 * (m.lo[1] + m.hi[1]) * in.sy;
        in.cx = in.x + mx * c - my * s;
        in.cy = in.y + mx * s + my * c;
        in.cz = in.z + 0.5 * (m.lo[2] + m.hi[2]) * in.sz;
        const float dx = (m.hi[0] - m.lo[0]) * in.sx, dy = (m.hi[1] - m.lo[1]) * in.sy, dz = (m.hi[2] - m.lo[2]) * in.sz;
        in.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        instances_.push_back(in);
    }
    if (instances_.empty()) return fail("no instances in " + featuresFile.string());

    // A-1's material: its manifest beside the biome table, its tiles at one
    // size in engine/ beside the model (tools/terrain_lab/import_megalith.py).
    if (std::any_of(instances_.begin(), instances_.end(), [](const Instance& i) { return i.slab; })) {
        const auto manifest = materialsJson.parent_path() / "megaliths" / "a1_monolith.material.json";
        const auto root = materialsJson.parent_path().parent_path().parent_path().parent_path();
        nlohmann::json mat;
        try {
            std::ifstream in(manifest);
            if (!in) return fail("cannot read " + manifest.string());
            mat = nlohmann::json::parse(in);
        } catch (const std::exception& e) {
            return fail(manifest.string() + ": " + e.what());
    // The decor lists: small rocks, each a model turned and scaled, at its height.
    for (const auto& d : features.value("decor", nlohmann::json::array())) {
        const auto file = featuresFile.parent_path() / d.value("file", std::string());
        const auto dir = featuresFile.parent_path() / d.value("dir", std::string());
        const std::string material = d.value("material", std::string());
        const float layer = layerOf.count(material) ? layerOf[material] : fallback;
        std::ifstream in(file, std::ios::binary);
        char magic[4];
        std::uint32_t version = 0, names = 0, count = 0;
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&names), 4);
        if (!in || std::memcmp(magic, "DECR", 4) != 0 || version != 1)
            return fail(file.string() + ": not a decor list (run tools/terrain_lab/canyon_features.py)");
        std::vector<int> model(names);
        for (auto& m : model) {
            std::uint16_t n = 0;
            in.read(reinterpret_cast<char*>(&n), 2);
            std::string name(n, '\0');
            in.read(name.data(), n);
            m = modelFor(name, dir, false);
            if (!in || m < 0) return fail("cannot read model " + name + " in " + dir.string());
        }
        in.read(reinterpret_cast<char*>(&count), 4);
        struct Rec { float x, y, z, yaw, size; std::uint32_t model; };
        std::vector<Rec> recs(count);
        in.read(reinterpret_cast<char*>(recs.data()), std::streamsize(count * sizeof(Rec)));
        if (!in) return fail(file.string() + ": truncated");
        instances_.reserve(instances_.size() + count);
        for (const Rec& r : recs) {
            if (r.model >= names) return fail(file.string() + ": bad model index");
            const Model& m = models_[std::size_t(model[r.model])];
            Instance a;
            a.model = std::uint32_t(model[r.model]);
            a.category = kTerrain;
            ++counts_[kTerrain];
            a.x = r.x;
            a.y = r.y;
            a.z = r.z;
            a.yaw = r.yaw;
            a.sx = a.sy = a.sz = r.size;
            a.layer = layer;
            const double c = std::cos(a.yaw), sn = std::sin(a.yaw);
            const double mx = 0.5 * (m.lo[0] + m.hi[0]) * a.sx, my = 0.5 * (m.lo[1] + m.hi[1]) * a.sy;
            a.cx = a.x + mx * c - my * sn;
            a.cy = a.y + mx * sn + my * c;
            a.cz = a.z + 0.5 * (m.lo[2] + m.hi[2]) * a.sz;
            a.radius = m.radius * r.size;
            instances_.push_back(a);
        }
    }
        }
    buildGrid();
        std::vector<std::vector<std::filesystem::path>> tiles;
        for (const char* key : {"hatch", "grooves", "moss", "strokes"}) {
            const std::filesystem::path file = mat["textures"][key].value("file", std::string());
            tiles.push_back({root / file.parent_path() / "engine" / (std::string(key) + ".png")});
        }
        megalithTiles_ = device.loadArrayMipped(tiles, true);
        if (!megalithTiles_) return fail("A-1 tiles (run tools/terrain_lab/import_megalith.py): " + device.error());
        const auto knobs = mat.value("knobs", nlohmann::json::object());
        for (std::size_t k = 0; k < std::size(MegalithLook::kKnobs); ++k)
            megalithLook_.knobs[k] = knobs.value(MegalithLook::kKnobs[k], 0.0f);
        megalithLook_.knobs[MegalithLook::kStoneTile] = mat["textures"]["hatch"].value("tile_m", 36.0f);
    }

    {
        engine::Device::Uploader upload(device);
        vertices_ = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX, allVertices.data(), allVertices.size() * sizeof(Vertex));
        indices_ = upload.add(SDL_GPU_BUFFERUSAGE_INDEX, allIndices.data(), allIndices.size() * 4);
        if (!upload.finish()) return fail("props upload: " + device.error());
    }
    // Room for every instance twice: the view's and the shadows' lists (of
    // the small ones only as many as are ever drawn).
    capacity_ = std::uint32_t(wide_.size() + std::min<std::size_t>(gridItems_.size(), kMaxSmallDrawn)) * 2;
    gpuInstances_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, std::size_t(capacity_) * sizeof(Gpu));
    argsCapacity_ = std::uint32_t(models_.size()) * kLevels * 2;
    args_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_INDIRECT,
                              std::size_t(argsCapacity_) * sizeof(SDL_GPUIndexedIndirectDrawCommand));
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = std::uint32_t(std::size_t(capacity_) * sizeof(Gpu) +
                              std::size_t(argsCapacity_) * sizeof(SDL_GPUIndexedIndirectDrawCommand));
    transfer_ = SDL_CreateGPUTransferBuffer(device.handle(), &info);
    gpuBytes_ = allVertices.size() * sizeof(Vertex) + allIndices.size() * 4 + std::uint64_t(capacity_) * sizeof(Gpu);

    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampling.max_lod = 1000.0f;
    sampling.enable_anisotropy = true;
    sampling.max_anisotropy = 8.0f;
    linear_ = device.makeSampler(sampling);
    // A-1's tiles: hard pixels up close, smooth mips far off.
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.enable_anisotropy = false;
    hardPixels_ = device.makeSampler(sampling);
    if (!hardPixels_) return fail("props sampler: " + std::string(SDL_GetError()));

    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/props.hlsl";
    wanted.vertexEntry = "propsVs";
    wanted.fragmentEntry = "propsPs";
    wanted.depthClip = true;
    // The models are wound counter-clockwise seen from outside. terrain_lab's
    // camera (eye-relative right, up, forward) keeps the world's handedness
    // on the screen, so with the device's clockwise front face the faces
    // turned to the eye arrive as "back": cull the other side. Measured: with
    // BACK the cliff walls facing the eye vanished and the spires showed
    // their insides.
    wanted.cull = SDL_GPU_CULLMODE_FRONT;
    wanted.buffers = {{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                      {1, sizeof(Gpu), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                         {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
                         {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                         {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                         {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32},
                         {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48},
                         {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 32}};
    draw_ = device.makePipeline(wanted);
    wanted.fragmentEntry = "propsShadowPs";
    wanted.depthOnly = true;
    wanted.cull = SDL_GPU_CULLMODE_NONE;
    wanted.depthBiasConstant = -2.0f;
    wanted.depthBiasSlope = -2.5f;
    shadow_ = device.makePipeline(wanted);
    if (!vertices_ || !indices_ || !gpuInstances_ || !args_ || !transfer_ || !linear_ || !hardPixels_ || !draw_ ||
        !shadow_)
        return fail("props GPU objects: " + device.error());
    buckets_.assign(models_.size() * kLevels, {});
    return true;
}

Props::Gpu Props::gpuOf(std::uint32_t i, const PropsFrame& frame) const {
    const Instance& in = instances_[i];
    Gpu g{};
    g.posYaw[0] = float(in.x - frame.eye.x);
    g.posYaw[1] = float(in.y - frame.eye.y);
    g.posYaw[2] = float(in.z - frame.eye.z);
    g.posYaw[3] = in.yaw;
    g.scale[0] = in.sx;
    g.scale[1] = in.sy;
    g.scale[2] = in.sz;
    // Layer in the whole part, moss in the fraction (a layer is a small integer).
    g.scale[3] = in.layer + std::min(in.moss, 0.99f) * 0.5f;
    // Where the model stands, wrapped: the rock's texture coordinates come
    // from it plus the local position, never from a float world coordinate
    // in the millions.
    g.anchor[0] = float(std::fmod(in.x, 4096.0));
    g.anchor[1] = float(std::fmod(in.y, 4096.0));
    g.anchor[2] = float(in.z);
    // A per-instance variation, 0..1; +2 for a megalith, whose silhouette
    // the haze softens but does not swallow (seen from across its biome).
    g.anchor[3] = float(i % 97) / 97.0f + (in.category == kMegaliths ? 2.0f : 0.0f);
    g.extra[0] = in.cover;
    g.extra[1] = in.slab ? -2.0f : float(in.biomes[0]);
    g.extra[2] = float(in.biomes[1]);
    g.extra[3] = in.mirror;
    return g;
}

void Props::fill(const PropsFrame& frame, bool shadow, std::vector<Batch>& batches) {
    for (auto& b : buckets_) b.clear();
    std::vector<std::uint32_t> refined;
    int builds = 0;
    // The wide ones one by one; the small ones from the grid's cells within
    // reach (as far as kSmall shows minPixels, or the shadows' range).
    std::vector<std::uint32_t> near;
    if (gridW_ > 0) {
        const double reach = shadow ? frame.shadowRange + kSmall
                                    : double(kSmall) * frame.pixelScale / std::max(frame.minPixels, 0.1f);
        const int cx0 = std::max(0, int((frame.eye.x - reach - gridX0_) / kCell));
        const int cy0 = std::max(0, int((frame.eye.y - reach - gridY0_) / kCell));
        const int cx1 = std::min(gridW_ - 1, int((frame.eye.x + reach - gridX0_) / kCell));
        const int cy1 = std::min(gridH_ - 1, int((frame.eye.y + reach - gridY0_) / kCell));
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const std::size_t c = std::size_t(cy) * std::size_t(gridW_) + std::size_t(cx);
                near.insert(near.end(), gridItems_.begin() + gridStart_[c], gridItems_.begin() + gridStart_[c + 1]);
            }
    }
    std::uint32_t smallTaken = 0;
    const std::size_t wideCount = wide_.size();
    for (std::size_t k = 0; k < wideCount + near.size(); ++k) {
        const std::uint32_t i = k < wideCount ? wide_[k] : near[k - wideCount];
        const Instance& in = instances_[i];
        if (!enabled_[in.category]) continue;
        const double rx = in.cx - frame.eye.x, ry = in.cy - frame.eye.y, rz = in.cz - frame.eye.z;
        const double d = std::sqrt(rx * rx + ry * ry + rz * rz);
        int level = 0;
        if (shadow) {
            if (d - in.radius > frame.shadowRange) continue;
            level = 1;   // shadows are soft at this size: the middle level does
        } else {
            bool inside = true;
            for (const auto& p : frame.frustum)
                if (p[0] * rx + p[1] * ry + p[2] * rz + p[3] < -double(in.radius) * std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2])) {
                    inside = false;
                    break;
                }
            if (!inside) continue;
            const float pixels = d > in.radius ? float(in.radius / d) * frame.pixelScale : 1e9f;
            if (pixels < frame.minPixels) continue;
            level = pixels > kLevelPixels[0] ? 0 : pixels > kLevelPixels[1] ? 1 : 2;
            if (in.detail && d - in.radius < frame.detailRange) {
                auto it = detail_.find(i);
                // Refined off the frame's thread: until it is ready, level 0.
                if (it == detail_.end() && !pending_.count(i) && builds < frame.detailBuilds) {
                    ++builds;
                    const Instance& at = instances_[i];
                    if (const auto c = cpu_.find(at.model); c != cpu_.end())
                        pending_[i] = std::async(std::launch::async, refineSurface, &c->second, at.x, at.y, at.z);
                }
                if (it != detail_.end()) {
                    it->second.lastUsed = frame_;
                    refined.push_back(i);
                    continue;
                }
void Props::buildGrid() {
    wide_.clear();
    std::vector<std::uint32_t> small;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (std::uint32_t i = 0; i < instances_.size(); ++i) {
        const Instance& in = instances_[i];
        if (in.radius >= kSmall || in.detail) {
            wide_.push_back(i);
            continue;
        }
        small.push_back(i);
        x0 = std::min(x0, in.cx), y0 = std::min(y0, in.cy), x1 = std::max(x1, in.cx), y1 = std::max(y1, in.cy);
    }
    gridItems_.clear();
    gridStart_.assign(1, 0);
    gridW_ = gridH_ = 0;
    if (small.empty()) return;
    gridX0_ = x0, gridY0_ = y0;
    gridW_ = int((x1 - x0) / kCell) + 1, gridH_ = int((y1 - y0) / kCell) + 1;
    auto cellOf = [&](std::uint32_t i) {
        return std::size_t(int((instances_[i].cy - y0) / kCell)) * std::size_t(gridW_) + std::size_t(int((instances_[i].cx - x0) / kCell));
    };
    gridStart_.assign(std::size_t(gridW_) * gridH_ + 1, 0);
    for (auto i : small) ++gridStart_[cellOf(i) + 1];
    for (std::size_t c = 1; c < gridStart_.size(); ++c) gridStart_[c] += gridStart_[c - 1];
    gridItems_.resize(small.size());
    std::vector<std::uint32_t> at(gridStart_.begin(), gridStart_.end() - 1);
    for (auto i : small) gridItems_[at[cellOf(i)]++] = i;
}

            }
        }
        buckets_[in.model * kLevels + level].push_back(i);
    }
    for (std::uint32_t b = 0; b < buckets_.size(); ++b) {
        if (buckets_[b].empty()) continue;
        Batch batch{b / kLevels, b % kLevels, std::uint32_t(staging_.size()), std::uint32_t(buckets_[b].size())};
        if (k >= wideCount && smallTaken >= kMaxSmallDrawn) break;
        for (auto i : buckets_[b]) staging_.push_back(gpuOf(i, frame));
        batches.push_back(batch);
    }
    for (auto i : refined) {
        batches.push_back({instances_[i].model, 0, std::uint32_t(staging_.size()), 1, &detail_[i]});
        staging_.push_back(gpuOf(i, frame));
    }
    if (shadow) return;
    detailDrawn_ = std::uint32_t(refined.size());
    // Forget surfaces the eye left long ago, the oldest first (never one
    // drawn this frame: the batches point at it).
    while (detail_.size() > 48) {
        auto oldest = detail_.begin();
        for (auto it = detail_.begin(); it != detail_.end(); ++it)
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        if (oldest->second.lastUsed + 3 >= frame_) break;
        forget(oldest);
    }
}

namespace {

// Value noise in 3D, smooth, 0..1.
double hashCell(std::int64_t x, std::int64_t y, std::int64_t z) {
    std::uint64_t h = std::uint64_t(x) * 0x9E3779B97F4A7C15ull ^ std::uint64_t(y) * 0xC2B2AE3D27D4EB4Full ^
                      std::uint64_t(z) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return double(h & 0xffffff) / double(0xffffff);
}

double valueNoise(double x, double y, double z) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        if (k >= wideCount) ++smallTaken;
    const auto ix = std::int64_t(fx), iy = std::int64_t(fy), iz = std::int64_t(fz);
    auto s = [](double t) { return t * t * (3 - 2 * t); };
    const double u = s(x - fx), v = s(y - fy), w = s(z - fz);
    double c[2][2][2];
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int d = 0; d < 2; ++d) c[a][b][d] = hashCell(ix + a, iy + b, iz + d);
    auto l = [](double p, double q, double t) { return p + (q - p) * t; };
    return l(l(l(c[0][0][0], c[1][0][0], u), l(c[0][1][0], c[1][1][0], u), v),
             l(l(c[0][0][1], c[1][0][1], u), l(c[0][1][1], c[1][1][1], u), v), w);
}

// Rock: broad lumps, then blocks split by joints - ridged, so the faces come
// out as facets and cracks rather than as a soft wobble.
double rock(double x, double y, double z) {
    double v = valueNoise(x / 23.0, y / 23.0, z / 17.0) - 0.5;
    v += (0.5 - std::abs(valueNoise(x / 8.5, y / 8.5, z / 6.0) - 0.5)) * 0.9 - 0.2;
    v += (valueNoise(x / 3.1, y / 3.1, z / 2.6) - 0.5) * 0.35;
    return v;
}

} // namespace

Props::Surface Props::refineSurface(const CpuMesh* mesh, double x, double y, double z) {
    const CpuMesh& base = *mesh;
    // Two rounds of splitting every edge at its middle: a 16 m grid becomes 4 m.
    std::vector<float> v = base.vertices;
    std::vector<std::uint32_t> tris = base.indices;
    for (int round = 0; round < 2; ++round) {
        std::map<std::uint64_t, std::uint32_t> mid;
        std::vector<std::uint32_t> next;
        next.reserve(tris.size() * 4);
        auto middle = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint64_t key = a < b ? (std::uint64_t(a) << 32 | b) : (std::uint64_t(b) << 32 | a);
            if (auto it = mid.find(key); it != mid.end()) return it->second;
            const auto n = std::uint32_t(v.size() / kFloats);
            for (int k = 0; k < kFloats; ++k)
                v.push_back(0.5f * (v[std::size_t(a) * kFloats + k] + v[std::size_t(b) * kFloats + k]));
            mid[key] = n;
            return n;
        };
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            const std::uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
            const std::uint32_t ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
            next.insert(next.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        }
        tris.swap(next);
    }
    // Roughen: each point pushed along its (smooth) normal by the rock noise,
    // much on a wall, little on a ledge. Fading out towards the tile's
    // border, which stays put, so the tiles round it meet it whatever their
    // level and with no step where the roughness would stop short.
    const std::size_t count = v.size() / kFloats;
    for (std::size_t k = 0; k < count; ++k) {
        float* p = &v[k * kFloats];
        const double edge = std::min(std::min(double(p[0]), 1024.0 - p[0]), std::min(double(p[1]), 1024.0 - p[1]));
        // (wide: a refined tile beside one drawn plain shows no step in roughness)
        const double fade = std::clamp((edge - 0.5) / 160.0, 0.0, 1.0);
        if (fade <= 0) continue;
        const double nl = std::sqrt(double(p[3]) * p[3] + double(p[4]) * p[4] + double(p[5]) * p[5]);
        if (nl < 1e-6) continue;
        const double nx = p[3] / nl, ny = p[4] / nl, nz = p[5] / nl;
        const double steep = 1.0 - std::abs(nz);
        const double amount = (0.4 + 2.6 * steep) * rock(x + p[0], y + p[1], z + p[2]) * fade * fade * (3 - 2 * fade);
        p[0] += float(nx * amount);
        p[1] += float(ny * amount);
        p[2] += float(nz * amount);
    }
    // Normals again, from the roughened faces.
    std::vector<double> n(count * 3, 0.0);
    for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
        const float* a = &v[std::size_t(tris[t]) * kFloats];
        const float* b = &v[std::size_t(tris[t + 1]) * kFloats];
        const float* c = &v[std::size_t(tris[t + 2]) * kFloats];
        const double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
        const double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        const double f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int q = 0; q < 3; ++q)
            for (int j = 0; j < 3; ++j) n[std::size_t(tris[t + q]) * 3 + j] += f[j];
    }
    for (std::size_t k = 0; k < count; ++k) {
        const double l = std::sqrt(n[k * 3] * n[k * 3] + n[k * 3 + 1] * n[k * 3 + 1] + n[k * 3 + 2] * n[k * 3 + 2]);
        if (l < 1e-12) continue;
        for (int j = 0; j < 3; ++j) v[k * kFloats + 3 + j] = float(n[k * 3 + j] / l);
    }
    return {std::move(v), std::move(tris)};
}

bool Props::upload(SDL_GPUCommandBuffer* commands, std::uint32_t i, const Surface& surface) {
    const auto start = std::chrono::steady_clock::now();
    const auto vbytes = std::uint32_t(surface.first.size() * 4), ibytes = std::uint32_t(surface.second.size() * 4);
    // Buffers of a surface forgotten earlier when they are big enough (making
    // new ones costs milliseconds on the frame), else new ones with room to spare.
    DetailMesh mesh;
    for (auto it = spare_.begin(); it != spare_.end(); ++it)
        if (it->vertexBytes >= vbytes && it->indexBytes >= ibytes) {
            mesh = std::move(*it);
            spare_.erase(it);
            break;
        }
    if (!mesh.transfer) {
        mesh.vertexBytes = vbytes + vbytes / 3;
        mesh.indexBytes = ibytes + ibytes / 3;
        mesh.vertices = device_->makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, mesh.vertexBytes);
        mesh.indices = device_->makeBuffer(SDL_GPU_BUFFERUSAGE_INDEX, mesh.indexBytes);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = mesh.vertexBytes + mesh.indexBytes;
        mesh.transfer = SDL_CreateGPUTransferBuffer(device_->handle(), &info);
        if (!mesh.vertices || !mesh.indices || !mesh.transfer) {
            if (mesh.transfer) SDL_ReleaseGPUTransferBuffer(device_->handle(), mesh.transfer);
            return false;
        }
    }
    auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_->handle(), mesh.transfer, true));
    std::memcpy(mapped, surface.first.data(), vbytes);
    std::memcpy(mapped + vbytes, surface.second.data(), ibytes);
    SDL_UnmapGPUTransferBuffer(device_->handle(), mesh.transfer);
    auto* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation fromV{mesh.transfer, 0}, fromI{mesh.transfer, vbytes};
    const SDL_GPUBufferRegion toV{mesh.vertices.get(), 0, vbytes}, toI{mesh.indices.get(), 0, ibytes};
    SDL_UploadToGPUBuffer(copy, &fromV, &toV, true);
    SDL_UploadToGPUBuffer(copy, &fromI, &toI, true);
    SDL_EndGPUCopyPass(copy);
    mesh.indexCount = std::uint32_t(surface.second.size());
    mesh.lastUsed = frame_;
    detail_[i] = std::move(mesh);
    detailMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
}

void Props::forget(std::map<std::uint32_t, DetailMesh>::iterator it) {
    // Unused for frames: its buffers go to the next surface (cycled on
    // upload, so nothing the card still reads is overwritten).
    if (spare_.size() < 16) spare_.push_back(std::move(it->second));
    else if (it->second.transfer) SDL_ReleaseGPUTransferBuffer(device_->handle(), it->second.transfer);
    detail_.erase(it);
}

void Props::collect(SDL_GPUCommandBuffer* commands, bool wait) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!wait && it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        upload(commands, it->first, it->second.get());
        it = pending_.erase(it);
    }
}

void Props::prepare(SDL_GPUCommandBuffer* commands, const PropsFrame& frame) {
    const auto start = std::chrono::steady_clock::now();
    staging_.clear();
    viewBatches_.clear();
    shadowBatches_.clear();
    ++frame_;
    // A still picture waits for its refined cliffs; a moving eye never does.
    collect(commands, frame.detailBuilds > 1);
    fill(frame, false, viewBatches_);
    drawn_ = 0;
    triangles_ = 0;
    for (const auto& b : viewBatches_) {
        drawn_ += b.count;
        triangles_ += std::uint64_t(b.count) *
                      (b.detail ? b.detail->indexCount : models_[b.model].levels[b.level].indexCount) / 3;
    }
    if (frame.shadowRange > 0) fill(frame, true, shadowBatches_);
    cullMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Indirect commands for the shared meshes: view's first, then shadows'.
    // Each reads its run of the instance buffer through first_instance (the
    // instance data are a vertex stream, so that is allowed).
    std::vector<SDL_GPUIndexedIndirectDrawCommand> commandsOut;
    auto addArgs = [&](const std::vector<Batch>& batches) {
        std::uint32_t n = 0;
        for (const auto& b : batches) {
            if (b.detail) continue;
            const Level& l = models_[b.model].levels[b.level];
            commandsOut.push_back({l.indexCount, b.count, l.firstIndex, 0, b.first});
            ++n;
        }
        return n;
    };
    viewDraws_ = addArgs(viewBatches_);
    shadowDraws_ = addArgs(shadowBatches_);
    if (staging_.empty()) return;
    const auto bytes = std::uint32_t(staging_.size() * sizeof(Gpu));
    const auto argBytes = std::uint32_t(commandsOut.size() * sizeof(SDL_GPUIndexedIndirectDrawCommand));
    const auto argsAt = std::uint32_t(std::size_t(capacity_) * sizeof(Gpu));
    auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_->handle(), transfer_, true));
    if (!mapped) return;
    std::memcpy(mapped, staging_.data(), bytes);
    if (argBytes) std::memcpy(mapped + argsAt, commandsOut.data(), argBytes);
    SDL_UnmapGPUTransferBuffer(device_->handle(), transfer_);
    auto* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation from{transfer_, 0};
    const SDL_GPUBufferRegion to{gpuInstances_.get(), 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    if (argBytes) {
        const SDL_GPUTransferBufferLocation fromArgs{transfer_, argsAt};
        const SDL_GPUBufferRegion toArgs{args_.get(), 0, argBytes};
        SDL_UploadToGPUBuffer(copy, &fromArgs, &toArgs, true);
    }
    SDL_EndGPUCopyPass(copy);
}

void Props::drawBatches(SDL_GPURenderPass* pass, const std::vector<Batch>& batches, std::uint32_t firstArg,
                        std::uint32_t args) {
    if (args) {
        const SDL_GPUBufferBinding index{indices_.get(), 0};
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const SDL_GPUBufferBinding buffers[2] = {{vertices_.get(), 0}, {gpuInstances_.get(), 0}};
        SDL_BindGPUVertexBuffers(pass, 0, buffers, 2);
        SDL_DrawGPUIndexedPrimitivesIndirect(pass, args_.get(),
                                             firstArg * std::uint32_t(sizeof(SDL_GPUIndexedIndirectDrawCommand)), args);
    }
    // The refined cliffs near the eye: their own buffers, a draw each.
    for (const auto& b : batches) {
        if (!b.detail) continue;
        const SDL_GPUBufferBinding index{b.detail->indices.get(), 0};
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const SDL_GPUBufferBinding buffers[2] = {{b.detail->vertices.get(), 0},
                                                 {gpuInstances_.get(), b.first * std::uint32_t(sizeof(Gpu))}};
        SDL_BindGPUVertexBuffers(pass, 0, buffers, 2);
        SDL_DrawGPUIndexedPrimitives(pass, b.detail->indexCount, b.count, 0, 0, 0);
    }
}

void Props::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view,
                 const LightUniforms& light, const PropsLook& look, SDL_GPUTexture* materials,
                 SDL_GPUTexture* normals, SDL_GPUTexture* properties, SDL_GPUTexture* shadowAtlas,
                 SDL_GPUSampler* shadowSampler) {
    if (viewBatches_.empty()) return;
    SDL_BindGPUGraphicsPipeline(pass, draw_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    SDL_PushGPUFragmentUniformData(commands, 1, &look, sizeof(look));
    SDL_PushGPUFragmentUniformData(commands, 2, &cliffBiomes_, sizeof(cliffBiomes_));
    SDL_PushGPUFragmentUniformData(commands, 3, &megalithLook_, sizeof(megalithLook_));
    // (no A-1 in this world: any array stands in for its tiles)
    SDL_GPUTexture* tiles = megalithTiles_ ? megalithTiles_.get() : materials;
    const SDL_GPUTextureSamplerBinding textures[6] = {{materials, linear_.get()},
                                                      {normals, linear_.get()},
                                                      {properties, linear_.get()},
                                                      {shadowAtlas, shadowSampler},
                                                      {tiles, hardPixels_.get()},
                                                      {tiles, linear_.get()}};
    SDL_BindGPUFragmentSamplers(pass, 0, textures, 6);
    drawBatches(pass, viewBatches_, 0, viewDraws_);
}

void Props::drawShadow(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view) {
    if (shadowBatches_.empty()) return;
    SDL_BindGPUGraphicsPipeline(pass, shadow_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    drawBatches(pass, shadowBatches_, viewDraws_, shadowDraws_);
}

} // namespace terrain_core
