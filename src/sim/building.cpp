// Building from modules and the colony's mandate (sim/building.hpp).

#include "sim/building.hpp"
#include "sim/rooms.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace altera::sim {

namespace {

constexpr int kMaxStorey = 8;

struct Support {
    SlotKey key;
    std::vector<const char*> cats;
};

bool catIs(const std::string& c, const std::vector<const char*>& list) {
    for (const char* s : list)
        if (c == s) return true;
    return false;
}

int dirX(int rot) { return rot % 4 == 1 ? 1 : rot % 4 == 3 ? -1 : 0; }
int dirY(int rot) { return rot % 4 == 0 ? -1 : rot % 4 == 2 ? 1 : 0; }

// What may hold a piece (the slots and the kinds of piece in them).
std::vector<Support> supportsOf(const BuildPieceDef& d, const BuildPiece& p) {
    std::vector<Support> s;
    const int x = p.x, y = p.y, z = p.z;
    const std::vector<const char*> floors = {"foundation", "floor"}, walls = {"wall", "gable"}, wallsOnly = {"wall"},
                                   pillars = {"pillar"}, roofs = {"roof"}, gables = {"gable"}, floorOnly = {"floor"};
    auto cellEdges = [&](int zz, const std::vector<const char*>& cats) {
        s.push_back({{'h', x, y, zz}, cats});
        s.push_back({{'h', x, y + 1, zz}, cats});
        s.push_back({{'v', x, y, zz}, cats});
        s.push_back({{'v', x + 1, y, zz}, cats});
    };
    auto cellCorners = [&](int zz) {
        for (int dx = 0; dx < 2; ++dx)
            for (int dy = 0; dy < 2; ++dy) s.push_back({{'p', x + dx, y + dy, zz}, pillars});
    };
    if (d.category == "furniture") {
        // a thing stands on the floor (footing, planks) of the cell it is in
        const int n = std::max(1, d.sub);
        auto fdiv = [n](int v) { return v >= 0 ? v / n : -((-v + n - 1) / n); };
        s.push_back({{'f', fdiv(x), fdiv(y), z}, floors});
    } else if (d.category == "floor") {
        cellEdges(z - 1, walls);
        cellCorners(z - 1);
        s.push_back({{'f', x + 1, y, z}, floorOnly});
        s.push_back({{'f', x - 1, y, z}, floorOnly});
        s.push_back({{'f', x, y + 1, z}, floorOnly});
        s.push_back({{'f', x, y - 1, z}, floorOnly});
    } else if (d.category == "wall" || d.category == "gable") {
        const SlotKey e = slotOf(d, x, y, z, p.rot);
        s.push_back({{'f', e.a, e.b, z}, floors});
        if (e.layer == 'h') {
            s.push_back({{'f', e.a, e.b - 1, z}, floors});
            s.push_back({{'p', e.a, e.b, z}, pillars});
            s.push_back({{'p', e.a + 1, e.b, z}, pillars});
        } else {
            s.push_back({{'f', e.a - 1, e.b, z}, floors});
            s.push_back({{'p', e.a, e.b, z}, pillars});
            s.push_back({{'p', e.a, e.b + 1, z}, pillars});
        }
        s.push_back({{e.layer, e.a, e.b, z - 1}, walls});
    } else if (d.category == "pillar") {
        const SlotKey c = slotOf(d, x, y, z, p.rot);
        for (int dx = -1; dx < 1; ++dx)
            for (int dy = -1; dy < 1; ++dy) s.push_back({{'f', c.a + dx, c.b + dy, z}, floors});
        s.push_back({{'p', c.a, c.b, z - 1}, pillars});
        s.push_back({{'h', c.a - 1, c.b, z - 1}, wallsOnly});
        s.push_back({{'h', c.a, c.b, z - 1}, wallsOnly});
        s.push_back({{'v', c.a, c.b - 1, z - 1}, wallsOnly});
        s.push_back({{'v', c.a, c.b, z - 1}, wallsOnly});
    } else if (d.category == "roof") {
        cellEdges(z - 1, walls);
        cellEdges(z, gables);
        cellCorners(z - 1);
        s.push_back({{'r', x + 1, y, z}, roofs});
        s.push_back({{'r', x - 1, y, z}, roofs});
        s.push_back({{'r', x, y + 1, z}, roofs});
        s.push_back({{'r', x, y - 1, z}, roofs});
        // a steep roof over a wide house: this slope rests on the one below its eave
        s.push_back({{'r', x + dirX(p.rot), y + dirY(p.rot), z - 1}, roofs});
    }
    return s;
}

std::map<SlotKey, EntityId> indexOf(const BuildCatalog& cat, const PieceMap& pieces, bool builtOnly) {
    std::map<SlotKey, EntityId> idx;
    for (const auto& [id, p] : pieces) {
        if (builtOnly && !p.built) continue;
        const BuildPieceDef* d = cat.find(p.kind);
        if (d) idx[slotOf(*d, p.x, p.y, p.z, p.rot)] = id;
    }
    return idx;
}

// Best support of a piece over an index, reading the stabilities `stab`.
double supportOf(const BuildCatalog& cat, const PieceMap& pieces, const std::map<SlotKey, EntityId>& idx,
                 const std::map<EntityId, double>& stab, const BuildPieceDef& d, const BuildPiece& p) {
    if (d.category == "foundation") return p.z == 0 ? 1.0 : 0.0;
    if (d.category == "furniture" && p.z == 0) return 1.0;   // on the ground or a footing
    double best = 0.0;
    for (const Support& s : supportsOf(d, p)) {
        auto it = idx.find(s.key);
        if (it == idx.end() || it->second == p.id) continue;
        const BuildPiece& o = pieces.at(it->second);
        const BuildPieceDef* od = cat.find(o.kind);
        if (!od || !catIs(od->category, s.cats)) continue;
        auto st = stab.find(o.id);
        if (st != stab.end()) best = std::max(best, st->second);
    }
    return best * d.carry;
}

std::map<EntityId, double> relax(const BuildCatalog& cat, const PieceMap& pieces, bool builtOnly) {
    const auto idx = indexOf(cat, pieces, builtOnly);
    std::map<EntityId, double> stab;
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false;
        for (const auto& [id, p] : pieces) {
            if (builtOnly && !p.built) continue;
            const BuildPieceDef* d = cat.find(p.kind);
            if (!d) continue;
            const double s = d->category == "foundation" ? (p.z == 0 ? 1.0 : 0.0) : supportOf(cat, pieces, idx, stab, *d, p);
            double& cur = stab[id];
            if (s > cur + 1e-9) {
                cur = s;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return stab;
}

Family* familyOfId(World& w, EntityId id) {
    auto it = w.families().find(id);
    return it == w.families().end() ? nullptr : &it->second;
}

const BuildPiece* pieceOfSite(const World& w, EntityId site) {
    for (const auto& [id, p] : w.pieces())
        if (p.site == site) return &p;
    return nullptr;
}
BuildPiece* pieceOfSite(World& w, EntityId site) {
    for (auto& [id, p] : w.pieces())
        if (p.site == site) return &p;
    return nullptr;
}

int missingTotal(const WorldObject& site) {
    int n = 0;
    for (const auto& [k, v] : site.stock) n += std::max(0, v);
    return n;
}

bool ready(const World& w, const BuildPiece& p) {
    if (p.built) return false;
    const WorldObject* site = w.object(p.site);
    if (!site || missingTotal(*site) > 0) return false;
    return p.builtStability >= w.content().build.minStability;
}

// Pieces that lost their hold fall (planned ones leave the plan); the site
// objects of fallen planned pieces go with them. Returns how many built fell.
int collapse(World& w) {
    int fell = 0;
    for (int guard = 0; guard < 16; ++guard) {
        computeStability(w.content().build, w.pieces());
        std::vector<EntityId> gone;
        for (const auto& [id, p] : w.pieces())
            if (p.stability < w.content().build.minStability) gone.push_back(id);
        if (gone.empty()) break;
        for (EntityId id : gone) {
            const BuildPiece& p = w.pieces().at(id);
            if (p.built) ++fell;
            if (p.site != kNone) w.removeObject(p.site);
            w.pieces().erase(id);
        }
    }
    return fell;
}

// A piece of furniture can not go where a thing already stands (of its storey, half a metre round).
std::string thingBlocked(const World& w, const BuildCatalog& cat, const BuildPiece& p) {
    const BuildPieceDef* d = cat.find(p.kind);
    if (!d || d->slot != "thing") return "";
    const Vec2 at = pieceCentre(cat, *d, p);
    const double r = 0.5 * cat.cellM / std::max(1, d->sub);
    for (const auto& [id, o] : w.objects())
        if (o.type != "build_site" && o.storey == p.z && std::abs(o.pos.x - at.x) < r && std::abs(o.pos.y - at.y) < r)
            return "occupied";
    return "";
}

bool sameSettlement(const Character& a, const Character& b) {
    return a.status.settlement != kNone && a.status.settlement == b.status.settlement;
}

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "place_piece" && t != "remove_piece" && t != "grant_right" && t != "revoke_right") return false;
    Character* actor = w.character(cmd.args.at("id").get<EntityId>());
    if (!actor || !actor->body.alive || actor->isAnimal()) {
        cmd.result = "rejected:no_such_character";
        return true;
    }
    const BuildCatalog& cat = w.content().build;
    if (t == "place_piece") {
        // 24.11: planning is free and without a limit, for one's household or
        // (for: settlement) for the settlement in common; only raising a piece
        // takes materials. A common piece belongs to the settlement (its id
        // stands for the owner), and every grown member may work on it.
        const bool common = cmd.args.value("for", std::string()) == "settlement";
        const EntityId owner = common ? actor->status.settlement : actor->status.family;
        if (owner == kNone) {
            cmd.result = common ? "rejected:no_settlement" : "rejected:no_family";
            return true;
        }
        if (w.piecesDirty) refreshPieces(w);
        BuildPiece p;
        p.kind = cmd.args.at("kind").get<std::string>();
        p.x = cmd.args.at("x").get<int>();
        p.y = cmd.args.at("y").get<int>();
        p.z = cmd.args.value("z", 0);
        p.rot = cmd.args.value("rot", 0);
        p.h = cmd.args.value("h", 0.0);
        p.family = owner;
        std::string why = checkPlacement(cat, w.pieces(), p);
        if (why.empty()) why = thingBlocked(w, cat, p);
        if (!why.empty()) {
            cmd.result = "rejected:" + why;
            return true;
        }
        const BuildPieceDef& d = *cat.find(p.kind);
        p.id = w.newId();
        WorldObject site;
        site.type = "build_site";
        site.pos = pieceCentre(cat, d, p);
        site.ownerFamily = p.family;
        site.storey = p.z;
        for (const auto& [item, n] : d.cost) site.stock[item] = n;
        p.site = w.addObject(site).id;
        if (Family* f = familyOfId(w, p.family))
            for (EntityId m : f->members)
                if (Character* mc = w.character(m)) mc->knownObjects.insert(p.site);
        if (common)
            for (auto& [cid, mc] : w.characters())
                if (!mc.isAnimal() && mc.status.settlement == owner) mc.knownObjects.insert(p.site);
        w.pieces()[p.id] = p;
        refreshPieces(w);
        return true;
    }
    if (t == "remove_piece") {
        auto it = w.pieces().find(cmd.args.at("piece").get<EntityId>());
        if (it == w.pieces().end()) {
            cmd.result = "rejected:no_such_piece";
            return true;
        }
        // one's household's pieces; the settlement's: a planned one by any
        // member, a standing one by the holder of the mandate
        const BuildPiece& was = it->second;
        const bool common = was.family != kNone && was.family == actor->status.settlement;
        const bool mayCommon = common && (!was.built || mandateHolder(w, was.family) == actor->id);
        if (was.family != actor->status.family && !mayCommon) {
            cmd.result = "rejected:not_own";
            return true;
        }
        BuildPiece p = it->second;
        // What was already brought goes back to the household's store (of the one who strikes a common piece).
        if (!p.built) {
            const BuildPieceDef* d = cat.find(p.kind);
            const WorldObject* site = w.object(p.site);
            Family* f = familyOfId(w, common ? actor->status.family : p.family);
            WorldObject* store = f ? w.object(f->storage) : nullptr;
            if (d && site && store)
                for (const auto& [item, n] : d->cost) {
                    auto s = site->stock.find(item);
                    const int brought = n - (s == site->stock.end() ? 0 : s->second);
                    if (brought > 0) store->stock[item] += brought;
                }
            w.removeObject(p.site);
        }
        w.pieces().erase(it);
        const int fell = collapse(w);
        if (fell > 0) w.chronicle("collapse", {actor->id}, "chronicle.build_collapse", {{"who", actor->id}, {"n", fell}});
        refreshPieces(w);
        return true;
    }
    // grant_right / revoke_right: the holder of `appoint` hands an office on
    // or takes it back; the change is at once the colony's law (a trial asks
    // who has the right to judge, sim/captivity.cpp judgeFor).
    Character* to = w.character(cmd.args.at("to").get<EntityId>());
    const std::string right = cmd.args.at("right").get<std::string>();
    const auto& rights = w.content().config.rights;
    if (std::find(rights.begin(), rights.end(), right) == rights.end()) {
        cmd.result = "rejected:unknown_right";
        return true;
    }
    if (!actor->status.rights.count("appoint")) {
        cmd.result = "rejected:no_mandate";
        return true;
    }
    if (!to || !to->body.alive || to->isAnimal() || !sameSettlement(*actor, *to)) {
        cmd.result = "rejected:not_of_the_colony";
        return true;
    }
    const auto st = w.settlements().find(actor->status.settlement);
    if (t == "grant_right") {
        if (to->status.rights.count(right)) {
            cmd.result = "rejected:already";
            return true;
        }
        to->status.rights.insert(right);
        w.chronicle("office", {actor->id, to->id}, "chronicle.right_granted." + right, {{"a", actor->id}, {"b", to->id}});
    } else {
        if (st != w.settlements().end() && st->second.chief == to->id) {
            cmd.result = "rejected:chief";
            return true;
        }
        if (!to->status.rights.erase(right)) {
            cmd.result = "rejected:not_held";
            return true;
        }
        w.chronicle("office", {actor->id, to->id}, "chronicle.right_revoked." + right, {{"a", actor->id}, {"b", to->id}});
    }
    return true;
}

} // namespace

SlotKey slotOf(const BuildPieceDef& d, int x, int y, int z, int rot) {
    const int r = ((rot % 4) + 4) % 4;
    if (d.slot == "roof") return {'r', x, y, z};
    if (d.slot == "thing") return {'t', x, y, z};
    if (d.slot == "edge") {
        if (r == 0) return {'h', x, y, z};
        if (r == 1) return {'v', x + 1, y, z};
        if (r == 2) return {'h', x, y + 1, z};
        return {'v', x, y, z};
    }
    if (d.slot == "corner") {
        if (r == 0) return {'p', x, y, z};
        if (r == 1) return {'p', x + 1, y, z};
        if (r == 2) return {'p', x + 1, y + 1, z};
        return {'p', x, y + 1, z};
    }
    return {'f', x, y, z};
}

Vec2 pieceCentre(const BuildCatalog& cat, const BuildPieceDef& d, const BuildPiece& p) {
    const double c = cat.cellM;
    if (d.slot == "thing") {
        const double t = c / std::max(1, d.sub);
        return {(p.x + 0.5) * t, (p.y + 0.5) * t};
    }
    Vec2 v{(p.x + 0.5) * c, (p.y + 0.5) * c};
    const int r = ((p.rot % 4) + 4) % 4;
    if (d.slot == "edge") {
        v.x += dirX(r) * c * 0.5;
        v.y += dirY(r) * c * 0.5;
    } else if (d.slot == "corner") {
        v.x += (r == 1 || r == 2 ? 0.5 : -0.5) * c;
        v.y += (r >= 2 ? 0.5 : -0.5) * c;
    }
    return v;
}

void computeStability(const BuildCatalog& cat, PieceMap& pieces) {
    const auto all = relax(cat, pieces, false);
    const auto built = relax(cat, pieces, true);
    // A planned piece: what it would have from the built ones now.
    const auto idxBuilt = indexOf(cat, pieces, true);
    for (auto& [id, p] : pieces) {
        auto a = all.find(id);
        p.stability = a == all.end() ? 0.0 : a->second;
        if (p.built) {
            auto b = built.find(id);
            p.builtStability = b == built.end() ? 0.0 : b->second;
        } else if (const BuildPieceDef* d = cat.find(p.kind)) {
            p.builtStability = supportOf(cat, pieces, idxBuilt, built, *d, p);
        }
    }
}

std::string checkPlacement(const BuildCatalog& cat, const PieceMap& pieces, const BuildPiece& p, double* stability) {
    if (stability) *stability = 0.0;
    const BuildPieceDef* d = cat.find(p.kind);
    if (!d) return "unknown_kind";
    if (p.z < 0 || p.z > kMaxStorey) return "bad_storey";
    if (d->category == "foundation" && p.z != 0) return "foundation_ground"; // AL-26: say which rule
    if (d->category == "floor" && p.z < 1) return "floor_upper";
    const SlotKey key = slotOf(*d, p.x, p.y, p.z, p.rot);
    for (const auto& [id, o] : pieces) {
        const BuildPieceDef* od = cat.find(o.kind);
        if (od && id != p.id && slotOf(*od, o.x, o.y, o.z, o.rot) == key) return "occupied";
    }
    // Stabilities of the plan as it stands (derived fields may be stale in a copy).
    PieceMap copy = pieces;
    const auto stab = relax(cat, copy, false);
    const auto idx = indexOf(cat, copy, false);
    const double s = supportOf(cat, copy, idx, stab, *d, p);
    if (stability) *stability = s;
    if (s < cat.minStability) return "unsupported";
    return "";
}

int autoStorey(const BuildCatalog& cat, const PieceMap& pieces, BuildPiece p, int from) {
    for (int z = std::max(0, from); z <= kMaxStorey; ++z) {
        p.z = z;
        if (checkPlacement(cat, pieces, p).empty()) return z;
    }
    return -1;
}

bool joinHeight(const BuildCatalog& cat, const PieceMap& pieces, const BuildPiece& p, double& h) {
    const BuildPieceDef* d = cat.find(p.kind);
    if (!d) return false;
    const auto idx = indexOf(cat, pieces, false);
    std::vector<SlotKey> near;
    if (d->category == "foundation") {
        // a foundation beside another one joins its structure
        near = {{'f', p.x + 1, p.y, 0}, {'f', p.x - 1, p.y, 0}, {'f', p.x, p.y + 1, 0}, {'f', p.x, p.y - 1, 0}};
    } else {
        for (const Support& s : supportsOf(*d, p)) near.push_back(s.key);
        for (int z = p.z; z >= 0; --z) near.push_back({'f', p.x, p.y, z});
    }
    for (const SlotKey& k : near) {
        auto it = idx.find(k);
        if (it == idx.end()) continue;
        h = pieces.at(it->second).h;
        return true;
    }
    return false;
}

std::map<std::string, int> missingOf(const World& w, const BuildPiece& p) {
    std::map<std::string, int> out;
    if (p.built) return out;
    if (const WorldObject* site = w.object(p.site))
        for (const auto& [k, v] : site->stock)
            if (v > 0) out[k] = v;
    return out;
}

void refreshPieces(World& w) {
    computeStability(w.content().build, w.pieces());
    w.piecesDirty = false;
    w.roomsDirty = true;   // AL-81
}

double siteValue(const World& w, const Character& c, const WorldObject* site, const std::string& key) {
    if (!site) return 0.0;
    if (key == "missing") return missingTotal(*site);
    if (key == "deliverable") {
        int n = 0;
        for (const auto& [item, need] : site->stock) n += std::min(std::max(0, need), int(c.has(item)));
        return n;
    }
    if (key == "ready") {
        const BuildPiece* p = pieceOfSite(w, site->id);
        return p && ready(w, *p) ? 1.0 : 0.0;
    }
    return 0.0;
}

double buildValue(const World& w, const Character& c, const std::string& key) {
    if (c.status.family == kNone) return 0.0;
    int sites = 0, readyN = 0;
    std::map<std::string, int> need;
    const EntityId town = c.status.settlement;
    for (const auto& [id, p] : w.pieces()) {
        if ((p.family != c.status.family && (town == kNone || p.family != town)) || p.built) continue;
        ++sites;
        if (ready(w, p)) ++readyN;
        for (const auto& [k, v] : missingOf(w, p)) need[k] += v;
    }
    if (key == "sites") return sites;
    if (key == "ready") return readyN;
    if (key == "carried") {
        int n = 0;
        for (const auto& [k, v] : need) n += std::min(v, int(c.has(k)));
        return n;
    }
    if (key == "work") return readyN > 0 || !need.empty() ? 1.0 : 0.0;
    auto it = need.find(key);
    return it == need.end() ? 0.0 : it->second;
}

void siteEffect(World& w, Character& c, WorldObject& site, const std::string& key) {
    if (key == "deliver") {
        for (auto& [item, need] : site.stock) {
            auto inv = c.inventory.find(item);
            if (need <= 0 || inv == c.inventory.end()) continue;
            const int n = std::min(need, inv->second);
            need -= n;
            inv->second -= n;
            if (inv->second <= 0) c.inventory.erase(inv);
        }
        return;
    }
    if (key != "work") return;
    if (w.piecesDirty) refreshPieces(w);
    BuildPiece* p = pieceOfSite(w, site.id);
    if (!p || !ready(w, *p)) return;
    const BuildPieceDef* d = w.content().build.find(p->kind);
    if (!d) return;
    if (++p->work < d->work) return;
    p->built = true;
    w.piecesDirty = true; // the site leaves the world in buildingTick
    if (d->slot == "thing") {
        // furniture: the thing itself stands now, the plan's piece is done with
        WorldObject o;
        o.type = d->makes;
        o.pos = pieceCentre(w.content().build, *d, *p);
        o.storey = p->z;
        o.yaw = p->rot * 1.5707963267948966;
        o.faced = true;
        const bool common = w.settlements().count(p->family) > 0;
        o.ownerFamily = common ? kNone : p->family;
        const EntityId made = w.addObject(o).id;
        if (Family* f = familyOfId(w, p->family))
            for (EntityId m : f->members)
                if (Character* mc = w.character(m)) mc->knownObjects.insert(made);
        if (common)
            for (auto& [cid, mc] : w.characters())
                if (!mc.isAnimal() && mc.status.settlement == p->family) mc.knownObjects.insert(made);
        const EntityId fam = p->family;
        w.pieces().erase(p->id);
        w.roomsDirty = true;
        int left = 0;
        for (const auto& [id, o2] : w.pieces()) left += o2.family == fam && !o2.built;
        if (left == 0) w.chronicle("build", {c.id}, "chronicle.build_done", {{"who", c.id}, {"family", w.familyOf(c) ? w.familyOf(c)->name : std::string()}});
        return;
    }
    if (d->shelter) {
        WorldObject roof;
        roof.type = "roof_piece";
        roof.pos = pieceCentre(w.content().build, *d, *p);
        roof.ownerFamily = p->family;
        w.addObject(roof);
    }
    int left = 0;
    for (const auto& [id, o] : w.pieces()) left += o.family == p->family && !o.built;
    if (left == 0) {
        const Family* f = w.familyOf(c);
        w.chronicle("build", {c.id}, "chronicle.build_done", {{"who", c.id}, {"family", f ? f->name : std::string()}});
    }
}

void buildingTick(World& w) {
    // Finished or orphaned sites leave the world.
    const std::vector<EntityId> sites = w.objectsOfType("build_site");
    for (EntityId s : sites) {
        const BuildPiece* p = pieceOfSite(w, s);
        if (!p || p->built) {
            w.removeObject(s);
            if (p) w.pieces().at(p->id).site = kNone;
        }
    }
    if (w.piecesDirty) refreshPieces(w);
    roomsTick(w);   // AL-81: the rooms follow the pieces and what stands in them
    // The mandate outlives the chief: his heir takes it (eldest grown child,
    // else the spouse, else the eldest grown person of the colony).
    for (auto& [sid, st] : w.settlements()) {
        if (st.chief == kNone || !st.full()) continue; // AL-92: beyond the map the realm crowns
        const Character* chief = w.character(st.chief);
        if (chief && chief->body.alive) continue;
        Character* heir = nullptr;
        if (chief) {
            for (EntityId k : chief->status.children) {
                Character* c = w.character(k);
                if (c && c->body.alive && c->body.stage() != "child" && (!heir || c->body.ageDays > heir->body.ageDays))
                    heir = c;
            }
            if (!heir)
                if (Character* sp = w.character(chief->status.spouse); sp && sp->body.alive) heir = sp;
        }
        if (!heir)
            for (auto& [id, c] : w.characters())
                if (!c.isAnimal() && c.body.alive && c.status.settlement == sid && c.body.stage() != "child" &&
                    (!heir || c.body.ageDays > heir->body.ageDays))
                    heir = &c;
        const EntityId was = st.chief;
        if (Character* old = w.character(was))
            for (const auto& r : w.content().config.rights) old->status.rights.erase(r);
        st.chief = heir ? heir->id : kNone;
        if (!heir) continue;
        for (const auto& r : w.content().config.rights) heir->status.rights.insert(r);
        w.chronicle("mandate", {was, heir->id}, "chronicle.mandate_passed", {{"a", was}, {"b", heir->id}});
    }
}

void installBuilding(World& w) { w.addCommandHandler(handle); }

EntityId mandateHolder(const World& w, EntityId settlement) {
    auto it = w.settlements().find(settlement);
    return it == w.settlements().end() ? kNone : it->second.chief;
}

EntityId judgeNow(const World& w, EntityId settlement) {
    const Character* best = nullptr;
    const Character* eldest = nullptr;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.captivity.held() || c.status.settlement != settlement) continue;
        if (c.status.rights.count("judge") && (!best || c.body.ageDays > best->body.ageDays)) best = &c;
        if (c.body.stage() != "child" && (!eldest || c.body.ageDays > eldest->body.ageDays)) eldest = &c;
    }
    return best ? best->id : eldest ? eldest->id : kNone;
}

void grantStartMandate(World& w, EntityId settlement, EntityId chief) {
    auto it = w.settlements().find(settlement);
    Character* c = w.character(chief);
    if (it == w.settlements().end() || !c) return;
    it->second.chief = chief;
    for (const auto& r : w.content().config.rights) c->status.rights.insert(r);
}

} // namespace altera::sim
