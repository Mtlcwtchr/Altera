#include "terrain_core/drainage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <vector>

namespace terrain_core {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, 1, -1, -1};

// Box-filtered down to `side` (a divisor of the map's size).
std::vector<float> downsample(const MacroMap& map, std::uint32_t side, double range) {
    const std::uint32_t n = map.size(), f = n / side;
    std::vector<float> out(std::size_t(side) * side);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            double sum = 0;
            for (std::uint32_t j = 0; j < f; ++j)
                for (std::uint32_t i = 0; i < f; ++i) sum += map.texels()[std::size_t(y * f + j) * n + x * f + i];
            out[std::size_t(y) * side + x] = float(sum / (f * f) / 65535.0 * range);
        }
    return out;
}

} // namespace

void erodeDrainage(MacroMap& map, const Params& params, const DrainageSettings& s, DrainageTimings& t) {
    const auto total = Clock::now();
    const double range = params.macroHeightRange;
    const double seaMetres = params.macroSeaLevel * range;   // heights here are 0..range, sea at this
    std::uint32_t side = map.size();
    while (side > s.resolution && side % 2 == 0) side /= 2;
    const std::size_t cells = std::size_t(side) * side;
    const double dx = params.worldSize / side;

    const std::vector<float> original = downsample(map, side, range);
    std::vector<float> h = original;
    std::vector<std::uint32_t> receiver(cells), order;
    std::vector<float> distance(cells, float(dx)), area(cells), slide(cells);
    order.reserve(cells);

    // Routing: priority-flood from the sea and the edge of the map. A cell is
    // reached from the lowest cell already reached next to it, and drains to
    // it; a hollow with no outlet is filled up to its spill point (flat, a
    // little rising, so water still finds its way across). `order` is the
    // order cells were reached: every cell after its receiver.
    auto flood = [&] {
        const auto start = Clock::now();
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        std::vector<std::uint8_t> done(cells, 0);
        order.clear();
        for (std::uint32_t y = 0; y < side; ++y)
            for (std::uint32_t x = 0; x < side; ++x) {
                const std::uint32_t i = y * side + x;
                const bool edge = x == 0 || y == 0 || x == side - 1 || y == side - 1;
                if (edge || h[i] <= seaMetres) {
                    done[i] = 1;
                    receiver[i] = i;
                    open.push({h[i], i});
                }
            }
        // A hollow is filled not quite flat: a little rise and a little
        // noise per cell, or the water crosses every filled flat in a dead
        // straight line along one of the eight directions.
        // Millimetres: the rise only orders the cells of a flat, and it adds
        // up along it - at a decimetre a cell, a plain 2000 cells across came
        // out raised by hundreds of metres.
        const float rise = 1e-3f;
        auto jitter = [&](std::uint32_t n) {
            std::uint32_t v = n * 0x9e3779b9u + 0x85ebca6bu;
            v ^= v >> 15; v *= 0x2c1b3c6du; v ^= v >> 12;
            return float(v & 0xffffu) / 65535.0f * rise;
        };
        while (!open.empty()) {
            const auto [height, i] = open.top();
            open.pop();
            order.push_back(i);
            const std::uint32_t x = i % side, y = i / side;
            for (int k = 0; k < 8; ++k) {
                const int nx = int(x) + kDx[k], ny = int(y) + kDy[k];
                if (nx < 0 || ny < 0 || nx >= int(side) || ny >= int(side)) continue;
                const std::uint32_t n = std::uint32_t(ny) * side + std::uint32_t(nx);
                if (done[n]) continue;
                done[n] = 1;
                receiver[n] = i;
                distance[n] = float(k < 4 ? dx : dx * 1.41421356);
                if (h[n] <= height) h[n] = height + rise + jitter(n);   // fill the hollow
                open.push({h[n], n});
            }
        }
        // Re-point each cell at one of its lower neighbours, by steepness
        // times a random factor. The flood's own choice - whichever neighbour
        // happened to reach it first - and plain steepest descent both send
        // water in dead straight lines along the grid's eight directions on
        // any smooth slope. A lower neighbour was always reached earlier, so
        // `order` stays upstream-after-downstream.
        const std::uint32_t salt = t.floods * 0x9e3779b9u;
        for (std::uint32_t y = 1; y + 1 < side; ++y)
            for (std::uint32_t x = 1; x + 1 < side; ++x) {
                const std::uint32_t i = y * side + x;
                if (receiver[i] == i) continue;
                float best = 0;
                for (int k = 0; k < 8; ++k) {
                    const std::uint32_t n = std::uint32_t(int(y) + kDy[k]) * side + std::uint32_t(int(x) + kDx[k]);
                    if (h[n] >= h[i]) continue;
                    const float d = float(k < 4 ? dx : dx * 1.41421356);
                    std::uint32_t v = (i * 8u + std::uint32_t(k)) * 0x85ebca6bu ^ salt;
                    v ^= v >> 13; v *= 0xc2b2ae35u; v ^= v >> 16;
                    const float score = (h[i] - h[n]) / d * (0.5f + float(v & 0xffffu) / 65535.0f);
                    if (score > best) { best = score; receiver[i] = n; distance[i] = d; }
                }
            }
        t.floodMs += msSince(start);
        ++t.floods;
    };

    for (std::uint32_t it = 0; it < s.iterations; ++it) {
        if (it % std::max(1u, s.floodEvery) == 0) flood();

        // Catchment: every cell's own area, passed down to its receiver,
        // upstream cells first.
        auto start = Clock::now();
        std::fill(area.begin(), area.end(), float(dx * dx));
        for (std::size_t k = order.size(); k-- > 0;) {
            const std::uint32_t i = order[k];
            if (receiver[i] != i) area[receiver[i]] += area[i];
        }
        t.areaMs += msSince(start);

        const float channelArea = float(s.channelCells * dx * dx);
        // Incision, implicit: downstream first, each cell solved against its
        // receiver's new height - stable however big the step.
        start = Clock::now();
        for (const std::uint32_t i : order) {
            const std::uint32_t r = receiver[i];
            // Only channels cut: a cell draining less than `channelCells`
            // is hillside, shaped by what slides (below), not by a river.
            if (r == i || h[i] <= seaMetres || area[i] < channelArea) continue;
            const double f = s.incision * std::sqrt(double(area[i])) / distance[i];   // m = 0.5
            const double next = (h[i] + f * h[r]) / (1.0 + f);
            h[i] = float(std::max(next, double(h[r])));
        }
        // Valley walls: slopes over the angle of repose shed a quarter of
        // their excess to the lowest neighbour. All transfers are worked out
        // first and applied after (Jacobi): done in place, in raster order,
        // each cell saw its upper-left neighbours already moved, and the whole
        // map came out hatched at 45 degrees.
        if (s.talusDegrees > 0) {
            const float talus = float(std::tan(s.talusDegrees * 3.14159265358979 / 180.0) * dx);
            std::fill(slide.begin(), slide.end(), 0.0f);
            for (std::uint32_t y = 1; y + 1 < side; ++y)
                for (std::uint32_t x = 1; x + 1 < side; ++x) {
                    const std::uint32_t i = y * side + x;
                    if (h[i] <= seaMetres) continue;
                    std::uint32_t low = i;
                    float drop = 0;
                    for (int k = 0; k < 8; ++k) {
                        const std::uint32_t n = std::uint32_t(int(y) + kDy[k]) * side + std::uint32_t(int(x) + kDx[k]);
                        const float d = h[i] - h[n] - talus * (k < 4 ? 1.0f : 1.41421356f);
                        if (d > drop) { drop = d; low = n; }
                    }
                    // Nothing slides into the sea: piled there it stands up as a
                    // new rim of land along every coast.
                    if (low != i && h[low] > seaMetres) { slide[i] -= drop * 0.25f; slide[low] += drop * 0.25f; }
                }
            for (std::size_t i = 0; i < cells; ++i) h[i] += slide[i];
        }
        // Hold the drawing: back towards the original, a little.
        for (std::size_t i = 0; i < cells; ++i)
            if (original[i] > seaMetres) h[i] += float(s.hold * (original[i] - h[i]));
        t.incisionMs += msSince(start);
    }

    // What the water changed, bilinear up to the map's resolution and added.
    const std::uint32_t n = map.size(), f = n / side;
    std::vector<std::uint16_t> out(map.texels());
    for (std::uint32_t y = 0; y < n; ++y) {
        const double gy = std::clamp((y + 0.5) / f - 0.5, 0.0, double(side - 1));
        const std::uint32_t y0 = std::uint32_t(gy), y1 = std::min(y0 + 1, side - 1);
        const double fy = gy - y0;
        for (std::uint32_t x = 0; x < n; ++x) {
            const double gx = std::clamp((x + 0.5) / f - 0.5, 0.0, double(side - 1));
            const std::uint32_t x0 = std::uint32_t(gx), x1 = std::min(x0 + 1, side - 1);
            const double fx = gx - x0;
            auto delta = [&](std::uint32_t a, std::uint32_t b) {
                const std::size_t i = std::size_t(b) * side + a;
                return double(h[i]) - double(original[i]);
            };
            const double d = (delta(x0, y0) * (1 - fx) + delta(x1, y0) * fx) * (1 - fy) +
                             (delta(x0, y1) * (1 - fx) + delta(x1, y1) * fx) * fy;
            const std::size_t i = std::size_t(y) * n + x;
            out[i] = std::uint16_t(std::clamp(double(out[i]) + d / range * 65535.0, 0.0, 65535.0));
        }
    }
    map.assign(n, std::move(out));
    t.totalMs = msSince(total);
}

} // namespace terrain_core
