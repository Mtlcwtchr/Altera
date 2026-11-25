// AL-115 M4: the elements meeting in the world (sim/world_reactions.hpp): the
// matrix of aspect pairs, cell rules (steam, ice, tainted water), spells mixing
// aspects, crops in fire and under a growth spell, beasts and people getting out
// of the flames, a burnt wood growing back.

#include <cstdio>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/world_reactions.hpp"
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

void run(World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        w.step();
        fieldsTick(w);
    }
}

void dry(World& w) {
    w.fields().weather[village(w)].rainMm = -100.0;
    w.fields().ambientDay = -1;
}

Vec2 cellOfBiome(const World& w, const std::string& biome, double minFromVillage) {
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
            if (ok && dist(p, c) > minFromVillage && dist(p, c) < bd) {
                bd = dist(p, c);
                best = p;
            }
        }
    return best;
}

} // namespace

TEST(lr_sim_reactions_matrix_complete) {
    World w = bare();
    const ReactionsConfig& rc = reactionsConfig(w);
    CHECK(rc.loaded);
    CHECK(rc.aspects.size() >= 20);
    const auto p = rc.problems();
    for (const auto& s : p) std::printf("  %s\n", s.c_str());
    CHECK(p.empty());
    CHECK(rc.pair("water", "fire") && rc.pair("water", "fire")->result == "steam");
    CHECK(rc.pair("death", "life") && rc.pair("death", "life")->cancel);
    CHECK(rc.pair("shadow", "light") && rc.pair("shadow", "light")->cancel);
    CHECK(rc.pair("sleep", "space") == nullptr);   // an explicit "none"
}

TEST(lr_sim_reactions_steam_and_ice) {
    World w = bare();
    dry(w);
    const Vec2 m = cellOfBiome(w, "meadow", 700.0);
    // a hot blast on soaked ground: steam, the water boils off
    fieldAdd(w, Field::moisture, m, 3.0, 150.0);
    World cold = w;
    fieldAdd(w, Field::heat, m, 3.0, 60.0);
    CHECK_EQ(cellStateAt(w, m), std::string("steam"));
    CHECK(moveFactorAt(w, m) < 1.0);
    run(w, 3);
    run(cold, 3);
    CHECK(fieldByte(w, Field::moisture, m) < fieldByte(cold, Field::moisture, m));
    // [Fire-] on wet ground freezes it
    fieldAdd(cold, Field::heat, m, 3.0, -60.0);
    CHECK_EQ(cellStateAt(cold, m), std::string("ice"));
    CHECK(std::abs(moveFactorAt(cold, m) - 0.7) < 1e-9);
}

TEST(lr_sim_reactions_tainted_water_spreads_and_sickens) {
    World w = bare();
    // the water the village draws from
    Character* villager = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.status.settlement == village(w)) villager = &c;
    CHECK(villager != nullptr);
    CHECK(!drinksTaintedWater(w, *villager));
    const WorldObject* src = nullptr;
    double bd = 1e18;
    const Vec2 c = w.settlements().at(village(w)).center;
    for (const char* t : {"well", "water"})
        for (EntityId id : w.objectsOfType(t))
            if (dist(w.object(id)->pos, c) < bd) {
                bd = dist(w.object(id)->pos, c);
                src = w.object(id);
            }
    CHECK(src != nullptr);
    fieldAdd(w, Field::taint, src->pos, 2.0, 160.0);
    CHECK(drinksTaintedWater(w, *villager));
    // a poisoned stretch of the river runs on along the water
    Vec2 river{};
    bool found = false;
    const MapGrid& m = w.map();
    for (int j = 2; j < m.h - 2 && !found; ++j)
        for (int i = 2; i < m.w - 2 && !found; ++i)
            if (m.water[j * m.w + i]) {
                river = {(i + 0.5) * m.cellM, (j + 0.5) * m.cellM};
                found = true;
            }
    CHECK(found);
    if (found) {
        fieldAdd(w, Field::taint, river, 1.0, 200.0);
        CHECK_EQ(fieldByte(w, Field::taint, {river.x + 3, river.y}), 0);
        run(w, int(kTicksPerDay) * 6);
        std::printf("  taint 3 m down the water after 6 days: %d\n", fieldByte(w, Field::taint, {river.x + 3, river.y}));
        CHECK(fieldByte(w, Field::taint, {river.x + 3, river.y}) > 0);
    }
}

TEST(lr_sim_reactions_spells_mix_aspects) {
    World w = bare();
    dry(w);
    const Vec2 m = cellOfBiome(w, "meadow", 700.0);
    const auto r = applySpellToWorld(w, m, 3.0, {{"fire", "+"}, {"water", "+"}}, 2.0);
    CHECK(r.reactions.size() == 1 && r.reactions[0] == "steam");
    CHECK(!r.cancelled);
    const int t0 = fieldByte(w, Field::taint, m);
    const auto c = applySpellToWorld(w, m, 3.0, {{"life", "+"}, {"death", "+"}}, 2.0);
    CHECK(c.cancelled);
    CHECK_EQ(fieldByte(w, Field::taint, m), t0);   // neither wrote
    const auto e = applySpellToWorld(w, m, 3.0, {{"earth", "~"}}, 1.0);
    CHECK(e.unhandled.empty());                 // Earth~ heaves the ground itself now
    CHECK(groundAt(w, m) > 0.2);
}

TEST(lr_sim_reactions_crops_burn_and_grow_by_magic) {
    World w = bare();
    dry(w);
    WorldObject* field = nullptr;
    for (EntityId id : w.objectsOfType("field")) field = w.object(id);
    CHECK(field != nullptr);
    field->stock["sown"] = 10;
    field->stock["grown"] = 5;
    field->stock["fertility"] = 700;
    const EntityId fid = field->id;
    // [Plants+] power 2: days of growth at once, the soil pays
    applyChannels(w, field->pos, 3.0, Json{{"growth", "+2*power"}}, 2.0);
    CHECK(w.object(fid)->stock.at("grown") > 5);
    CHECK(w.object(fid)->stock.at("fertility") < 700);
    // [Life+] feeds its soil back
    const int f0 = w.object(fid)->stock.at("fertility");
    applyChannels(w, field->pos, 3.0, Json{{"fertility", "+1.5*power"}}, 2.0);
    CHECK(w.object(fid)->stock.at("fertility") > f0);
    // flames on it: the standing crop is lost, the field itself stays
    fieldAdd(w, Field::moisture, w.object(fid)->pos, 3.0, -200.0);
    ignite(w, w.object(fid)->pos, 220);
    run(w, 2);
    CHECK(w.object(fid) != nullptr);
    CHECK_EQ(w.object(fid)->stock.at("sown"), 0);
    bool told = false;
    for (const auto& e : w.chronicleLog()) told = told || e.code == "chronicle.crop_burnt";
    CHECK(told);
}

TEST(lr_sim_reactions_beasts_and_people_get_out_of_fire) {
    World w = bare();
    dry(w);
    const Vec2 f = cellOfBiome(w, "forest", 700.0);
    ignite(w, f, 255);
    run(w, 3);
    CHECK(fireAt(w, f) > 0);
    Character* beast = nullptr;
    Character* man = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() && c.body.alive && !beast)
            if (const SpeciesDef* sp = w.content().speciesOf(c.species); sp && sp->speedMPerTick > 0 && !sp->domestic) beast = &c;
        if (!c.isAnimal() && c.body.alive && !man) man = &c;
    }
    CHECK(beast && man);
    beast->pos = f;
    animalThink(w, *beast, AnimalTick{});
    CHECK(dist(beast->pos, f) > 1.0);
    man->pos = f;
    const double h = man->body.health;
    fieldsTick(w);
    CHECK(man->body.alive);
    CHECK(dist(man->pos, f) > 1.0);
    CHECK(fireAt(w, man->pos) == 0 || man->body.health < h);
}

TEST(lr_sim_reactions_burnt_wood_grows_back) {
    World w = bare();
    dry(w);
    // a tree broken by a blow (a natural source): it comes back where it stood
    EntityId tree = kNone;
    const Vec2 c = w.settlements().at(village(w)).center;
    for (EntityId id : w.objectsOfType("tree"))
        if (dist(w.object(id)->pos, c) > 500.0) {
            tree = id;
            break;
        }
    CHECK(tree != kNone);
    const Vec2 at = w.object(tree)->pos;
    const std::size_t trees = w.objectsOfType("tree").size();
    CHECK(damageObject(w, tree, 10000, "force"));
    CHECK(w.objectsOfType("tree").size() == trees - 1);
    CHECK_EQ(w.fields().regrow.size(), std::size_t(1));
    run(w, int(kTicksPerDay) * 32);
    CHECK(w.objectsOfType("tree").size() == trees);
    bool there = false;
    for (EntityId id : w.objectsOfType("tree")) there = there || dist(w.object(id)->pos, at) < 0.01;
    CHECK(there);
}
