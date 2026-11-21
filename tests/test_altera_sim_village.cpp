// AL-8: the start village laid out on the ground (sim/village_plan).
#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
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
            CHECK(!site.water(L.house.c));
            // fronting a road: the road is just ahead of the door
            const Vec2 door = L.house.at(0, L.house.hd + 3.0);
            CHECK(distToPolyline(door, p.roads[std::size_t(L.road)]) < 4.0);
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
