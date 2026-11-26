// Peoples as data (AL-114): kinds over the one Character, their life in human years,
// half-bloods, trolls asleep as stone in the sun, kinds of the realm (a dragon's lair),
// monsters on the same ladder as the beasts.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/species.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Json& worldCfg() {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return cfg;
}

Character* anAdult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") return &c;
    return nullptr;
}

} // namespace

TEST(lr_sim_species_load) {
    const Content& c = realContent();
    CHECK(c.peoples.loaded);
    for (const char* k : {"human", "elf", "dwarf", "orc", "goblin", "ogre", "troll", "half_elf", "half_orc", "dragon"})
        CHECK(c.raceOf(k) != nullptr);
    CHECK_EQ(c.peoples.defaultRace, std::string("human"));
    CHECK(c.raceOf("") == c.raceOf("human"));
    // a blend is its parents' average where it says nothing itself
    const RaceDef& he = *c.raceOf("half_elf");
    CHECK_EQ(he.adultYears, (14 + 50) / 2);
    CHECK(he.lifespanYears > c.raceOf("human")->lifespanYears && he.lifespanYears < c.raceOf("elf")->lifespanYears);
    CHECK(c.raceOf("goblin")->spritePx == 16 && c.raceOf("ogre")->spritePx == 32);
    CHECK(c.raceOf("troll")->sunSleep.on);
    CHECK(!c.raceOf("human")->sunSleep.on);
    CHECK(c.raceOf("dragon")->might > 0.0);
    CHECK(c.raceOf("dragon")->innateSpells.is_array() && !c.raceOf("dragon")->innateSpells.empty());
    for (const char* cu : {"polis_hellene", "forest_elf", "dwarf_hold", "steppe_rider", "port_kobal", "jotun_hold", "troll_fell", "draconic"})
        CHECK(c.peoples.cultures.count(cu) == 1);
    ValidationReport r;
    validateSpecies(c, r);
    for (const auto& e : r.errors) std::printf("  species error: %s\n", e.c_str());
    CHECK(r.ok());
}

TEST(lr_sim_species_loc_keys) {
    const Content& c = realContent();
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const char* lang : {"ru", "en"}) {
        for (const auto& [id, r] : c.peoples.races) CHECK(loc.has(lang, "species." + id + ".name"));
        for (const auto& [id, cu] : c.peoples.cultures) CHECK(loc.has(lang, "culture." + id + ".name"));
        CHECK(loc.has(lang, "chronicle.species.petrified"));
        CHECK(loc.has(lang, "species.varg.name"));
        CHECK(loc.has(lang, "item.basilisk_eye.name"));
    }
}

// The human is the measure: the rules that count years read the same as before.
TEST(lr_sim_species_human_years_are_years) {
    Body b;
    for (int y : {0, 5, 14, 30, 60, 80}) {
        b.ageDays = y * kDaysPerYear + 100;
        CHECK_EQ(b.lifeYears(), y);
    }
    const Content& c = realContent();
    Character h;
    CHECK_EQ(raceNeedMult(c, h, "hunger"), 1.0);
    CHECK_EQ(raceLearnMult(c, h, "smithing"), 1.0);
    CHECK_EQ(raceAgeDays(raceOf(c, h), 30.0), 30 * kDaysPerYear);
}

TEST(lr_sim_species_life_by_kind) {
    const Content& c = realContent();
    Character e;
    setRace(c, e, "elf");
    CHECK_EQ(e.race, std::string("elf"));
    e.body.ageDays = 40 * kDaysPerYear;
    CHECK_EQ(e.body.stage(), std::string("child")); // an elf of 40 is not grown
    e.body.ageDays = 200 * kDaysPerYear;
    CHECK_EQ(e.body.stage(), std::string("adult"));
    CHECK(e.body.lifeYears() > 30 && e.body.lifeYears() < 45); // like a man in his thirties
    Character o;
    setRace(c, o, "orc");
    o.body.ageDays = 12 * kDaysPerYear;
    CHECK_EQ(o.body.stage(), std::string("adult")); // orcs grow up fast
    o.body.ageDays = 40 * kDaysPerYear;
    CHECK_EQ(o.body.stage(), std::string("elder"));
    // a human 30 years old in elf terms
    CHECK(raceAgeDays(*c.raceOf("elf"), 30.0) > 100 * kDaysPerYear);
    // the kind's needs and aptitudes
    Character d;
    setRace(c, d, "dwarf");
    CHECK(raceLearnMult(c, d, "smithing") > 1.3);
    CHECK(raceCapMult(c, d, "swim") < 1.0);
    CHECK(raceNeedMult(c, d, "warmth") < 1.0);
    Character g;
    setRace(c, g, "ogre");
    CHECK(raceNeedMult(c, g, "hunger") > 2.0);
    CHECK(raceLearnMult(c, g, "farming") < 1.0); // "*": slow at everything else
    // a default-kind person keeps an empty race (saves stay as they were)
    Character h;
    setRace(c, h, "human");
    CHECK(h.race.empty());
}

TEST(lr_sim_species_half_bloods) {
    const Content& c = realContent();
    CHECK_EQ(childRace(c, "", "elf"), std::string("half_elf"));
    CHECK_EQ(childRace(c, "elf", ""), std::string("half_elf"));
    CHECK_EQ(childRace(c, "orc", "human"), std::string("half_orc"));
    CHECK_EQ(childRace(c, "dwarf", "dwarf"), std::string("dwarf"));
    CHECK(childRace(c, "dwarf", "elf").empty());
    CHECK(childRace(c, "troll", "human").empty());
    CHECK_EQ(childRace(c, "", ""), std::string("human"));
    CHECK(raceBias(c, "elf", "orc") < -0.3);
    CHECK_EQ(raceBias(c, "elf", "elf"), 0.0);
}

TEST(lr_sim_species_mixed_village_worldgen) {
    Json cfg = worldCfg();
    cfg["peoples"] = {{"dwarf", 0.5}, {"human", 0.5}};
    // AL-136: one kind per household, so a seed whose village has both
    std::uint64_t seed = 11;
    for (std::uint64_t s = 11; s < 40; ++s) {
        const World t = makeTestWorld(realContent(), cfg, s);
        int d = 0, n = 0;
        for (const auto& [id, c] : t.characters())
            if (!c.isAnimal()) ++n, d += c.race == "dwarf";
        if (d > 0 && d < n) {
            seed = s;
            break;
        }
    }
    World a = makeTestWorld(realContent(), cfg, seed);
    World b = makeTestWorld(realContent(), cfg, seed);
    int dwarves = 0, people = 0;
    for (const auto& [id, c] : a.characters()) {
        if (c.isAnimal()) continue;
        ++people;
        if (c.race == "dwarf") {
            ++dwarves;
            CHECK_EQ(c.body.adultYears, realContent().raceOf("dwarf")->adultYears);
        }
        CHECK_EQ(b.character(id)->race, c.race); // deterministic
    }
    CHECK(dwarves > 0 && dwarves < people);
    // a dwarf head of a household is a grown dwarf (the age in dwarf years)
    for (const auto& [id, c] : a.characters())
        if (c.race == "dwarf" && c.status.role == "head") CHECK(c.body.ageDays >= 30 * kDaysPerYear);
    // AL-136: a household is of one kind
    for (const auto& [fid, f] : a.families()) {
        std::string kind = "?";
        for (EntityId m : f.members)
            if (const Character* c = a.character(m)) {
                if (kind == "?") kind = c->race;
                CHECK_EQ(c->race, kind);
            }
    }
    // a village of one kind still is
    Json hc = worldCfg();
    hc["peoples"] = {{"human", 1.0}};
    World h = makeTestWorld(realContent(), hc, 11);
    for (const auto& [id, c] : h.characters()) CHECK(c.race.empty());
}

TEST(lr_sim_species_kind_saves) {
    World w = makeTestWorld(realContent(), worldCfg(), 5);
    Character* p = anAdult(w);
    CHECK(p != nullptr);
    setRace(realContent(), *p, "goblin");
    p->body.petrified = true;
    Json j = *p;
    Character q = j.get<Character>();
    CHECK_EQ(q.race, std::string("goblin"));
    CHECK_EQ(q.body.adultYears, realContent().raceOf("goblin")->adultYears);
    CHECK(q.body.petrified);
}

// A troll in the open turns to stone at day and wakes at night; its needs wait meanwhile.
TEST(lr_sim_species_troll_sleeps_in_the_sun) {
    World w = makeTestWorld(realContent(), worldCfg(), 5);
    Character* t = anAdult(w);
    CHECK(t != nullptr);
    const EntityId tid = t->id;
    setRace(realContent(), *t, "troll");
    t->body.ageDays = 60 * kDaysPerYear;
    t->pos = {t->pos.x + 900.0, t->pos.y + 900.0}; // far from any roof
    // run to midday
    while (w.isNight()) w.run(1);
    w.run(2);
    t = w.character(tid);
    CHECK(t->body.petrified);
    const double hunger = t->needs["hunger"];
    w.run(12);
    t = w.character(tid);
    CHECK(t->body.petrified);
    CHECK_EQ(t->needs["hunger"], hunger);
    CHECK(t->body.downUntil > w.now()); // the mind does not run
    while (!w.isNight()) w.run(1);
    w.run(1);
    t = w.character(tid);
    CHECK(!t->body.petrified);
    // magic hooks (AL-116): Light+ at night puts it to sleep, Shadow+ wakes it
    forcePetrify(w, *t);
    CHECK(petrified(*t));
    wakePetrified(w, *t);
    CHECK(!petrified(*t));
    int stone = 0;
    for (const auto& e : w.chronicleLog()) stone += e.type == "petrified";
    CHECK(stone >= 2);
}

// Trolls regenerate (not when burned); a human does not.
TEST(lr_sim_species_troll_regenerates) {
    World w = makeTestWorld(realContent(), worldCfg(), 5);
    Character* t = anAdult(w);
    const EntityId tid = t->id;
    setRace(realContent(), *t, "troll");
    t->body.health = 0.5;
    w.run(1);
    CHECK(w.character(tid)->body.health > 0.5);
}

TEST(lr_sim_species_realm_peoples_and_dragon) {
    World w = makeTestWorld(realContent(), worldCfg(), 7);
    installRealm(w);
    realmDaily(w);
    const RealmState s = loadRealm(w);
    const Settlement* lair = nullptr;
    const Settlement* hold = nullptr;
    for (const auto& [id, p] : w.settlements()) {
        if (p.peoples.count("dragon")) lair = &p;
        if (p.peoples.count("dwarf") && p.peoples.at("dwarf") > 0.9) hold = &p;
    }
    CHECK(lair != nullptr);
    CHECK(hold != nullptr);
    // the lair: one dragon, its hoard, its might
    const Character* dragon = w.person(lair->chief);
    CHECK(dragon != nullptr);
    CHECK_EQ(dragon->race, std::string("dragon"));
    CHECK(dragon->body.stage() != "child");
    CHECK(lair->wealth > 200.0);
    CHECK(lair->tagged("dragon_lair"));
    CHECK(realmStrength(w, s, *lair) >= 0.5 * realContent().raceOf("dragon")->might);
    // the hold: mostly dwarves, with dwarf names
    int dw = 0, all = 0;
    for (const auto& [id, f] : w.offstage())
        if (f.body.alive && f.status.settlement == hold->id) ++all, dw += f.race == "dwarf";
    CHECK(all > 0);
    CHECK(dw * 10 >= all * 8);
    // classified by who lives there (the first pass, then once a season)
    CHECK(w.settlements().at(hold->id).tagged("dwarven"));
}

TEST(lr_sim_species_monsters_on_the_ladder) {
    const Content& c = realContent();
    for (const char* m : {"varg", "cave_spider", "drowner", "harpy", "basilisk", "manticore", "griffin"}) {
        const SpeciesDef* s = c.speciesOf(m);
        CHECK(s != nullptr);
        if (!s) continue;
        CHECK(!s->loot.empty());
        for (const auto& [item, n] : s->loot) CHECK(c.items.count(item) == 1);
        const InteractionDef* hunt = c.interaction(std::string("hunt_") + m);
        CHECK(hunt != nullptr);
        if (hunt) CHECK_EQ(hunt->targetType, std::string(m));
    }
    CHECK(c.speciesOf("wolf")->sapience < c.speciesOf("griffin")->sapience);
}

// Built-in spells go through the magic core (AL-116): a dragon's breath on a place, a basilisk's gaze.
TEST(lr_sim_species_innate_spells_cast) {
    World w = makeTestWorld(realContent(), worldCfg(), 5);
    const Content& c = realContent();
    const Vec2 at = w.settlements().begin()->second.center;
    CHECK(innateStrike(w, nullptr, c.raceOf("dragon")->innateSpells, kNone, &at, "test"));
    CHECK(!c.speciesOf("basilisk")->innateSpells.empty());
    Character* p = anAdult(w);
    Json gaze = c.speciesOf("basilisk")->innateSpells;
    gaze[0]["chance"] = 1.0;
    CHECK(innateStrike(w, nullptr, gaze, p->id, nullptr, "test"));
}
