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

    // The lakes: fill, spill, merge over the hollows' hierarchy (after
    // Barnes, Callaghan & Wickert, "Computing water flow through complex
    // landscapes", 2020).
    //
    // Swept by height, the ground under each level is a set of separate
    // hollows; where two meet is a saddle, where one meets the sea or the
    // map's edge its outlet. That is a tree: a caldera is a hollow of its own
    // inside the basin round it, joined to it only at its rim. Each hollow is
    // fed by the ground that drains into it (steepest descent), and holds a
    // lake as big as `humidity` times that: if the water would reach the
    // saddle, the hollow is full and the rest runs over into its neighbour;
    // two full neighbours become one lake. So a caldera with its own walls
    // for a catchment gets its own lake, at its own level - not the level of
    // the plateau round it.
    Hydrology out;
    std::vector<float> area(cells, float(cellKm2));
    std::vector<std::uint8_t> lake(cells, 0);
    std::vector<float> lakeLevel(cells, WaterMap::kNoWater);
    std::vector<std::uint8_t> closed(cells, 0);   // 1: dry, in a hollow that keeps its water; 2: its lake
    {
        struct Hollow {
            float z = std::numeric_limits<float>::max();   // the level it joins its parent at
            std::uint32_t parent = kNone;                   // kNone: never joined; 0: the sea
            std::uint32_t cellsBelow = 0;                   // its cells when it joins: the lake's area at z
            double water = 0;                               // km^2 of lake its own catchment keeps
            std::vector<std::uint32_t> children, own;       // own: cells added to it directly, by height
        };
        std::vector<Hollow> hollows(1);                     // 0: the sea and the map's edge
        std::vector<std::uint32_t> byHeight(cells);
        for (std::uint32_t i = 0; i < cells; ++i) byHeight[i] = i;
        std::sort(byHeight.begin(), byHeight.end(), [&](std::uint32_t a, std::uint32_t b) {
            return h[a] < h[b] || (h[a] == h[b] && a < b);
        });
        std::vector<std::uint32_t> set(cells, kNone), setSize(cells, 0), hollowOf(cells, 0), owner(cells, 0);
        auto find = [&](std::uint32_t i) {
            while (set[i] != i) { set[i] = set[set[i]]; i = set[i]; }
            return i;
        };
        auto join = [&](std::uint32_t a, std::uint32_t b) {   // b's set into a's; a stays the root
            set[b] = a;
            setSize[a] += setSize[b];
        };
        for (const std::uint32_t i : byHeight) {
            const std::uint32_t x = i % side, y = i / side;
            const bool outlet = ocean[i] || x == 0 || y == 0 || x == side - 1 || y == side - 1;
            std::uint32_t roots[8];
            int count = 0;
            for (int k = 0; k < 8; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c == kNone || set[c] == kNone) continue;
                const std::uint32_t r = find(c);
                if (std::find(roots, roots + count, r) == roots + count) roots[count++] = r;
            }
            set[i] = i;
            setSize[i] = 1;
            bool sea = outlet;
            for (int k = 0; k < count; ++k) sea = sea || hollowOf[roots[k]] == 0;
            if (count == 0 && !sea) {
                hollowOf[i] = std::uint32_t(hollows.size());
                owner[i] = hollowOf[i];
                hollows.emplace_back().own.push_back(i);
                continue;
            }
            if (sea) {
                // Everything that meets the sea here spills into it at this level.
                for (int k = 0; k < count; ++k) {
                    if (hollowOf[roots[k]] == 0) continue;
                    Hollow& hollow = hollows[hollowOf[roots[k]]];
                    hollow.z = h[i];
                    hollow.parent = 0;
                    hollow.cellsBelow = setSize[roots[k]];
                }
                for (int k = 0; k < count; ++k) join(i, roots[k]);
                hollowOf[i] = 0;
                owner[i] = 0;
                continue;
            }
            if (count == 1) {
                join(roots[0], i);
                owner[i] = hollowOf[roots[0]];
                hollows[owner[i]].own.push_back(i);
                continue;
            }
            // A saddle: the hollows meeting here become one, above this level.
            const auto merged = std::uint32_t(hollows.size());
            hollows.emplace_back();
            for (int k = 0; k < count; ++k) {
                Hollow& child = hollows[hollowOf[roots[k]]];
                child.z = h[i];
                child.parent = merged;
                child.cellsBelow = setSize[roots[k]];
                hollows[merged].children.push_back(hollowOf[roots[k]]);
            }
            for (int k = 0; k < count; ++k) join(i, roots[k]);
            hollowOf[i] = merged;
            owner[i] = merged;
            hollows[merged].own.push_back(i);
        }

        // Where each cell's water ends up: down the steepest drop of the true
        // ground to a cell with nothing lower, which belongs to some hollow.
        std::vector<std::uint32_t> end(cells);
        for (const std::uint32_t i : byHeight) {
            std::uint32_t low = i;
            for (int k = 0; k < 8; ++k) {
                const std::uint32_t c = neighbour(i, k);
                if (c != kNone && h[c] < h[low]) low = c;
            }
            end[i] = low == i ? i : end[low];
        }
        for (std::uint32_t i = 0; i < cells; ++i) hollows[owner[end[i]]].water += cellKm2 * s.humidity;
        // A hollow's water is its own and all it contains: children first
        // (a hollow is always made after the ones it joins).
        std::vector<double> total(hollows.size(), 0.0);
        std::vector<float> floor(hollows.size(), std::numeric_limits<float>::max());
        for (std::size_t n = 1; n < hollows.size(); ++n) {
            total[n] += hollows[n].water;
            if (!hollows[n].own.empty()) floor[n] = std::min(floor[n], h[hollows[n].own.front()]);
            if (hollows[n].parent != kNone && hollows[n].parent != 0) {
                total[hollows[n].parent] += total[n];
                floor[hollows[n].parent] = std::min(floor[hollows[n].parent], floor[n]);
            }
        }
        // A shallow or small hollow is a dent in the ground (the detail makes
        // them on every slope), not a basin with a climate of its own: its
        // water runs straight through, as if the stream had long since cut
        // its way out - no pond, and no water kept.
        auto dent = [&](std::uint32_t n) {
            const Hollow& hollow = hollows[n];
            return hollow.z - floor[n] < s.fillDepth || hollow.cellsBelow * cellKm2 < s.fillArea;
        };

        // Down the tree from each hollow that spills into the sea, sharing out
        // the water: a hollow with enough fills to the level it joins at; one
        // without makes its own lake and keeps its water.
        struct Lake { float level; std::vector<std::uint32_t> cells; bool closed; };
        std::vector<Lake> lakes;
        auto everything = [&](std::uint32_t n, std::vector<std::uint32_t>& into) {
            std::vector<std::uint32_t> stack(1, n);
            while (!stack.empty()) {
                const Hollow& hollow = hollows[stack.back()];
                stack.pop_back();
                into.insert(into.end(), hollow.own.begin(), hollow.own.end());
                stack.insert(stack.end(), hollow.children.begin(), hollow.children.end());
            }
        };
        struct Task { std::uint32_t n; double water; };
        std::vector<Task> tasks;
        for (std::uint32_t n = 1; n < hollows.size(); ++n)
            if (hollows[n].parent == 0) tasks.push_back({n, total[n]});
        while (!tasks.empty()) {
            const Task task = tasks.back();
            tasks.pop_back();
            const Hollow& hollow = hollows[task.n];
            const double capacity = hollow.cellsBelow * cellKm2;
            if (dent(task.n)) continue;
            if (task.water >= capacity) {
                // Full: a lake to the level it spills at; the rest goes on.
                Lake full{hollow.z, {}, false};
                everything(task.n, full.cells);
                lakes.push_back(std::move(full));
                continue;
            }
            // What it takes to fill its parts to where they meet. A dent needs
            // nothing: silted up, it is just more floor for the lake.
            double childrenNeed = 0;
            for (const std::uint32_t c : hollow.children)
                if (!dent(c)) childrenNeed += hollows[c].cellsBelow * cellKm2;
            if (hollow.children.empty() || task.water >= childrenNeed) {
                // One lake over all of it, its level where its area is the water.
                Lake open{0, {}, true};
                for (const std::uint32_t c : hollow.children) everything(c, open.cells);
                const auto more = std::size_t(std::max(0.0, task.water - childrenNeed) / cellKm2);
                const std::size_t wet = std::min(more, hollow.own.size());
                open.cells.insert(open.cells.end(), hollow.own.begin(), hollow.own.begin() + std::ptrdiff_t(wet));
                open.level = wet < hollow.own.size() ? h[hollow.own[wet]] : hollow.z;
                for (std::size_t k = wet; k < hollow.own.size(); ++k) closed[hollow.own[k]] = 1;
                lakes.push_back(std::move(open));
                continue;
            }
            // The water does not reach the saddle: each part keeps its own, a
            // full one spilling over into the others. What falls on the saddle
            // ground itself goes to the biggest.
            for (const std::uint32_t own : hollow.own) closed[own] = 1;
            std::vector<double> share;
            double spare = task.water;
            std::size_t biggest = 0;
            for (std::size_t k = 0; k < hollow.children.size(); ++k) {
                share.push_back(total[hollow.children[k]]);
                spare -= share.back();
                if (hollows[hollow.children[k]].cellsBelow > hollows[hollow.children[biggest]].cellsBelow) biggest = k;
            }
            share[biggest] += std::max(0.0, spare);
            for (int round = 0; round < 8; ++round) {
                double over = 0, deficit = -1;
                std::size_t driest = 0;
                for (std::size_t k = 0; k < share.size(); ++k) {
                    // A dent holds nothing: what reaches it runs on.
                    const double room = dent(hollow.children[k]) ? 0.0 : hollows[hollow.children[k]].cellsBelow * cellKm2;
                    if (share[k] > room) { over += share[k] - room; share[k] = room; }
                    else if (room - share[k] > deficit) { deficit = room - share[k]; driest = k; }
                }
                if (over <= 0) break;
                share[driest] += over;
            }
            for (std::size_t k = 0; k < share.size(); ++k) tasks.push_back({hollow.children[k], share[k]});
        }

        // Too small or too shallow to be a lake: dry ground (still keeping
        // its water if it was closed).
        for (const Lake& l : lakes) {
            if (l.cells.empty()) continue;
            float bottom = l.level;
            for (const std::uint32_t i : l.cells) bottom = std::min(bottom, h[i]);
            const bool keep = l.level - bottom >= s.lakeDepth && double(l.cells.size()) * cellKm2 >= s.minLakeArea;
            for (const std::uint32_t i : l.cells) {
                if (l.closed) closed[i] = keep ? 2 : 1;
                if (!keep) continue;
                lake[i] = 1;
                lakeLevel[i] = l.level;
            }
            if (keep) {
                ++out.lakes;
                out.closedLakes += l.closed ? 1 : 0;
                out.water.lakeCells += std::uint32_t(l.cells.size());
            }
        }
    }
    // In a hollow that keeps its water the flow stops: down the true ground
    // to the lake, which is where it ends.
    for (std::uint32_t i = 0; i < cells; ++i)
        if (closed[i] == 2) receiver[i] = i;
        else if (closed[i] == 1) receiver[i] = steepest(i, route, i);

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
        node.width = float(s.widthScale * std::pow(node.area, s.widthExponent));
        node.depth = float(s.depthScale * std::pow(node.area, 0.33) + 0.3);
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
    // Deltas: a big river reaching the sea over low, flat ground splits a
    // way above its mouth into branches fanning out to the shore - two for
    // a river, three or four for a great one. Each is narrower than the
    // river and cut like any other; the river's own channel stays as one.
    auto isSea = [&](double x, double y) {
        const auto cx = std::int64_t(x / dx), cy = std::int64_t(y / dx);
        if (cx < 0 || cy < 0 || cx >= std::int64_t(side) || cy >= std::int64_t(side)) return true;
        return ocean[std::size_t(cy) * side + std::size_t(cx)] != 0;
    };
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const Node& last = nodes[k];
        if (last.down == kNone || !nodes[last.down].terminal || last.area < s.deltaArea) continue;
        if (!ocean[nodes[last.down].cell]) continue;   // into a lake: no delta (yet)
        const Node& mouth = nodes[last.down];
        // Up the main line to the head of the delta.
        const double reach = std::clamp(0.25 * std::sqrt(double(last.area)), 4.0, 15.0) * 1000.0;
        std::size_t head = k;
        double walked = std::hypot(mouth.x - last.x, mouth.y - last.y);
        while (walked < reach && nodes[head].upMain != kNone) {
            const Node& up = nodes[nodes[head].upMain];
            walked += std::hypot(nodes[head].x - up.x, nodes[head].y - up.y);
            head = nodes[head].upMain;
        }
        const Node& top = nodes[head];
        const float topGround = ground.at(top.x, top.y, 8.0);
        if (topGround > s.deltaHeight) continue;   // a river falling to the sea off high ground: no delta
        const int branches = 2 + (last.area > 2000 ? 1 : 0) + (last.area > 8000 ? 1 : 0);
        const double ax = mouth.x - top.x, ay = mouth.y - top.y, along = std::max(std::hypot(ax, ay), 1.0);
        const double ux = ax / along, uy = ay / along;
        const float width = float(last.width / std::pow(double(branches), 0.6));
        const float depth = float(last.depth / std::pow(double(branches), 0.4));
        const float topSurface = std::max(0.5f, std::min(topGround - 0.3f, top.surface));
        const float flow = float(std::clamp(std::log(double(last.area) / s.streamArea) / logBiggest, 0.0, 1.0)) * 0.8f;
        // The directions from the head that reach the sea soon enough: a
        // fan across the river's line, kept only where it meets the water
        // (a narrow bay takes fewer branches than an open coast).
        struct Way { double x, y; };
        std::vector<Way> ways;
        for (int a = 0; a <= 24; ++a) {
            const double turn = -1.1 + 2.2 * a / 24.0;
            const double c = std::cos(turn), sn = std::sin(turn);
            const double dxA = ux * c - uy * sn, dyA = ux * sn + uy * c;
            for (double t = 0.3 * along; t < 1.4 * along; t += 100.0)
                if (isSea(top.x + dxA * t, top.y + dyA * t)) { ways.push_back({top.x + dxA * t, top.y + dyA * t}); break; }
        }
        if (ways.size() < 3) continue;
        const int made = std::min<int>(branches, int(ways.size() / 3) + 1);
        if (made < 2) continue;
        for (int b = 0; b < made; ++b) {
            const Way& way = ways[std::size_t((ways.size() - 1) * (b + 0.5) / made)];
            const double ex = way.x, ey = way.y;
            // A curve: leaving along the river, bending out to its own mouth,
            // and swinging from side to side on the way as a channel over a
            // flat does - never at the fork or the mouth.
            const double mx = top.x + ux * along * 0.35, my = top.y + uy * along * 0.35;
            const double length = std::hypot(ex - top.x, ey - top.y);
            const double bends = std::max(2.0, length / std::max(30.0 * double(width), 1500.0));
            const double swing = std::min(0.06 * length, 8.0 * double(width) + 150.0);
            const double phase = valueNoise(top.x, top.y + b * 1777.0, 2000.0, 57) * 6.2831853;
            const int kPieces = std::max(10, int(bends * 8));
            double px = top.x, py = top.y;
            for (int j = 1; j <= kPieces; ++j) {
                const double t = double(j) / kPieces, t0 = double(j - 1) / kPieces;
                double qx = (1 - t) * (1 - t) * top.x + 2 * (1 - t) * t * mx + t * t * ex;
                double qy = (1 - t) * (1 - t) * top.y + 2 * (1 - t) * t * my + t * t * ey;
                // Sideways to the curve's own direction there.
                const double tx = 2 * (1 - t) * (mx - top.x) + 2 * t * (ex - mx), ty = 2 * (1 - t) * (my - top.y) + 2 * t * (ey - my);
                const double tl = std::max(std::hypot(tx, ty), 1e-3);
                const double off = swing * std::sin(t * bends * 6.2831853 + phase) * std::sin(t * 3.14159265);
                qx += -ty / tl * off;
                qy += tx / tl * off;
                RiverSegment seg{};
                seg.x0 = float(px); seg.y0 = float(py); seg.x1 = float(qx); seg.y1 = float(qy);
                seg.surface0 = float(topSurface * (1.0 - t0));
                seg.surface1 = float(topSurface * (1.0 - t));
                seg.width0 = float(last.width + (width - last.width) * std::min(1.0, t0 * 3.0));
                seg.width1 = float(last.width + (width - last.width) * std::min(1.0, t * 3.0));
                seg.depth0 = seg.depth1 = depth;
                seg.flow = flow;
                seg.bank = std::max(6.0f, width) + 4.0f * depth;
                out.rivers.push_back(seg);
                px = qx;
                py = qy;
            }
        }
        ++out.deltas;
        out.deltaHeads.push_back({float(top.x), float(top.y), last.area});
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return out;
}

} // namespace terrain_core
