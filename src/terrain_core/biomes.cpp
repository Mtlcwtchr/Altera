#include "terrain_core/biomes.hpp"

#include "engine/world_source/png_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace terrain_core {
namespace {

std::uint32_t hash2(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = std::uint32_t(x) * 0x8da6b343u ^ std::uint32_t(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const auto ix = std::int32_t(fx), iy = std::int32_t(fy);
    const float tx = x - fx, ty = y - fy;
    const float ux = tx * tx * (3 - 2 * tx), uy = ty * ty * (3 - 2 * ty);
    auto v = [&](int a, int b) { return float(hash2(ix + a, iy + b, seed) & 0xffffu) / 65535.0f; };
    return (v(0, 0) * (1 - ux) + v(1, 0) * ux) * (1 - uy) + (v(0, 1) * (1 - ux) + v(1, 1) * ux) * uy;
}

float fbm(float x, float y, std::uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < 5; ++o) {
        sum += amp * valueNoise(x, y, seed + std::uint32_t(o) * 977u);
        norm += amp;
        x *= 2.07f; y *= 2.07f; amp *= 0.5f;
    }
    return sum / norm;
}

} // namespace

bool BiomeTable::load(const std::filesystem::path& json, const Params& params, std::string* why) {
    std::ifstream in(json);
    if (!in) { if (why) *why = "cannot read " + json.string(); return false; }
    nlohmann::json config;
    try { config = nlohmann::json::parse(in); } catch (const std::exception& e) { if (why) *why = e.what(); return false; }

    uniforms_ = {};
    // The materials, in the order the config lists them: that order is the array.
    const std::filesystem::path root = json.parent_path().parent_path().parent_path().parent_path();
    std::map<std::string, float> layer;
    files_.clear();
    for (const auto& [name, spec] : config["materials"].items()) {
        // Grass unless said otherwise: every grass_* material, and whatever
        // carries "grass": 0..1.
        if (files_.size() < kMaxMaterials)
            uniforms_.materialGrass[files_.size()] = spec.value("grass", name.rfind("grass", 0) == 0 ? 1.0f : 0.0f);
        layer[name] = float(files_.size());
        // AL-11: a dev stand-in from Skyrim (tools/game_assets/skyrim/world_tex.py, git-ignored) wins when baked.
        const auto dir = root / "assets" / "terrain_core" / "materials";
        const auto standIn = dir / "skyrim" / (name + ".png");
        files_.push_back(std::filesystem::exists(standIn) ? standIn : dir / (name + ".png"));
    }
    auto material = [&](const nlohmann::json& name) -> float {
        if (!name.is_string()) return -1.0f;
        const auto it = layer.find(name.get<std::string>());
        return it == layer.end() ? -1.0f : it->second;
    };
    auto log2Texels = [&](double metres) { return float(std::round(std::log2(std::max(metres / params.spacing, 1.0)))); };

    for (auto& b : uniforms_.biomes) {
        b = {{-1, -1, -1, -1}, {0, 0, 0, 0}, {2, 2, 0, 0}, {1e9f, 1e9f, 0, 0}, {1, 1, 1, 1}};
    }
    colours.fill(0x808080u);
    for (const auto& b : config["biomes"]) {
        const std::uint32_t id = b.value("id", 0u);
        if (id >= kMaxBiomes) continue;
        BiomeGpu& g = uniforms_.biomes[id];
        g.materials[0] = material(b["base"]);
        if (b.contains("patch")) {
            g.materials[1] = material(b["patch"]["material"]);
            g.patch[0] = log2Texels(b["patch"].value("cell_m", 64.0));
            g.patch[1] = b["patch"].value("cover", 0.3f);
        }
        if (b.contains("slope")) {
            g.materials[2] = material(b["slope"]["material"]);
            g.slope[0] = b["slope"].value("from", 0.85f);
            g.slope[1] = b["slope"].value("to", 0.7f);
        }
        if (b.contains("high")) {
            g.materials[3] = material(b["high"]["material"]);
            g.high[0] = b["high"].value("from", 2000.0f);
            g.high[1] = b["high"].value("to", 2500.0f);
        }
        if (b.contains("tint"))
            for (int i = 0; i < 3; ++i) g.tint[i] = b["tint"][i].get<float>();
    }
    // Display colours from the layout's own config, where the ids come from.
    std::ifstream layout(root / "content" / "config" / "biomes" / "fallen_gods.json");
    if (layout) {
        try {
            for (const auto& b : nlohmann::json::parse(layout)["biomes"]) {
                const std::uint32_t id = b.value("id", 0u);
                if (id < kMaxBiomes && b.contains("colour"))
                    colours[id] = b["colour"][0].get<std::uint32_t>() << 16 | b["colour"][1].get<std::uint32_t>() << 8 |
                                  b["colour"][2].get<std::uint32_t>();
            }
        } catch (...) {}
    }
    colours[0] = 0x203860u;

    uniforms_.shoreMaterial = material(config["shore"]["material"]);
    uniforms_.shoreHeight = config["shore"].value("height_m", 4.0f);
    uniforms_.seaFloorMaterial = material(config["sea_floor"]);
    uniforms_.tileLog2 = log2Texels(config.value("tile_m", 4.0));
    uniforms_.worldSize = float(params.worldSize);
    uniforms_.spacing = float(params.spacing);
    for (const auto& f : files_)
        if (!std::filesystem::exists(f)) {
            if (why) *why = "missing " + f.string() + " (run tools/terrain_lab/build_materials.py)";
            return false;
        }
    return true;
}

bool BiomeMap::load(const std::filesystem::path& png, std::string* why) {
    auto image = engine::world_source::readPng(png, why);
    if (!image) return false;
    if (image->width != image->height) { if (why) *why = "biome map must be square"; return false; }
    size_ = image->width;
    ids_.resize(std::size_t(size_) * size_);
    for (std::uint32_t y = 0; y < size_; ++y)
        for (std::uint32_t x = 0; x < size_; ++x) {
            const std::uint32_t v = image->at(x, y, 0);
            ids_[std::size_t(size_ - 1 - y) * size_ + x] = std::uint8_t(image->bits == 16 ? v >> 8 : v);   // north up
        }
    return true;
}

void BiomeMap::generate(const MacroMap& macro, const Params& params, std::uint32_t size) {
    size_ = size;
    ids_.assign(std::size_t(size) * size, 0);
    const std::uint32_t seed = params.seed * 31u + 5u;
    double top = 0;   // the highest land, for "high" relative to this world
    for (std::uint32_t y = 0; y < size; y += 4)
        for (std::uint32_t x = 0; x < size; x += 4)
            top = std::max(top, double(macro.sample((x + 0.5) / size, (y + 0.5) / size)) - params.macroSeaLevel);
    top = std::max(top, 1e-3);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (float(x) + 0.5f) / float(size), v = (float(y) + 0.5f) / float(size);
            const double above = double(macro.sample(u, v)) - params.macroSeaLevel;
            std::uint8_t id = 0;
            if (above > 0) {
                const float elevation = float(above / top);
                // North (+y, the top of the picture) cold, south warm; height cools.
                const float warm = 0.25f + 0.6f * (1.0f - v) + 0.35f * (fbm(u * 4, v * 4, seed) - 0.5f) - 0.5f * elevation;
                const float wet = fbm(u * 6 + 3.1f, v * 6 + 7.7f, seed + 1);
                const float odd = fbm(u * 14, v * 14, seed + 2);
                if (elevation > 0.55f) id = warm < 0.25f ? 13 : 8;                 // northern isles / white mountains
                else if (odd > 0.72f && elevation < 0.3f) id = 7;                  // flower lake valleys
                else if (odd < 0.26f && warm > 0.4f) id = 10;                      // black lands
                else if (warm > 0.62f && wet < 0.42f) id = 4;                      // golden desert
                else if (warm > 0.5f && wet < 0.52f) id = 3;                       // canyon steppe
                else if (wet > 0.62f && elevation < 0.12f) id = 1;                 // great marshes
                else if (wet > 0.56f) id = 2;                                      // dark forest
                else if (wet > 0.47f) id = 9;                                      // river valley
                else if (warm < 0.3f) id = 13;                                     // northern isles
                else if (elevation > 0.35f) id = 5;                                // dragon cliffs
                else id = 6;                                                       // green plateau
            }
            ids_[std::size_t(y) * size + x] = id;
        }
}

namespace {

// Metres from every texel to the nearest texel of `from` (0 on it): an
// 8-neighbour chamfer, two passes.
std::vector<float> distanceTo(const std::vector<std::uint8_t>& from, std::uint32_t n, float cell) {
    const float far = 1e9f, side = cell, diagonal = cell * 1.41421356f;
    std::vector<float> d(from.size());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = from[i] ? 0.0f : far;
    auto at = [&](std::uint32_t x, std::uint32_t y) -> float& { return d[std::size_t(y) * n + x]; };
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            float& v = at(x, y);
            if (x > 0) v = std::min(v, at(x - 1, y) + side);
            if (y > 0) {
                v = std::min(v, at(x, y - 1) + side);
                if (x > 0) v = std::min(v, at(x - 1, y - 1) + diagonal);
                if (x + 1 < n) v = std::min(v, at(x + 1, y - 1) + diagonal);
            }
        }
    for (std::uint32_t y = n; y-- > 0;)
        for (std::uint32_t x = n; x-- > 0;) {
            float& v = at(x, y);
            if (x + 1 < n) v = std::min(v, at(x + 1, y) + side);
            if (y + 1 < n) {
                v = std::min(v, at(x, y + 1) + side);
                if (x + 1 < n) v = std::min(v, at(x + 1, y + 1) + diagonal);
                if (x > 0) v = std::min(v, at(x - 1, y + 1) + diagonal);
            }
        }
    return d;
}

std::vector<std::uint8_t> idsOf(const nlohmann::json& list) {
    std::vector<std::uint8_t> out;
    if (list.is_array())
        for (const auto& v : list) out.push_back(v.get<std::uint8_t>());
    else if (list.is_number())
        out.push_back(list.get<std::uint8_t>());
    return out;
}

} // namespace

void BiomeMap::shape(const std::filesystem::path& table, double worldSize) {
    nlohmann::json config;
    try {
        std::ifstream in(table);
        if (!in) return;
        config = nlohmann::json::parse(in);
    } catch (const std::exception&) {
        return;
    }
    if (size_ == 0) return;
    const float cell = float(worldSize / size_);
    auto mask = [&](const std::vector<std::uint8_t>& want) {
        std::vector<std::uint8_t> m(ids_.size());
        for (std::size_t i = 0; i < ids_.size(); ++i)
            m[i] = std::find(want.begin(), want.end(), ids_[i]) != want.end();
        return m;
    };
    // Bands first: the water's hue counts the band as the biome it was cut from.
    for (const auto& band : config.value("bands", nlohmann::json::array())) {
        const auto in = idsOf(band["in"]), toward = idsOf(band["toward"]);
        const auto id = band.value("id", 0u);
        const float width = band.value("width_m", 2000.0f);
        if (in.empty() || toward.empty() || id >= kMaxBiomes) continue;
        const auto d = distanceTo(mask(toward), size_, cell);
        for (std::size_t i = 0; i < ids_.size(); ++i)
            if (std::find(in.begin(), in.end(), ids_[i]) != in.end() && d[i] <= width) ids_[i] = std::uint8_t(id);
    }
    const auto hue = config.value("water_hue", nlohmann::json::object());
    if (hue.contains("red") && hue.contains("blue")) {
        // Red over the `red` biomes (the marshes), but the ordinary water
        // within `sea_m` of the sea (a delta's mouth); over the `blue` ones
        // (the dark forest) red at the border, purple, blue `blue_m` in.
        const auto red = idsOf(hue["red"]), blue = idsOf(hue["blue"]);
        const float blueM = hue.value("blue_m", 3000.0f), seaM = hue.value("sea_m", 0.0f);
        const auto toRed = distanceTo(mask(red), size_, cell);
        std::vector<float> toSea;
        if (seaM > 0) toSea = distanceTo(mask({0}), size_, cell);
        hue_.assign(ids_.size(), 255);
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            float v = 1.0f;
            if (std::find(red.begin(), red.end(), ids_[i]) != red.end())
                v = seaM > 0 ? std::clamp(1.0f - toSea[i] / seaM, 0.0f, 1.0f) : 0.0f;
            else if (std::find(blue.begin(), blue.end(), ids_[i]) != blue.end())
                v = std::clamp(toRed[i] / blueM, 0.0f, 1.0f);
            hue_[i] = std::uint8_t(std::lround(v * 255.0f));
        }
    }
}

bool BiomeMap::save(const std::filesystem::path& png, std::string* why) const {
    engine::world_source::Image image;
    image.width = image.height = size_;
    image.channels = 1;
    image.bits = 8;
    image.bytes.resize(ids_.size());
    for (std::uint32_t y = 0; y < size_; ++y)   // north (+y) at the top of the picture
        std::copy_n(ids_.begin() + std::ptrdiff_t(std::size_t(size_ - 1 - y) * size_), size_,
                    image.bytes.begin() + std::ptrdiff_t(std::size_t(y) * size_));
    std::error_code ignored;
    std::filesystem::create_directories(png.parent_path(), ignored);
    return engine::world_source::writePng(png, image, why);
}

} // namespace terrain_core
