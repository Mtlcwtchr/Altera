// AL-76: buildings by purpose, from modules of the build kit (sim/structure_plan.hpp).

#include "sim/structure_plan.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "sim/building.hpp"

namespace altera::sim {
namespace {
constexpr double kPi = 3.14159265358979323846;

void range(const Json& j, const char* key, int& lo, int& hi) {
    if (!j.contains(key)) return;
    const Json& v = j[key];
    if (v.is_array() && v.size() >= 2) lo = v[0].get<int>(), hi = v[1].get<int>();
    else if (v.is_number()) lo = hi = v.get<int>();
    if (hi < lo) std::swap(lo, hi);
}

std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& s : j[key]) out.push_back(s.get<std::string>());
    return out;
}

template <class T>
const T& pick(Rng& r, const std::vector<T>& v) {
    return v[std::size_t(r.irange(0, int(v.size()) - 1))];
}

// rot of the way out of a side, in the building's frame
int outRot(const std::string& side) { return side == "front" ? 0 : side == "right" ? 1 : side == "back" ? 2 : 3; }
bool alongJ(const std::string& side) { return side == "left" || side == "right"; }

// The mirrored half of a gable at an end (end rot E) of a slope whose eave faces R (as the game's roof tool lays it).
int gableFor(int endRot, int eaveRot) {
    const bool half = eaveRot == 0 || eaveRot == 1;
    switch (endRot) {
    case 3: return half ? 3 + 4 : 3;
    case 1: return half ? 1 : 1 + 4;
    case 0: return half ? 0 + 4 : 0;
    default: return half ? 2 : 2 + 4;
    }
}

struct Cell {
    int i, j;
    bool operator<(const Cell& o) const { return i != o.i ? i < o.i : j < o.j; }
    bool operator==(const Cell& o) const { return i == o.i && j == o.j; }
};

// Walking at ground level through a plan: cells, the walls between them, the
// cells things stand on. Reached from outside the footprint.
struct Walk {
    const BuildCatalog& build;
    std::set<SlotKey> walls;   // non-door wall slots at storey 0
    std::set<Cell> taken;
    int i0, j0, i1, j1;        // the footprint
    Walk(const BuildCatalog& b, const StructurePlan& s) : build(b), i0(s.i0), j0(s.j0), i1(s.i1), j1(s.j1) {
        for (const auto& p : s.pieces) {
            const BuildPieceDef* d = b.find(p.kind);
            if (!d || p.z != 0 || d->slot != "edge" || d->door) continue;
            if (d->category == "wall" || d->category == "gable") walls.insert(slotOf(*d, p.i, p.j, 0, p.rot));
        }
    }
    bool open(Cell a, int rot) const {
        static const BuildPieceDef edge{"", "edge", "wall"};
        return !walls.count(slotOf(edge, a.i, a.j, 0, rot));
    }
    static Cell step(Cell a, int rot) {
        return {a.i + (rot == 1 ? 1 : rot == 3 ? -1 : 0), a.j + (rot == 2 ? 1 : rot == 0 ? -1 : 0)};
    }
    std::set<Cell> reach() const {
        std::set<Cell> seen;
        std::vector<Cell> stack;
        for (int i = i0 - 1; i <= i1; ++i)
            for (int j = j0 - 1; j <= j1; ++j)
                if (i < i0 || j < j0 || i >= i1 || j >= j1) {
                    seen.insert({i, j});
                    stack.push_back({i, j});
                }
        while (!stack.empty()) {
            const Cell a = stack.back();
            stack.pop_back();
            for (int r = 0; r < 4; ++r) {
                const Cell b = step(a, r);
                if (b.i < i0 - 1 || b.j < j0 - 1 || b.i > i1 || b.j > j1) continue;
                if (seen.count(b) || taken.count(b) || !open(a, r)) continue;
                seen.insert(b);
                stack.push_back(b);
            }
        }
        return seen;
    }
    // somebody can stand by a thing on cell c
    bool usable(const std::set<Cell>& seen, Cell c) const {
        for (int r = 0; r < 4; ++r)
            if (open(c, r) && seen.count(step(c, r))) return true;
        return false;
    }
};

}  // namespace

StructureCatalog parseStructures(const Json& j) {
    StructureCatalog c;
    if (!j.is_object()) return c;
    c.foundation = j.value("foundation", c.foundation);
    c.floor = j.value("floor", c.floor);
    c.pillar = j.value("pillar", c.pillar);
    if (j.contains("materials"))
        for (const auto& [k, v] : j["materials"].items()) {
            StructureMaterial m;
            m.wall = v.value("wall", std::string());
            m.door = v.value("door", m.wall);
            m.window = v.value("window", m.wall);
            m.gable = v.value("gable", std::string());
            c.materials[k] = m;
        }
    const Json village = j.value("village", Json::object());
    c.trades = strings(village, "trades");
    c.shared = strings(village, "shared");
    c.tradeShare = village.value("trade_share", c.tradeShare);
    c.yardD = village.value("yard_d", c.yardD);
    c.fence = village.value("fence", std::string());
    for (const auto& t : j.value("types", Json::array())) {
        StructureTypeDef d;
        d.id = t.at("id").get<std::string>();
        d.home = t.value("home", false);
        d.shared = t.value("shared", false);
        d.trade = t.value("trade", std::string());
        d.hosts = strings(t, "hosts");
        d.zone = t.value("zone", d.zone);
        const Json m = t.value("main", Json::object());
        range(m, "w", d.wLo, d.wHi);
        range(m, "d", d.dLo, d.dHi);
        d.bigFamily = m.value("big_family", 0);
        d.storeys = std::clamp(m.value("storeys", 1), 1, 3);
        d.walls = strings(m, "walls");
        d.roofs = strings(m, "roof");
        d.gableFront = m.value("gable_front", 0.0);
        d.rooms = m.value("rooms", false);
        d.aisle = m.value("aisle", false);
        for (const auto& a : t.value("annexes", Json::array())) {
            StructureAnnexDef x;
            x.kind = a.at("kind").get<std::string>();
            x.sides = strings(a, "side");
            x.chance = a.value("chance", 1.0);
            x.required = a.value("required", false);
            range(a, "len", x.lenLo, x.lenHi);
            range(a, "depth", x.depthLo, x.depthHi);
            x.door = a.value("door", false);
            x.stations = strings(a, "stations");
            d.annexes.push_back(std::move(x));
        }
        for (const char* key : {"stations", "decor"})
            for (const auto& s : t.value(key, Json::array())) {
                StructureSlotDef x;
                x.what = s.value(std::string(key) == "stations" ? "object" : "piece", std::string());
                x.at = s.value("at", x.at);
                (std::string(key) == "stations" ? d.stations : d.decor).push_back(x);
            }
        c.types.push_back(std::move(d));
    }
    return c;
}

// ---- the frame --------------------------------------------------------------
void StructureFrame::cell(int i, int j, int& x, int& y) const {
    // cell centres turn by (x, y) -> (-y, x) per quarter
    double cx = i + 0.5, cy = j + 0.5;
    for (int k = 0; k < ((q % 4) + 4) % 4; ++k) {
        const double t = cx;
        cx = -cy;
        cy = t;
    }
    x = int(std::floor(cx)) + gx;
    y = int(std::floor(cy)) + gy;
}
int StructureFrame::rot(int localRot) const {
    const int m = localRot >= 4 ? 4 : 0;
    return (((localRot % 4) + q) % 4 + 4) % 4 + m;
}
Vec2 StructureFrame::point(double u, double v) const {
    double x = u, y = v;
    for (int k = 0; k < ((q % 4) + 4) % 4; ++k) {
        const double t = x;
        x = -y;
        y = t;
    }
    return {x + gx * cellM, y + gy * cellM};
}

Vec2 footprintCentre(const StructurePlan& s, double cellM) {
    return {(s.i0 + s.i1) * 0.5 * cellM, (s.j0 + s.j1) * 0.5 * cellM};
}

// ---- the plan ----------------------------------------------------------------
StructurePlan planStructure(const StructureCatalog& cat, const BuildCatalog& build, const StructureTypeDef& type,
                            const StructureParams& prm) {
    StructurePlan s;
    s.type = type.id;
    Rng r(hashCombine(prm.seed, hashStr("structure:" + type.id)));
    const double cm = build.cellM;
    int W = r.irange(type.wLo, type.wHi), D = r.irange(type.dLo, type.dHi);
    if (type.home && type.bigFamily > 0 && prm.people >= type.bigFamily) ++W;
    W = std::max(W, 2), D = std::max(D, 2);
    const int S = type.storeys;
    // a gable over the front: the roof rises across the width, so not over a wide house
    // (a third storey of slope does not hold on daub)
    const bool gableFront = r.chance(type.gableFront) && W <= 4;
    StructureMaterial mat{"wall_daub", "wall_door_daub", "wall_window_daub", "gable_daub"};
    if (!type.walls.empty()) {
        auto it = cat.materials.find(pick(r, type.walls));
        if (it != cat.materials.end()) mat = it->second;
    }
    const std::string roof = type.roofs.empty() ? "roof_thatch" : pick(r, type.roofs);
    s.volumes.push_back({"main", 0, 0, W, D, S});
    // the door: the middle of a gable front, else any cell but the corners
    s.doorI = gableFront ? (W % 2 ? W / 2 : W / 2 - r.irange(0, 1)) : (W > 2 ? r.irange(1, W - 2) : 0);

    std::set<Cell> used;   // cells of every volume
    for (int i = 0; i < W; ++i)
        for (int j = 0; j < D; ++j) used.insert({i, j});

    // ---- annexes: the side, the span along the wall, the cells ----
    struct Annex {
        const StructureAnnexDef* def;
        std::string side;
        int t0, len, depth;
        StructureVolume vol;
    };
    std::vector<Annex> annexes;
    for (const auto& a : type.annexes) {
        if (!a.required && !r.chance(a.chance)) continue;
        bool placed = false;
        std::vector<std::string> sides = a.sides;
        if (!a.required && sides.size() > 1 && r.chance(0.5)) std::rotate(sides.begin(), sides.begin() + 1, sides.end());
        for (const auto& side : sides) {
            const bool front = side == "front", back = side == "back";
            if (!front && !back && side != "left" && side != "right") continue;
            const bool gableEnd = gableFront ? (front || back) : !(front || back);
            const bool leanTo = a.kind != "canopy";
            // a one-storey eave is too low for anything to lean on it
            if (S == 1 && !gableEnd) continue;
            if (a.kind == "porch" && !front) continue;
            const int wallLen = alongJ(side) ? D : W;
            const int len = std::min(r.irange(a.lenLo, a.lenHi), wallLen);
            const int depth = leanTo ? 1 : std::max(1, r.irange(a.depthLo, a.depthHi));
            if (a.kind == "canopy" && len != 2) continue;   // its own two slopes
            // where along the wall it may start: a lean-to's top meets the
            // wall a storey above the eave, so on a one-storey gable end it
            // keeps off the low corners
            int lo = 0, hi = wallLen - len;
            if (leanTo && S == 1) lo = 1, hi = wallLen - 1 - len;
            std::vector<int> starts;
            for (int t = lo; t <= hi; ++t) {
                if (a.kind == "porch" && (s.doorI < t || s.doorI >= t + len)) continue;
                starts.push_back(t);
            }
            if (starts.empty()) continue;
            const int t0 = pick(r, starts);
            StructureVolume v;
            v.role = a.kind;
            if (side == "left") v = {a.kind, -depth, t0, 0, t0 + len, 1};
            else if (side == "right") v = {a.kind, W, t0, W + depth, t0 + len, 1};
            else if (back) v = {a.kind, t0, D, t0 + len, D + depth, 1};
            else v = {a.kind, t0, -depth, t0 + len, 0, 1};
            bool clash = false;
            for (int i = v.i0; i < v.i1; ++i)
                for (int j = v.j0; j < v.j1; ++j)
                    if (used.count({i, j})) clash = true;
            if (clash) continue;
            for (int i = v.i0; i < v.i1; ++i)
                for (int j = v.j0; j < v.j1; ++j) used.insert({i, j});
            annexes.push_back({&a, side, t0, len, depth, v});
            s.volumes.push_back(v);
            placed = true;
            break;
        }
        if (!placed && a.required) s.notes.push_back("no room for the " + a.kind);
    }

    std::set<SlotKey> posts;   // a post two annexes share stands once
    auto add = [&](const std::string& kind, int i, int j, int z, int rot) {
        if (kind == cat.pillar) {
            static const BuildPieceDef corner{"", "corner", "pillar"};
            if (!posts.insert(slotOf(corner, i, j, z, rot)).second) return;
        }
        s.pieces.push_back({kind, i, j, z, rot});
    };

    // ---- the main volume ----
    // where an annex meets the main walls: which edges get a door into it
    std::set<std::pair<Cell, int>> annexDoors, annexWalls;
    for (const auto& a : annexes) {
        const int out = outRot(a.side);
        std::vector<Cell> cells;
        for (int t = a.t0; t < a.t0 + a.len; ++t) {
            if (a.side == "left") cells.push_back({0, t});
            else if (a.side == "right") cells.push_back({W - 1, t});
            else if (a.side == "back") cells.push_back({t, D - 1});
            else cells.push_back({t, 0});
        }
        for (const Cell& c : cells) annexWalls.insert({c, out});
        if (a.def->door && !cells.empty()) annexDoors.insert({cells[cells.size() / 2], out});
    }
    for (int i = 0; i < W; ++i)
        for (int j = 0; j < D; ++j) add(cat.foundation, i, j, 0, 0);
    for (int z = 0; z < S; ++z) {
        if (z > 0)
            for (int i = 0; i < W; ++i)
                for (int j = 0; j < D; ++j) add(cat.floor, i, j, z, 0);
        auto wall = [&](int i, int j, int rot, bool corner) {
            const Cell c{i, j};
            std::string kind = mat.wall;
            if (z == 0 && rot == 0 && i == s.doorI) kind = mat.door;
            else if (z == 0 && annexDoors.count({c, rot})) kind = mat.door;
            else if (!annexWalls.count({c, rot}) && !corner && r.chance(z > 0 ? 0.6 : rot == 2 ? 0.2 : 0.5)) kind = mat.window;
            add(kind, i, j, z, rot);
        };
        for (int i = 0; i < W; ++i) {
            wall(i, 0, 0, i == 0 || i == W - 1);
            wall(i, D - 1, 2, i == 0 || i == W - 1);
        }
        for (int j = 0; j < D; ++j) {
            wall(0, j, 3, j == 0 || j == D - 1);
            wall(W - 1, j, 1, j == 0 || j == D - 1);
        }
        add(cat.pillar, 0, 0, z, 0);
        add(cat.pillar, W - 1, 0, z, 1);
        add(cat.pillar, W - 1, D - 1, z, 2);
        add(cat.pillar, 0, D - 1, z, 3);
    }
    // the upper storey cut into rooms along the back, a door into each from the front half
    if (type.rooms && S >= 2 && D >= 3) {
        const int jm = D / 2;
        for (int a = 0; a < W; a += 2) {
            for (int i = a; i < std::min(W, a + 2); ++i) add(i == a ? mat.door : mat.wall, i, jm - 1, 1, 2);
            if (a + 2 < W)
                for (int j = jm; j < D; ++j) add(mat.wall, a + 1, j, 1, 1);
        }
    }
    // the roof: the ridge along i (eaves front and back) or, with a gable
    // front, along j; each row a storey higher toward it; at the ends gables
    // and walls under them (as the game's roof tool lays one)
    {
        const int zb = S;
        const bool alongX = !gableFront;
        const int rows = alongX ? D : W, len = alongX ? W : D;
        for (int row = 0; row < rows; ++row) {
            const int k = std::min(row, rows - 1 - row);
            const bool lower = row < rows / 2 || (rows % 2 && row == rows / 2);
            const int rot = alongX ? (lower ? 0 : 2) : (lower ? 3 : 1);
            for (int t = 0; t < len; ++t) add(roof, alongX ? t : row, alongX ? row : t, zb + k, rot);
            for (int end = 0; end < 2; ++end) {
                const int i = alongX ? (end ? W - 1 : 0) : row, j = alongX ? row : (end ? D - 1 : 0);
                const int endRot = alongX ? (end ? 1 : 3) : (end ? 2 : 0);
                for (int z = 0; z < k; ++z) add(mat.wall, i, j, zb + z, endRot);
                if (!mat.gable.empty()) add(mat.gable, i, j, zb + k, gableFor(endRot, rot));
            }
        }
    }

    // ---- the annexes' modules ----
    for (const auto& a : annexes) {
        const StructureVolume& v = a.vol;
        const int out = outRot(a.side);
        const bool j_ = alongJ(a.side);
        const int lowT = j_ ? 0 : 3, highT = j_ ? 2 : 1;   // rot toward -t / +t along the wall
        for (int i = v.i0; i < v.i1; ++i)
            for (int j = v.j0; j < v.j1; ++j) add(cat.foundation, i, j, 0, 0);
        // a cell of the annex by (t along the wall, d out from it)
        auto at = [&](int t, int d, int& i, int& j) {
            if (a.side == "left") i = -1 - d, j = t;
            else if (a.side == "right") i = W + d, j = t;
            else if (a.side == "back") i = t, j = D + d;
            else i = t, j = -1 - d;
        };
        // the corner of cell (t, d) on the side `towardT` (+t or -t) and the outer side (or the inner, outer = false)
        auto corner = [&](int t, int d, bool plusT, bool outer, int z) {
            int i, j;
            at(t, d, i, j);
            // corners of a cell by rot: 0 (-i,-j) 1 (+i,-j) 2 (+i,+j) 3 (-i,+j)
            int di, dj;
            const int ti = j_ ? 0 : (plusT ? 1 : -1), tj = j_ ? (plusT ? 1 : -1) : 0;
            const int oi = out == 1 ? 1 : out == 3 ? -1 : 0, oj = out == 2 ? 1 : out == 0 ? -1 : 0;
            const int sgn = outer ? 1 : -1;
            di = ti != 0 ? ti : oi * sgn;
            dj = tj != 0 ? tj : oj * sgn;
            const int rot = di < 0 && dj < 0 ? 0 : di > 0 && dj < 0 ? 1 : di > 0 && dj > 0 ? 2 : 3;
            add(cat.pillar, i, j, z, rot);
        };
        if (a.def->kind == "canopy") {
            // posts along both eaves out from the wall, a ridge post at the
            // outer end (two storeys: it holds the gables), two slopes meeting
            // over the middle
            for (int d = 0; d < a.depth; ++d) {
                corner(a.t0, d, false, true, 0);
                corner(a.t0 + 1, d, true, true, 0);
            }
            corner(a.t0, a.depth - 1, true, true, 0);   // the ridge post (the corner the rows share)
            corner(a.t0, a.depth - 1, true, true, 1);
            for (int d = 0; d < a.depth; ++d) {
                int i, j;
                at(a.t0, d, i, j);
                add(roof, i, j, 1, lowT);
                at(a.t0 + 1, d, i, j);
                add(roof, i, j, 1, highT);
            }
            if (!mat.gable.empty()) {
                int i, j;
                at(a.t0, a.depth - 1, i, j);
                add(mat.gable, i, j, 1, gableFor(out, lowT));
                at(a.t0 + 1, a.depth - 1, i, j);
                add(mat.gable, i, j, 1, gableFor(out, highT));
            }
        } else if (a.def->kind == "porch") {
            for (int t = a.t0; t < a.t0 + a.len; ++t) {
                int i, j;
                at(t, 0, i, j);
                add(roof, i, j, 1, out);
                corner(t, 0, false, true, 0);
            }
            corner(a.t0 + a.len - 1, 0, true, true, 0);
        } else {   // shed, byre: a closed lean-to, its roof falling outward, the door at an end or outside
            for (int t = a.t0; t < a.t0 + a.len; ++t) {
                int i, j;
                at(t, 0, i, j);
                add(roof, i, j, 1, out);
            }
            // the door: at the end toward the front for a side shed, else in the outer wall
            const bool doorEnd = j_;
            const int doorT = a.t0 + a.len / 2;
            for (int t = a.t0; t < a.t0 + a.len; ++t) {
                int i, j;
                at(t, 0, i, j);
                add(!doorEnd && t == doorT ? mat.door : mat.wall, i, j, 0, out);
            }
            for (int end = 0; end < 2; ++end) {
                const int t = end ? a.t0 + a.len - 1 : a.t0;
                const int endRot = end ? highT : lowT;
                int i, j;
                at(t, 0, i, j);
                add(doorEnd && end == 0 ? mat.door : mat.wall, i, j, 0, endRot);
                if (!mat.gable.empty()) add(mat.gable, i, j, 1, gableFor(endRot, out));
            }
            corner(a.t0, 0, false, true, 0);
            corner(a.t0 + a.len - 1, 0, true, true, 0);
        }
    }

    // the footprint
    s.i0 = s.j0 = 1 << 20;
    s.i1 = s.j1 = -(1 << 20);
    for (const auto& v : s.volumes) {
        s.i0 = std::min(s.i0, v.i0), s.j0 = std::min(s.j0, v.j0);
        s.i1 = std::max(s.i1, v.i1), s.j1 = std::max(s.j1, v.j1);
    }

    // ---- what stands in it ----
    Walk walk(build, s);
    std::set<Cell> reserved{{s.doorI, 0}};
    for (const auto& [c, rot] : annexDoors) reserved.insert(c);
    if (type.aisle)
        for (int j = 0; j < D - 1; ++j) reserved.insert({W / 2, j});   // the altar closes it at the back
    std::vector<Cell> placedCells;
    auto mainCells = [&](const std::string& at) {
        std::vector<Cell> v;
        auto push = [&](int i, int j) {
            if (i >= 0 && j >= 0 && i < W && j < D) v.push_back({i, j});
        };
        if (at == "back")
            for (int k = 0; k < W; ++k) push(W / 2 + (k % 2 ? -(k + 1) / 2 : k / 2), D - 1);
        else if (at == "side")
            for (int j = 1; j < D; ++j) push(0, j), push(W - 1, j);
        else if (at == "middle")
            for (int j = 1; j < D - 1; ++j)
                for (int i = 1; i < W - 1; ++i) push(i, j);
        else if (at == "front")
            for (int i = 0; i < W; ++i) push(i, 0);
        // then anywhere in the main volume, the back first
        for (int j = D - 1; j >= 0; --j)
            for (int i = 0; i < W; ++i) push(i, j);
        return v;
    };
    auto annexCells = [&](const std::string& what) {
        std::vector<Cell> v;
        for (const auto& a : annexes)
            if (std::find(a.def->stations.begin(), a.def->stations.end(), what) != a.def->stations.end())
                for (int j = a.vol.j0; j < a.vol.j1; ++j)
                    for (int i = a.vol.i0; i < a.vol.i1; ++i) v.push_back({i, j});
        return v;
    };
    auto anyAnnexCells = [&]() {
        std::vector<Cell> v;
        for (const auto& a : annexes)
            for (int j = a.vol.j0; j < a.vol.j1; ++j)
                for (int i = a.vol.i0; i < a.vol.i1; ++i) v.push_back({i, j});
        return v;
    };
    // the wall a cell leans on (rot) or -1 (none): back first, then the sides
    auto leanOn = [&](Cell c) {
        if (c.i >= 0 && c.j >= 0 && c.i < W && c.j < D) {
            if (c.j == D - 1) return 2;
            if (c.i == 0) return 3;
            if (c.i == W - 1) return 1;
            if (c.j == 0) return 0;
        }
        return -1;
    };
    int yardN = 0;
    auto place = [&](const std::string& what, const std::vector<Cell>& cells, bool station) {
        for (const Cell& c : cells) {
            if (walk.taken.count(c) || reserved.count(c)) continue;
            walk.taken.insert(c);
            const auto seen = walk.reach();
            bool ok = seen.count({s.doorI, 0}) > 0;
            for (const Cell& p : placedCells)
                if (!walk.usable(seen, p)) ok = false;
            if (!walk.usable(seen, c)) ok = false;
            if (!ok) {
                walk.taken.erase(c);
                continue;
            }
            placedCells.push_back(c);
            LocalThing t;
            t.what = what;
            t.i = c.i, t.j = c.j;
            const int lean = leanOn(c);
            t.u = (c.i + 0.5) * cm;
            t.v = (c.j + 0.5) * cm;
            const double off = 0.28 * cm;
            if (lean == 2) t.v += off, t.yaw = 0.0;
            else if (lean == 3) t.u -= off, t.yaw = kPi / 2;
            else if (lean == 1) t.u += off, t.yaw = -kPi / 2;
            else if (lean == 0) t.v -= off, t.yaw = kPi;
            else t.yaw = r.irange(0, 3) * kPi / 2;
            (station ? s.stations : s.decor).push_back(t);
            return true;
        }
        return false;
    };
    auto yard = [&](const std::string& what, bool station) {
        LocalThing t;
        t.what = what;
        t.yard = true;
        t.u = (s.i0 + s.i1) * 0.5 * cm + (yardN % 2 ? 1 : -1) * ((yardN + 1) / 2) * 3.5;
        t.v = s.j1 * cm + 3.0;
        t.i = int(std::floor(t.u / cm)), t.j = s.j1 + 1;
        t.yaw = 0.0;
        ++yardN;
        (station ? s.stations : s.decor).push_back(t);
    };
    // the annexes' own stations first (they are what the annex is for), then the main volume's
    for (const auto& a : annexes)
        for (const auto& what : a.def->stations) {
            std::vector<Cell> cells = annexCells(what);
            // a canopy's outer cells first: the worker stands outside
            std::stable_sort(cells.begin(), cells.end(), [&](Cell x, Cell y) {
                auto outer = [&](Cell c) {
                    return a.side == "left" ? -c.i : a.side == "right" ? c.i : a.side == "back" ? c.j : -c.j;
                };
                return outer(x) > outer(y);
            });
            if (!place(what, cells, true)) {
                const auto m = mainCells("side");
                if (!place(what, m, true)) s.notes.push_back("no place for " + what);
            }
        }
    for (const auto& st : type.stations) {
        bool dup = false;
        for (const auto& t : s.stations) dup = dup || t.what == st.what;
        if (dup) continue;   // an annex holds it already
        if (st.at == "yard") {
            yard(st.what, true);
            continue;
        }
        if (!place(st.what, st.at == "annex" ? anyAnnexCells() : mainCells(st.at), true))
            s.notes.push_back("no place for " + st.what);
    }
    for (const auto& dc : type.decor) {
        if (dc.at == "yard") {
            yard(dc.what, false);
            continue;
        }
        std::vector<Cell> cells = dc.at == "annex" ? anyAnnexCells() : mainCells(dc.at);
        if (type.aisle && dc.at == "middle") {   // pews either side of the aisle, front to back
            cells.clear();
            for (int j = 1; j < D - 1; ++j)
                for (int i = 0; i < W; ++i)
                    if (i != W / 2) cells.push_back({i, j});
        }
        place(dc.what, cells, false);   // decor is a nicety: it may find no room
    }
    return s;
}

std::vector<std::string> checkStructure(const BuildCatalog& build, const StructurePlan& s) {
    std::vector<std::string> bad;
    std::map<SlotKey, std::string> slots;
    for (const auto& p : s.pieces) {
        const BuildPieceDef* d = build.find(p.kind);
        if (!d) {
            bad.push_back("unknown piece " + p.kind);
            continue;
        }
        const SlotKey k = slotOf(*d, p.i, p.j, p.z, p.rot);
        auto [it, fresh] = slots.emplace(k, p.kind);
        if (!fresh)
            bad.push_back("two pieces in one slot: " + it->second + " and " + p.kind + " at " + std::to_string(p.i) + "," +
                          std::to_string(p.j) + "," + std::to_string(p.z));
    }
    std::set<Cell> cells;
    auto inside = [&](int i, int j) {
        for (const auto& v : s.volumes)
            if (i >= v.i0 && i < v.i1 && j >= v.j0 && j < v.j1) return true;
        return false;
    };
    Walk walk(build, s);
    for (const auto* list : {&s.stations, &s.decor})
        for (const auto& t : *list) {
            if (t.yard) {
                if (t.j < s.j1) bad.push_back(t.what + " in the yard is inside the footprint");
                continue;
            }
            if (!inside(t.i, t.j)) bad.push_back(t.what + " stands outside the building");
            if (!cells.insert({t.i, t.j}).second) bad.push_back(t.what + " shares a cell");
            walk.taken.insert({t.i, t.j});
        }
    const auto seen = walk.reach();
    if (!seen.count({s.doorI, 0})) bad.push_back("the door is shut");
    for (const auto& t : s.stations)
        if (!t.yard && !walk.usable(seen, {t.i, t.j})) bad.push_back(t.what + " cannot be reached");
    if (s.volumes.empty() || s.volumes[0].role != "main") bad.push_back("no main volume");
    return bad;
}

}  // namespace altera::sim
