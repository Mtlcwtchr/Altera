#pragma once
// Which pieces of ground are drawn at which level, and which texels of the
// clipmap have to be (re)generated for that - both on the CPU, both cheap.
//
// The selection is CDLOD (Strugar 2009): a quadtree over the whole world, each
// node drawn with one shared G x G grid, each LOD out to a range that doubles
// per level, vertices morphing into the parent's grid before the range ends so
// that no seam and no pop is ever visible. A node is never drawn whole when
// only part of it needs the finer level: its quarters are drawn separately, so
// what goes to the GPU is a list of quarter-nodes ("patches"), bucketed by
// quarter so each bucket is one instanced draw of one index range.
//
// Positions are double here and integer texel indices on the GPU: on a world
// two thousand kilometres across a float position is a quarter of a metre out.

#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace terrain_core {

struct Vec3d { double x = 0, y = 0, z = 0; };

// One quarter of a node, as the vertex shader receives it (instance rate).
struct Patch {
    std::int32_t originX = 0, originY = 0;   // node origin, in its level's texels
    std::uint32_t level = 0;
    std::uint32_t pad = 0;
    float relX = 0, relY = 0;                // node origin minus eye, metres
    float pad2[2] = {};
};
static_assert(sizeof(Patch) == 32);

// Planes as (a, b, c, d) with a*x + b*y + c*z + d >= 0 inside, in eye-relative
// metres.
using Frustum = std::array<std::array<float, 4>, 6>;
Frustum frustumFrom(const float viewProjection[16]);   // row-major, clip = M * p

struct Selection {
    std::array<std::vector<Patch>, 4> byQuarter;
    std::uint32_t nodesVisited = 0;
    std::uint32_t patches = 0;
    // Patches whose texels (or the next level's, for morphing) fall outside
    // the clipmap - would read stale heights. Must be zero for a valid set of
    // Params; the benchmark reports it rather than trusting it.
    std::uint32_t coverageMisses = 0;
    std::array<std::uint32_t, 32> perLevel{};
};

// A rectangle of texels at one level, in absolute texel indices of that level.
struct Rect {
    std::uint32_t level = 0;
    std::int32_t x = 0, y = 0;
    std::uint32_t w = 0, h = 0;
};

class Clipmap {
public:
    explicit Clipmap(const Params& params) : params_(params) { centres_.resize(params.levels); }
    // Moves every level's window to the eye and returns the texels that came
    // into a window and so need generating. Everything, the first time or
    // after a jump further than a window.
    std::vector<Rect> follow(const Vec3d& eye);
    void invalidate() { for (auto& c : centres_) c.valid = false; }
    // Whether [x0, x1) x [y0, y1) at `level` lies inside the window with
    // `margin` texels to spare.
    bool covers(std::uint32_t level, std::int64_t x0, std::int64_t y0, std::int64_t x1,
                std::int64_t y1, std::int64_t margin) const;
    std::int64_t centreX(std::uint32_t level) const { return centres_[level].x; }
    std::int64_t centreY(std::uint32_t level) const { return centres_[level].y; }

private:
    struct Centre { std::int64_t x = 0, y = 0; bool valid = false; };
    const Params& params_;
    std::vector<Centre> centres_;
};

Selection select(const Params& params, const MacroMap& macro, const Clipmap& clipmap,
                 const Vec3d& eye, const Frustum& frustum);

} // namespace terrain_core
