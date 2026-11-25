// AL-81: rooms told apart by what stands in them, for any building (sim/rooms).
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/rooms.hpp"
#include "sim/world.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

// A room the way a player lays one with the build tools: footings over
// [x0, x1) x [y0, y1), walls round it (a door in the front), posts at the
// corners, a roof over it; `built` as the people would leave it.
void layRoom(World& w, int x0, int y0, int x1, int y1, bool built, bool roof = true, bool walls = true) {
    auto add = [&](const std::string& kind, int x, int y, int z, int rot) {
        BuildPiece p;
        p.id = w.newId();
        p.kind = kind;
        p.x = x, p.y = y, p.z = z, p.rot = rot;
        p.built = built;
        w.pieces()[p.id] = p;
    };
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            add("foundation_stone", x, y, 0, 0);
            if (roof) add("roof_thatch", x, y, 1, y < (y0 + y1) / 2 ? 0 : 2);
        }
    if (walls) {
        for (int x = x0; x < x1; ++x) {
            add(x == x0 + 1 ? "wall_door_log" : "wall_log", x, y0, 0, 0);
            add("wall_log", x, y1 - 1, 0, 2);
        }
        for (int y = y0; y < y1; ++y) {
            add("wall_log", x0, y, 0, 3);
            add("wall_log", x1 - 1, y, 0, 1);
        }
    } else {
        for (int x = x0; x <= x1; x += x1 - x0)
            for (int y = y0; y <= y1; y += y1 - y0) add("pillar_log", std::min(x, x1 - 1), std::min(y, y1 - 1), 0,
                                                         x == x0 ? (y == y0 ? 0 : 3) : (y == y0 ? 1 : 2));
    }
    w.piecesDirty = true;
}

EntityId put(World& w, const std::string& type, double x, double y) {
    WorldObject o;
    o.type = type;
    o.pos = {x, y};
    return w.addObject(o).id;
}

World emptyWorld() {
    Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 3);
    w.pieces().clear();
    return w;
}

}  // namespace

TEST(lr_sim_rooms_rules) {
    const RoomCatalog& r = realContent().rooms;
    CHECK(!r.rules.empty());
    // every type a rule reads is an object of the world
    for (const auto& t : r.thingTypes) CHECK(realContent().objects.count(t) > 0);
    CHECK(roomType(r, {"forge", "anvil"}, false) == "smithy");
    CHECK(roomType(r, {"forge", "anvil", "bed"}, true) == "smithy");   // the higher one wins
    CHECK(roomType(r, {"hearth", "bed", "table"}, true) == "home");
    CHECK(roomType(r, {"hearth", "table"}, true) == "kitchen");
    CHECK(roomType(r, {"bed", "bed", "chest"}, true) == "bedroom");
    CHECK(roomType(r, {"bed"}, false) == "shelter");                    // a bedroom needs walls
    CHECK(roomType(r, {"table_bench", "table_bench", "counter", "hearth"}, true) == "tavern_hall");
    CHECK(roomType(r, {"table_bench", "counter", "hearth"}, true) == "kitchen");   // one table is no hall
    CHECK(roomType(r, {"oven", "quern"}, true) == "bakery");
    CHECK(roomType(r, {"shrine", "bench"}, true) == "chapel");
    CHECK(roomType(r, {"livestock_pen"}, false) == "byre");
    CHECK(roomType(r, {"barrel"}, true) == "storeroom");
    CHECK(roomType(r, {}, true) == "room");
    CHECK(roomType(r, {}, false) == "shelter");
}

TEST(lr_sim_rooms_player_built) {
    World w = emptyWorld();
    const double c = realContent().build.cellM;
    // planned (not yet raised) pieces make no room
    layRoom(w, 10, 10, 13, 13, false);
    const EntityId bed = put(w, "bed", 11.5 * c, 11.5 * c);
    refreshRooms(w);
    CHECK(w.rooms().empty());
    // raised: a closed room with a bed in it - a bedroom
    for (auto& [id, p] : w.pieces()) p.built = true;
    w.piecesDirty = true;
    roomsTick(w);
    CHECK(w.rooms().size() == 1);
    if (w.rooms().empty()) return;
    CHECK(w.rooms()[0].enclosed);
    CHECK(w.rooms()[0].type == "bedroom");
    CHECK(int(w.rooms()[0].cells.size()) == 9);
    CHECK(w.roomBuildings().size() == 1 && w.roomBuildings()[0].purpose == "bedroom");
    // a hearth and a table brought in: a home; the bed carried out: a kitchen
    put(w, "hearth", 10.5 * c, 12.5 * c);
    put(w, "table", 12.5 * c, 11.5 * c);
    roomsTick(w);
    CHECK(w.rooms()[0].type == "home");
    w.object(bed)->pos = {30 * c, 30 * c};
    roomsTick(w);
    CHECK(w.rooms()[0].type == "kitchen");
    // a forge and an anvil set up: the smithy outranks the kitchen
    const EntityId forge = put(w, "forge", 11.5 * c, 12.5 * c);
    put(w, "anvil", 11.5 * c, 10.5 * c);
    roomsTick(w);
    CHECK(w.rooms()[0].type == "smithy");
    CHECK(w.roomBuildings()[0].purpose == "smithy");
    w.removeObject(forge);
    roomsTick(w);
    CHECK(w.rooms()[0].type == "kitchen");
    // a wall knocked out: the room leaks, but under its roof it is still one (open)
    for (auto it = w.pieces().begin(); it != w.pieces().end(); ++it)
        if (it->second.kind == "wall_log" && it->second.rot == 2) {
            w.pieces().erase(it);
            break;
        }
    w.piecesDirty = true;
    roomsTick(w);
    CHECK(w.rooms().size() == 1);
    CHECK(!w.rooms()[0].enclosed);
    CHECK(w.rooms()[0].type == "shelter");   // a kitchen needs walls
    // a second room behind a wall: two rooms, one building
    layRoom(w, 13, 10, 15, 13, true);
    put(w, "bed", 13.5 * c, 11.5 * c);
    roomsTick(w);
    CHECK(w.rooms().size() == 2);
    CHECK(w.roomBuildings().size() == 1);
}

TEST(lr_sim_rooms_canopy_and_bare_footings) {
    World w = emptyWorld();
    const double c = realContent().build.cellM;
    // posts and a roof, no walls: an open workshop - a smithy outdoors
    layRoom(w, 0, 0, 2, 3, true, true, false);
    put(w, "forge", 0.5 * c, 0.5 * c);
    put(w, "anvil", 1.5 * c, 1.5 * c);
    // bare footings far off: no room at all
    layRoom(w, 40, 40, 43, 43, true, false, false);
    put(w, "bed", 41.5 * c, 41.5 * c);
    refreshRooms(w);
    CHECK(w.rooms().size() == 1);
    if (w.rooms().empty()) return;
    CHECK(!w.rooms()[0].enclosed);
    CHECK(w.rooms()[0].type == "smithy");
    CHECK(roomAt(w, {0.5 * c, 0.5 * c}) == &w.rooms()[0]);
    CHECK(roomAt(w, {41.5 * c, 41.5 * c}) == nullptr);
}
