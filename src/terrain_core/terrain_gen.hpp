#pragma once
// The terrain steps of Campfire's terrain_gen (CampfireLite core/src/landscape/
// terrain_gen.cpp, CAM-124): the ones behind its Sculpt brush "Auto", which
// made the mountains people liked, and the geology under its map generator.
// Pure functions over a regular grid of heights in metres, deterministic in
// their seed, no GPU.
//
//  - geology: tectonic uplift against the stream power law, solved implicitly
//    over a depression-free drainage graph (Braun & Willett 2013, Cordonnier et
//    al. 2016; Barnes' priority-flood for the hollows). Ranges come out as
//    ridges between branching valleys instead of noise.
//  - water: hydraulic erosion by droplets (Beyer 2015): gullies down the
//    slopes, fans of sediment at their feet.
//  - thermal: talus sliding (Musgrave et al. 1989): scree under the crests.
//  - noise, smooth: the brush's finishing touches.
//
// The Auto brush ran water + thermal + noise on the ground under it; the map
// generator ran geology first. world_gen.cpp runs all of them.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace terrain_core::gen {

struct Grid {
    int w = 0, h = 0;
    float spacing = 1;            // metres between samples
    std::vector<float> heights;   // metres, row j = 0 first
    static Grid make(int w, int h, float spacing, float height = 0);
    std::size_t index(int i, int j) const { return std::size_t(j) * std::size_t(w) + std::size_t(i); }
    float& at(int i, int j) { return heights[index(i, j)]; }
    float at(int i, int j) const { return heights[index(i, j)]; }
    // Bilinear at grid coordinates (clamped).
    float sample(double i, double j) const;
    // The same ground on another grid, bilinear.
    Grid resampled(int w2, int h2) const;
};

// Progress 0..1 of a long step; may be empty.
using Progress = std::function<void(float)>;

struct GeologySettings {
    int iterations = 40;
    float upliftRate = 5e-4f;     // metres a year at uplift 1
    float erodibility = 5.6e-6f;
    float areaExponent = 0.5f;
    float timeStep = 2.5e5f;      // years
    float maxHeight = 0;          // > 0: the relief above the sea scaled to reach it
    float diffusion = 0;          // 0..0.25: hillslope creep a step (altera addition; Campfire: 0)
};
// `uplift` per cell (0..1, empty: 1 everywhere); cells at or below `seaLevel`
// and the border are base level, where the water leaves.
void geology(Grid& g, const std::vector<float>& uplift, const GeologySettings& s, float seaLevel, const Progress& p = {});

struct WaterSettings {
    float drops = 1;              // droplets per grid cell
    float inertia = 0.05f, capacity = 4, deposition = 0.3f, erosion = 0.3f, evaporation = 0.01f, gravity = 4,
          minSlope = 0.01f;
    int maxSteps = 64;
    int radius = 2;               // cells a droplet erodes around itself
};
// `mask` per cell (0..1, empty: 1): how much of the change is kept there.
void water(Grid& g, const WaterSettings& s, std::uint32_t seed, const std::vector<float>& mask, const Progress& p = {});

// Slopes steeper than `talusDegrees` shed into the cell below, `iterations` times.
void thermal(Grid& g, float talusDegrees, int iterations);

// fBm of `amplitude` metres at `scale` features a metre (octaves finer than
// four cells are left out); `ridged` sharpens crests, `warp` metres bends it.
void noise(Grid& g, float amplitude, float scale, int octaves, bool ridged, float warp, std::uint32_t seed);

void smooth(Grid& g, int passes);

// Raises every hollow with no way out to its spill height (+ eps a cell).
void fillDepressions(std::vector<float>& h, int w, int ht, float eps);

// Catchment of every cell in cells (D8 over the filled surface); `seaLevel`
// cells drain nowhere.
std::vector<float> catchment(const Grid& g, float seaLevel);

// The value noise and fBm the steps use, for the generator's own fields.
float valueNoise(double x, double z, std::uint32_t seed);              // 0..1
float fbm(double x, double z, int octaves, bool ridged, std::uint32_t seed);   // -1..1 (ridged 0..1)

} // namespace terrain_core::gen
