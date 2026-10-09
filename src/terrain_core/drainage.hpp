#pragma once
// Large-scale hydraulic erosion: valleys cut by the water a whole catchment
// sends through them (the stream power law, E = K * A^m * S, solved
// implicitly - Braun & Willett 2013).
//
// Thermal erosion works at the scale of a texel and cannot make a valley ten
// kilometres long; this can. Every cell drains to its lowest neighbour (after
// filling the hollows that have no outlet, Barnes' priority-flood), the area
// draining through each is summed down the network, and each cell is lowered
// towards its receiver the harder the bigger its catchment. Repeated, the
// network cuts branching valleys and leaves ridges - divides - between them.
//
// Applied to a map somebody drew, it must not wipe the drawing out: each
// iteration pulls the ground a little back towards the original (`hold`), so
// the ranges stay where they were drawn and the water carves into them. On a
// work grid of at most `resolution` a side; what it changed is added to the
// full-resolution map, so the map's own fine detail survives.
//
// Rivers cut the floor of a valley; its walls are made by what slides off
// them. So each iteration also lets slopes steeper than `talusDegrees`
// shed into the cell below - without it the result is a map scored with
// one-texel trenches instead of valleys.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

namespace terrain_core {

struct DrainageSettings {
    std::uint32_t resolution = 2048;   // work grid, at most this many texels a side
    std::uint32_t iterations = 100;
    double incision = 0.2;             // K * dt: how hard a catchment cuts
    double channelCells = 64;          // catchment (in cells) where a channel starts; above it slopes only slide
    double hold = 0.03;                // pull back towards the original per iteration
    double talusDegrees = 25.0;        // valley walls settle to this (0: off)
    std::uint32_t floodEvery = 4;      // re-route the water every n iterations
};

struct DrainageTimings {
    double floodMs = 0, areaMs = 0, incisionMs = 0, totalMs = 0;
    std::uint32_t floods = 0;
};

void erodeDrainage(MacroMap& map, const Params& params, const DrainageSettings& settings,
                   DrainageTimings& timings);

} // namespace terrain_core
