#include "terrain_core/hydrology.hpp"
#include "terrain_core/ground_height.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <thread>

namespace terrain_core {
namespace {

constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, 1, -1, -1};
constexpr std::uint32_t kNone = 0xffffffffu;

// f(i) for i in [0, count), over the machine's cores. f must only write what
// belongs to its own i.
template <class F>
void parallelFor(std::size_t count, F&& f) {
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    if (threads == 1 || count < 2048) {
        for (std::size_t i = 0; i < count; ++i) f(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (;;) {
                const std::size_t begin = next.fetch_add(512);
                if (begin >= count) break;
                const std::size_t end = std::min(count, begin + 512);
                for (std::size_t i = begin; i < end; ++i) f(i);
            }
        });
    for (std::thread& t : pool) t.join();
}

std::uint32_t mix(std::uint32_t v) {
    v ^= v >> 15; v *= 0x2c1b3c6du; v ^= v >> 12; v *= 0x297a2d39u; v ^= v >> 15;
    return v;
}

// Smooth value noise, 0..1, on a lattice of `cell` units.
float valueNoise(double x, double y, double cell, std::uint32_t seed) {
    auto hash = [seed](std::int64_t a, std::int64_t b) {
        const std::uint32_t v = mix(std::uint32_t(a) * 0x8da6b343u ^ std::uint32_t(b) * 0xd8163841u ^ seed * 0xcb1ab31fu);
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

struct Node {
    double x = 0, y = 0;
    std::uint32_t cell = 0;
    std::uint32_t down = kNone;     // the next node downstream
    std::uint32_t upMain = kNone;   // the biggest of the ones flowing in
    float area = 0;                 // km^2
    float surface = 0, width = 0, depth = 0;
    bool terminal = false;          // where a river meets the sea or a lake
};

} // namespace

Hydrology buildHydrology(const MacroMap& macro, const Params& params, const HydrologySettings& s) {
    const auto start = std::chrono::steady_clock::now();
    const double range = params.macroHeightRange, sea = params.macroSeaLevel * range;
    const std::uint32_t n = macro.size();
    std::uint32_t side = n;
    while (side > s.resolution && side % 2 == 0) side /= 2;
    const std::uint32_t f = n / side;
    const std::size_t cells = std::size_t(side) * side;
    const double dx = params.worldSize / side, cellKm2 = dx * dx * 1e-6;
    auto neighbour = [side](std::uint32_t i, int k) -> std::uint32_t {
        const int x = int(i % side) + kDx[k], y = int(i / side) + kDy[k];
        if (x < 0 || y < 0 || x >= int(side) || y >= int(side)) return kNone;
        return std::uint32_t(y) * side + std::uint32_t(x);
    };

    // Heights in metres over the sea: the macro map box-filtered to the work
    // grid, and the procedural detail the GPU puts on it as far as the grid
    // can hold it (wavelengths of two cells and up). Without the detail the
    // rivers would follow the macro map's own valleys, which the drawn ground
    // does not have: tens of metres of ridge and hollow lie across them.
    const GroundHeight ground(macro, params);
    std::vector<float> h(cells);
    parallelFor(side, [&](std::size_t row) {
        const auto y = std::uint32_t(row);
        for (std::uint32_t x = 0; x < side; ++x) {
            double sum = 0;
            for (std::uint32_t j = 0; j < f; ++j)
                for (std::uint32_t i = 0; i < f; ++i) sum += macro.texels()[std::size_t(y * f + j) * n + x * f + i];
            h[std::size_t(y) * side + x] = float(sum / (f * f) / 65535.0 * range - sea) +
                                           ground.detail((x + 0.5) * dx, (y + 0.5) * dx, 2.0 * dx);
        }
    });

    // The sea: what under sea level joins the map's edge. A hollow inland
    // that dips under sea level is a hollow (a caldera, a rift).
    std::vector<std::uint8_t> ocean(cells, 0);
    {
        std::vector<std::uint32_t> stack;
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint32_t x = i % side, y = i / side;
            if ((x == 0 || y == 0 || x == side - 1 || y == side - 1) && h[i] <= 0) { ocean[i] = 1; stack.push_back(i); }
        }
        while (!stack.empty()) {
            const std::uint32_t i = stack.back();
            stack.pop_back();
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c != kNone && !ocean[c] && h[c] <= 0) { ocean[c] = 1; stack.push_back(c); }
            }
        }
    }

    // The routing sees a few metres of smooth noise on top: on a flat plain
    // the flood would otherwise run rivers along ruler lines.
    std::vector<float> route(h);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const double km = dx * 1e-3;
            const float nz = valueNoise(x, y, 6.0 / km, 11) * 4.0f + valueNoise(x, y, 1.5 / km, 23) * 1.5f +
                             valueNoise(x, y, 0.5 / km, 37) * 0.5f;
            const std::size_t i = std::size_t(y) * side + x;
            if (!ocean[i]) route[i] += nz;
        }

    // Priority-flood: from the sea and the map's edge, lowest first. Each cell
    // drains to the one that reached it; a hollow fills to its spill point.
    std::vector<float> filled = route;
    std::vector<std::uint32_t> receiver(cells), order;
    order.reserve(cells);
    {
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        std::vector<std::uint8_t> done(cells, 0);
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint32_t x = i % side, y = i / side;
            if (x == 0 || y == 0 || x == side - 1 || y == side - 1 || ocean[i]) {
                done[i] = 1;
                receiver[i] = i;
                open.push({filled[i], i});
            }
        }
        while (!open.empty()) {
            const auto [level, i] = open.top();
            open.pop();
            order.push_back(i);
            for (int k = 0; k < 8; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c == kNone || done[c]) continue;
                done[c] = 1;
                receiver[c] = i;
                filled[c] = std::max(filled[c], level + 1e-3f);
                open.push({filled[c], c});
            }
        }
    }
    // Receivers again, down the filled surface: the steepest drop, jittered
    // so a river on an even slope meanders instead of keeping to the line the
    // flood front happened to take.
    auto steepest = [&](std::uint32_t i, const std::vector<float>& surface, std::uint32_t fallback) {
        float best = 0;
        std::uint32_t to = fallback;
        for (int k = 0; k < 8; ++k) {
            const std::uint32_t c = neighbour(i, k);
            if (c == kNone) continue;
            const float drop = surface[i] - surface[c];
            if (drop <= 0) continue;
            const float jitter = 0.5f + float(mix(i * 0x9e3779b1u ^ std::uint32_t(k) * 0x85ebca6bu) & 0xffff) / 65535.0f;
            const float score = drop / (k < 4 ? 1.0f : 1.4142f) * jitter;
            if (score > best) { best = score; to = c; }
        }
        return to;
    };
    for (std::uint32_t i = 0; i < cells; ++i)
        if (receiver[i] != i) receiver[i] = steepest(i, filled, receiver[i]);

    // The hollows themselves, from the true ground: the routing's noise
    // makes a pit of every bump, and the pits of a plain would join into one
    // vast "hollow". A second flood, no noise, no slope on the flats.
    std::vector<float> brim(h);
    {
        using Item = std::pair<float, std::uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        std::vector<std::uint8_t> done(cells, 0);
        for (std::uint32_t i = 0; i < cells; ++i) {
            const std::uint32_t x = i % side, y = i / side;
            if (x == 0 || y == 0 || x == side - 1 || y == side - 1 || ocean[i]) {
                done[i] = 1;
                open.push({brim[i], i});
            }
        }
        while (!open.empty()) {
            const auto [level, i] = open.top();
            open.pop();
            for (int k = 0; k < 8; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c == kNone || done[c]) continue;
                done[c] = 1;
                brim[c] = std::max(brim[c], level);
                open.push({brim[c], c});
            }
        }
    }
    auto hollow = [&](std::uint32_t i) { return !ocean[i] && brim[i] - h[i] > 0.3f; };

    // Catchment as the flood routes it, km^2: what each hollow is fed.
    std::vector<float> area(cells, float(cellKm2));
    for (std::size_t k = order.size(); k-- > 0;) {
        const std::uint32_t i = order[k];
        if (receiver[i] != i) area[receiver[i]] += area[i];
    }

    // The hollows and their lakes, by water balance.
    Hydrology out;
    std::vector<std::uint8_t> lake(cells, 0);
    std::vector<float> lakeLevel(cells, WaterMap::kNoWater);
    {
        std::vector<std::uint8_t> seen(cells, 0);
        std::vector<std::uint32_t> stack, members, extent, grow;
        for (std::uint32_t first = 0; first < cells; ++first) {
            if (seen[first] || !hollow(first)) continue;
            members.clear();
            stack.assign(1, first);
            seen[first] = 1;
            float fed = 0, spill = 0;
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                members.push_back(i);
                fed = std::max(fed, area[i]);
                spill = std::max(spill, brim[i]);
                for (int k = 0; k < 8; ++k) {
                    const std::uint32_t c = neighbour(i, k);
                    if (c == kNone || seen[c] || !hollow(c)) continue;
                    seen[c] = 1;
                    stack.push_back(c);
                }
            }
            std::sort(members.begin(), members.end(), [&](std::uint32_t a, std::uint32_t b) { return h[a] < h[b]; });
            // Fill from the bottom until the lake is as big as its water allows.
            const double wanted = s.humidity * fed;
            float level = spill;
            bool brim = true;
            // A small or shallow hollow fills and spills whatever the climate:
            // a dent in the map, not a basin with a water balance of its own.
            const bool basin = spill - h[members[0]] >= s.closedDepth && double(members.size()) * cellKm2 >= s.closedArea;
            for (std::size_t k = 0; basin && k + 1 < members.size(); ++k)
                if (double(k + 1) * cellKm2 >= wanted) {
                    level = h[members[k + 1]];
                    brim = level >= spill;
                    break;
                }
            // The lake: what lies under that level, joined to the lowest point.
            extent.assign(1, members[0]);
            grow.assign(1, members[0]);
            lake[members[0]] = 2;   // 2: being grown
            while (!grow.empty()) {
                const std::uint32_t i = grow.back();
                grow.pop_back();
                for (int k = 0; k < 8; ++k) {
                    const std::uint32_t c = neighbour(i, k);
                    if (c == kNone || lake[c] || !hollow(c) || h[c] >= level) continue;
                    lake[c] = 2;
                    extent.push_back(c);
                    grow.push_back(c);
                }
            }
            const bool keep = level - h[members[0]] >= s.lakeDepth && double(extent.size()) * cellKm2 >= s.minLakeArea;
            for (const std::uint32_t i : extent) {
                lake[i] = keep ? 1 : 0;
                if (keep) lakeLevel[i] = level;
            }
            if (keep) {
                ++out.lakes;
                out.water.lakeCells += std::uint32_t(extent.size());
            }
            if (!brim) {
                // Closed: its water stays. Down the true ground to the lake
                // (or the floor's own pits), and no further.
                ++out.closedLakes;
                for (const std::uint32_t i : members)
                    receiver[i] = lake[i] ? i : steepest(i, route, i);
            }
        }
    }

    // Catchment again, now that closed hollows keep theirs: in dependency
    // order (Kahn), which is also upstream-before-downstream for the graph.
    std::fill(area.begin(), area.end(), float(cellKm2));
    order.clear();
    {
        std::vector<std::uint8_t> pending(cells, 0);
        for (std::uint32_t i = 0; i < cells; ++i)
            if (receiver[i] != i) ++pending[receiver[i]];
        for (std::uint32_t i = 0; i < cells; ++i)
            if (!pending[i]) order.push_back(i);
        for (std::size_t k = 0; k < order.size(); ++k) {
            const std::uint32_t i = order[k], r = receiver[i];
            if (r == i) continue;
            area[r] += area[i];
            if (--pending[r] == 0) order.push_back(r);
        }
    }

    // The water map: the sea and the lakes. Each wet cell's level spread one
    // cell onto its dry neighbours, kept just under their ground: the
    // bilinear surface then slopes into the bank and the shore is drawn where
    // it meets the ground, not along the cells' staircase.
    WaterMap& water = out.water;
    water.size = side;
    water.texels.assign(cells * 4, 0.0f);
    for (std::uint32_t i = 0; i < cells; ++i)
        water.texels[std::size_t(i) * 4] = ocean[i] ? 0.0f : lake[i] ? lakeLevel[i] : WaterMap::kNoWater;
    {
        std::vector<float> spread(water.texels);
        for (std::uint32_t i = 0; i < cells; ++i) {
            if (water.texels[std::size_t(i) * 4] != WaterMap::kNoWater) continue;
            float level = WaterMap::kNoWater;
            for (int k = 0; k < 8; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c != kNone) level = std::max(level, water.texels[std::size_t(c) * 4]);
            }
            if (level != WaterMap::kNoWater) spread[std::size_t(i) * 4] = std::min(level, h[i] - 0.5f);
        }
        water.texels.swap(spread);
    }

    // The river graph: a node per cell draining enough, in upstream-first
    // order, and one more where each river meets the sea or a lake.
    std::vector<Node> nodes;
    std::vector<std::uint32_t> nodeOf(cells, kNone);
    for (const std::uint32_t i : order)
        if (!ocean[i] && !lake[i] && area[i] >= s.streamArea) {
            nodeOf[i] = std::uint32_t(nodes.size());
            Node node;
            node.cell = i;
            node.area = area[i];
            nodes.push_back(node);
        }
    const std::size_t inland = nodes.size();
    for (std::size_t k = 0; k < inland; ++k) {
        const std::uint32_t r = receiver[nodes[k].cell];
        if (r == nodes[k].cell) continue;   // ends on a dry floor
        if (nodeOf[r] == kNone && (ocean[r] || lake[r])) {
            nodeOf[r] = std::uint32_t(nodes.size());
            Node mouth;
            mouth.cell = r;
            mouth.terminal = true;
            mouth.surface = ocean[r] ? 0.0f : lakeLevel[r];
            nodes.push_back(mouth);
        }
        nodes[k].down = nodeOf[r];
    }
    out.nodes = std::uint32_t(nodes.size());
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        Node& node = nodes[k];
        node.x = (node.cell % side + 0.5) * dx;
        node.y = (node.cell / side + 0.5) * dx;
        if (node.down == kNone) continue;
        Node& d = nodes[node.down];
        if (d.upMain == kNone || nodes[d.upMain].area < node.area) d.upMain = std::uint32_t(k);
        if (d.terminal) d.area = std::max(d.area, node.area);
    }
    double biggest = s.streamArea;
    for (Node& node : nodes) {
        biggest = std::max(biggest, double(node.area));
        node.width = float(s.widthScale * std::sqrt(node.area));
        node.depth = float(s.depthScale * std::pow(node.area, 0.3) + 0.3);
        if (node.upMain == kNone && !node.terminal) ++out.sources;
        if (node.down == kNone) ++out.mouths;
    }
    out.biggestKm2 = biggest;

    // Smoothed along each river's main line: the grid's 45-degree steps
    // become curves. Sources and mouths stay put.
    auto inner = [&](const Node& node) { return node.down != kNone && node.upMain != kNone && !node.terminal; };
    auto smooth = [&] {
        std::vector<double> px(nodes.size()), py(nodes.size());
        for (std::size_t k = 0; k < nodes.size(); ++k) {
            const Node& node = nodes[k];
            px[k] = node.x;
            py[k] = node.y;
            if (!inner(node)) continue;
            const Node &u = nodes[node.upMain], &d = nodes[node.down];
            px[k] = node.x * 0.5 + (u.x + d.x) * 0.25;
            py[k] = node.y * 0.5 + (u.y + d.y) * 0.25;
        }
        for (std::size_t k = 0; k < nodes.size(); ++k) { nodes[k].x = px[k]; nodes[k].y = py[k]; }
    };
    // Into the valley the drawn ground has there: across the river's line,
    // within `reach`, to the lowest ground (a little in favour of staying).
    // The grid only knows its cells; the valley floor between them, and the
    // detail shorter than a cell, it cannot see - a river laid straight from
    // cell to cell runs up the side of its own valley and over spurs.
    auto snap = [&](double reach, double minWavelength) {
        std::vector<double> px(nodes.size()), py(nodes.size());
        parallelFor(nodes.size(), [&](std::size_t k) {
            const Node& node = nodes[k];
            px[k] = node.x;
            py[k] = node.y;
            if (!inner(node)) return;
            const Node &u = nodes[node.upMain], &d = nodes[node.down];
            const double tx = d.x - u.x, ty = d.y - u.y, len = std::hypot(tx, ty);
            if (len < 1e-3) return;
            const double nx = -ty / len, ny = tx / len;
            constexpr int kSteps = 6;
            double best = std::numeric_limits<double>::max(), offset = 0;
            for (int j = -kSteps; j <= kSteps; ++j) {
                const double o = reach * j / kSteps;
                const double g = ground.at(node.x + nx * o, node.y + ny * o, minWavelength) + std::abs(o) * 0.01;
                if (g < best) { best = g; offset = o; }
            }
            px[k] = node.x + nx * offset;
            py[k] = node.y + ny * offset;
        });
        for (std::size_t k = 0; k < nodes.size(); ++k) { nodes[k].x = px[k]; nodes[k].y = py[k]; }
    };
    for (int pass = 0; pass < 4; ++pass) smooth();
    snap(0.45 * dx, 0.25 * dx);
    smooth();
    snap(0.15 * dx, 0.1 * dx);
    // Meanders: where the ground is flat a river swings from side to side,
    // the wider the river the longer the bends. Phase by the distance to the
    // mouth, so the swing runs continuously along the river.
    {
        std::vector<double> toMouth(nodes.size(), 0.0);
        for (std::size_t k = nodes.size(); k-- > 0;) {
            const Node& node = nodes[k];
            if (node.down != kNone)
                toMouth[k] = toMouth[node.down] + std::hypot(nodes[node.down].x - node.x, nodes[node.down].y - node.y);
        }
        std::vector<double> ox(nodes.size(), 0.0), oy(nodes.size(), 0.0);
        parallelFor(nodes.size(), [&](std::size_t k) {
            const Node& node = nodes[k];
            if (!inner(node)) return;
            const Node &u = nodes[node.upMain], &d = nodes[node.down];
            const double tx = d.x - u.x, ty = d.y - u.y, len = std::max(std::hypot(tx, ty), 1.0);
            const double slope = std::max(0.0, double(h[u.cell] - h[d.cell])) / len;
            const double flat = 1.0 - std::clamp(slope / 0.008, 0.0, 1.0);
            const double wavelength = std::clamp(14.0 * node.width, 5.0 * dx, 20.0 * dx);
            const double swing = std::min(0.4 * dx, 3.0 * node.width) * flat * s.meander;
            if (swing <= 0) return;
            const double phase = toMouth[k] / wavelength * 6.2831853 + 2.0 * valueNoise(node.x, node.y, 8.0 * dx, 71);
            double mx = -ty / len * swing * std::sin(phase), my = tx / len * swing * std::sin(phase);
            // A bend swings across the flat, never up out of it: where the
            // ground beside the river rises, the bend is held back.
            const double here = ground.at(node.x, node.y, 0.1 * dx);
            for (int tries = 0; tries < 4; ++tries, mx *= 0.5, my *= 0.5)
                if (ground.at(node.x + mx, node.y + my, 0.1 * dx) <= here + 0.5) {
                    ox[k] = mx;
                    oy[k] = my;
                    break;
                }
        });
        for (std::size_t k = 0; k < nodes.size(); ++k) { nodes[k].x += ox[k]; nodes[k].y += oy[k]; }
    }

    // Each edge a Catmull-Rom curve through its neighbours along the main
    // line, cut into a few straight pieces.
    struct Edge { std::size_t first = 0; int pieces = 0; };
    std::vector<Edge> edges(nodes.size());
    std::vector<double> pointX, pointY;
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const Node& a = nodes[k];
        if (a.down == kNone || a.terminal) continue;
        const Node& b = nodes[a.down];
        const Node& before = a.upMain != kNone ? nodes[a.upMain] : a;
        const Node& after = b.down != kNone ? nodes[b.down] : b;
        Edge& e = edges[k];
        e.first = pointX.size();
        e.pieces = a.area < 50 ? 2 : 4;
        for (int j = 0; j <= e.pieces; ++j) {
            const double t = double(j) / e.pieces, t2 = t * t, t3 = t2 * t;
            const double c0 = -0.5 * t3 + t2 - 0.5 * t, c1 = 1.5 * t3 - 2.5 * t2 + 1, c2 = -1.5 * t3 + 2 * t2 + 0.5 * t,
                         c3 = 0.5 * t3 - 0.5 * t2;
            pointX.push_back(before.x * c0 + a.x * c1 + b.x * c2 + after.x * c3);
            pointY.push_back(before.y * c0 + a.y * c1 + b.y * c2 + after.y * c3);
        }
    }
    auto widthAt = [&](std::size_t k, double t) {
        const Node& a = nodes[k];
        const Node& b = nodes[a.down];
        const bool main = b.upMain == k && !b.terminal;   // a tributary keeps its own size to the junction
        return a.width + ((main ? b.width : a.width) - a.width) * float(t);
    };
    auto depthAt = [&](std::size_t k, double t) {
        const Node& a = nodes[k];
        const Node& b = nodes[a.down];
        const bool main = b.upMain == k && !b.terminal;
        return a.depth + ((main ? b.depth : a.depth) - a.depth) * float(t);
    };

    // The drawn ground along each piece: the lowest of it across the water
    // (the middle and both edges, every few tens of metres) - what the water
    // must stay under, or it stands on a bank of its own - and the highest
    // along the middle, how deep the river has to cut.
    constexpr double kAlong = 60.0;        // metres between samples along a piece
    constexpr double kFinest = 8.0;        // detail shorter than this does not matter to a river
    std::vector<float> lowest(pointX.size(), 0.0f), highest(pointX.size(), 0.0f);
    parallelFor(nodes.size(), [&](std::size_t k) {
        const Edge& e = edges[k];
        for (int j = 0; j < e.pieces; ++j) {
            const std::size_t p = e.first + std::size_t(j);
            const double x0 = pointX[p], y0 = pointY[p], x1 = pointX[p + 1], y1 = pointY[p + 1];
            const double len = std::hypot(x1 - x0, y1 - y0);
            const double nx = len > 1e-3 ? -(y1 - y0) / len : 0.0, ny = len > 1e-3 ? (x1 - x0) / len : 0.0;
            const double side = widthAt(k, (j + 0.5) / e.pieces) * 0.5 + 1.0;
            const int steps = std::max(2, int(std::ceil(len / kAlong)));
            float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
            for (int i = 0; i <= steps; ++i) {
                const double t = double(i) / steps, cx = x0 + (x1 - x0) * t, cy = y0 + (y1 - y0) * t;
                const float middle = ground.at(cx, cy, kFinest);
                high = std::max(high, middle);
                low = std::min({low, middle, ground.at(cx + nx * side, cy + ny * side, kFinest),
                                ground.at(cx - nx * side, cy - ny * side, kFinest)});
            }
            lowest[p] = low;
            highest[p] = high;
        }
    });

    // The surface at every point: under the ground of the pieces either side
    // of it, and never rising downstream - through a junction too. Upstream
    // first (the nodes are in that order), so everything flowing into a node
    // is known before its own river goes on.
    std::vector<float> surface(pointX.size(), 0.0f);
    std::vector<float> into(nodes.size(), std::numeric_limits<float>::max());
    for (std::size_t k = 0; k < nodes.size(); ++k)
        if (nodes[k].terminal) into[k] = nodes[k].surface;
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const Edge& e = edges[k];
        if (!e.pieces) continue;
        const Node& b = nodes[nodes[k].down];
        auto under = [&](int j) {
            if (j < 0 || j >= e.pieces) return std::numeric_limits<float>::max();
            return lowest[e.first + std::size_t(j)] - (0.15f * depthAt(k, (j + 0.5) / e.pieces) + 0.3f);
        };
        float level = into[k];
        for (int j = 0; j <= e.pieces; ++j) {
            level = std::min({level, under(j - 1), under(j)});
            // Into the sea or a lake: no lower than its water.
            surface[e.first + std::size_t(j)] = b.terminal ? std::max(level, b.surface) : level;
        }
        if (!b.terminal) into[nodes[k].down] = std::min(into[nodes[k].down], level);
    }

    // Segments. A tributary ends at the level the river it joins has there.
    // The banks reach as far as the cut is deep, three to one: a river that
    // has to cut through a rise makes a valley through it, not a slot.
    const double logBiggest = std::log(std::max(biggest / s.streamArea, 2.0));
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const Edge& e = edges[k];
        if (!e.pieces) continue;
        const Node& a = nodes[k];
        const Node& b = nodes[a.down];
        if (!b.terminal) surface[e.first + std::size_t(e.pieces)] = into[a.down];
        const float flow = float(std::clamp(std::log(double(a.area) / s.streamArea) / logBiggest, 0.0, 1.0));
        for (int j = 0; j < e.pieces; ++j) {
            const std::size_t p = e.first + std::size_t(j);
            const double t0 = double(j) / e.pieces, t1 = double(j + 1) / e.pieces;
            RiverSegment seg{};
            seg.x0 = float(pointX[p]); seg.y0 = float(pointY[p]);
            seg.x1 = float(pointX[p + 1]); seg.y1 = float(pointY[p + 1]);
            seg.surface0 = surface[p];
            seg.surface1 = surface[p + 1];
            seg.width0 = widthAt(k, t0);
            seg.width1 = widthAt(k, t1);
            seg.depth0 = depthAt(k, t0);
            seg.depth1 = depthAt(k, t1);
            seg.flow = flow;
            const float cut = std::max(0.0f, highest[p] - std::min(seg.surface0, seg.surface1));
            const float bank = std::max(6.0f, std::max(seg.width0, seg.width1)) + 4.0f * std::max(seg.depth0, seg.depth1);
            seg.bank = std::max(bank, std::min(3.0f * cut, 400.0f));
            out.rivers.push_back(seg);
        }
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return out;
}

} // namespace terrain_core
