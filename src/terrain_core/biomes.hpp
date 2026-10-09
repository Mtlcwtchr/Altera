#pragma once
// What the ground is made of: the biome map over the world and, per biome,
// which materials it lays and where.
//
// The biome map is to the ground what the macro map is to the heights: one
// 8-bit picture of biome ids stretched over the whole world, imported (the
// fallen-gods layout, tools/biome_layout.py) or drawn by a small generator
// from the macro map and a climate noise. The shader reads it at the four
// nearest texels, warped by a noise so that a border is a ragged line and
// not the map's grid, and blends the biomes it finds.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace terrain_core {

constexpr std::uint32_t kMaxBiomes = 32;

// One biome as the fragment shader reads it. Material indices are layers of
// the material array as floats, -1 for "none".
struct BiomeGpu {
    float materials[4];   // base, patch, slope, high
    float patch[4];       // log2(cell in level-0 texels), cover, -, -
    float slope[4];       // normal.z where the slope material starts, where it is full, -, -
    float high[4];        // height (m) where the high material starts, full, -, -
    float tint[4];
};
static_assert(sizeof(BiomeGpu) == 80);

// The fragment stage's second uniform block: must match cbuffer Ground in
// assets/shaders/terrain_core/terrain.hlsl.
struct GroundUniforms {
    BiomeGpu biomes[kMaxBiomes];
    float shoreMaterial, shoreHeight, seaFloorMaterial, tileLog2;   // tile: log2(level-0 texels)
    std::uint32_t biomeSize, pad0, pad1, pad2;
    float worldSize, spacing, pad3, pad4;
    // Per material layer: how much of it is grass (the grass cover grows there).
    float materialGrass[kMaxBiomes];
};

class BiomeTable {
public:
    bool load(const std::filesystem::path& json, const Params& params, std::string* why = nullptr);
    // The pictures of the material array, in layer order.
    const std::vector<std::filesystem::path>& materialFiles() const { return files_; }
    const GroundUniforms& uniforms() const { return uniforms_; }
    // Display colour of each id, for the debug view and previews.
    std::array<std::uint32_t, kMaxBiomes> colours{};

private:
    std::vector<std::filesystem::path> files_;
    GroundUniforms uniforms_{};
};

class BiomeMap {
public:
    bool load(const std::filesystem::path& png, std::string* why = nullptr);
    // Ids from the macro map and a seeded climate: north cold, south warm,
    // high ground mountains, wet lowland marsh, dry warm desert.
    void generate(const MacroMap& macro, const Params& params, std::uint32_t size);
    bool save(const std::filesystem::path& png, std::string* why = nullptr) const;
    std::uint32_t size() const { return size_; }
    const std::vector<std::uint8_t>& ids() const { return ids_; }

private:
    std::vector<std::uint8_t> ids_;
    std::uint32_t size_ = 0;
};

} // namespace terrain_core
