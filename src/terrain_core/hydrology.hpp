#pragma once
// Rivers and lakes, worked out once from the macro map when a world loads.
//
// The water's own pass: priority-flood from the sea (Barnes) routes every
// cell to the sea and fills the hollows that have no outlet; a hollow filled
// deeper than `lakeDepth` is a lake, its surface the level it spills at; a cell
// draining more than `riverArea` km^2 is a river, its surface the routed
// (never rising) height a little under the bank. River beds are cut into the
// macro map - deeper the bigger the river - so the water lies in a channel
// (the drainage erosion's job, done only where the rivers are).
//
// What comes out is the water map: per work cell the water's surface height
// (or none), how big a flow it is, and which way it runs - what
// assets/shaders/terrain_core/water.hlsl draws.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <cstdint>
#include <vector>

namespace terrain_core {

struct HydrologySettings {
    std::uint32_t resolution = 2048;   // work grid, at most this many texels a side
    double lakeDepth = 1.5;            // metres a hollow must be filled to count as a lake
    double riverArea = 250.0;          // km^2 a cell must drain to carry a river
    // A hollow is a lake only if it is no deeper and no larger than these:
    // a mountain-ringed plateau is a dry basin, not a sea filled to its rim.
    double maxLakeDepth = 80.0;        // metres
    double maxLakeArea = 1300.0;       // km^2
    double minLakeArea = 3.0;          // km^2: smaller hollows are puddles, left dry
    double carve = 1.0;                // multiplies how deep river beds are cut
};

struct WaterMap {
    std::uint32_t size = 0;
    // Per cell: surface height (m, sea level 0; kNoWater where dry), flow
    // (0 still .. 1 the biggest river), flow direction x, y.
    std::vector<float> texels;
    static constexpr float kNoWater = -30000.0f;
    std::uint32_t lakeCells = 0, riverCells = 0;
    double ms = 0;
};

// Carves the river beds into `macro` and returns the water map.
WaterMap buildHydrology(MacroMap& macro, const Params& params, const HydrologySettings& settings);

} // namespace terrain_core
