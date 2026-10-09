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
    double deltaArea = 300.0;          // km^2: a river this big reaching the sea over low ground splits
    double deltaHeight = 150.0;        // metres: ...if the ground where it would split is no higher
    double meander = 1.0;              // multiplies how far big rivers on the flat swing
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
    std::vector<std::array<float, 3>> deltaHeads;   // x, y (m), catchment km^2 of each delta
    double biggestKm2 = 0;
    double ms = 0;
};

Hydrology buildHydrology(const MacroMap& macro, const Params& params, const HydrologySettings& settings);

} // namespace terrain_core
