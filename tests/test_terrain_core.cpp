#include "framework.hpp"

#include "terrain_core/lod_select.hpp"
#include "terrain_core/macro_map.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace terrain_core;

namespace {

Params smallParams() {
    Params p;
    p.worldSize = 4096;
    p.spacing = 1;
    p.clipSize = 64;
    p.grid = 8;
    p.levels = 5;
    p.rangeFactor = 2.5;
    return p;
}

} // namespace

// Whatever path the eye takes, after each step every texel of every level's
// window holds exactly that texel - the rects the clipmap asks for are the
// texels that came into view, no fewer.
TEST(terrain_core_clipmap_rects_keep_every_window_current) {
    Params p = smallParams();
    p.clipSize = 16;
    Clipmap clipmap(p);
    const std::int64_t n = p.clipSize;
    std::vector<std::vector<std::int64_t>> slots(p.levels, std::vector<std::int64_t>(n * n, INT64_MIN));
    std::uint32_t rng = 7;
    Vec3d eye{2000, 2000, 10};
    for (int step = 0; step < 400; ++step) {
        rng = rng * 1664525u + 1013904223u;
        const double jump = step % 50 == 49 ? 900.0 : 7.0;   // mostly walks, sometimes leaps
        eye.x += (double(rng >> 8) / double(1u << 24) - 0.5) * jump;
        rng = rng * 1664525u + 1013904223u;
        eye.y += (double(rng >> 8) / double(1u << 24) - 0.5) * jump;
        for (const Rect& r : clipmap.follow(eye))
            for (std::int64_t y = r.y; y < r.y + std::int64_t(r.h); ++y)
                for (std::int64_t x = r.x; x < r.x + std::int64_t(r.w); ++x) {
                    const std::int64_t sx = ((x % n) + n) % n, sy = ((y % n) + n) % n;
                    slots[r.level][sy * n + sx] = (y << 32) ^ x;
                }
        for (std::uint32_t level = 0; level < p.levels; ++level) {
            const std::int64_t cx = clipmap.centreX(level), cy = clipmap.centreY(level);
            for (std::int64_t y = cy - n / 2; y < cy + n / 2; ++y)
                for (std::int64_t x = cx - n / 2; x < cx + n / 2; ++x) {
                    const std::int64_t sx = ((x % n) + n) % n, sy = ((y % n) + n) % n;
                    CHECK(slots[level][sy * n + sx] == ((y << 32) ^ x));
                }
        }
    }
}

// The patches tile the ground: every point near the eye is covered by exactly
// one patch, at whatever level.
TEST(terrain_core_selection_tiles_the_ground_once) {
    const Params p = smallParams();
    MacroMap macro;
    macro.generate(64, 3);
    Clipmap clipmap(p);
    const Vec3d eye{1500.5, 1700.25, 30};
    clipmap.follow(eye);
    const Frustum everything{};   // all-zero planes: everything is inside
    const Selection s = select(p, macro, clipmap, eye, everything);
    CHECK(s.patches > 0);
    CHECK(s.coverageMisses == 0);

    // Count cover on a grid of level-0 quarter cells inside a square the top
    // level's range surely reaches.
    const double cell = p.spacing * p.grid / 2;
    const double half = p.range(p.levels - 1) * 0.6;
    const int cells = int(2 * half / cell);
    const double x0 = std::floor((eye.x - half) / cell) * cell, y0 = std::floor((eye.y - half) / cell) * cell;
    std::vector<int> cover(std::size_t(cells) * cells, 0);
    for (std::uint32_t q = 0; q < 4; ++q)
        for (const Patch& patch : s.byQuarter[q]) {
            const double spacing = p.levelSpacing(patch.level), size = spacing * p.grid / 2;
            const double px = patch.originX * spacing + (q & 1) * size, py = patch.originY * spacing + (q >> 1) * size;
            for (int y = 0; y < cells; ++y)
                for (int x = 0; x < cells; ++x) {
                    const double cx = x0 + (x + 0.5) * cell, cy = y0 + (y + 0.5) * cell;
                    if (cx >= px && cx < px + size && cy >= py && cy < py + size) ++cover[std::size_t(y) * cells + x];
                }
        }
    for (int c : cover) CHECK(c == 1);
}

// The finest level is where the eye is; coarser ones only further out.
TEST(terrain_core_selection_is_finest_at_the_eye) {
    const Params p = smallParams();
    MacroMap macro;
    macro.generate(64, 3);
    Clipmap clipmap(p);
    const Vec3d eye{2048, 2048, 5};
    clipmap.follow(eye);
    const Selection s = select(p, macro, clipmap, eye, Frustum{});
    for (std::uint32_t q = 0; q < 4; ++q)
        for (const Patch& patch : s.byQuarter[q]) {
            const double spacing = p.levelSpacing(patch.level), size = spacing * p.grid / 2;
            const double px = patch.originX * spacing + (q & 1) * size, py = patch.originY * spacing + (q >> 1) * size;
            const bool holdsEye = eye.x >= px && eye.x < px + size && eye.y >= py && eye.y < py + size;
            if (holdsEye) CHECK(patch.level == 0);
        }
}
