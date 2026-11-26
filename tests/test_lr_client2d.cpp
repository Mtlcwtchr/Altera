// AL-95: the 2D client's map (src/client2d): the sim's planned village cut
// into 1 m cells - roads, fields, floors, walls and doors, rooms, roofs - and
// the people as the page reads them.
#include <map>
#include <set>
#include <string>

#include "client2d/acts2d.hpp"
#include "client2d/grid2d.hpp"
#include "client2d/view2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/worldgen.hpp"
#include "sim/transport.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& content2d() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

World& village2d() {
    static World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        cfg["village_plan"] = true;
        World v = makeTestWorld(content2d(), cfg, 7);
        installAi(v);
        return v;
    }();
    return w;
}

c2::GroundRules rules2d() {
    return c2::GroundRules::fromJson(loadJsonFile(kRoot + "/content/sprites_2d/map2d.json").value("ground", Json::object()));
}

const Settlement& village(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (!s.roads.empty()) return s;
    return w.settlements().begin()->second;
}

// The cells round the village: its pieces' bounds and a margin.
c2::Grid2d villageGrid(const World& w) {
    const double c = w.content().build.cellM;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const auto& [id, p] : w.pieces()) {
        x0 = std::min(x0, p.x * c), y0 = std::min(y0, p.y * c);
        x1 = std::max(x1, (p.x + 1) * c), y1 = std::max(y1, (p.y + 1) * c);
    }
    return c2::buildGrid(w, rules2d(), int(x0) - 8, int(y0) - 8, int(x1 - x0) + 16, int(y1 - y0) + 16);
}

} // namespace

TEST(lr_client2d_grid_window) {
    const World& w = village2d();
    const c2::Grid2d g = c2::buildGrid(w, rules2d(), 100, 200, 64, 32);
    CHECK_EQ(g.x0, 100);
    CHECK_EQ(g.y0, 200);
    CHECK_EQ(g.w, 64);
    CHECK_EQ(g.h, 32);
    CHECK_EQ(g.ground.size(), std::size_t(64 * 32));
    // clamped to the map
    const int size = int(w.map().sizeM);
    const c2::Grid2d edge = c2::buildGrid(w, rules2d(), size - 10, -5, 64, 20);
    CHECK_EQ(edge.w, 10);
    CHECK_EQ(edge.y0, 0);
    CHECK_EQ(edge.h, 15);
    // the same world gives the same cells
    const c2::Grid2d again = c2::buildGrid(w, rules2d(), 100, 200, 64, 32);
    CHECK(again.ground == g.ground);
    CHECK(again.var == g.var);
}

TEST(lr_client2d_village_ground) {
    const World& w = village2d();
    const Settlement& s = village(w);
    CHECK(!s.roads.empty());
    const auto r = rules2d();
    // a road's middle is road (or its kind's ground), a field's middle is a field
    int roadCells = 0;
    for (std::size_t i = 0; i < s.roads.size(); ++i) {
        const Vec2 a = s.roads[i].front();
        const c2::Ground g = c2::groundAt(w, r, int(std::floor(a.x)), int(std::floor(a.y)));
        if (g == c2::Ground::road || g == c2::Ground::dirt) ++roadCells;
    }
    CHECK(roadCells >= int(s.roads.size()) - 1);   // one may start under the square or water
    int fields = 0;
    for (const auto& z : s.zones)
        if (z.kind == "field") {
            ++fields;
            const c2::Ground g = c2::groundAt(w, r, int(std::floor(z.c.x)), int(std::floor(z.c.y)));
            CHECK(g == c2::Ground::field || g == c2::Ground::road || g == c2::Ground::dirt);
        }
    CHECK(fields > 0);
    // the square
    if (s.squareR > 0) CHECK(c2::groundAt(w, r, int(s.square.x), int(s.square.y)) == r.square);
}

TEST(lr_client2d_houses) {
    const World& w = village2d();
    const c2::Grid2d g = villageGrid(w);
    const BuildCatalog& cat = w.content().build;
    const int cell = int(std::lround(cat.cellM));
    // every built footing of the ground storey is floor, every built wall of it is an edge
    int footings = 0, walls = 0, doors = 0, roofs = 0;
    for (const auto& [id, p] : w.pieces()) {
        const BuildPieceDef* d = cat.find(p.kind);
        CHECK(d != nullptr);
        if (!d || !p.built) continue;
        const SlotKey k = slotOf(*d, p.x, p.y, p.z, p.rot);
        if (k.layer == 'f' && p.z == 0) {
            ++footings;
            for (int dy = 0; dy < cell; ++dy)
                for (int dx = 0; dx < cell; ++dx) CHECK(g.at(p.x * cell + dx, p.y * cell + dy) == c2::Ground::floor);
        }
        if ((k.layer == 'h' || k.layer == 'v') && d->category == "wall") ++walls, doors += d->door ? 1 : 0;
        if (k.layer == 'r') ++roofs;
    }
    CHECK(footings > 0);
    int gWalls = 0, gDoors = 0;
    for (const auto& e : g.edges) {
        if (e.kind != "gable") ++gWalls;
        if (e.kind == "door") ++gDoors;
        CHECK_EQ(e.len, cell);
        CHECK(e.axis == 'h' || e.axis == 'v');
    }
    CHECK_EQ(gWalls, walls);
    CHECK_EQ(gDoors, doors);
    CHECK(gDoors > 0);
    CHECK_EQ(int(g.roofs.size()), roofs);
    // roofs know the building under them (the page lifts a roof by building)
    int known = 0;
    for (const auto& rf : g.roofs) known += rf.building >= 0 ? 1 : 0;
    CHECK(known * 10 >= int(g.roofs.size()) * 8);
    // rooms come over with their types and cell bounds
    CHECK_EQ(g.rooms.size(), w.rooms().size());
    for (const auto& rm : g.rooms) {
        CHECK(!rm.type.empty());
        CHECK(rm.x1 > rm.x0 && rm.y1 > rm.y0);
    }
    // the things of the village are in the window (beds, hearths...)
    std::set<std::string> types;
    for (const auto& t : g.things) types.insert(t.type);
    CHECK(types.count("bed") > 0);
    CHECK(types.count("well") > 0);
}

TEST(lr_client2d_json) {
    const World& w = village2d();
    const c2::Grid2d g = c2::buildGrid(w, rules2d(), int(village(w).center.x) - 32, int(village(w).center.y) - 32, 64, 64);
    const Json j = c2::toJson(g);
    CHECK_EQ(j["ground"].get<std::string>().size(), std::size_t(64 * 64));
    CHECK_EQ(j["var"].get<std::string>().size(), std::size_t(64 * 64 * 2));
    for (char ch : j["ground"].get<std::string>()) CHECK(ch >= 'a' && ch < char('a' + int(c2::Ground::count)));
    c2::ViewText t;   // no loc: ids for names
    const Json people = c2::peopleJson(w, t);
    std::size_t vehicles = 0;   // LR-159: vehicles go in the list as beasts do (veh: true)
    for (const auto& [id, o] : w.objects()) vehicles += lr::sim::isVehicle(w, o) ? 1 : 0;
    CHECK_EQ(people.size(), w.characters().size() + vehicles);
    int humans = 0;
    for (const auto& p : people) {
        CHECK(p.contains("x") && p.contains("y") && p.contains("ia"));
        if (p["sp"].get<std::string>().empty()) {
            ++humans;
            CHECK(p.contains("worn") && p.contains("inv"));
        }
    }
    CHECK(humans > 0);
    const Json card = c2::personJson(w, t, w.characters().begin()->first);
    CHECK(card.is_object());
    CHECK(card["needs"].is_array());
    CHECK(c2::personJson(w, t, 999999999).is_null());
    const Json st = c2::staticJson(w, t);
    CHECK(st["ground"].size() == std::size_t(c2::Ground::count));
    CHECK(st["rooms"].is_object() && !st["rooms"].empty());
}

// AL-96: the act menu and the quick bar for the player's person
TEST(lr_client2d_acts) {
    World& w = village2d();
    EntityId me = kNone;
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.ageDays / kDaysPerYear >= 18) { me = id; break; }
    CHECK(me != kNone);
    c2::ViewText t;
    c2::ActCatalog cat = c2::ActCatalog::make(w, loadJsonFile(kRoot + "/content/ui/wheel.json"));
    CHECK(!cat.personActs.empty());
    EntityId well = kNone;
    for (const auto& [id, o] : w.objects())
        if (o.type == "well") well = id;
    CHECK(well != kNone);
    const Json acts = c2::actsJson(w, t, cat, me, well);
    CHECK(!acts.empty());
    bool drink = false;
    for (const auto& a : acts) {
        CHECK(a.contains("can") && a.contains("why") && a.contains("minutes"));
        if (a["id"] == "drink_from_well") drink = true;
        if (!a["can"].get<bool>()) CHECK(!a["why"].get<std::string>().empty());
    }
    CHECK(drink);
    // on oneself: only what can be done now
    for (const auto& a : c2::actsJson(w, t, cat, me, kNone)) CHECK(a["can"].get<bool>());
    // a person: the wheel's acts
    EntityId other = kNone;
    for (const auto& [id, c] : w.characters())
        if (id != me && !c.isAnimal() && c.body.alive) other = id;
    CHECK(!c2::actsJson(w, t, cat, me, other).empty());
    // the bar: an act on a person without one chosen says why
    const Json bar = c2::barJson(w, t, me, kNone, Json::array({{{"kind", "act"}, {"id", "chat"}}, {{"kind", "goal"}, {"id", "eat"}}}));
    CHECK_EQ(bar.size(), std::size_t(2));
    CHECK(!bar[0]["can"].get<bool>());
    CHECK(!bar[0]["why"].get<std::string>().empty());
    CHECK(c2::resultText(t, "").empty());
    CHECK(!c2::resultText(t, "rejected:cannot:no_method").empty());
}

// AL-109: the pawn holds the tool of the act (the axe to chop), a weapon he carries
// otherwise, nothing asleep.
TEST(lr_client2d_held_tool) {
    World w = village2d();
    Character* c = nullptr;
    for (auto& [id, ch] : w.characters())
        if (!ch.isAnimal() && ch.body.alive && ch.body.stage() == "adult") c = &ch;
    if (!c || !w.content().interaction("chop_tree")) return;
    c2::ViewText t;
    auto held = [&] {
        for (const auto& p : c2::peopleJson(w, t))
            if (p["id"].get<EntityId>() == c->id) return p.value("held", std::string());
        return std::string("?");
    };
    c->inventory = {{"axe", 1}, {"spear", 1}, {"bread", 1}};
    c->ai.plan.actions.clear();
    c->ai.fsm.state = "Work";
    c->ai.fsm.interaction = "chop_tree";
    CHECK_EQ(held(), std::string("axe"));
    c->ai.fsm.interaction = "";
    c->ai.fsm.state = "Idle";
    CHECK_EQ(held(), std::string("spear"));
    c->ai.fsm.state = "Sleep";
    CHECK_EQ(held(), std::string(""));
}
