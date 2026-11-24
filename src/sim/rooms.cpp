// AL-81: rooms by what stands in them (sim/rooms.hpp).

#include "sim/rooms.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace altera::sim {
namespace {

using Key = std::tuple<int, int, int>;   // x, y, storey

int stepX(int rot) { return rot == 1 ? 1 : rot == 3 ? -1 : 0; }
int stepY(int rot) { return rot == 2 ? 1 : rot == 0 ? -1 : 0; }

int purposeRank(const RoomCatalog& rules, const std::string& type) {
    const RoomRuleDef* r = rules.find(type);
    return r ? r->purposeRank : -1;
}

}  // namespace

std::string roomType(const RoomCatalog& rules, const std::vector<std::string>& things, bool enclosed) {
    for (const auto& r : rules.rules) {
        if (r.enclosure == "closed" && !enclosed) continue;
        if (r.enclosure == "open" && enclosed) continue;
        bool fits = !r.needs.empty();
        for (const auto& need : r.needs) {
            int n = 0;
            for (const auto& t : things)
                if (std::find(need.any.begin(), need.any.end(), t) != need.any.end()) ++n;
            if (n < need.n) fits = false;
        }
        if (fits) return r.id;
    }
    return enclosed ? rules.closedFallback : rules.openFallback;
}

RoomsResult detectRooms(const BuildCatalog& build, const RoomCatalog& rules, const PieceMap& pieces,
                        const std::vector<RoomThing>& things) {
    RoomsResult out;
    std::set<Key> floors;
    std::set<SlotKey> edges;                 // walls, gables, doors: a room ends at them
    std::map<std::pair<int, int>, int> roofTop;   // the highest roof over a cell
    for (const auto& [id, p] : pieces) {
        if (!p.built) continue;
        const BuildPieceDef* d = build.find(p.kind);
        if (!d) continue;
        if ((d->category == "foundation" && p.z == 0) || (d->category == "floor" && p.z > 0)) floors.insert({p.x, p.y, p.z});
        else if (d->slot == "edge") edges.insert(slotOf(*d, p.x, p.y, p.z, p.rot));
        else if (d->category == "roof") {
            auto& top = roofTop[{p.x, p.y}];
            top = std::max(top, p.z);
        }
    }
    if (floors.empty()) return out;
    static const BuildPieceDef kEdge{"", "edge", "wall"};
    auto blocked = [&](int x, int y, int z, int rot) { return edges.count(slotOf(kEdge, x, y, z, rot)) > 0; };
    auto covered = [&](int x, int y, int z) {
        if (floors.count({x, y, z + 1})) return true;
        auto it = roofTop.find({x, y});
        return it != roofTop.end() && it->second > z;
    };
    std::map<Key, int> regionOf;
    std::vector<int> roomOfRegion;
    for (const Key& start : floors) {
        if (regionOf.count(start)) continue;
        const int region = int(roomOfRegion.size());
        std::vector<Key> stack{start}, cells;
        regionOf[start] = region;
        bool leak = false;
        while (!stack.empty()) {
            const Key k = stack.back();
            stack.pop_back();
            cells.push_back(k);
            const auto [x, y, z] = k;
            for (int rot = 0; rot < 4; ++rot) {
                if (blocked(x, y, z, rot)) continue;
                const Key n{x + stepX(rot), y + stepY(rot), z};
                if (!floors.count(n)) {
                    leak = true;
                    continue;
                }
                if (regionOf.emplace(n, region).second) stack.push_back(n);
            }
        }
        int roofed = 0;
        for (const auto& [x, y, z] : cells) roofed += covered(x, y, z);
        const bool room = !leak || double(roofed) >= rules.roofCover * double(cells.size());
        if (!room) {   // bare footings, a terrace: no room
            roomOfRegion.push_back(-1);
            continue;
        }
        Room r;
        r.id = int(out.rooms.size()) + 1;
        r.enclosed = !leak;
        r.z = std::get<2>(cells.front());
        r.x0 = r.y0 = 1 << 30;
        r.x1 = r.y1 = -(1 << 30);
        std::sort(cells.begin(), cells.end());
        for (const auto& [x, y, z] : cells) {
            r.cells.push_back({x, y});
            r.x0 = std::min(r.x0, x), r.y0 = std::min(r.y0, y);
            r.x1 = std::max(r.x1, x + 1), r.y1 = std::max(r.y1, y + 1);
        }
        roomOfRegion.push_back(int(out.rooms.size()));
        out.rooms.push_back(std::move(r));
    }
    // what stands in each room
    std::vector<std::vector<std::string>> held(out.rooms.size());
    const double c = build.cellM;
    for (const auto& t : things) {
        const Key k{int(std::floor(t.pos.x / c)), int(std::floor(t.pos.y / c)), t.storey};
        auto it = regionOf.find(k);
        if (it == regionOf.end()) continue;
        const int ri = roomOfRegion[std::size_t(it->second)];
        if (ri < 0) continue;
        out.rooms[std::size_t(ri)].things.push_back(t.id);
        held[std::size_t(ri)].push_back(t.type);
    }
    for (std::size_t i = 0; i < out.rooms.size(); ++i) out.rooms[i].type = roomType(rules, held[i], out.rooms[i].enclosed);
    // buildings: the rooms whose cells (seen from above) touch
    std::set<std::pair<int, int>> plan;
    for (const auto& [x, y, z] : floors) plan.insert({x, y});
    std::map<std::pair<int, int>, int> part;
    int parts = 0;
    for (const auto& start : plan) {
        if (part.count(start)) continue;
        std::vector<std::pair<int, int>> stack{start};
        part[start] = parts;
        while (!stack.empty()) {
            const auto q = stack.back();
            stack.pop_back();
            for (int rot = 0; rot < 4; ++rot) {
                const std::pair<int, int> n{q.first + stepX(rot), q.second + stepY(rot)};
                if (plan.count(n) && part.emplace(n, parts).second) stack.push_back(n);
            }
        }
        ++parts;
    }
    std::map<int, int> buildingOfPart;
    for (std::size_t i = 0; i < out.rooms.size(); ++i) {
        Room& r = out.rooms[i];
        const int pt = part.at(r.cells.front());
        auto [it, fresh] = buildingOfPart.emplace(pt, int(out.buildings.size()));
        if (fresh) {
            RoomBuilding b;
            b.id = int(out.buildings.size()) + 1;
            b.x0 = r.x0, b.y0 = r.y0, b.x1 = r.x1, b.y1 = r.y1;
            out.buildings.push_back(b);
        }
        RoomBuilding& b = out.buildings[std::size_t(it->second)];
        r.building = it->second;
        b.rooms.push_back(int(i));
        b.x0 = std::min(b.x0, r.x0), b.y0 = std::min(b.y0, r.y0), b.x1 = std::max(b.x1, r.x1), b.y1 = std::max(b.y1, r.y1);
        if (b.purpose.empty() || purposeRank(rules, r.type) > purposeRank(rules, b.purpose)) b.purpose = r.type;
    }
    return out;
}

namespace {
std::uint64_t roomsSignature(const World& w) {
    std::uint64_t h = 0x51ed27u;
    for (const auto& [id, p] : w.pieces()) h = hashCombine(h, (std::uint64_t(id) << 1) | (p.built ? 1u : 0u));
    for (const auto& type : w.content().rooms.thingTypes)
        for (EntityId id : w.objectsOfType(type))
            if (const WorldObject* o = w.object(id)) {
                h = hashCombine(h, id);
                h = hashCombine(h, std::uint64_t(std::llround(o->pos.x * 10)) * 1000003u + std::uint64_t(std::llround(o->pos.y * 10)));
                h = hashCombine(h, std::uint64_t(o->storey));
            }
    return h;
}
}  // namespace

void refreshRooms(World& w) {
    std::vector<RoomThing> things;
    for (const auto& type : w.content().rooms.thingTypes)
        for (EntityId id : w.objectsOfType(type))
            if (const WorldObject* o = w.object(id)) things.push_back({id, o->type, o->pos, o->storey});
    RoomsResult r = detectRooms(w.content().build, w.content().rooms, w.pieces(), things);
    w.rooms() = std::move(r.rooms);
    w.roomBuildings() = std::move(r.buildings);
    w.roomsSig = roomsSignature(w);
    w.roomsDirty = false;
}

void roomsTick(World& w) {
    if (w.pieces().empty()) {
        if (!w.rooms().empty()) w.rooms().clear(), w.roomBuildings().clear();
        return;
    }
    if (w.roomsDirty || roomsSignature(w) != w.roomsSig) refreshRooms(w);
}

const Room* roomAt(const World& w, Vec2 p, int storey) {
    const double c = w.content().build.cellM;
    const int x = int(std::floor(p.x / c)), y = int(std::floor(p.y / c));
    for (const auto& r : w.rooms()) {
        if (r.z != storey || x < r.x0 || y < r.y0 || x >= r.x1 || y >= r.y1) continue;
        for (const auto& cell : r.cells)
            if (cell.first == x && cell.second == y) return &r;
    }
    return nullptr;
}

}  // namespace altera::sim
