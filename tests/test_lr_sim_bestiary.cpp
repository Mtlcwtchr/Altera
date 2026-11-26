// LR-170: the myth creatures of the belts (sim/bestiary.hpp, content/creatures): data,
// belts, the behaviours each a field (night, the lair's guard, the lure, heads growing
// back, the undead rising), their bodies in the combat core, the lairs on the world map.
#include <set>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/bestiary.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/worldgen.hpp"
#include "sim/worldmap.hpp"

using namespace lr::sim;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& beastContent() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

World beastWorld(std::uint64_t seed = 5) {
    static const Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    return makeTestWorld(beastContent(), cfg, seed);
}

Character* anAdult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") return &c;
    return nullptr;
}

// a land point with water within `r` m and none at the point itself
bool shore(const World& w, Vec2& out) {
    const double r = w.map().cellM;
    const MapGrid& m = w.map();
    for (int y = 2; y < m.h - 2; ++y)
        for (int x = 2; x < m.w - 2; ++x) {
            const Vec2 p{(x + 0.5) * m.cellM, (y + 0.5) * m.cellM};
            if (m.waterAt(p)) continue;
            for (int k = 0; k < 4; ++k) {
                const Vec2 q{p.x + (k == 0 ? r : k == 1 ? -r : 0), p.y + (k == 2 ? r : k == 3 ? -r : 0)};
                if (m.waterAt(q)) return out = p, true;
            }
        }
    return false;
}

Character& addBeast(World& w, const std::string& species, Vec2 at, int heads) {
    Character a;
    a.species = species;
    a.name = species;
    a.group = heads;
    a.pos = at;
    a.den = at;
    a.needs["hunger"] = 0.9;
    a.body.ageDays = 2 * kDaysPerYear;
    return w.addCharacter(std::move(a));
}

void stepTo(World& w, bool night) {
    for (int i = 0; i < kTicksPerDay && w.isNight() != night; ++i) w.step();
}

// people far away from `p`, so a lure or a guard sees only the one we place
void clearAround(World& w, Vec2 p, const Character* keep) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && &c != keep && dist(c.pos, p) < 600.0) c.pos = {p.x + 5000.0, p.y + 5000.0};
}

AnimalTick peopleTick(World& w) {
    AnimalTick t;
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) t.people.push_back(&c);
    return t;
}

} // namespace

TEST(lr_bestiary_content) {
    const Content& c = beastContent();
    for (const auto& e : checkBestiary(c)) {
        std::printf("  %s\n", e.c_str());
        CHECK(false);
    }
    const Loc loc = Loc::load(kRoot + "/content/loc");
    std::map<std::string, int> perBelt;
    std::set<std::string> lairs;
    int mythic = 0, undead = 0;
    for (const auto& [id, s] : c.species) {
        if (!s.mythic.set) continue;
        ++mythic;
        undead += s.mythic.undead;
        for (const auto& b : s.mythic.belts) ++perBelt[b];
        CHECK(loc.has("ru", "species." + id + ".name"));
        const std::string poi = s.mythic.lair.is_object() ? s.mythic.lair.value("poi", std::string()) : std::string();
        if (!poi.empty()) {
            CHECK(lairs.insert(poi).second);
            CHECK(loc.has("ru", "worldmap.poi." + poi + ".name"));
            if (s.mythic.lair.contains("raid")) CHECK(loc.has("ru", "chronicle.lair_raid." + id));
        }
    }
    CHECK(mythic >= 20);
    CHECK(undead >= 6);
    for (const char* b : {"mediterranean", "forest", "north", "steppe", "nile"}) CHECK(perBelt[b] >= 3);
    for (const char* k : {"siren", "gorgon", "hydra", "draugr", "leshy", "upyr", "rusalka", "zmey_gorynych", "albasty", "mummy"})
        CHECK(c.speciesOf(k) && c.speciesOf(k)->mythic.set);
}

TEST(lr_bestiary_belts) {
    const Content& c = beastContent();
    // a culture's own belt first (LR-166), else the fallback map of the old ones
    CHECK_EQ(beltOfCulture(c, "dwarven"), std::string("north"));
    CHECK_EQ(beltOfCulture(c, "orcish"), std::string("steppe"));
    CHECK_EQ(beltOfCulture(c, "no_such_culture"), std::string(""));
    const SpeciesDef& siren = *c.speciesOf("siren");
    CHECK(creatureInBelt(siren, "mediterranean"));
    CHECK(!creatureInBelt(siren, "north"));
    CHECK(creatureInBelt(*c.speciesOf("deer"), "north"));   // no belts: everywhere
    // the local map spawns only its belt's creatures
    World w = beastWorld();
    const std::string belt = localBelt(w);
    CHECK(!belt.empty());
    for (const auto& [id, a] : w.characters())
        if (a.isAnimal())
            if (const SpeciesDef* s = c.speciesOf(a.species)) CHECK(creatureInBelt(*s, belt));
}

TEST(lr_bestiary_night_creature_keeps_to_its_lair_by_day) {
    World w = beastWorld();
    Vec2 den{w.map().w * w.map().cellM * 0.5, w.map().h * w.map().cellM * 0.5};
    Character& u = addBeast(w, "upyr", den, 8);   // heads to spare: the sun may take some on the way home (rng by id)
    stepTo(w, false);
    u.pos = {den.x + 400.0, den.y};
    const double far = dist(u.pos, den);
    for (int i = 0; i < 6; ++i) animalThink(w, u, AnimalTick{});
    CHECK(dist(u.pos, den) < far);   // home before the sun gets it
    // caught out a whole day it burns
    int lost = 0;
    for (int i = 0; i < 60 && u.body.alive; ++i) {
        u.pos = {den.x + 400.0, den.y};
        const int g = u.group;
        w.step();
        animalThink(w, u, AnimalTick{});
        lost += g - u.group;
    }
    CHECK(lost >= 1);
}

TEST(lr_bestiary_guard_falls_on_who_comes_near) {
    World w = beastWorld();
    Character* man = anAdult(w);
    CHECK(man);
    const Vec2 den{man->pos.x + 60.0, man->pos.y};
    clearAround(w, den, man);
    Character& d = addBeast(w, "draugr", den, 1);
    const double h = man->body.health;
    const size_t mem = man->memory.size();
    bool fought = false;
    for (int i = 0; i < 40 && !fought; ++i) {
        animalThink(w, d, peopleTick(w));
        fought = man->body.health < h || man->memory.size() > mem || d.hasMoodlet("alarmed", w.now());
    }
    CHECK(fought);
    // nobody near the barrow: it stays home, the hunger-less undead dares no one
    Character& k = addBeast(w, "draugr", {den.x + 3000.0, den.y}, 1);
    k.pos = {den.x + 3000.0 + 900.0, den.y};
    for (int i = 0; i < 10; ++i) animalThink(w, k, peopleTick(w));
    CHECK(dist(k.pos, k.den) < 900.0);
}

TEST(lr_bestiary_siren_song_draws_a_lone_one) {
    World w = beastWorld();
    Vec2 rock;
    if (!shore(w, rock)) {
        std::printf("  (no shore on the test map)\n");
        return;
    }
    Character* man = anAdult(w);
    CHECK(man);
    clearAround(w, rock, man);
    man->pos = {rock.x + 150.0, rock.y};
    if (w.map().waterAt(man->pos)) man->pos = {rock.x, rock.y + 150.0};
    man->attrsNow["willpower"] = 0.0;   // no will to hold
    Character& s = addBeast(w, "siren", rock, 3);
    const double start = dist(man->pos, rock);
    for (int i = 0; i < 80; ++i) {
        w.step();
        animalThink(w, s, peopleTick(w));
    }
    CHECK(dist(man->pos, rock) < start - 20.0);
    CHECK(man->hasMoodlet("lured", w.now()) || dist(man->pos, rock) <= 6.0 || man->memory.size() > 0);
}

TEST(lr_bestiary_leshy_leads_astray_not_kills) {
    World w = beastWorld();
    Character* man = anAdult(w);
    CHECK(man);
    const Vec2 den{man->pos.x + 120.0, man->pos.y};
    clearAround(w, den, man);
    man->attrsNow["willpower"] = 0.0;
    Character& l = addBeast(w, "leshy", den, 1);
    for (int i = 0; i < 144 && !man->hasMoodlet("lost_in_woods", w.now()); ++i) {
        l.pos = den;   // it waits in its thicket
        w.step();
        clearAround(w, den, man);
        animalThink(w, l, peopleTick(w));
    }
    CHECK(man->hasMoodlet("lost_in_woods", w.now()));
    CHECK(man->body.alive);
}

TEST(lr_bestiary_hydra_heads_grow_back_unless_burned) {
    World w = beastWorld();
    const Vec2 p{100.0, 100.0};
    Character& h = addBeast(w, "hydra", p, 3);
    const int days = beastContent().speciesOf("hydra")->mythic.regrowDays;
    CHECK(days > 0);
    for (int d = 0; d < days; ++d) animalsDaily(w);
    CHECK_EQ(h.group, 4);
    h.moodlets.push_back({"burned", 0.0, w.now() + 30 * kTicksPerDay});
    for (int d = 0; d < days * 3; ++d) animalsDaily(w);
    CHECK_EQ(h.group, 4);
}

TEST(lr_bestiary_undead_rise_unless_laid_to_rest) {
    World w = beastWorld();
    const Vec2 p{200.0, 200.0};
    Character& d = addBeast(w, "draugr", p, 1);
    Character& e = addBeast(w, "draugr", {p.x + 50.0, p.y}, 1);
    loseHeads(w, d, 1, "combat");
    loseHeads(w, e, 1, "laid_to_rest");
    CHECK(!d.body.alive && !e.body.alive);
    const int days = beastContent().speciesOf("draugr")->mythic.riseDays;
    for (int i = 0; i < days; ++i) animalsDaily(w);
    CHECK(d.body.alive);
    CHECK(d.group >= 1);
    CHECK(!e.body.alive);
}

TEST(lr_bestiary_bodies_in_the_combat_core) {
    World w = beastWorld();
    const CombatBridge* b = combatBridge(w.content());
    CHECK(b);
    if (!b) return;
    Character& h = addBeast(w, "hydra", {10.0, 10.0}, 5);
    const auto fh = fighterOf(*b, h, "fighting");
    CHECK_EQ(fh.weapon, std::string("great_jaws"));
    CHECK(fh.armor.size() == 1 && fh.armor[0] == "scales");
    Character& l = addBeast(w, "nemean_lion", {20.0, 10.0}, 1);
    CHECK(fighterOf(*b, l, "fighting").armor == std::vector<std::string>{"stone_hide"});
    Character& wolf = addBeast(w, "wolf", {30.0, 10.0}, 3);   // a plain beast keeps the bridge's fangs
    CHECK_EQ(fighterOf(*b, wolf, "fighting").weapon, std::string("fangs"));
}

TEST(lr_bestiary_lairs_on_the_world_map) {
    World w = beastWorld(42);
    installRealm(w);
    installWorldMap(w);
    w.run(kTicksPerDay);
    realmDaily(w);
    CHECK(worldMapEnsure(w));
    const WorldMapState s = loadWorldMap(w);
    std::set<std::string> belts;
    for (const auto& [id, p] : w.settlements()) belts.insert(beltOfCulture(w.content(), p.culture));
    int lairs = 0;
    for (const auto& p : s.pois) {
        if (p.creature.empty()) continue;
        ++lairs;
        const SpeciesDef* sp = w.content().speciesOf(p.creature);
        CHECK(sp);
        bool ok = sp->mythic.belts.empty();
        for (const auto& b : sp->mythic.belts) ok |= belts.count(b) > 0;
        CHECK(ok);   // only creatures of the belts that live here
        CHECK_EQ(p.kind, sp->mythic.lair.value("poi", std::string()));
        CHECK(!s.kindAt(p.km).empty());
    }
    CHECK(lairs >= 2);
    // the save keeps who lives there
    WorldMapState back;
    from_json(Json(s), back);
    int kept = 0;
    for (const auto& p : back.pois) kept += !p.creature.empty();
    CHECK_EQ(kept, lairs);
    // raids: over two years some settlement nearby loses goods to a lair
    double before = 0.0;
    for (const auto& [id, p] : w.settlements()) before += p.full() ? 0.0 : p.wealth;
    int raids = 0;
    for (int d = 1; d <= 720; ++d) raids += worldMapLairRaids(w, d);
    double after = 0.0;
    for (const auto& [id, p] : w.settlements()) after += p.full() ? 0.0 : p.wealth;
    std::printf("  lairs %d, raids in two years %d, wealth %.0f -> %.0f\n", lairs, raids, before, after);
    CHECK(raids >= 1);
    CHECK(after < before);
}
