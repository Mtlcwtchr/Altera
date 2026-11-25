#include "client2d/build2d.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lr::client2d {

using namespace lr::sim;

namespace {

// ids of a stroke's pieces in the copy of the plan (above any the world gives out)
constexpr EntityId kFakeId = EntityId(0xF0000000u);

int cellOf(double m, double cellM) { return int(std::floor(m / cellM)); }
int lineOf(double m, double cellM) { return int(std::lround(m / cellM)); }

std::string pieceFor(const BuildCatalog& cat, const std::string& what, const std::string& material) {
    // wall_log, wall_door_daub, gable_stone...; falls back to log, then to any of the category
    const std::string want = what + "_" + material;
    if (cat.find(want)) return want;
    if (cat.find(what + "_log")) return what + "_log";
    return "";
}

// The mirrored half of a gable at an end (endRot) of a slope whose eave faces eaveRot (as sim/structure_plan.cpp lays it).
int gableFor(int endRot, int eaveRot) {
    const bool half = eaveRot == 0 || eaveRot == 1;
    switch (endRot) {
    case 3: return half ? 3 + 4 : 3;
    case 1: return half ? 1 : 1 + 4;
    case 0: return half ? 0 + 4 : 0;
    default: return half ? 2 : 2 + 4;
    }
}

// The piece of the plan in a slot (kNone: free).
EntityId inSlot(const BuildCatalog& cat, const PieceMap& pieces, const SlotKey& k) {
    for (const auto& [id, p] : pieces) {
        const BuildPieceDef* d = cat.find(p.kind);
        if (d && slotOf(*d, p.x, p.y, p.z, p.rot) == k) return id;
    }
    return kNone;
}

// A thing standing (or a planned one) where furniture would go.
bool thingAt(const World& w, const BuildCatalog& cat, const BuildPieceDef& d, const BuildPiece& p) {
    const Vec2 at = pieceCentre(cat, d, p);
    const double r = 0.5 * cat.cellM / std::max(1, d.sub);
    for (const auto& [id, o] : w.objects())
        if (o.type != "build_site" && o.storey == p.z && std::abs(o.pos.x - at.x) < r && std::abs(o.pos.y - at.y) < r)
            return true;
    return false;
}

const char* kOrder[] = {"foundation", "floor", "pillar", "wall", "gable", "roof", "furniture"};
int orderOf(const std::string& cat) {
    for (int i = 0; i < 7; ++i)
        if (cat == kOrder[i]) return i;
    return 7;
}

std::string ownerTag(const World& w, EntityId owner, const Character* viewer) {
    if (viewer && owner == viewer->status.family) return "mine";
    if (viewer && owner == viewer->status.settlement && owner != kNone) return "settlement";
    return w.settlements().count(owner) ? "common" : "other";
}

// One stroke checked piece by piece over a copy of the plan (each one that
// can go stays in the copy, so the walls of a new room rest on its footings).
struct Checked {
    StrokePiece sp;
    std::string why;           // "" = can go
    EntityId replaces = kNone; // a door or window takes a wall's place
    EntityId removes = kNone;  // the remove tool: this piece
};

std::vector<Checked> check(const World& w, const Planner& who, const Stroke& s) {
    const BuildCatalog& cat = w.content().build;
    std::vector<Checked> out;
    const Character* actor = w.character(who.actor);
    const EntityId owner = who.owner(w);
    if (s.tool == "remove") {
        const double x0 = std::min(s.ax, s.bx), x1 = std::max(s.ax, s.bx), y0 = std::min(s.ay, s.by),
                     y1 = std::max(s.ay, s.by);
        for (const auto& [id, p] : w.pieces()) {
            const BuildPieceDef* d = cat.find(p.kind);
            if (!d || p.z != s.z) continue;
            const Vec2 c = pieceCentre(cat, *d, p);
            if (c.x < x0 - 0.5 || c.x > x1 + 0.5 || c.y < y0 - 0.5 || c.y > y1 + 0.5) continue;
            Checked ch;
            ch.sp = {p.kind, p.x, p.y, p.z, p.rot};
            ch.removes = id;
            const bool mine = actor && p.family == actor->status.family;
            const bool common = actor && p.family != kNone && p.family == actor->status.settlement;
            if (!mine && !(common && (!p.built || mandateHolder(w, p.family) == actor->id))) ch.why = "not_own";
            out.push_back(ch);
        }
        return out;
    }
    PieceMap copy = w.pieces();
    EntityId fake = kFakeId;
    // what holds first: the storeys from the ground up, footings, walls, gables, then slopes
    std::vector<StrokePiece> stroke = expandStroke(cat, s);
    std::stable_sort(stroke.begin(), stroke.end(), [&](const StrokePiece& a, const StrokePiece& b) {
        if (a.z != b.z) return a.z < b.z;
        const BuildPieceDef* da = cat.find(a.kind);
        const BuildPieceDef* db = cat.find(b.kind);
        return orderOf(da ? da->category : "") < orderOf(db ? db->category : "");
    });
    for (const StrokePiece& sp : stroke) {
        Checked ch;
        ch.sp = sp;
        BuildPiece p;
        p.id = ++fake;
        p.kind = sp.kind;
        p.x = sp.x, p.y = sp.y, p.z = sp.z, p.rot = sp.rot;
        p.family = owner;
        const BuildPieceDef* d = cat.find(sp.kind);
        if (owner == kNone) ch.why = who.forSettlement ? "no_settlement" : "no_family";
        else if (!d) ch.why = "unknown_kind";
        else {
            ch.why = checkPlacement(cat, copy, p);
            // a door or a window goes in place of a wall of one's own
            if (ch.why == "occupied" && d->slot == "edge" && d->category == "wall" &&
                (s.tool == "door" || s.tool == "window")) {
                const EntityId was = inSlot(cat, copy, slotOf(*d, p.x, p.y, p.z, p.rot));
                const BuildPiece* wp = was != kNone ? &copy.at(was) : nullptr;
                const BuildPieceDef* wd = wp ? cat.find(wp->kind) : nullptr;
                if (wp && wd && wd->category == "wall" && wp->family == owner && wp->kind != p.kind && was < kFakeId) {
                    PieceMap without = copy;
                    without.erase(was);
                    ch.why = checkPlacement(cat, without, p);
                    if (ch.why.empty()) {
                        ch.replaces = was;
                        copy.erase(was);
                    }
                }
            }
            if (ch.why.empty() && d->slot == "thing" && thingAt(w, cat, *d, p)) ch.why = "occupied";
        }
        if (ch.why.empty()) copy[p.id] = p;
        out.push_back(ch);
    }
    return out;
}

Json answer(const World& w, const ViewText& t, const std::vector<Checked>& list, const std::string& tool) {
    const BuildCatalog& cat = w.content().build;
    Json pieces = Json::array();
    std::map<std::string, int> cost;
    int ok = 0, bad = 0;
    std::map<std::string, int> whys;
    for (const Checked& c : list) {
        const BuildPieceDef* d = cat.find(c.sp.kind);
        BuildPiece p;
        p.kind = c.sp.kind;
        p.x = c.sp.x, p.y = c.sp.y, p.z = c.sp.z, p.rot = c.sp.rot;
        const Vec2 at = d ? pieceCentre(cat, *d, p) : Vec2{};
        Json j = {{"kind", c.sp.kind}, {"x", c.sp.x}, {"y", c.sp.y}, {"z", c.sp.z}, {"rot", c.sp.rot},
                  {"cat", d ? d->category : ""}, {"slot", d ? d->slot : ""}, {"cx", at.x}, {"cy", at.y},
                  {"ok", c.why.empty()}};
        if (!c.why.empty()) {
            j["why"] = c.why;
            ++whys[c.why];
            ++bad;
        } else {
            ++ok;
            if (d && tool != "remove")
                for (const auto& [item, n] : d->cost) cost[item] += n;
        }
        if (c.replaces != kNone) j["replaces"] = c.replaces;
        if (c.removes != kNone) j["piece"] = c.removes;
        pieces.push_back(std::move(j));
    }
    Json costJ = Json::array();
    for (const auto& [item, n] : cost) costJ.push_back({{"item", item}, {"name", t.name("item", item)}, {"n", n}});
    std::string topWhy;
    int topN = 0;
    for (const auto& [k, n] : whys)
        if (n > topN) topWhy = k, topN = n;
    return {{"tool", tool}, {"pieces", pieces}, {"ok", ok}, {"bad", bad}, {"why", topWhy}, {"cost", costJ}};
}

void add(std::vector<StrokePiece>& v, const std::string& kind, int x, int y, int z, int rot) {
    if (!kind.empty()) v.push_back({kind, x, y, z, rot});
}

} // namespace

Stroke Stroke::fromJson(const Json& j) {
    Stroke s;
    s.tool = j.value("tool", s.tool);
    const Json a = j.value("a", Json::array({0, 0})), b = j.value("b", a);
    s.ax = a.at(0).get<double>(), s.ay = a.at(1).get<double>();
    s.bx = b.at(0).get<double>(), s.by = b.at(1).get<double>();
    s.z = std::clamp(j.value("z", 0), 0, 8);
    s.rot = ((j.value("rot", 0) % 4) + 4) % 4;
    s.side = ((j.value("side", 0) % 4) + 4) % 4;
    s.material = j.value("material", s.material);
    s.roof = j.value("roof", s.roof);
    s.thing = j.value("thing", s.thing);
    s.forSettlement = j.value("for", std::string()) == "settlement";
    s.roofed = j.value("roofed", true);
    return s;
}

std::vector<StrokePiece> roofPieces(const BuildCatalog& cat, int x0, int y0, int x1, int y1, int z,
                                    const std::string& material, const std::string& roof) {
    std::vector<StrokePiece> v;
    const int W = x1 - x0, D = y1 - y0;
    if (W <= 0 || D <= 0 || !cat.find(roof)) return v;
    const std::string wall = pieceFor(cat, "wall", material), gable = pieceFor(cat, "gable", material);
    // the ridge along the longer side (eaves on the long walls)
    const bool alongX = W >= D;
    const int rows = alongX ? D : W, len = alongX ? W : D;
    for (int row = 0; row < rows; ++row) {
        const int k = std::min(row, rows - 1 - row);
        const bool lower = row < rows / 2 || (rows % 2 && row == rows / 2);
        const int rot = alongX ? (lower ? 0 : 2) : (lower ? 3 : 1);
        for (int t = 0; t < len; ++t) add(v, roof, x0 + (alongX ? t : row), y0 + (alongX ? row : t), z + k, rot);
    }
    // the ends: walls up to the slope's storey, a gable on top
    for (int row = 0; row < rows; ++row) {
        const int k = std::min(row, rows - 1 - row);
        const bool lower = row < rows / 2 || (rows % 2 && row == rows / 2);
        const int rot = alongX ? (lower ? 0 : 2) : (lower ? 3 : 1);
        for (int end = 0; end < 2; ++end) {
            const int i = alongX ? (end ? W - 1 : 0) : row, j = alongX ? row : (end ? D - 1 : 0);
            const int endRot = alongX ? (end ? 1 : 3) : (end ? 2 : 0);
            for (int zz = 0; zz < k; ++zz) add(v, wall, x0 + i, y0 + j, z + zz, endRot);
            add(v, gable, x0 + i, y0 + j, z + k, gableFor(endRot, rot));
        }
    }
    return v;
}

std::vector<StrokePiece> roomPieces(const BuildCatalog& cat, int x0, int y0, int x1, int y1, int z,
                                    const std::string& material, int doorSide, bool roofed, const std::string& roof) {
    std::vector<StrokePiece> v;
    const int W = x1 - x0, D = y1 - y0;
    if (W <= 0 || D <= 0) return v;
    const std::string base = z == 0 ? "foundation_stone" : "floor_plank";
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) add(v, base, x, y, z, 0);
    const std::string wall = pieceFor(cat, "wall", material), door = pieceFor(cat, "wall_door", material),
                      window = pieceFor(cat, "wall_window", material);
    // the door in the middle of its side; a window in the middle of each other side three cells long or more
    auto sideLen = [&](int side) { return side == 0 || side == 2 ? W : D; };
    for (int side = 0; side < 4; ++side) {
        const int n = sideLen(side), mid = (n - 1) / 2;
        for (int t = 0; t < n; ++t) {
            int x = x0, y = y0;
            if (side == 0) x = x0 + t, y = y0;
            else if (side == 2) x = x0 + t, y = y1 - 1;
            else if (side == 1) x = x1 - 1, y = y0 + t;
            else x = x0, y = y0 + t;
            std::string kind = wall;
            if (side == doorSide && t == mid) kind = door;
            else if (side != doorSide && n >= 3 && t == mid) kind = window;
            add(v, kind, x, y, z, side);
        }
    }
    if (roofed) {
        const auto r = roofPieces(cat, x0, y0, x1, y1, z + 1, material, roof);
        v.insert(v.end(), r.begin(), r.end());
    }
    return v;
}

std::vector<StrokePiece> expandStroke(const BuildCatalog& cat, const Stroke& s) {
    std::vector<StrokePiece> v;
    const double c = cat.cellM;
    // cells of the rectangle a..b (both ends inside it)
    const int cx0 = cellOf(std::min(s.ax, s.bx), c), cx1 = cellOf(std::max(s.ax, s.bx), c) + 1;
    const int cy0 = cellOf(std::min(s.ay, s.by), c), cy1 = cellOf(std::max(s.ay, s.by), c) + 1;
    const int area = (cx1 - cx0) * (cy1 - cy0);
    const bool point = s.tool == "door" || s.tool == "window" || s.tool == "gable" || s.tool == "pillar" || s.tool == "thing";
    if (!point && area > 40 * 40) return v;   // a stroke over a whole valley is a slip of the hand
    if (s.tool == "room") return roomPieces(cat, cx0, cy0, cx1, cy1, s.z, s.material, s.side, s.roofed, s.roof);
    if (s.tool == "roof") return roofPieces(cat, cx0, cy0, cx1, cy1, s.z + 1, s.material, s.roof);
    if (s.tool == "floor") {
        const std::string base = s.z == 0 ? "foundation_stone" : "floor_plank";
        for (int y = cy0; y < cy1; ++y)
            for (int x = cx0; x < cx1; ++x) add(v, base, x, y, s.z, 0);
        return v;
    }
    if (s.tool == "wall") {
        // along the cell line nearest the start, the way the stroke goes farther
        const bool alongX = std::abs(s.bx - s.ax) >= std::abs(s.by - s.ay);
        const std::string wall = pieceFor(cat, "wall", s.material);
        if (alongX) {
            const int line = lineOf(s.ay, c);
            for (int x = cx0; x < cx1; ++x) add(v, wall, x, line, s.z, 0);
        } else {
            const int line = lineOf(s.ax, c);
            for (int y = cy0; y < cy1; ++y) add(v, wall, line, y, s.z, 3);
        }
        return v;
    }
    if (s.tool == "door" || s.tool == "window" || s.tool == "gable") {
        // the cell edge nearest the point
        const double fx = s.ax / c - std::floor(s.ax / c), fy = s.ay / c - std::floor(s.ay / c);
        const int x = cellOf(s.ax, c), y = cellOf(s.ay, c);
        const double dl = fx, dr = 1 - fx, dt = fy, db = 1 - fy;
        const double m = std::min({dl, dr, dt, db});
        const int rot = m == dt ? 0 : m == dr ? 1 : m == db ? 2 : 3;
        const std::string kind = s.tool == "gable" ? pieceFor(cat, "gable", s.material)
                                                   : pieceFor(cat, s.tool == "door" ? "wall_door" : "wall_window", s.material);
        add(v, kind, x, y, s.z, s.tool == "gable" ? s.rot : rot);
        return v;
    }
    if (s.tool == "pillar") {
        const int gx = lineOf(s.ax, c), gy = lineOf(s.ay, c);
        add(v, "pillar_log", gx, gy, s.z, 0);   // rot 0: the corner at the cell's own grid point
        return v;
    }
    if (s.tool == "thing") {
        const BuildPieceDef* d = cat.find(s.thing);
        if (!d || d->slot != "thing") return v;
        const double tm = c / std::max(1, d->sub);
        add(v, d->id, int(std::floor(s.ax / tm)), int(std::floor(s.ay / tm)), s.z, s.rot);
        return v;
    }
    return v;
}

EntityId Planner::owner(const World& w) const {
    const Character* c = w.character(actor);
    if (!c) return kNone;
    return forSettlement ? c->status.settlement : c->status.family;
}

Json previewJson(const World& w, const ViewText& t, const Planner& who, const Stroke& s) {
    return answer(w, t, check(w, who, s), s.tool);
}

Json placeJson(World& w, const ViewText& t, const Planner& who, const Stroke& s) {
    std::vector<Checked> list = check(w, who, s);
    // supports first: what holds goes into the plan before what it holds
    std::stable_sort(list.begin(), list.end(), [&](const Checked& a, const Checked& b) {
        const BuildPieceDef* da = w.content().build.find(a.sp.kind);
        const BuildPieceDef* db = w.content().build.find(b.sp.kind);
        const int za = a.sp.z, zb = b.sp.z;
        if (s.tool == "remove") return za > zb;   // strike from the top down
        if (za != zb) return za < zb;
        return orderOf(da ? da->category : "") < orderOf(db ? db->category : "");
    });
    int placed = 0;
    for (Checked& c : list) {
        if (!c.why.empty()) continue;
        if (c.removes != kNone || c.replaces != kNone) {
            Command rm;
            rm.source = "player:1";
            rm.type = "remove_piece";
            rm.args = {{"id", who.actor}, {"piece", c.removes != kNone ? c.removes : c.replaces}, {"player", 1}};
            const Command r = w.applyNow(rm);
            if (r.result != "ok" && !r.result.empty()) {
                c.why = r.result.rfind("rejected:", 0) == 0 ? r.result.substr(9) : r.result;
                continue;
            }
            if (c.removes != kNone) {
                ++placed;
                continue;
            }
        }
        BuildPiece probe;
        probe.kind = c.sp.kind;
        probe.x = c.sp.x, probe.y = c.sp.y, probe.z = c.sp.z, probe.rot = c.sp.rot;
        double h = 0.0;
        joinHeight(w.content().build, w.pieces(), probe, h);
        Command cmd;
        cmd.source = "player:1";
        cmd.type = "place_piece";
        cmd.args = {{"id", who.actor}, {"kind", c.sp.kind}, {"x", c.sp.x}, {"y", c.sp.y}, {"z", c.sp.z},
                    {"rot", c.sp.rot},  {"h", h},           {"player", 1}};
        if (who.forSettlement) cmd.args["for"] = "settlement";
        const Command r = w.applyNow(cmd);
        if (r.result == "ok" || r.result.empty()) ++placed;
        else c.why = r.result.rfind("rejected:", 0) == 0 ? r.result.substr(9) : r.result;
    }
    Json j = answer(w, t, list, s.tool);
    j["placed"] = placed;
    return j;
}

Json catalogJson(const World& w, const ViewText& t) {
    const BuildCatalog& cat = w.content().build;
    Json pieces = Json::array();
    for (const auto& d : cat.pieces) {
        Json cost = Json::array();
        for (const auto& [item, n] : d.cost) cost.push_back({{"item", item}, {"name", t.name("item", item)}, {"n", n}});
        std::string name = d.slot == "thing" ? t.name("object", d.makes) : "";
        if (name.empty() || name == d.makes) {
            const std::string k = "hud.build.piece." + d.id;
            name = t.loc && t.loc->has(t.lang, k) ? t.loc->text(t.lang, k) : (d.makes.empty() ? d.id : d.makes);
        }
        pieces.push_back({{"id", d.id}, {"slot", d.slot}, {"cat", d.category}, {"material", d.material},
                          {"makes", d.makes}, {"name", name}, {"cost", cost}, {"work", d.work}, {"door", d.door}});
    }
    // which rooms a thing makes (the rules' needs): the palette groups furniture by them
    Json rooms = Json::array();
    for (const auto& r : w.content().rooms.rules) {
        Json needs = Json::array();
        for (const auto& n : r.needs) needs.push_back({{"any", n.any}, {"n", n.n}});
        rooms.push_back({{"id", r.id}, {"name", t.name("room", r.id)}, {"needs", needs}, {"closed", r.enclosure == "closed"}});
    }
    return {{"cell_m", cat.cellM}, {"storey_m", cat.storeyM}, {"thing_m", cat.thingM}, {"pieces", pieces}, {"rooms", rooms}};
}

Json planJson(const World& w, const ViewText& t, EntityId viewer, int x0, int y0, int wM, int hM) {
    const BuildCatalog& cat = w.content().build;
    const Character* me = w.character(viewer);
    Json out = Json::array();
    for (const auto& [id, p] : w.pieces()) {
        const BuildPieceDef* d = cat.find(p.kind);
        if (!d) continue;
        const Vec2 c = pieceCentre(cat, *d, p);
        if (c.x < x0 - 2 || c.y < y0 - 2 || c.x > x0 + wM + 2 || c.y > y0 + hM + 2) continue;
        // the village's own houses stand from the start: only what is planned, and furniture, goes out
        if (p.built && d->slot != "thing") continue;
        Json j = {{"id", id}, {"kind", p.kind}, {"x", p.x}, {"y", p.y}, {"z", p.z}, {"rot", p.rot}, {"cat", d->category},
                  {"slot", d->slot}, {"cx", c.x}, {"cy", c.y}, {"built", p.built}, {"who", ownerTag(w, p.family, me)},
                  {"ready", p.builtStability >= cat.minStability}, {"work", p.work}, {"of", d->work}};
        if (!d->makes.empty()) j["makes"] = d->makes;
        Json lack = Json::array();
        for (const auto& [item, n] : missingOf(w, p)) lack.push_back({{"item", item}, {"name", t.name("item", item)}, {"n", n}});
        j["lack"] = lack;
        out.push_back(std::move(j));
    }
    return {{"pieces", out}};
}

Json colonyJson(const World& w, const ViewText& t, EntityId viewer) {
    const Character* me = w.character(viewer);
    EntityId sid = me ? me->status.settlement : kNone;
    if (sid == kNone)
        for (const auto& [id, s] : w.settlements())
            if (s.full()) {
                sid = id;
                break;
            }
    auto it = w.settlements().find(sid);
    if (it == w.settlements().end()) return nullptr;
    const Settlement& st = it->second;
    const BuildCatalog& cat = w.content().build;
    Json j = {{"id", sid}, {"name", st.name}, {"x", st.center.x}, {"y", st.center.y}, {"chief", mandateHolder(w, sid)},
              {"mode", st.mode}, {"me", viewer}};
    // households: members, the rooms of their buildings, their sites
    Json fams = Json::array();
    int people = 0, grown = 0, kids = 0;
    std::set<EntityId> famIds(st.families.begin(), st.families.end());
    for (EntityId fid : st.families) {
        auto f = w.families().find(fid);
        if (f == w.families().end()) continue;
        Json members = Json::array();
        for (EntityId m : f->second.members) {
            const Character* c = w.character(m);
            if (!c || !c->body.alive || c->isAnimal()) continue;
            members.push_back(m);
            ++people;
            (c->body.stage() == "child" ? kids : grown)++;
        }
        int planned = 0, ready = 0;
        for (const auto& [id, p] : w.pieces())
            if (p.family == fid && !p.built) {
                ++planned;
                ready += p.builtStability >= cat.minStability && missingOf(w, p).empty();
            }
        fams.push_back({{"id", fid}, {"name", f->second.name}, {"members", members}, {"planned", planned}, {"ready", ready}});
    }
    j["families"] = fams;
    j["people"] = people;
    j["grown"] = grown;
    j["kids"] = kids;
    // rooms and buildings by type (of the settlement's households and its common ones)
    std::map<std::string, int> roomN, buildingN;
    const double r2 = 300.0 * 300.0;
    for (const Room& r : w.rooms()) {
        const double cx = (r.x0 + r.x1) * 0.5 * cat.cellM - st.center.x, cy = (r.y0 + r.y1) * 0.5 * cat.cellM - st.center.y;
        if (cx * cx + cy * cy <= r2) ++roomN[r.type];
    }
    for (const RoomBuilding& b : w.roomBuildings()) {
        const double cx = (b.x0 + b.x1) * 0.5 * cat.cellM - st.center.x, cy = (b.y0 + b.y1) * 0.5 * cat.cellM - st.center.y;
        if (cx * cx + cy * cy <= r2) ++buildingN[b.purpose];
    }
    Json rooms = Json::array(), buildings = Json::array();
    for (const auto& [k, n] : roomN) rooms.push_back({{"type", k}, {"name", t.name("room", k)}, {"n", n}});
    for (const auto& [k, n] : buildingN) buildings.push_back({{"type", k}, {"name", t.name("room", k)}, {"n", n}});
    j["rooms"] = rooms;
    j["buildings"] = buildings;
    // the plan's sites: whose, how many, what they lack in all
    Json sites = Json::array();
    std::map<std::string, int> lackAll;
    int planned = 0, readyAll = 0;
    for (const auto& [id, p] : w.pieces()) {
        if (p.built || (p.family != sid && !famIds.count(p.family))) continue;
        const BuildPieceDef* d = cat.find(p.kind);
        if (!d) continue;
        ++planned;
        const auto lack = missingOf(w, p);
        const bool rdy = p.builtStability >= cat.minStability && lack.empty();
        readyAll += rdy;
        for (const auto& [k, n] : lack) lackAll[k] += n;
        if (sites.size() < 400) {
            const Vec2 c = pieceCentre(cat, *d, p);
            Json l = Json::array();
            for (const auto& [k, n] : lack) l.push_back({{"name", t.name("item", k)}, {"n", n}});
            sites.push_back({{"id", id}, {"kind", p.kind}, {"cat", d->category}, {"who", ownerTag(w, p.family, me)},
                             {"fam", p.family}, {"cx", c.x}, {"cy", c.y}, {"z", p.z}, {"ready", rdy},
                             {"held", p.builtStability >= cat.minStability}, {"work", p.work}, {"of", d->work}, {"lack", l}});
        }
    }
    Json lackJ = Json::array();
    for (const auto& [k, n] : lackAll) lackJ.push_back({{"item", k}, {"name", t.name("item", k)}, {"n", n}});
    j["sites"] = sites;
    j["planned"] = planned;
    j["ready"] = readyAll;
    j["lack"] = lackJ;
    // the offices: who holds which right; the viewer's own
    Json offices = Json::array();
    for (const std::string& right : w.content().config.rights) {
        Json holders = Json::array();
        for (const auto& [id, c] : w.characters())
            if (!c.isAnimal() && c.body.alive && c.status.settlement == sid && c.status.rights.count(right)) holders.push_back(id);
        offices.push_back({{"right", right}, {"holders", holders}});
    }
    j["offices"] = offices;
    Json mine = Json::array();
    if (me)
        for (const auto& r : me->status.rights) mine.push_back(r);
    j["my_rights"] = mine;
    j["free_will"] = me ? me->controller.freeWill : false;
    return j;
}

}  // namespace lr::client2d
