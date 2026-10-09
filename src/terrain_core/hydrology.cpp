#include "terrain_core/hydrology.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>

namespace terrain_core {
namespace {

constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, 1, -1, -1};

// Smooth value noise, 0..1, on a lattice of `cell` work cells.
float valueNoise(double x, double y, double cell, std::uint32_t seed) {
    auto hash = [seed](std::int64_t a, std::int64_t b) {
        std::uint32_t v = std::uint32_t(a) * 0x8da6b343u ^ std::uint32_t(b) * 0xd8163841u ^ seed * 0xcb1ab31fu;
        v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
        return float(v & 0xffffff) / float(0xffffff);
    };
    x /= cell; y /= cell;
    const double fx = std::floor(x), fy = std::floor(y);
    const auto ix = std::int64_t(fx), iy = std::int64_t(fy);
    const float tx = float(x - fx), ty = float(y - fy);
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}

} // namespace

WaterMap buildHydrology(MacroMap& macro, const Params& params, const HydrologySettings& s) {
    const auto start = std::chrono::steady_clock::now();
    const double range = params.macroHeightRange, sea = params.macroSeaLevel * range;
    const std::uint32_t n = macro.size();
    std::uint32_t side = n;
    while (side > s.resolution && side % 2 == 0) side /= 2;
    const std::uint32_t f = n / side;
    const std::size_t cells = std::size_t(side) * side;
    const double dx = params.worldSize / side;

    // Heights in metres over the sea, box-filtered to the work grid.
    std::vector<float> h(cells);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            double sum = 0;
            for (std::uint32_t j = 0; j < f; ++j)
                for (std::uint32_t i = 0; i < f; ++i) sum += macro.texels()[std::size_t(y * f + j) * n + x * f + i];
            h[std::size_t(y) * side + x] = float(sum / (f * f) / 65535.0 * range - sea);
        }

    // Priority-flood: from the sea and the map's edge, lowest first. Each cell
    // drains to the one that reached it; a hollow fills to its spill point.
    // The routing sees a few metres of smooth noise on top: on a flat plain
    // the flood would otherwise run rivers along ruler lines.
    std::vector<float> route(h);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const double km = dx * 1e-3;
            const float nz = valueNoise(x, y, 6.0 / km, 11) * 4.0f + valueNoise(x, y, 1.5 / km, 23) * 1.5f +
                             valueNoise(x, y, 0.5 / km, 37) * 0.5f;
            if (h[std::size_t(y) * side + x] > 0) route[std::size_t(y) * side + x] += nz;
        }
    std::vector<float> filled = route;
    std::vector<std::uint32_t> receiver(cells), order;
    order.reserve(cells);
    {
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        std::vector<std::uint8_t> done(cells, 0);
        for (std::uint32_t y = 0; y < side; ++y)
            for (std::uint32_t x = 0; x < side; ++x) {
                const std::uint32_t i = y * side + x;
                if (x == 0 || y == 0 || x == side - 1 || y == side - 1 || h[i] <= 0) {
                    done[i] = 1;
                    receiver[i] = i;
                    open.push({filled[i], i});
                }
            }
        while (!open.empty()) {
            const auto [level, i] = open.top();
            open.pop();
            order.push_back(i);
            const std::uint32_t x = i % side, y = i / side;
            for (int k = 0; k < 8; ++k) {
                const int nx = int(x) + kDx[k], ny = int(y) + kDy[k];
                if (nx < 0 || ny < 0 || nx >= int(side) || ny >= int(side)) continue;
                const std::uint32_t c = std::uint32_t(ny) * side + std::uint32_t(nx);
                if (done[c]) continue;
                done[c] = 1;
                receiver[c] = i;
                filled[c] = std::max(filled[c], level + 1e-3f);
                open.push({filled[c], c});
            }
        }
    }
    // Receivers again, now down the filled surface: the steepest drop, jittered
    // so a river on an even slope meanders instead of keeping to the line the
    // flood front happened to take. The filled surface always has a lower
    // neighbour (the flood's), and the flood order stays a valid order.
    for (std::uint32_t i = 0; i < cells; ++i) {
        if (receiver[i] == i) continue;
        const std::uint32_t x = i % side, y = i / side;
        float best = 0;
        for (int k = 0; k < 8; ++k) {
            const int nx = int(x) + kDx[k], ny = int(y) + kDy[k];
            if (nx < 0 || ny < 0 || nx >= int(side) || ny >= int(side)) continue;
            const std::uint32_t c = std::uint32_t(ny) * side + std::uint32_t(nx);
            const float drop = filled[i] - filled[c];
            if (drop <= 0) continue;
            std::uint32_t v = i * 0x9e3779b1u ^ std::uint32_t(k) * 0x85ebca6bu;
            v ^= v >> 15; v *= 0x2c1b3c6du; v ^= v >> 12;
            const float jitter = 0.5f + float(v & 0xffff) / 65535.0f;
            const float score = drop / (k < 4 ? 1.0f : 1.4142f) * jitter;
            if (score > best) { best = score; receiver[i] = c; }
        }
    }
    // Catchment, km^2: every cell's own area passed down to its receiver.
    std::vector<float> area(cells, float(dx * dx * 1e-6));
    for (std::size_t k = order.size(); k-- > 0;) {
        const std::uint32_t i = order[k];
        if (receiver[i] != i) area[receiver[i]] += area[i];
    }

    // Which filled hollows are lakes: each connected hollow as a whole, by its
    // depth and size. The rest are dry basins - endorheic, their rivers end
    // on the floor.
    std::vector<std::uint8_t> lake(cells, 0);
    {
        std::vector<std::uint32_t> stack, members;
        std::vector<std::uint8_t> seen(cells, 0);
        const double cellKm2 = dx * dx * 1e-6;
        for (std::uint32_t start = 0; start < cells; ++start) {
            if (seen[start] || filled[start] - route[start] <= s.lakeDepth || h[start] <= 0) continue;
            members.clear();
            stack.assign(1, start);
            seen[start] = 1;
            float deepest = 0;
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                members.push_back(i);
                deepest = std::max(deepest, filled[i] - route[i]);
                const std::uint32_t x = i % side, y = i / side;
                for (int k = 0; k < 8; ++k) {
                    const int nx = int(x) + kDx[k], ny = int(y) + kDy[k];
                    if (nx < 0 || ny < 0 || nx >= int(side) || ny >= int(side)) continue;
                    const std::uint32_t c = std::uint32_t(ny) * side + std::uint32_t(nx);
                    if (seen[c] || filled[c] - route[c] <= s.lakeDepth || h[c] <= 0) continue;
                    seen[c] = 1;
                    stack.push_back(c);
                }
            }
            const double km2 = double(members.size()) * cellKm2;
            const bool keep = deepest <= s.maxLakeDepth && km2 <= s.maxLakeArea && km2 >= s.minLakeArea;
            for (const std::uint32_t i : members) lake[i] = keep ? 1 : 2;   // 2: a dry basin or a puddle
        }
    }

    WaterMap water;
    water.size = side;
    water.texels.assign(cells * 4, 0.0f);
    std::vector<float> carve(cells, 0.0f);
    const double biggest = std::log10(std::max<double>(*std::max_element(area.begin(), area.end()) / s.riverArea, 10.0));
    for (std::uint32_t i = 0; i < cells; ++i) {
        float* w = &water.texels[std::size_t(i) * 4];
        w[0] = WaterMap::kNoWater;
        if (h[i] <= 0) { w[0] = 0.0f; continue; }   // the sea: level 0, still
        const std::uint32_t r = receiver[i];
        const float rx = float(int(r % side) - int(i % side)), ry = float(int(r / side) - int(i / side));
        const float rl = std::max(std::hypot(rx, ry), 1e-3f);
        if (lake[i] == 1) {
            w[0] = filled[i];   // a lake: flat at its spill level
            w[1] = 0.0f;
            ++water.lakeCells;
        } else if (area[i] > s.riverArea) {
            // A river: a bed cut deeper the bigger it is, the surface a little
            // under the bank.
            const double size = std::log10(area[i] / s.riverArea + 1.0);
            const float depth = float((6.0 + 10.0 * size) * s.carve);
            carve[i] = depth;
            // The surface follows the ground (not the routed, noisy or filled
            // level): through a dry hollow the river keeps to the floor.
            w[0] = h[i] - depth * 0.15f;
            w[1] = float(std::clamp(size / biggest, 0.05, 1.0));
            w[2] = rx / rl;
            w[3] = ry / rl;
            ++water.riverCells;
        }
    }
    // A river never runs uphill: upstream first, each cell's level caps the
    // next one's (the ground under a river is noisy, its surface is not;
    // the bed is cut to match below).
    for (std::size_t k = order.size(); k-- > 0;) {
        const std::uint32_t i = order[k], r = receiver[i];
        if (carve[i] > 0 && carve[r] > 0)
            water.texels[std::size_t(r) * 4] = std::min(water.texels[std::size_t(r) * 4], water.texels[std::size_t(i) * 4]);
    }
    // Where a river steps diagonally its two cells touch only at a corner, and
    // the bilinear surface sinks into the bank between them: a side cell joins
    // it, so the channel stays a channel.
    for (std::uint32_t i = 0; i < cells; ++i) {
        const std::uint32_t r = receiver[i];
        if (carve[i] <= 0 || r == i || r % side == i % side || r / side == i / side) continue;
        const std::uint32_t c = (i / side) * side + r % side;
        float* w = &water.texels[std::size_t(c) * 4];
        if (w[0] != WaterMap::kNoWater) continue;
        const float* from = &water.texels[std::size_t(i) * 4];
        w[0] = std::min(from[0], water.texels[std::size_t(r) * 4]);
        w[1] = from[1];
        w[2] = from[2];
        w[3] = from[3];
        carve[c] = carve[i];
        ++water.riverCells;
    }
    // The bed cut to the surface, not the surface laid on the ground: under
    // the falling surface the water keeps its depth, and where the ground
    // rises along the way (a bump, a sill) the river cuts through it.
    for (std::uint32_t i = 0; i < cells; ++i)
        if (carve[i] > 0) carve[i] = std::max(0.0f, h[i] - (water.texels[std::size_t(i) * 4] - carve[i] * 0.85f));
    // The beds into the macro map: the carve spread a little (banks, not a
    // trench), up to full resolution, subtracted.
    std::vector<float> soft(cells, 0.0f);
    for (std::uint32_t y = 1; y + 1 < side; ++y)
        for (std::uint32_t x = 1; x + 1 < side; ++x) {
            const std::uint32_t i = y * side + x;
            float m = carve[i];
            for (int k = 0; k < 8; ++k)
                m = std::max(m, carve[std::uint32_t(int(y) + kDy[k]) * side + std::uint32_t(int(x) + kDx[k])] * (k < 4 ? 0.45f : 0.3f));
            soft[i] = m;
        }
    // Each wet cell's level spread one cell onto its dry neighbours, but kept
    // just under that neighbour's (carved) ground: the bilinear surface then
    // slopes from the water into the bank, and the shore is drawn where it
    // meets the ground - a smooth line, not the cells' staircase.
    {
        std::vector<float> spread(water.texels);
        for (std::uint32_t y = 1; y + 1 < side; ++y)
            for (std::uint32_t x = 1; x + 1 < side; ++x) {
                const std::uint32_t i = y * side + x;
                if (water.texels[std::size_t(i) * 4] != WaterMap::kNoWater) continue;
                float level = WaterMap::kNoWater;
                std::uint32_t from = i;
                for (int k = 0; k < 8; ++k) {
                    const std::uint32_t c = std::uint32_t(int(y) + kDy[k]) * side + std::uint32_t(int(x) + kDx[k]);
                    if (water.texels[std::size_t(c) * 4] > level) { level = water.texels[std::size_t(c) * 4]; from = c; }
                }
                if (from == i) continue;
                for (int j = 0; j < 4; ++j) spread[std::size_t(i) * 4 + j] = water.texels[std::size_t(from) * 4 + j];
                spread[std::size_t(i) * 4] = std::min(level, h[i] - soft[i] - 0.5f);
            }
        water.texels.swap(spread);
    }
    if (water.riverCells) {
        std::vector<std::uint16_t> out(macro.texels());
        for (std::uint32_t y = 0; y < n; ++y) {
            const double gy = std::clamp((y + 0.5) / f - 0.5, 0.0, double(side - 1));
            const std::uint32_t y0 = std::uint32_t(gy), y1 = std::min(y0 + 1, side - 1);
            const double fy = gy - y0;
            for (std::uint32_t x = 0; x < n; ++x) {
                const double gx = std::clamp((x + 0.5) / f - 0.5, 0.0, double(side - 1));
                const std::uint32_t x0 = std::uint32_t(gx), x1 = std::min(x0 + 1, side - 1);
                const double fx = gx - x0;
                auto at = [&](std::uint32_t a, std::uint32_t b) { return double(soft[std::size_t(b) * side + a]); };
                const double d = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                if (d <= 0) continue;
                const std::size_t i = std::size_t(y) * n + x;
                out[i] = std::uint16_t(std::clamp(double(out[i]) - d / range * 65535.0, 0.0, 65535.0));
            }
        }
        macro.assign(n, std::move(out));
    }
    water.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return water;
}

} // namespace terrain_core
