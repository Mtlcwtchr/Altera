#pragma once
// Rivers and lakes, worked out once from the macro map when a world loads.
//
// The water's own pass:
//   - the sea is the water joined to the open ocean (an inland hollow under
//     sea level is a hollow, not sea);
//   - priority-flood from the sea and the map's edge (Barnes) routes every
//     cell down and fills the hollows; the flow then takes the steepest drop
//     of the filled surface, jittered so rivers meander;
//   - a hollow holds a lake by its water balance: it fills until the lake's
//     area is `humidity` times the area draining into it, or to its brim, and
//     then it spills. A hollow that never spills keeps its water: the rivers
//     below it do not get its catchment (endorheic);
//   - that pass only proposes: the framework - the main lakes and rivers -
//     is painted (`skeleton`) or the biggest of its lakes and the rivers
//     draining most. A second flood, from the sea and the framework, then
//     drains every other hollow into it: off the framework nothing holds
//     water, the brooks and tributaries just run down to it;
//   - rivers are a graph, not cells: a node per cell draining more than
//     `streamArea`, joined downstream, smoothed, meandering where the ground
//     is flat, each with its discharge, width, depth and a surface that only
//     ever falls. The GPU carves the bed and draws the water from the graph's
//     segments at any resolution (rivers.hlsli), so a brook is a brook and a
//     big river is wide, whatever the work grid.
//
// What comes out: the water map (sea and lakes: per work cell the surface's
// height, or none) and the river segments.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace terrain_core {

// The main lakes and rivers, as painted: per pixel 0 nothing, 1 river (a
// line, any thickness), 2 lake. Row 0 is the north edge, like the heightmap.
struct WaterSkeleton {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> cells;
};

// What the ground gives to the rivers, as the world's rules set it: per
// pixel `runoff` (0 none .. 255 the most: desert and steppe 0, mountains
// all of it) and `delta` (255 where a river reaching the sea may split into
// a delta) and `absorb`. Row 0 is the north edge.
struct WaterRules {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> runoff, delta;
    std::vector<std::uint8_t> absorb;   // 255 where the ground swallows water (a desert): a river ends at its edge
};

struct HydrologySettings {
    std::uint32_t resolution = 2048;   // work grid, at most this many texels a side
    double streamArea = 2.0;           // km^2 a cell must drain to be on the river graph
    double humidity = 0.5;             // a lake's area, as a share of the ground that drains into it
    double fillDepth = 30.0;           // a hollow shallower than this (m) or smaller than
    double fillArea = 2.0;             //   this (km^2) drains: no pond, its water runs through
    double lakeDepth = 1.0;            // metres: a shallower lake is left dry
    double minLakeArea = 0.3;          // km^2: a smaller one too
    // River width (m) = widthScale * catchment(km^2)^widthExponent: a brook
    // of 2 km^2 ~1.4 m, a tributary of 200 km^2 ~25 m, a river of 20 000 km^2
    // ~400 m. Steeper than nature's square root, so the sizes read apart.
    double widthScale = 0.9;
    double widthExponent = 0.62;
    double depthScale = 0.35;          // river depth (m) = depthScale * catchment^0.33 + 0.3
    double deltaArea = 75.0;           // km^2: a river this big reaching the sea over low ground splits (a 131 km world)
    double deltaHeight = 150.0;        // metres: ...if the ground where it would split is no higher
    double meander = 1.0;              // multiplies how far big rivers on the flat swing
    // The framework: the main lakes and rivers everything else drains to.
    // Painted (`skeleton`), or picked by the generator: rivers draining more
    // than `mainRiverArea`, the `mainLakes` biggest lakes over `mainLakeArea`.
    // Nothing outside it holds water: every other hollow drains.
    const WaterSkeleton* skeleton = nullptr;
    const WaterRules* rules = nullptr;   // runoff and where deltas are allowed; none: everywhere, all of it
    double mainRiverArea = 1000.0;     // km^2
    double mainLakeArea = 15.0;        // km^2
    std::uint32_t mainLakes = 24;
    double mainMinWidth = 12.0;        // metres: a painted river is at least this wide              // multiplies how far big rivers on the flat swing
};

struct WaterMap {
    std::uint32_t size = 0;
    // Per cell: surface height (m, sea level 0; kNoWater where dry), flow
    // (0 still .. 1), flow direction x, y. Sea and lakes only: rivers are the
    // graph's.
    std::vector<float> texels;
    static constexpr float kNoWater = -30000.0f;
    std::uint32_t lakeCells = 0;
};

// One straight piece of a river, start (0) to end (1) downstream, metres.
// The layout the GPU reads: three float4 (rivers.hlsli).
struct RiverSegment {
    float x0, y0, x1, y1;
    float surface0, surface1, width0, width1;
    float depth0, depth1;
    float flow;    // 0 a brook .. 1 the biggest river
    float bank;    // metres beyond the water the banks are shaped over
};
static_assert(sizeof(RiverSegment) == 48);

struct Hydrology {
    WaterMap water;
    std::vector<RiverSegment> rivers;
    std::uint32_t nodes = 0, sources = 0, mouths = 0, lakes = 0, closedLakes = 0, deltas = 0;
    // The framework as used, on the work grid (row 0 south): 0, 1 river, 2 lake.
    std::uint32_t skeletonSize = 0;
    std::vector<std::uint8_t> skeleton;
    std::uint32_t mainLakeCount = 0, mainRiverCells = 0;
    std::vector<std::array<float, 3>> deltaHeads;   // x, y (m), catchment km^2 of each delta
    std::uint32_t falls = 0;                        // segments dropping steeply (a waterfall's face)
    std::uint32_t drySegments = 0;                  // left out: running along a steep slope
    std::array<float, 3> biggestFall{};             // x, y (m) of its lip, and its drop (m)
    double biggestKm2 = 0;
    double ms = 0;
};

// May cut the beds of painted lakes into `macro`.
Hydrology buildHydrology(MacroMap& macro, const Params& params, const HydrologySettings& settings);

} // namespace terrain_core
