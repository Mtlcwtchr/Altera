// AL-94: the 2D client's build mode (src/client2d/build2d): a tool's stroke
// expands into the sim's modules and goes into the plan as the sim checks it;
// planning is free and without a limit, for the household or the settlement;
// furniture is planned like a piece and, raised, becomes the thing that tells
// the room what it is.
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "client2d/build2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/rooms.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;

namespace {

const std::string kRoot = ALTERA_SIM_SOURCE_DIR;

const World& baseWorld() {
    static const Content content = Content::load(kRoot + "/content/ai");
    static const World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        cfg["village_plan"] = true;
        World v = makeTestWorld(content, cfg, 7);
        installAi(v);
        return v;
    }();
    return w;
}

// A grown villager with a household.
EntityId builder(const World& w) {
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.family != kNone && c.status.settlement != kNone &&
            c.body.stage() != "child")
            return id;
    return kNone;
}

// Free land near the village: cells [x0, x0 + W) x [y0, y0 + D) with no piece and no thing on them (and a margin).
bool freeLot(const World& w, int W, int D, int& gx, int& gy) {
    const double c = w.content().build.cellM;
    const Character* b = w.character(builder(w));
    const Settlement& s = w.settlements().at(b->status.settlement);
    const int cx = int(s.center.x / c), cy = int(s.center.y / c);
    for (int r = 10; r < 200; r += 3)
        for (int k = 0; k < 16; ++k) {
            const int x0 = cx + int(r * std::cos(k * 0.3927)), y0 = cy + int(r * std::sin(k * 0.3927));
            bool ok = true;
            for (const auto& [id, p] : w.pieces())
                if (p.x >= x0 - 2 && p.x < x0 + W + 2 && p.y >= y0 - 2 && p.y < y0 + D + 2) ok = false;
            for (const auto& [id, o] : w.objects())
                if (o.pos.x >= (x0 - 1) * c && o.pos.x < (x0 + W + 1) * c && o.pos.y >= (y0 - 1) * c &&
                    o.pos.y < (y0 + D + 1) * c)
                    ok = false;
            if (ok) {
                gx = x0, gy = y0;
                return true;
            }
        }
    return false;
}

c2::Stroke rect(const std::string& tool, const World& w, int x0, int y0, int x1, int y1) {
    const double c = w.content().build.cellM;
    c2::Stroke s;
    s.tool = tool;
    s.ax = (x0 + 0.5) * c, s.ay = (y0 + 0.5) * c;
    s.bx = (x1 - 0.5) * c, s.by = (y1 - 0.5) * c;
    return s;
}

c2::ViewText noText() { return {}; }

int ownPieces(const World& w, EntityId owner) {
    int n = 0;
    for (const auto& [id, p] : w.pieces()) n += p.family == owner;
    return n;
}

// Raise every planned piece of `owner` at once (materials brought, work done), supports first.
void raiseAll(World& w, EntityId owner, Character& by) {
    for (int guard = 0; guard < 64; ++guard) {
        refreshPieces(w);
        bool any = false;
        std::vector<EntityId> sites;
        for (const auto& [id, p] : w.pieces())
            if (p.family == owner && !p.built && p.builtStability >= w.content().build.minStability) sites.push_back(p.site);
        for (EntityId sid : sites) {
            WorldObject* site = w.object(sid);
            if (!site) continue;
            for (auto& [k, v] : site->stock) v = 0;
            for (int i = 0; i < 8; ++i) {
                WorldObject* s2 = w.object(sid);
                if (!s2) break;
                siteEffect(w, by, *s2, "work");
            }
            any = true;
        }
        buildingTick(w);
        if (!any) break;
    }
    refreshPieces(w);
    refreshRooms(w);
}

} // namespace

TEST(altera_build2d_room_tool) {
    World w = baseWorld();
    const EntityId me = builder(w);
    CHECK(me != kNone);
    int gx = 0, gy = 0;
    CHECK(freeLot(w, 4, 3, gx, gy));
    const BuildCatalog& cat = w.content().build;
    // the room's modules: 12 footings, the walls round it (one door), a roof with gables
    const auto pieces = c2::roomPieces(cat, gx, gy, gx + 4, gy + 3, 0, "log", 0, true, "roof_thatch");
    int found = 0, walls = 0, doors = 0, windows = 0, roofs = 0, gables = 0;
    for (const auto& p : pieces) {
        const BuildPieceDef* d = cat.find(p.kind);
        CHECK(d != nullptr);
        if (!d) continue;
        found += d->category == "foundation";
        walls += d->category == "wall";
        doors += d->door;
        windows += p.kind.find("window") != std::string::npos;
        roofs += d->category == "roof";
        gables += d->category == "gable";
    }
    CHECK_EQ(found, 12);
    CHECK(walls >= 14);
    CHECK_EQ(doors, 1);
    CHECK(windows >= 1);
    CHECK_EQ(roofs, 12);
    CHECK(gables >= 2);
    // the preview takes it all; placing lays it into the plan, nothing is spent
    c2::Stroke s = rect("room", w, gx, gy, gx + 4, gy + 3);
    const c2::Planner who{me, false};
    const Json pre = c2::previewJson(w, noText(), who, s);
    CHECK_EQ(pre["bad"].get<int>(), 0);
    CHECK_EQ(pre["ok"].get<int>(), int(pieces.size()));
    CHECK(!pre["cost"].empty());
    const EntityId fam = w.character(me)->status.family;
    const Family& f = w.families().at(fam);
    const WorldObject* store = w.object(f.storage);
    const auto stockBefore = store ? store->stock : std::map<std::string, int>{};
    const auto invBefore = w.character(me)->inventory;
    const int before = ownPieces(w, fam);
    const Json put = c2::placeJson(w, noText(), who, s);
    CHECK_EQ(put["placed"].get<int>(), int(pieces.size()));
    CHECK_EQ(ownPieces(w, fam), before + int(pieces.size()));
    if (store) CHECK(w.object(f.storage)->stock == stockBefore);
    CHECK(w.character(me)->inventory == invBefore);
    // every planned piece has a site with what is to be brought
    int sites = 0;
    for (const auto& [id, p] : w.pieces())
        if (p.family == fam && !p.built && w.object(p.site)) ++sites;
    CHECK(sites >= int(pieces.size()));
    // the plan in the window shows them, mine
    const Json plan = c2::planJson(w, noText(), me, int(gx * cat.cellM) - 4, int(gy * cat.cellM) - 4, 20, 20);
    int mine = 0;
    for (const auto& p : plan["pieces"]) mine += p["who"] == "mine";
    CHECK(mine >= int(pieces.size()));
    // a wider room (four rows of slopes): everything rests on what is under it
    int hx = 0, hy = 0;
    CHECK(freeLot(w, 5, 4, hx, hy));
    const Json wide = c2::previewJson(w, noText(), who, rect("room", w, hx, hy, hx + 5, hy + 4));
    CHECK_EQ(wide["bad"].get<int>(), 0);
    // the same stroke again: every slot is taken
    const Json again = c2::previewJson(w, noText(), who, s);
    CHECK_EQ(again["ok"].get<int>(), 0);
    CHECK_EQ(again["why"].get<std::string>(), std::string("occupied"));
}

TEST(altera_build2d_unlimited_plan) {
    World w = baseWorld();
    const EntityId me = builder(w);
    int gx = 0, gy = 0;
    CHECK(freeLot(w, 22, 22, gx, gy));
    const EntityId fam = w.character(me)->status.family;
    // more footings than the 3D mode's cap: the sim plans them all
    c2::Stroke s = rect("floor", w, gx, gy, gx + 22, gy + 21);
    const Json put = c2::placeJson(w, noText(), {me, false}, s);
    CHECK_EQ(put["placed"].get<int>(), 22 * 21);
    CHECK(ownPieces(w, fam) > w.content().build.maxPerFamily);
}

TEST(altera_build2d_settlement_plan) {
    World w = baseWorld();
    const EntityId me = builder(w);
    const EntityId town = w.character(me)->status.settlement;
    int gx = 0, gy = 0;
    CHECK(freeLot(w, 3, 3, gx, gy));
    c2::Stroke s = rect("room", w, gx, gy, gx + 3, gy + 3);
    s.forSettlement = true;
    const Json put = c2::placeJson(w, noText(), {me, true}, s);
    CHECK(put["placed"].get<int>() > 0);
    CHECK_EQ(ownPieces(w, town), put["placed"].get<int>());
    // another household of the settlement counts the common sites as work of its own
    const Character* other = nullptr;
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement == town && c.status.family != kNone &&
            c.status.family != w.character(me)->status.family && c.body.stage() != "child") {
            other = &c;
            break;
        }
    CHECK(other != nullptr);
    if (other) CHECK(buildValue(w, *other, "sites") >= put["placed"].get<double>());
    // a common site is the members' own (deliver_to_site / construct_piece take target_owner:self)
    const Json plan = c2::planJson(w, noText(), me, int(gx * 2) - 4, int(gy * 2) - 4, 16, 16);
    int common = 0;
    for (const auto& p : plan["pieces"]) common += p["who"] == "settlement";
    CHECK(common > 0);
    // a member strikes a planned common piece
    c2::Stroke rm = rect("remove", w, gx, gy, gx + 3, gy + 3);
    const Json gone = c2::placeJson(w, noText(), {me, true}, rm);
    CHECK(gone["placed"].get<int>() > 0);
    CHECK_EQ(ownPieces(w, town), 0);
}

TEST(altera_build2d_door_replaces_wall) {
    World w = baseWorld();
    const EntityId me = builder(w);
    int gx = 0, gy = 0;
    CHECK(freeLot(w, 3, 3, gx, gy));
    c2::Stroke s = rect("room", w, gx, gy, gx + 3, gy + 3);
    s.roofed = false;
    const Json room = c2::placeJson(w, noText(), {me, false}, s);
    CHECK(room["placed"].get<int>() > 0);
    // a door in the back wall: it takes the wall's place
    const double c = w.content().build.cellM;
    c2::Stroke d;
    d.tool = "door";
    d.ax = (gx + 0.5) * c, d.ay = (gy + 3) * c - 0.1;   // just inside the back line
    const Json pre = c2::previewJson(w, noText(), {me, false}, d);
    CHECK_EQ(pre["ok"].get<int>(), 1);
    CHECK(pre["pieces"][0].contains("replaces"));
    int doorsBefore = 0, doorsAfter = 0;
    for (const auto& [id, p] : w.pieces()) doorsBefore += p.kind == "wall_door_log";
    c2::placeJson(w, noText(), {me, false}, d);
    for (const auto& [id, p] : w.pieces()) doorsAfter += p.kind == "wall_door_log";
    CHECK_EQ(doorsAfter, doorsBefore + 1);
}

TEST(altera_build2d_furniture_makes_room) {
    World w = baseWorld();
    const EntityId me = builder(w);
    int gx = 0, gy = 0;
    CHECK(freeLot(w, 3, 3, gx, gy));
    const EntityId fam = w.character(me)->status.family;
    c2::Stroke s = rect("room", w, gx, gy, gx + 3, gy + 3);
    c2::placeJson(w, noText(), {me, false}, s);
    // a hearth and a bed inside (1 m furniture cells)
    const double c = w.content().build.cellM;
    c2::Stroke hearth;
    hearth.tool = "thing";
    hearth.thing = "thing_hearth";
    hearth.ax = gx * c + 2.5, hearth.ay = gy * c + 2.5;
    c2::Stroke bed = hearth;
    bed.thing = "thing_bed";
    bed.ax = gx * c + 4.5, bed.ay = gy * c + 1.5;
    CHECK_EQ(c2::placeJson(w, noText(), {me, false}, hearth)["placed"].get<int>(), 1);
    CHECK_EQ(c2::placeJson(w, noText(), {me, false}, bed)["placed"].get<int>(), 1);
    // the same spot again: taken
    CHECK_EQ(c2::previewJson(w, noText(), {me, false}, bed)["ok"].get<int>(), 0);
    // raised: the walls stand, the things are in the world, the room is a home
    Character& by = *w.character(me);
    raiseAll(w, fam, by);
    int planned = 0;
    for (const auto& [id, p] : w.pieces()) planned += p.family == fam && !p.built && p.x >= gx && p.x < gx + 3;
    CHECK_EQ(planned, 0);
    int beds = 0, hearths = 0;
    for (const auto& [id, o] : w.objects()) {
        if (o.pos.x < gx * c || o.pos.x >= (gx + 3) * c || o.pos.y < gy * c || o.pos.y >= (gy + 3) * c) continue;
        beds += o.type == "bed";
        hearths += o.type == "hearth";
        if (o.type == "bed" || o.type == "hearth") CHECK_EQ(o.ownerFamily, fam);
    }
    CHECK_EQ(beds, 1);
    CHECK_EQ(hearths, 1);
    const Room* r = roomAt(w, {gx * c + 3.0, gy * c + 3.0}, 0);
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->enclosed);
        CHECK_EQ(r->type, std::string("home"));
    }
}

TEST(altera_build2d_catalog_and_colony) {
    World w = baseWorld();
    const EntityId me = builder(w);
    const Json cat = c2::catalogJson(w, noText());
    std::set<std::string> things;
    for (const auto& p : cat["pieces"])
        if (p["slot"] == "thing") things.insert(p["makes"].get<std::string>());
    // every room a rule names can be furnished from the palette
    for (const auto& r : w.content().rooms.rules) {
        bool known = false;
        for (const auto& n : r.needs)
            for (const auto& t : n.any) known = known || w.content().objects.count(t);
        if (!known) continue;   // a rule over things the world has no object for yet (a livestock pen)
        bool any = false;
        for (const auto& n : r.needs)
            for (const auto& t : n.any) any = any || things.count(t);
        if (!any) std::printf("no furniture for room %s\n", r.id.c_str());
        CHECK(any);
    }
    const Json col = c2::colonyJson(w, noText(), me);
    CHECK(col.is_object());
    CHECK(col["people"].get<int>() > 0);
    CHECK(!col["families"].empty());
    CHECK(!col["rooms"].empty());
    CHECK(col["chief"].get<EntityId>() != kNone);
}
