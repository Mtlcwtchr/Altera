#pragma once
// Clearings: discs where plants do not grow (the game's village, its yards
// and works). The game sets them; the foliage and grass spawn passes read
// them as one more cbuffer (assets/shaders/terrain_core/clearings.hlsli).
// Plain data, one set for the process: the world view has one world.
#include <cstdint>

namespace terrain_core {

struct ClearingUniforms {
    float info[4] = {0, 0, 0, 0};   // count, -, -, -
    float discs[32][4] = {};        // world x, y (m), inner radius (nothing grows), outer (back to full)
};
static_assert(sizeof(ClearingUniforms) == 528);

inline ClearingUniforms& clearings() {
    static ClearingUniforms c;
    return c;
}

} // namespace terrain_core
