#pragma once
// The few numbers the whole terrain core is built from.
//
// Everything else - how far the terrain is drawn, how much memory the clipmap
// takes, how many vertices a frame has - follows from these, and the lab's
// benchmark exists to choose them. Kept as plain data so a sweep can write a
// table of them and the report can print one back.

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace terrain_core {

// Defaults are the base preset chosen by the step-1 benchmark
// (doc/reports/terrain_core/step1/README.md): ~41 km view, 0.3-0.45 ms a frame.
struct Params {
    // The world is [0, worldSize] metres square. Up to 2 000 km.
    double worldSize = 2'000'000.0;
    // Metres between samples at the finest level (level 0).
    double spacing = 0.5;
    // Clipmap texels per side, per level. A power of two.
    std::uint32_t clipSize = 256;
    // Quads per side of one CDLOD node's grid. Even, a power of two.
    std::uint32_t grid = 32;
    // Number of LOD levels = number of clipmap levels.
    std::uint32_t levels = 11;
    // LOD l is drawn out to rangeFactor * nodeSize(l) from the eye.
    double rangeFactor = 2.5;
    // Where in that range the vertices start morphing to the next level.
    double morphStart = 0.8;

    // Heights.
    std::uint32_t seed = 11;
    double macroHeightRange = 3000.0;   // metres spanned by the 16-bit macro map
    double macroSeaLevel = 0.0;         // macro value (0..1) that is height 0
    double detailAmplitude = 1.0;       // scales the procedural detail
    // Metres one macro-map texel covers; set once the map is loaded. The
    // detail starts where the map stops: at two of its texels.
    double macroTexel = 0.0;

    double levelSpacing(std::uint32_t level) const { return spacing * double(1u << level); }
    double nodeSize(std::uint32_t level) const { return levelSpacing(level) * grid; }
    double range(std::uint32_t level) const { return rangeFactor * nodeSize(level); }
    double viewDistance() const { return range(levels - 1); }
    // Bytes of the clipmap: RGBA32F (height, d/dx, d/dy, spare) per texel.
    std::uint64_t clipmapBytes() const { return std::uint64_t(clipSize) * clipSize * levels * 16; }
    // Upper bound of the procedural detail, for culling bounds. Must agree with
    // terrainHeight() in assets/shaders/terrain_core/height.hlsli (top octave ~4 km).
    double detailBound() const { return 0.13 * topWavelength() * detailAmplitude + 10.0; }
    // The longest detail octave: two macro texels, 64 m .. 4 km.
    double topWavelength() const {
        const double wanted = macroTexel > 0 ? 2.0 * macroTexel : 4096.0;
        return std::exp2(std::round(std::log2(std::clamp(wanted, 64.0, 4096.0))));
    }
    // ... as a power of two of level-0 texels.
    std::uint32_t octaveTop() const { return std::uint32_t(std::lround(std::log2(topWavelength() / spacing))); }
};

} // namespace terrain_core
