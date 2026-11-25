// AL-115 §5.7: elementals live while the cell feeds them (sim/elementals.hpp).

#include <cstdio>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/elementals.hpp"
#include "sim/world_fields.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World bare() {
    return makeTestWorld(real(), loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
}

EntityId village(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return id;
    return kNone;
}

void rain(World& w, double mm) {
    w.fields().weather[village(w)].rainMm = mm;
    w.fields().ambientDay = -1;
}

void run(World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        w.step();
        fieldsTick(w);
    }
}

Vec2 cellOfBiome(const World& w, const std::string& biome) {
    const MapGrid& m = w.map();
    const Vec2 c = w.settlements().at(village(w)).center;
    Vec2 best{};
    double bd = 1e18;
    for (int j = 2; j < m.h - 2; ++j)
        for (int i = 2; i < m.w - 2; ++i) {
            bool ok = true;
            for (int dj = -1; dj <= 1 && ok; ++dj)
                for (int di = -1; di <= 1 && ok; ++di) {
                    const int k = (j + dj) * m.w + (i + di);
                    ok = m.biomeNames[m.biome[k]] == biome && !m.water[k];
                }
            const Vec2 p{(i + 0.5) * m.cellM, (j + 0.5) * m.cellM};
            if (ok && dist(p, c) > 700.0 && dist(p, c) < bd) {
                bd = dist(p, c);
                best = p;
            }
        }
    return best;
}

int alive(const World& w, const std::string& species) {
    int n = 0;
    for (const auto& [id, c] : w.characters()) n += c.species == species && c.body.alive;
    return n;
}

} // namespace

TEST(lr_sim_elementals_load_and_do_not_roam_the_map) {
    World w = bare();
    for (const char* s : {"elemental_fire", "elemental_water", "elemental_air", "elemental_earth"}) {
        CHECK(real().species.count(s) == 1);
        CHECK(isElemental(w, s));
        CHECK_EQ(alive(w, s), 0);
    }
    CHECK(!isElemental(w, "deer"));
}

TEST(lr_sim_elementals_fire_one_lives_in_a_dry_wood_and_dies_in_rain) {
    World w = bare();
    rain(w, -100.0);
    const Vec2 f = cellOfBiome(w, "forest");
    World wet = w;
    const EntityId e = summonElemental(w, "fire", f, 2.0);
    CHECK(e != kNone);
    const double h0 = w.character(e)->body.health;
    run(w, 30);
    CHECK(w.character(e)->body.alive);
    CHECK(w.character(e)->body.health > h0);
    CHECK(fireAt(w, w.character(e)->pos) > 0 || fireAt(w, f) > 0);
    // the same in a downpour: it goes out
    rain(wet, 6.0);
    CHECK_EQ(summonElemental(wet, "fire", f, 2.0), kNone);   // a storm does not let it form at all
    rain(wet, 0.0);
    wet.fields().weather.clear();
    const EntityId e2 = summonElemental(wet, "fire", f, 2.0);
    if (e2 != kNone) {
        rain(wet, 6.0);
        run(wet, 3);
        CHECK(!wet.character(e2)->body.alive);
        CHECK_EQ(wet.character(e2)->body.causeOfDeath, std::string("dispersed"));
    }
}

TEST(lr_sim_elementals_water_one_soaks_the_marsh_and_boils_away) {
    World w = bare();
    rain(w, -100.0);
    const Vec2 m = cellOfBiome(w, "marsh");
    const int m0 = fieldByte(w, Field::moisture, m);
    const EntityId e = summonElemental(w, "water", m, 2.0);
    CHECK(e != kNone);
    run(w, 10);
    CHECK(w.character(e)->body.alive);
    CHECK(fieldByte(w, Field::moisture, w.character(e)->pos) > m0);
    // a blast of heat under it: dispersed
    fieldAdd(w, Field::heat, w.character(e)->pos, 3.0, 200.0);
    run(w, 1);
    CHECK(!w.character(e)->body.alive);
}

TEST(lr_sim_elementals_wild_fire_one_born_of_a_great_fire) {
    World w = bare();
    rain(w, -100.0);
    w.fields().weather[village(w)].wind = 3.0;
    const Vec2 f = cellOfBiome(w, "forest");
    for (int k = -2; k <= 2; ++k) ignite(w, {f.x + 8.0 * k, f.y}, 255);
    int born = 0;
    for (int d = 0; d < 8 && born == 0; ++d) {
        run(w, int(kTicksPerDay));
        born = alive(w, "elemental_fire");
        for (const auto& e : w.chronicleLog()) born += e.code == "chronicle.elemental_born";
    }
    std::printf("  wild fire elementals born: %d\n", born);
    CHECK(born > 0);
}

TEST(lr_sim_elementals_walk_with_the_beasts) {
    World w = bare();
    installAi(w);
    const Vec2 m = cellOfBiome(w, "meadow");
    const EntityId e = summonElemental(w, "earth", m, 3.0);
    CHECK(e != kNone);
    w.run(kTicksPerDay);
    CHECK(w.character(e) != nullptr);
    CHECK(w.character(e)->body.alive);
}
