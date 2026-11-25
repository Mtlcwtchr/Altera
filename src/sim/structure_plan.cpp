// AL-76 / AL-81: houses put together from rooms, from modules of the build kit (sim/structure_plan.hpp).

#include "sim/structure_plan.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "sim/building.hpp"

namespace lr::sim {
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
    c.layout = village.value("layout", Json::object());
    // how each room is furnished when the generator makes one
    for (const auto& r : j.value("rooms", Json::array())) {
        RoomFurnish f;
        const Json fj = r.value("furnish", Json::object());
        for (const char* key : {"stations", "decor"})
            for (const auto& s : fj.value(key, Json::array())) {
                StructureSlotDef x;
                x.what = s.value("object", std::string());
                x.at = s.value("at", x.at);
                (std::string(key) == "stations" ? f.stations : f.decor).push_back(x);
            }
        f.beds = r.value("beds", false);
        c.furnish[r.at("id").get<std::string>()] = std::move(f);
    }
    // the recipes: the rooms a house is put together from, and its shell
    auto recipes = [&](const Json& list, bool home) {
        for (const auto& t : list) {
            StructureTypeDef d;
            d.id = t.at("id").get<std::string>();
            d.home = home;
            d.shared = !home;
            d.trade = t.value("skill", std::string());
            d.hosts = strings(t, "hosts");
            d.yard = strings(t, "yard");
            d.zone = t.value("zone", d.zone);
            const Json site = t.value("site", Json::object());   // AL-84
            d.siteWater = site.value("water", 0.0);
            d.siteWood = site.value("wood", 0.0);
            d.siteEntrance = site.value("entrance", 0.0);
            d.siteDownwind = site.value("downwind", 0.0);
            d.nuisance = site.value("nuisance", false);
            d.quiet = site.value("quiet", false);
            const Json m = t.value("shell", Json::object());
            range(m, "w", d.wLo, d.wHi);
            range(m, "d", d.dLo, d.dHi);
            d.bigFamily = m.value("big_family", 0);
            d.storeys = std::clamp(m.value("storeys", 1), 1, 3);
            d.walls = strings(m, "walls");
            d.roofs = strings(m, "roof");
            d.gableFront = m.value("gable_front", 0.0);
            d.aisle = m.value("aisle", false);
            for (const auto& a : t.value("rooms", Json::array())) {
                const std::string at = a.value("at", std::string("main"));
                const std::string room = a.value("room", std::string());
                if (at == "main") d.mainRoom = room;
                else if (at == "split") d.splitRoom = room, d.splitW = std::max(1, a.value("w", 2));
                else if (at == "upper") d.upperRoom = room, d.upperN = std::max(1, a.value("n", 1));
                else {
                    StructureAnnexDef x;
                    x.kind = at == "lean_to" ? "shed" : at;
                    x.room = room;
                    x.sides = strings(a, "side");
                    if (x.sides.empty()) x.sides = at == "porch" ? std::vector<std::string>{"front"}
                                                                 : std::vector<std::string>{"left", "right", "back"};
                    x.chance = a.value("chance", 1.0);
                    x.required = a.value("required", false);
                    if (x.kind == "canopy") x.lenLo = x.lenHi = 2;
                    range(a, "len", x.lenLo, x.lenHi);
                    range(a, "depth", x.depthLo, x.depthHi);
                    x.door = a.value("door", false);
                    d.annexes.push_back(std::move(x));
                }
            }
            if (d.storeys < 2) d.upperRoom.clear();
            c.types.push_back(std::move(d));
        }
    };
    recipes(j.value("households", Json::array()), true);
    recipes(j.value("shared", Json::array()), false);
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
    s.recipe = type.id;
    Rng r(hashCombine(prm.seed, hashStr("structure:" + type.id)));
    const double cm = build.cellM;
    int W0 = r.irange(type.wLo, type.wHi), D = r.irange(type.dLo, type.dHi);
    if (type.home && type.bigFamily > 0 && prm.people >= type.bigFamily) ++W0;
    W0 = std::max(W0, 2), D = std::max(D, 2);
    // a second ground-floor room behind a partition widens the house
    const int splitW = type.splitRoom.empty() ? 0 : type.splitW;
    const int W = W0 + splitW;
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
    // the door: the middle of a gable front, else any cell but the corners; into the main room
    s.doorI = gableFront ? (W % 2 ? W / 2 : W / 2 - r.irange(0, 1)) : (W > 2 ? r.irange(1, W - 2) : 0);
    if (splitW && s.doorI >= W0) s.doorI = W0 > 2 ? r.irange(1, W0 - 1) : W0 - 1;
    // the rooms of the main volume
    s.parts.push_back({type.mainRoom, "main", 0, 0, 0, W0, D});
    if (splitW) s.parts.push_back({type.splitRoom, "split", 0, W0, 0, W, D});
    std::vector<int> upperCuts;
    if (!type.upperRoom.empty() && S >= 2) {
        const int n = std::clamp(type.upperN, 1, std::max(1, W / 2));
        for (int k = 0; k <= n; ++k) upperCuts.push_back(int(std::lround(double(W) * k / n)));
        for (int k = 0; k < n; ++k) s.parts.push_back({type.upperRoom, "upper", 1, upperCuts[k], 0, upperCuts[k + 1], D});
    }

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
            s.parts.push_back({a.room, a.kind, 0, v.i0, v.j0, v.i1, v.j1});
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
    // the partition of the ground floor: a wall with a door between the main room and the second one
    if (splitW)
        for (int j = 0; j < D; ++j) add(j == D / 2 ? mat.door : mat.wall, W0, j, 0, 3);
    // the upper storey cut into rooms across its width, a door between each two
    for (std::size_t k = 1; k + 1 < upperCuts.size(); ++k)
        if (upperCuts[k] > upperCuts[k - 1])
            for (int j = 0; j < D; ++j) add(j == D / 2 ? mat.door : mat.wall, upperCuts[k], j, 1, 3);
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
            // the door: at the end toward the front for a side shed, else in the outer wall;
            // never onto another annex (its things would stand in the way)
            int ni, nj;
            at(a.t0 - 1, 0, ni, nj);
            const bool frontFree = !used.count({ni, nj});
            at(a.t0 + a.len, 0, ni, nj);
            const bool backFree = !used.count({ni, nj});
            const int doorAt = !j_ ? -1 : frontFree ? 0 : backFree ? 1 : -1;   // the end, -1: the outer wall
            const int doorT = a.t0 + a.len / 2;
            for (int t = a.t0; t < a.t0 + a.len; ++t) {
                int i, j;
                at(t, 0, i, j);
                add(doorAt < 0 && t == doorT ? mat.door : mat.wall, i, j, 0, out);
            }
            for (int end = 0; end < 2; ++end) {
                const int t = end ? a.t0 + a.len - 1 : a.t0;
                const int endRot = end ? highT : lowT;
                int i, j;
                at(t, 0, i, j);
                add(doorAt == end ? mat.door : mat.wall, i, j, 0, endRot);
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

    // ---- what stands in it: each room furnished as what it is ----
    Walk walk(build, s);
    std::set<Cell> reserved{{s.doorI, 0}};
    for (const auto& [c, rot] : annexDoors) reserved.insert(c);
    if (splitW) reserved.insert({W0 - 1, D / 2}), reserved.insert({W0, D / 2});
    if (type.aisle)
        for (int j = 0; j < D - 1; ++j) reserved.insert({W0 / 2, j});   // the altar closes it at the back
    std::set<std::pair<Cell, int>> upTaken;   // cells taken on the upper storeys
    std::vector<Cell> placedCells;            // ground things that must stay usable
    // the cells of a room for a slot, the preferred first, then the rest (back first)
    auto partCells = [&](const StructurePart& p, const std::string& at) {
        std::vector<Cell> v;
        auto push = [&](int i, int j) {
            if (i >= p.i0 && j >= p.j0 && i < p.i1 && j < p.j1) v.push_back({i, j});
        };
        const int w = p.i1 - p.i0, mid = p.i0 + w / 2;
        const bool annex = p.at == "canopy" || p.at == "shed" || p.at == "porch";
        if (!annex) {
            if (at == "back")
                for (int k = 0; k < w; ++k) push(mid + (k % 2 ? -(k + 1) / 2 : k / 2), p.j1 - 1);
            else if (at == "side")
                for (int j = p.j0 + 1; j < p.j1; ++j) push(p.i0, j), push(p.i1 - 1, j);
            else if (at == "middle")
                for (int j = p.j0 + 1; j < p.j1 - 1; ++j)
                    for (int i = p.i0 + 1; i < p.i1 - 1; ++i) push(i, j);
            else if (at == "front")
                for (int i = p.i0; i < p.i1; ++i) push(i, p.j0);
        }
        for (int j = p.j1 - 1; j >= p.j0; --j)
            for (int i = p.i0; i < p.i1; ++i) push(i, j);
        return v;
    };
    // the wall a ground cell leans on (rot) or -1 (none): back first, then the sides, the partition
    auto leanOn = [&](Cell c, int z) {
        if (c.i >= 0 && c.j >= 0 && c.i < W && c.j < D) {
            if (c.j == D - 1) return 2;
            if (c.i == 0) return 3;
            if (c.i == W - 1) return 1;
            if (z == 0 && splitW && c.i == W0 - 1) return 1;
            if (z == 0 && splitW && c.i == W0) return 3;
            if (c.j == 0) return 0;
        }
        return -1;
    };
    auto thing = [&](const std::string& what, Cell c, int z, int part) {
        LocalThing t;
        t.what = what;
        t.i = c.i, t.j = c.j, t.z = z, t.part = part;
        const int lean = leanOn(c, z);
        t.u = (c.i + 0.5) * cm;
        t.v = (c.j + 0.5) * cm;
        const double off = 0.28 * cm;
        if (lean == 2) t.v += off, t.yaw = 0.0;
        else if (lean == 3) t.u -= off, t.yaw = kPi / 2;
        else if (lean == 1) t.u += off, t.yaw = -kPi / 2;
        else if (lean == 0) t.v -= off, t.yaw = kPi;
        else t.yaw = r.irange(0, 3) * kPi / 2;
        return t;
    };
    // a thing on a free cell of the list that keeps the door and every placed thing in reach
    auto place = [&](const std::string& what, const std::vector<Cell>& cells, int z, int part,
                     std::vector<LocalThing>& into) {
        for (const Cell& c : cells) {
            if (z > 0) {
                if (upTaken.count({c, z})) continue;
                upTaken.insert({c, z});
                into.push_back(thing(what, c, z, part));
                return true;
            }
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
            into.push_back(thing(what, c, z, part));
            return true;
        }
        return false;
    };
    int yardN = 0;
    auto yard = [&](const std::string& what) {
        LocalThing t;
        t.what = what;
        t.yard = true;
        t.u = (s.i0 + s.i1) * 0.5 * cm + (yardN % 2 ? 1 : -1) * ((yardN + 1) / 2) * 3.5;
        t.v = s.j1 * cm + 3.0;
        t.i = int(std::floor(t.u / cm)), t.j = s.j1 + 1;
        t.yaw = 0.0;
        ++yardN;
        s.stations.push_back(t);
    };
    // the rooms in order: the annexes (what they are for), the ground floor, the upper storey
    std::vector<int> order;
    for (int k = 0; k < int(s.parts.size()); ++k)
        if (s.parts[std::size_t(k)].at != "main" && s.parts[std::size_t(k)].at != "split" && s.parts[std::size_t(k)].at != "upper")
            order.push_back(k);
    for (int k = 0; k < int(s.parts.size()); ++k)
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    auto outerFirst = [&](std::vector<Cell>& cells, const StructurePart& p) {
        if (p.at != "canopy") return;
        // a canopy's outer cells first: the worker stands outside
        const bool left = p.i1 <= 0, right = p.i0 >= W, back = p.j0 >= D;
        std::stable_sort(cells.begin(), cells.end(), [&](Cell x, Cell y) {
            auto outer = [&](Cell c) { return left ? -c.i : right ? c.i : back ? c.j : -c.j; };
            return outer(x) > outer(y);
        });
    };
    for (int k : order) {
        const StructurePart& p = s.parts[std::size_t(k)];
        auto f = cat.furnish.find(p.room);
        if (f == cat.furnish.end()) continue;
        for (const auto& st : f->second.stations) {
            std::vector<Cell> cells = partCells(p, st.at);
            outerFirst(cells, p);
            if (!place(st.what, cells, p.z, k, s.stations)) s.notes.push_back("no place for " + st.what + " in the " + p.room);
        }
    }
    for (const auto& y : type.yard) yard(y);
    // the beds: where the household sleeps (bedrooms upstairs first, then a home)
    if (type.home && prm.people > 0) {
        std::vector<int> bedParts;
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < int(s.parts.size()); ++k) {
                const StructurePart& p = s.parts[std::size_t(k)];
                auto f = cat.furnish.find(p.room);
                if (f != cat.furnish.end() && f->second.beds && (p.z > 0) == (pass == 0)) bedParts.push_back(k);
            }
        for (int b = 0; b < prm.people && !bedParts.empty(); ++b) {
            const int k = bedParts[std::size_t(b) % bedParts.size()];
            const StructurePart& p = s.parts[std::size_t(k)];
            if (place("bed", partCells(p, "side"), p.z, k, s.beds)) continue;
            // no free cell: a bed by one that has one (a pallet beside it), in the same cell
            LocalThing t = thing("bed", {p.i0, p.j1 - 1}, p.z, k);
            std::vector<const LocalThing*> own;
            for (const auto& b2 : s.beds)
                if (b2.part == k && !b2.stacked) own.push_back(&b2);
            int extra = 0;
            for (const auto& b2 : s.beds) extra += b2.part == k && b2.stacked;
            if (!own.empty()) t = *own[std::size_t(extra) % own.size()];
            const double ox = (extra % 2 ? -1 : 1) * 0.35, oy = ((extra / 2) % 2 ? 1 : -1) * 0.3;
            t.u = std::clamp(t.u + ox, t.i * cm + 0.15, (t.i + 1) * cm - 0.15);
            t.v = std::clamp(t.v + oy, t.j * cm + 0.15, (t.j + 1) * cm - 0.15);
            t.stacked = true;
            s.beds.push_back(t);
        }
    }
    for (int k : order) {
        const StructurePart& p = s.parts[std::size_t(k)];
        auto f = cat.furnish.find(p.room);
        if (f == cat.furnish.end()) continue;
        for (const auto& dc : f->second.decor) {
            std::vector<Cell> cells = partCells(p, dc.at);
            if (type.aisle && dc.at == "middle") {   // pews either side of the aisle, front to back
                cells.clear();
                for (int j = p.j0 + 1; j < p.j1 - 1; ++j)
                    for (int i = p.i0; i < p.i1; ++i)
                        if (i != W0 / 2) cells.push_back({i, j});
            }
            place(dc.what, cells, p.z, k, s.decor);   // furniture is a nicety: it may find no room
        }
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
    auto inside = [&](int i, int j) {
        for (const auto& v : s.volumes)
            if (i >= v.i0 && i < v.i1 && j >= v.j0 && j < v.j1) return true;
        return false;
    };
    Walk walk(build, s);
    std::set<std::pair<Cell, int>> onCells;
    for (const auto* list : {&s.stations, &s.decor, &s.beds})
        for (const auto& t : *list) {
            if (t.yard) {
                if (t.j < s.j1) bad.push_back(t.what + " in the yard is inside the footprint");
                continue;
            }
            if (!inside(t.i, t.j)) bad.push_back(t.what + " stands outside the building");
            if (t.stacked) continue;
            if (!onCells.insert({{t.i, t.j}, t.z}).second) bad.push_back(t.what + " shares a cell");
            if (t.z == 0) walk.taken.insert({t.i, t.j});
        }
    const auto seen = walk.reach();
    if (!seen.count({s.doorI, 0})) bad.push_back("the door is shut");
    for (const auto& t : s.stations)
        if (!t.yard && t.z == 0 && !walk.usable(seen, {t.i, t.j})) bad.push_back(t.what + " cannot be reached");
    if (s.volumes.empty() || s.volumes[0].role != "main") bad.push_back("no main volume");
    return bad;
}

}  // namespace lr::sim
