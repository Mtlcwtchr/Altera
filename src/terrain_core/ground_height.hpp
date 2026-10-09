#pragma once
// The ground's height on the CPU, as the GPU makes it: terrainHeight() in
// assets/shaders/terrain_core/height.hlsli, at level 0 - the macro map
// (bicubic) and the procedural detail on it. The two must be kept in step.
//
// For whatever has to agree with the ground the player sees before the GPU
// has drawn any of it: the river graph (hydrology.cpp) finds the valleys in
// it and lays the water's surface on it.
//
// `minWavelength` leaves out the octaves shorter than that many metres: what
// a coarse question (a grid of a kilometre) cannot hold anyway, and cheaper.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

namespace terrain_core {

class GroundHeight {
public:
    GroundHeight(const MacroMap& macro, const Params& params);
    // Metres over the sea at (x, y) metres.
    float at(double x, double y, double minWavelength = 0.0) const;
    // The procedural detail alone (what rides on the macro map) there.
    float detail(double x, double y, double minWavelength = 0.0) const;

private:
    struct Macro { float value, dx, dy; };   // 0..1, and per macro texel
    Macro macro(double x, double y) const;
    float detailOn(const Macro& m, double x, double y, double minWavelength) const;

    const MacroMap& macro_;
    Params params_;
    double macroTexel_ = 1.0;
    std::uint32_t octaveTop_ = 0;
};

} // namespace terrain_core

