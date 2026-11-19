// Building from modules (content/ai/build.json, sim/building.hpp) and the
// colony's mandate and rights.

#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/captivity.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World testWorld() {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    installAi(w);
    return w;
}

Command cmd(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = 1;
    return c;
}

BuildPiece piece(const std::string& kind, int x, int y, int z, int rot, EntityId id) {
    BuildPiece p;
    p.id = id;
    p.kind = kind;
    p.x = x, p.y = y, p.z = z, p.rot = rot;
    return p;
}

Character* adultHead(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.role == "head") return &c;
    return nullptr;
}

} // namespace

TEST(altera_sim_build_catalog_loads) {
    const BuildCatalog& b = real().build;
    CHECK(b.pieces.size() >= 12);
    CHECK(b.find("foundation_stone") != nullptr);
    CHECK(b.find("wall_door_log") != nullptr && b.find("wall_door_log")->door);
    CHECK(b.find("roof_thatch") != nullptr && b.find("roof_thatch")->shelter);
    CHECK(real().objects.count("build_site") == 1);
}

TEST(altera_sim_build_support_rules) {
    const BuildCatalog& b = real().build;
    PieceMap m;
    // nothing holds a wall in the air; a foundation stands on the ground only
    CHECK(checkPlacement(b, m, piece("wall_log", 0, 0, 0, 0, 1)) == "unsupported");
    CHECK(checkPlacement(b, m, piece("foundation_stone", 0, 0, 1, 0, 1)) == "bad_storey");
    CHECK(checkPlacement(b, m, piece("foundation_stone", 0, 0, 0, 0, 1)).empty());
    m[1] = piece("foundation_stone", 0, 0, 0, 0, 1);
    m[2] = piece("foundation_stone", 1, 0, 0, 0, 2);
    CHECK(checkPlacement(b, m, piece("foundation_stone", 0, 0, 0, 0, 3)) == "occupied");
    // walls round the two cells; the same edge seen from the next cell is taken
    m[3] = piece("wall_log", 0, 0, 0, 0, 3);
    CHECK(checkPlacement(b, m, piece("wall_daub", 0, -1, 0, 2, 4)) == "occupied");
    CHECK(checkPlacement(b, m, piece("wall_log", 0, 0, 0, 3, 4)).empty());
    m[4] = piece("wall_log", 0, 0, 0, 3, 4);
    m[5] = piece("wall_log", 0, 0, 0, 2, 5);
    m[6] = piece("wall_door_log", 1, 0, 0, 0, 6);
    m[7] = piece("wall_log", 1, 0, 0, 1, 7);
    m[8] = piece("wall_log", 1, 0, 0, 2, 8);
    // a roof sits on the walls one storey up, not on the ground floor
    CHECK(checkPlacement(b, m, piece("roof_thatch", 0, 0, 0, 0, 9)) == "unsupported");
    CHECK(autoStorey(b, m, piece("roof_thatch", 0, 0, 0, 0, 9)) == 1);
    // a wall stacks on a wall; a floor goes on the walls
    CHECK(autoStorey(b, m, piece("wall_log", 0, 0, 0, 0, 9)) == 1);
    CHECK(checkPlacement(b, m, piece("floor_plank", 0, 0, 1, 0, 9)).empty());
    // daub is weak: five storeys of it do not stand
    PieceMap d;
    d[1] = piece("foundation_stone", 5, 5, 0, 0, 1);
    for (int z = 0; z < 5; ++z) d[EntityId(2 + z)] = piece("wall_daub", 5, 5, z, 0, EntityId(2 + z));
    computeStability(b, d);
    CHECK(d[2].stability > 0.6);
    CHECK(d[5].stability >= b.minStability); // four storeys stand, the fifth does not
    CHECK(d[6].stability < b.minStability);
    double h = -1;
    m[1].h = 12.5;
    CHECK(joinHeight(b, m, piece("wall_log", 0, 0, 0, 0, 20), h) && h == 12.5);
    CHECK(joinHeight(b, m, piece("foundation_stone", 0, 1, 0, 0, 20), h) && h == 12.5);
    CHECK(!joinHeight(b, m, piece("foundation_stone", 9, 9, 0, 0, 20), h));
}

TEST(altera_sim_build_household_raises_it) {
    World w = testWorld();
    Character* head = adultHead(w);
    CHECK(head != nullptr);
    if (!head) return;
    const EntityId hid = head->id;
    // The household's materials, carried by its head (the gathering chains are
    // tested elsewhere; here: deliveries, support order, work).
    head->inventory["stone"] += 40;
    head->inventory["log"] += 30;
    head->inventory["branch"] += 20;
    head->inventory["clay"] += 12;
    head->inventory["thatch"] += 6;
    const int bx = int(head->pos.x / 2.0) + 6, by = int(head->pos.y / 2.0) + 6;
    auto place = [&](const std::string& kind, int x, int y, int z, int rot) {
        Command r = w.applyNow(cmd("place_piece", {{"id", hid}, {"kind", kind}, {"x", bx + x}, {"y", by + y}, {"z", z},
                                                   {"rot", rot}, {"h", 40.0}}));
        if (r.result != "ok") std::fprintf(stderr, "    place %s: %s\n", kind.c_str(), r.result.c_str());
        return r.result == "ok";
    };
    CHECK(place("foundation_stone", 0, 0, 0, 0));
    CHECK(place("foundation_stone", 1, 0, 0, 0));
    CHECK(place("wall_door_log", 0, 0, 0, 0));
    CHECK(place("wall_log", 1, 0, 0, 0));
    CHECK(place("wall_log", 0, 0, 0, 3));
    CHECK(place("wall_log", 1, 0, 0, 1));
    CHECK(place("wall_daub", 0, 0, 0, 2));
    CHECK(place("wall_daub", 1, 0, 0, 2));
    CHECK(place("roof_thatch", 0, 0, 1, 0));
    CHECK(place("roof_thatch", 1, 0, 1, 0));
    CHECK(!place("wall_log", 4, 4, 0, 0)); // in the air: refused
    CHECK(w.pieces().size() == 10);
    CHECK(w.objectsOfType("build_site").size() == 10);
    w.run(4 * kTicksPerDay);
    int built = 0;
    for (const auto& [id, p] : w.pieces()) built += p.built;
    std::fprintf(stderr, "    built %d of %zu, sites left %zu\n", built, w.pieces().size(), w.objectsOfType("build_site").size());
    CHECK(built >= 4);
    // Support order: nothing stands on what is not standing.
    for (const auto& [id, p] : w.pieces())
        if (p.built) CHECK(p.builtStability >= real().build.minStability);
    CHECK(w.objectsOfType("build_site").size() == w.pieces().size() - std::size_t(built));

    // Saved and loaded, the world goes on the same.
    World copy(real(), w.seed());
    installAi(copy);
    copy.loadJson(w.toJson());
    CHECK(copy.pieces().size() == w.pieces().size());
    CHECK(copy.stateHash() == w.stateHash());
    w.run(kTicksPerDay);
    copy.run(kTicksPerDay);
    CHECK(copy.stateHash() == w.stateHash());

    // Taking a planned piece off the plan returns what was brought; a wall
    // taken from under a roof brings the roof down with it.
    const std::size_t before = w.pieces().size();
    EntityId wall = kNone;
    for (const auto& [id, p] : w.pieces())
        if (p.kind == "wall_daub" && p.x == bx) wall = id;
    if (wall != kNone) {
        Command r = w.applyNow(cmd("remove_piece", {{"id", hid}, {"piece", wall}}));
        CHECK(r.result == "ok");
        CHECK(w.pieces().size() < before);
    }
}

TEST(altera_sim_colony_rights_and_trials) {
    World w = testWorld();
    const auto& st = w.settlements().begin()->second;
    const EntityId chiefId = st.chief;
    CHECK(chiefId != kNone);
    Character* chief = w.character(chiefId);
    if (!chief) return;
    for (const auto& r : real().config.rights) CHECK(chief->status.rights.count(r) == 1);
    // the chief judges by his right
    CHECK(judgeNow(w, st.id) == chiefId);
    // somebody else, a grown person of the colony
    Character* other = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && id != chiefId && c.body.stage() == "adult" && c.status.settlement == st.id) {
            other = &c;
            break;
        }
    CHECK(other != nullptr);
    if (!other) return;
    // without the mandate no one hands out offices
    Command bad = w.applyNow(cmd("grant_right", {{"id", other->id}, {"to", other->id}, {"right", "judge"}}));
    CHECK(bad.result == "rejected:no_mandate");
    // the chief makes him a judge, and takes the right back: trials follow at once
    w.applyNow(cmd("revoke_right", {{"id", chiefId}, {"to", chiefId}, {"right", "judge"}}));
    CHECK(chief->status.rights.count("judge") == 1); // the chief keeps his own
    chief->status.rights.erase("judge");             // he lays it down (the law: an appointed judge)
    Command ok = w.applyNow(cmd("grant_right", {{"id", chiefId}, {"to", other->id}, {"right", "judge"}}));
    CHECK(ok.result == "ok");
    CHECK(judgeNow(w, st.id) == other->id);
    Command back = w.applyNow(cmd("revoke_right", {{"id", chiefId}, {"to", other->id}, {"right", "judge"}}));
    CHECK(back.result == "ok");
    CHECK(other->status.rights.count("judge") == 0);
    // the mandate outlives the chief
    chief->body.alive = false;
    chief->body.health = 0.0;
    w.step();
    const EntityId heir = w.settlements().begin()->second.chief;
    CHECK(heir != kNone && heir != chiefId);
    if (const Character* h = w.character(heir)) CHECK(h->status.rights.count("appoint") == 1);
    bool told = false;
    for (const auto& e : w.chronicleLog()) told |= e.code == "chronicle.mandate_passed";
    CHECK(told);
}
