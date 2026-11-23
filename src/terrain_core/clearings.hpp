#pragma once
// Clearings: discs where plants do not grow (the game's village, its yards
// and works). The game sets them; the foliage and grass spawn passes read
// them as one more cbuffer (assets/shaders/terrain_core/clearings.hlsli).
// Plain data, one set for the process: the world view has one world.
#include <cstdint>
#include <vector>

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

// AL-78: the forest the game holds. Inside `rect` (world metres x0, y0, x1,
// y1) the foliage layers marked "held" (foliage.json) are not scattered by the
// GPU: the trees there are exactly `trees` - the game's own (every one a tree
// the people can fell), drawn with the same models, wind and shadows. The
// game builds them with Foliage::heldTrees (the GPU's scattering, on the CPU)
// and bumps `version` when the list changes (a tree felled).
struct HeldTree {
    float x = 0, y = 0, z = 0, yaw = 0;   // world metres, radians
    std::uint32_t model = 0;              // the foliage's model index
    float scale = 1, range = 0;           // range: the layer's draw distance
    std::uint32_t layer = 0;
};
class Foliage;
struct ForestHold {
    bool active = false;
    double rect[4] = {0, 0, 0, 0};
    std::vector<HeldTree> trees;
    std::uint64_t version = 0;
    Foliage* foliage = nullptr;           // set by Foliage::open: who can build the list
};
inline ForestHold& forestHold() {
    static ForestHold h;
    return h;
}

} // namespace terrain_core
