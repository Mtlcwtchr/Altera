#include "terrain_core/lod_select.hpp"

#include <algorithm>
#include <cmath>

namespace terrain_core {
namespace {

std::int64_t floorDiv(double value, double step) { return std::int64_t(std::floor(value / step)); }

struct Box { double x0, y0, z0, x1, y1, z1; };

bool sphereHits(const Box& b, const Vec3d& e, double r) {
    const double dx = std::max({b.x0 - e.x, 0.0, e.x - b.x1});
    const double dy = std::max({b.y0 - e.y, 0.0, e.y - b.y1});
    const double dz = std::max({b.z0 - e.z, 0.0, e.z - b.z1});
    return dx * dx + dy * dy + dz * dz <= r * r;
}

bool inFrustum(const Box& b, const Vec3d& e, const Frustum& f) {
    const double x0 = b.x0 - e.x, x1 = b.x1 - e.x, y0 = b.y0 - e.y, y1 = b.y1 - e.y;
    const double z0 = b.z0 - e.z, z1 = b.z1 - e.z;
    for (const auto& p : f) {
        // The corner furthest along the plane's normal.
        const double px = p[0] >= 0 ? x1 : x0, py = p[1] >= 0 ? y1 : y0, pz = p[2] >= 0 ? z1 : z0;
        if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0) return false;
    }
    return true;
}

struct Walk {
    const Params& params;
    const MacroMap& macro;
    const Clipmap& clipmap;
    const Vec3d& eye;
    const Frustum& frustum;
    Selection& out;

    Box bounds(std::uint32_t level, std::int64_t ix, std::int64_t iy) const {
        const double size = params.nodeSize(level);
        Box b{double(ix) * size, double(iy) * size, 0, double(ix + 1) * size, double(iy + 1) * size, 0};
        const double w = params.worldSize;
        const auto [lo, hi] = macro.range(std::clamp(b.x0 / w, 0.0, 1.0), std::clamp(b.y0 / w, 0.0, 1.0),
                                          std::clamp(b.x1 / w, 0.0, 1.0), std::clamp(b.y1 / w, 0.0, 1.0));
        b.z0 = (double(lo) - params.macroSeaLevel) * params.macroHeightRange - params.detailBound();
        b.z1 = (double(hi) - params.macroSeaLevel) * params.macroHeightRange + params.detailBound();
        return b;
    }

    void emit(std::uint32_t level, std::int64_t ix, std::int64_t iy, std::uint32_t quarter) {
        const std::int64_t g = params.grid, half = g / 2;
        Patch p;
        p.originX = std::int32_t(ix * g);
        p.originY = std::int32_t(iy * g);
        p.level = level;
        const double spacing = params.levelSpacing(level);
        p.relX = float(double(p.originX) * spacing - eye.x);
        p.relY = float(double(p.originY) * spacing - eye.y);
        const std::int64_t qx = quarter & 1, qy = quarter >> 1;
        const std::int64_t x0 = p.originX + qx * half, y0 = p.originY + qy * half;
        // This level's texels, one beyond for the bilinear, and the next
        // level's under the morph.
        bool covered = clipmap.covers(level, x0, y0, x0 + half + 1, y0 + half + 1, 1);
        if (level + 1 < params.levels)
            covered = covered && clipmap.covers(level + 1, x0 / 2, y0 / 2, (x0 + half) / 2 + 2,
                                                (y0 + half) / 2 + 2, 1);
        if (!covered) ++out.coverageMisses;
        out.byQuarter[quarter].push_back(p);
        ++out.patches;
        ++out.perLevel[level];
    }

    // True if the node was dealt with (drawn, or culled as invisible); false
    // if it is out of its level's range and the parent has to draw the area.
    bool visit(std::uint32_t level, std::int64_t ix, std::int64_t iy) {
        ++out.nodesVisited;
        const Box b = bounds(level, ix, iy);
        if (!sphereHits(b, eye, params.range(level))) return false;
        if (!inFrustum(b, eye, frustum)) return true;
        if (level == 0 || !sphereHits(b, eye, params.range(level - 1))) {
            for (std::uint32_t q = 0; q < 4; ++q) emit(level, ix, iy, q);
            return true;
        }
        const std::int64_t nodes = std::int64_t(std::ceil(params.worldSize / params.nodeSize(level - 1)));
        for (std::uint32_t q = 0; q < 4; ++q) {
            const std::int64_t cx = ix * 2 + (q & 1), cy = iy * 2 + (q >> 1);
            if (cx >= nodes || cy >= nodes) continue;
            if (!visit(level - 1, cx, cy)) emit(level, ix, iy, q);
        }
        return true;
    }
};

} // namespace

Frustum frustumFrom(const float m[16]) {
    auto row = [&](int r) { return std::array<float, 4>{m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f{};
    for (int i = 0; i < 4; ++i) {
        f[0][i] = r3[i] + r0[i];
        f[1][i] = r3[i] - r0[i];
        f[2][i] = r3[i] + r1[i];
        f[3][i] = r3[i] - r1[i];
        f[4][i] = r3[i] - r2[i];   // reversed depth: near is z = w
        f[5][i] = r2[i];           // far is z = 0 (at infinity: always passes)
    }
    return f;
}

std::vector<Rect> Clipmap::follow(const Vec3d& eye) {
    std::vector<Rect> rects;
    const std::int64_t n = params_.clipSize, half = n / 2;
    for (std::uint32_t level = 0; level < params_.levels; ++level) {
        Centre& c = centres_[level];
        const double spacing = params_.levelSpacing(level);
        const std::int64_t nx = floorDiv(eye.x, spacing), ny = floorDiv(eye.y, spacing);
        const std::int64_t dx = nx - c.x, dy = ny - c.y;
        auto add = [&](std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1) {
            if (x1 > x0 && y1 > y0)
                rects.push_back({level, std::int32_t(x0), std::int32_t(y0), std::uint32_t(x1 - x0),
                                 std::uint32_t(y1 - y0)});
        };
        if (!c.valid || std::llabs(dx) >= n || std::llabs(dy) >= n) {
            add(nx - half, ny - half, nx + half, ny + half);
        } else if (dx || dy) {
            // The new window minus the old one: a column strip the full new
            // height, then a row strip over the columns the old window had.
            const std::int64_t ox0 = c.x - half, ox1 = c.x + half;
            const std::int64_t nx0 = nx - half, nx1 = nx + half, ny0 = ny - half, ny1 = ny + half;
            if (dx > 0) add(ox1, ny0, nx1, ny1);
            if (dx < 0) add(nx0, ny0, ox0, ny1);
            const std::int64_t kx0 = std::max(nx0, ox0), kx1 = std::min(nx1, ox1);
            if (dy > 0) add(kx0, c.y + half, kx1, ny1);
            if (dy < 0) add(kx0, ny0, kx1, c.y - half);
        }
        c = {nx, ny, true};
    }
    return rects;
}

bool Clipmap::covers(std::uint32_t level, std::int64_t x0, std::int64_t y0, std::int64_t x1,
                     std::int64_t y1, std::int64_t margin) const {
    const Centre& c = centres_[level];
    const std::int64_t half = params_.clipSize / 2;
    return c.valid && x0 - margin >= c.x - half && y0 - margin >= c.y - half &&
           x1 + margin <= c.x + half && y1 + margin <= c.y + half;
}

Selection select(const Params& params, const MacroMap& macro, const Clipmap& clipmap,
                 const Vec3d& eye, const Frustum& frustum) {
    Selection out;
    Walk walk{params, macro, clipmap, eye, frustum, out};
    const std::uint32_t top = params.levels - 1;
    const double size = params.nodeSize(top), reach = params.range(top);
    const std::int64_t nodes = std::int64_t(std::ceil(params.worldSize / size));
    const std::int64_t x0 = std::max<std::int64_t>(0, floorDiv(eye.x - reach, size));
    const std::int64_t x1 = std::min<std::int64_t>(nodes - 1, floorDiv(eye.x + reach, size));
    const std::int64_t y0 = std::max<std::int64_t>(0, floorDiv(eye.y - reach, size));
    const std::int64_t y1 = std::min<std::int64_t>(nodes - 1, floorDiv(eye.y + reach, size));
    for (std::int64_t y = y0; y <= y1; ++y)
        for (std::int64_t x = x0; x <= x1; ++x) walk.visit(top, x, y);
    return out;
}

} // namespace terrain_core
