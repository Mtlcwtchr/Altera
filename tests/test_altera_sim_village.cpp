// AL-8: the start village laid out on the ground (sim/village_plan).
#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/structure_plan.hpp"
#include "sim/village_plan.hpp"
#include "sim/world.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

MapGrid dryMap() {
    MapGrid m;
    m.sizeM = 4000;
    m.cellM = 250;
    m.w = m.h = 16;
    m.biomeNames = {"meadow"};
    m.biome.assign(256, 0);
    m.water.assign(256, 0);
    return m;
}

// A slope rising to the east with a river along x = 2100 (the village at 2000, 2000).
VillageSite riverSite() {
    VillageSite s;
    s.x0 = 2000 - 320;
    s.y0 = 2000 - 320;
    s.cell = 4;
    s.n = 161;
    for (int y = 0; y < s.n; ++y)
        for (int x = 0; x < s.n; ++x) {
            const double wx = s.x0 + x * s.cell;
            s.h.push_back(float((wx - 2000) * 0.03));
            s.wet.push_back(std::fabs(wx - 2100) < 10 ? 1 : 0);
        }
    return s;
}

}  // namespace

TEST(altera_sim_village_plan_lots) {
    const MapGrid m = dryMap();
    for (const VillageSite& site : {VillageSite{}, riverSite()}) {
        VillageWanted want;
        want.houses = want.fields = 6;
        const VillagePlan p = planVillage(site, m, {2000, 2000}, want, 11);
        CHECK(!p.roads.empty());
        CHECK(p.roads[0].size() >= 3);
        CHECK(int(p.lots.size()) == 6);
        CHECK(int(p.fields.size()) >= 4);
        for (std::size_t i = 0; i < p.lots.size(); ++i) {
            const VillageLot& L = p.lots[i];
            CHECK(L.placed);
            CHECK(!site.water(L.house.c));
            // AL-76: turned by quarters onto the build grid
            const double q = L.house.yaw / (3.14159265358979323846 / 2);
            CHECK(std::fabs(q - std::round(q)) < 1e-9);
            // fronting a road: the road is just ahead of the door (the house turned to the nearest quarter)
            const Vec2 door = L.house.at(0, L.house.hd + 2.0);
            CHECK(distToPolyline(door, p.roads[std::size_t(L.road)]) < 4.0 + L.house.hw * 0.75);
            for (std::size_t j = i + 1; j < p.lots.size(); ++j) CHECK(!L.house.overlaps(p.lots[j].house, 1.0));
            for (const auto& f : p.fields) CHECK(!L.house.overlaps(f, 1.0));
        }
        // the same seed, the same village
        const VillagePlan q = planVillage(site, m, {2000, 2000}, want, 11);
        CHECK(q.lots.size() == p.lots.size());
        if (!q.lots.empty()) CHECK(dist(q.lots[0].house.c, p.lots[0].house.c) < 1e-9);
    }
}

TEST(altera_sim_village_worldgen_planned) {
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    World w = makeTestWorld(realContent(), cfg, 7);
    const Settlement& st = w.settlements().begin()->second;
    CHECK(!st.roads.empty());
    int houses = 0, fields = 0;
    for (const auto& [id, o] : w.objects()) {
        if (o.type == "house") {
            ++houses;
            CHECK(o.faced);
            CHECK(dist(o.pos, st.square) < 200.0);
        }
        if (o.type == "field" && o.faced) ++fields;
    }
    CHECK(houses == int(cfg.value("families", 3)));
    CHECK(fields >= 1);
    // saved and loaded, the plan stays
    const Json j = w.toJson();
    CHECK(j.dump().find("road_width") != std::string::npos);
}

// AL-84: a living, sensible layout: trades by what they need (the tanner by
// the water and downwind, the carpenter toward the wood), the nuisance off the
// chapel and the inn, houses in rows, the main road out, footpaths, strips
// behind the yards, the commons, spare lots; and the plan passes its checks.
TEST(altera_sim_village_layout_logic) {
    const MapGrid m = dryMap();
    VillageSite site = riverSite();
    site.wood.assign(site.h.size(), 0);
    for (int y = 0; y < site.n; ++y)
        for (int x = 0; x < site.n; ++x)
            if (site.x0 + x * site.cell < 1780) site.wood[std::size_t(y * site.n + x)] = 1;
    const StructureCatalog cat = parseStructures(realContent().roomsJson);
    VillageWanted want;
    want.layout = VillageLayout::fromJson(cat.layout);
    auto spec = [&](const std::string& id) {
        const StructureTypeDef* d = cat.find(id);
        LotSpec L;
        L.recipe = id;
        L.w = d ? d->wHi : 3;
        L.d = d ? d->dHi : 3;
        L.zone = d ? d->zone : "lane";
        L.home = d ? d->home : true;
        L.yardD = L.home ? cat.yardD : 4.0;
        if (d) L.water = d->siteWater, L.wood = d->siteWood, L.entrance = d->siteEntrance, L.downwind = d->siteDownwind,
               L.nuisance = d->nuisance, L.quiet = d->quiet;
        return L;
    };
    for (const char* id : {"innkeeper", "chapel", "smith", "tanner", "carpenter", "baker"}) want.lots.push_back(spec(id));
    for (int k = 0; k < 8; ++k) want.lots.push_back(spec("farmer"));
    int homes = 0;
    for (const auto& L : want.lots) homes += L.home;
    want.houses = want.fields = homes;
    for (std::uint64_t seed : {3ull, 11ull, 29ull}) {
        const VillagePlan p = planVillage(site, m, {2000, 2000}, want, seed);
        const auto bad = checkVillage(p, site, m, want);
        for (const auto& b : bad) printf("  village %llu: %s\n", (unsigned long long)seed, b.c_str());
        CHECK(bad.empty());
        auto lotOf = [&](const std::string& id) -> const VillageLot& {
            for (std::size_t i = 0; i < want.lots.size(); ++i)
                if (want.lots[i].recipe == id) return p.lots[i];
            return p.lots[0];
        };
        int placed = 0;
        double farmWater = 0, farmWood = 0;
        int farms = 0;
        for (std::size_t i = 0; i < p.lots.size(); ++i) {
            placed += p.lots[i].placed;
            if (want.lots[i].recipe == "farmer" && p.lots[i].placed) {
                farmWater += std::fabs(p.lots[i].house.c.x - 2100), farmWood += std::fabs(p.lots[i].house.c.x - 1780), ++farms;
                // in a row: a neighbour close by
                double nearest = 1e9;
                for (std::size_t j = 0; j < p.lots.size(); ++j)
                    if (j != i && p.lots[j].placed) nearest = std::min(nearest, dist(p.lots[i].house.c, p.lots[j].house.c));
                CHECK(nearest < 30.0);
            }
        }
        CHECK(placed == int(want.lots.size()));
        CHECK(farms > 0);
        // the tanner nearer the water, the carpenter nearer the wood than the farmers
        CHECK(std::fabs(lotOf("tanner").house.c.x - 2100) < farmWater / std::max(1, farms));
        CHECK(std::fabs(lotOf("carpenter").house.c.x - 1780) < farmWood / std::max(1, farms));
        // the nuisance off the quiet ones
        CHECK(dist(lotOf("smith").house.c, lotOf("chapel").house.c) >= want.layout.nuisanceR);
        CHECK(dist(lotOf("tanner").house.c, lotOf("innkeeper").house.c) >= want.layout.nuisanceR);
        // roads: the main road goes out, paths to the doors
        int out = 0, paths = 0;
        for (const auto& k : p.roadKind) out += k == "out", paths += k == "path";
        CHECK(out >= 1);
        CHECK(paths >= placed);
        CHECK(p.roadKind.size() == p.roads.size());
        // every home has its strip, and no field across the river
        int fields = 0, cemetery = 0, pasture = 0, woodlot = 0, spare = 0;
        for (const auto& z : p.zones) {
            fields += z.kind == "field", cemetery += z.kind == "cemetery", pasture += z.kind == "pasture";
            woodlot += z.kind == "woodlot", spare += z.kind == "spare";
            if (z.kind == "field") CHECK(z.rect.c.x < 2100);
        }
        CHECK(fields == homes);
        CHECK(cemetery == 1);
        if (cemetery != 1) for (const auto& n : p.notes) printf("  note %s\n", n.c_str());
        CHECK(pasture == 1);
        CHECK(woodlot == 1);
        CHECK(spare >= 1);
        // the same seed, the same village
        const VillagePlan q = planVillage(site, m, {2000, 2000}, want, seed);
        CHECK(q.roads.size() == p.roads.size() && q.zones.size() == p.zones.size());
    }
}

TEST(altera_sim_village_worldgen_layout_saved) {
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    cfg["families"] = 8;
    World w = makeTestWorld(realContent(), cfg, 7);
    const Settlement& st = w.settlements().begin()->second;
    CHECK(st.roadKind.size() == st.roads.size());
    CHECK(!st.zones.empty());
    bool well = false;
    for (const auto& [id, o] : w.objects())
        if (o.type == "well" && dist(o.pos, st.square) < 0.5) well = true;
    CHECK(well);   // the well in the middle of the square
    // the hives in the yards: close behind a home, not on the square
    for (const auto& [id, o] : w.objects())
        if (o.type == "beehive") {
            CHECK(dist(o.pos, st.square) > st.squareR);
            double nearest = 1e9;
            for (const auto& s : st.structures)
                if (s.family != kNone) nearest = std::min(nearest, dist(o.pos, s.door));
            CHECK(nearest < 30.0);
        }
    // the smoky works stand out of the village, off every house
    for (const auto& [id, o] : w.objects())
        if (o.type == "charcoal_clamp" || o.type == "lime_kiln") {
            CHECK(dist(o.pos, st.square) > st.squareR + 20.0);
            for (const auto& s : st.structures)
                if (s.family != kNone) CHECK(dist(o.pos, s.door) > 35.0);
        }
    bool recipes = true;
    for (const auto& s : st.structures) recipes = recipes && !s.recipe.empty();
    CHECK(recipes);
    // saved and loaded, the layout stays
    World back(realContent(), 7);
    back.loadJson(w.toJson());
    const Settlement& sb = back.settlements().begin()->second;
    CHECK(sb.zones.size() == st.zones.size());
    CHECK(sb.roadKind == st.roadKind);
    CHECK(dist(sb.entrance, st.entrance) < 1e-9);
    CHECK(!sb.structures.empty() && sb.structures[0].recipe == st.structures[0].recipe);
}
