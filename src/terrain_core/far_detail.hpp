#pragma once
// The far ground's painted detail (far_detail.hlsli): gullies, strata,
// cavities, mottle, far grass and the forest canopy, past the distance where the
// mesh and the ground's textures run out of detail. Settings in
// content/config/terrain_core/far_detail.json; the canopy follows the tree
// layers of the foliage (the same rules and seeds), so it is made from them.

#include "terrain_core/biomes.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace terrain_core {

struct FoliageLayer;

constexpr std::uint32_t kFarCanopyLayers = 4;

// Must match far_detail.hlsli's cbuffer Far. All nought = the layer off.
struct FarUniforms {
    float range[4]{};          // start m, full m; the meadow's start m, full m
    float strength[4]{};       // erosion, strata, cavity, mottle
    float strength2[4]{};      // meadow, canopy, canopy layer count, -
    float canopyColour[kFarCanopyLayers][4]{};   // rgb, min normal z
    float canopyShape[kFarCanopyLayers][4]{};    // candidate cell (m), crown radius (m), -, -
    float canopyRules[kFarCanopyLayers][kMaxBiomes][4]{};   // density, log2 cluster cell, cover, seed (as uint bits)
};

// Reads the settings and takes the canopy from the foliage's tree layers;
// `seed` is the foliage's (Params::seed). False with `why` if the file is bad.
bool makeFarUniforms(const std::filesystem::path& file, const std::vector<FoliageLayer>& layers, std::uint32_t seed, FarUniforms& out,
                     std::string* why);

} // namespace terrain_core
