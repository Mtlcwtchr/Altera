// AL-136: the village's peoples come from its culture (cultures/*.json peoples,
// settle.json names the culture), one kind per household; the character's
// creation (content/ai/character_creation.json, sim/charcreate): points, the
// gift's price by the kind, the household takes the kind, the look saves, a
// kind the culture takes badly starts as a stranger.

#include <algorithm>
#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/charcreate.hpp"
#include "sim/species.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7, const Json& extra = Json::object()) {
    static const Json base = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    Json cfg = base;
    for (auto it = extra.begin(); it != extra.end(); ++it) cfg[it.key()] = it.value();
    World w = makeTestWorld(real(), cfg, seed);
    installAi(w);
    return w;
}

Command create(Json args) {
    Command c;
    c.source = "player:1";
    c.type = "create_character";
    args["player"] = 1;
    c.args = args;
    return c;
}

const Settlement& mapVillage(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return s;
    return w.settlements().begin()->second;
}

} // namespace

TEST(lr_sim_charcreate_culture_peoples) {
    const SpeciesCatalog& s = real().peoples;
    CHECK_EQ(s.mapCulture, std::string("fantasy_european"));
    // the map's mix is the culture's village mix: mostly humans, other kinds too
    const auto vill = culturePeoples(s, "fantasy_european", "village");
    CHECK(!vill.empty());
    CHECK(vill.count("human") && vill.at("human") > 0.4);
    CHECK(vill.size() >= 4);
    CHECK(s.mapMix == vill);
    // a template names a culture; its kind of settlement picks the culture's mix, else the default
    CHECK_EQ(s.templateCulture.at("dwarf_hold"), std::string("dwarven"));
    CHECK(s.templateMix.at("dwarf_hold").at("dwarf") > 0.9);
    CHECK(s.templateMix.at("town") == culturePeoples(s, "fantasy_european", "town"));
    // welcome: own kind 1, others by the file
    CHECK_EQ(cultureWelcome(s, "fantasy_european", "human"), 1.0);
    CHECK(cultureWelcome(s, "fantasy_european", "troll") < 0.5);
    CHECK(cultureWelcome(s, "dwarven", "goblin") < 0.1);
    // a written-out mix still works, and a culture with an override
    const SpeciesCatalog p = parseSpecies(
        {Json{{"id", "human"}, {"glyph", "h"}}, Json{{"id", "elf"}, {"glyph", "e"}}},
        {Json{{"id", "c1"}, {"species", "human"}, {"peoples", {{"human", 0.5}, {"elf", 0.5}}}}},
        Json{{"map", {{"culture", "c1"}, {"peoples", {{"elf", 0.0}}}}}, {"realm_templates", {{"t", {{"elf", 1.0}}}}}});
    CHECK(p.errors.empty());
    CHECK_EQ(p.mapMix.size(), std::size_t(1));
    CHECK(p.mapMix.count("human"));
    CHECK_EQ(p.templateMix.at("t").at("elf"), 1.0);
    ValidationReport r;
    validateSpecies(real(), r);
    CHECK(r.errors.empty());
}

TEST(lr_sim_charcreate_village_households_one_kind) {
    // over a few seeds the map's village has other kinds than humans, each household of one
    int nonHuman = 0;
    for (std::uint64_t seed : {3, 7, 11, 19, 23}) {
        World w = realWorld(seed);
        const Settlement& st = mapVillage(w);
        CHECK_EQ(st.culture, std::string("fantasy_european"));
        CHECK(!st.peoples.empty());
        for (EntityId fid : st.families) {
            const Family& f = w.families().at(fid);
            std::string kind = "?";
            for (EntityId m : f.members) {
                const Character* c = w.character(m);
                if (!c) continue;
                if (kind == "?") kind = c->race;
                CHECK_EQ(c->race, kind);
            }
            if (!kind.empty() && kind != "?") ++nonHuman;
        }
    }
    CHECK(nonHuman > 0);
    // a world may name its own culture
    World d = realWorld(7, Json{{"culture", "dwarven"}});
    CHECK_EQ(mapVillage(d).culture, std::string("dwarven"));
    int dwarves = 0, all = 0;
    for (const auto& [id, c] : d.characters())
        if (!c.isAnimal() && c.status.settlement == mapVillage(d).id) ++all, dwarves += c.race == "dwarf";
    CHECK(dwarves * 2 > all);
}

TEST(lr_sim_charcreate_rules_and_cost) {
    const Content& c = real();
    ValidationReport v;
    validateCreation(c, v);
    CHECK(v.errors.empty());
    const Json R = creationRules(c, "fantasy_european");
    CHECK(R["races"].size() >= 8);
    CHECK(R["talents"].size() >= 10);
    CHECK_EQ(R["attrs"]["ids"].size(), std::size_t(16)); // LR-141
    int giftHuman = 0, giftElf = 0;
    for (const auto& r : R["races"]) {
        if (r["id"] == "human") giftHuman = r["gift_cost"];
        if (r["id"] == "elf") giftElf = r["gift_cost"];
        CHECK(r["look"]["m"]["skin"].size() >= 1);
        CHECK(r["look"]["f"].contains("hair"));
        CHECK(!r["look"]["f"].contains("beard"));
        // LR-139: each kind its own cuts and beards
        const Json& mb = r["look"]["m"]["beard"];
        auto has = [](const Json& a, const char* v) { return std::find(a.begin(), a.end(), v) != a.end(); };
        if (r["id"] == "elf") CHECK(mb == Json::array({"no"}));
        if (r["id"] == "dwarf") CHECK(!has(mb, "no") && has(mb, "braided"));
        if (r["id"] == "human") CHECK(has(mb, "stubble") && has(r["look"]["f"]["hair"], "ponytail"));
    }
    CHECK(giftHuman > 0);
    CHECK(giftElf <= giftHuman); // elves are gifted more often: the gift costs them less
    std::string why;
    CHECK_EQ(creationCost(c, Json{{"race", "human"}, {"sex", "m"}}, &why), 0);
    // talents cost, flaws give back, nature is free
    const int t = creationCost(c, Json{{"race", "human"}, {"traits", {"quick_learner", "lazy", "curious"}}}, &why);
    CHECK_EQ(t, 3 - 2);
    // attributes: two steps up cost two
    CHECK_EQ(creationCost(c, Json{{"race", "human"}, {"attrs", {{"strength", 0.6}}}}, &why), 2);
    CHECK_EQ(creationCost(c, Json{{"race", "human"}, {"attrs", {{"strength", 0.4}}}}, &why), -2);
    // the rules
    CHECK_EQ(creationCost(c, Json{{"race", "dragon"}}, &why), -1);
    CHECK_EQ(why, std::string("race_not_playable"));
    CHECK_EQ(creationCost(c, Json{{"traits", {"brawny", "puny"}}}, &why), -1); // exclusive
    CHECK_EQ(creationCost(c, Json{{"traits", {"undead"}}}, &why), -1);
    CHECK_EQ(creationCost(c, Json{{"attrs", {{"strength", 0.95}}}}, &why), -1);
    CHECK_EQ(creationCost(c, Json{{"traits", {"gifted", "quick_learner", "lucky"}}}, &why), -1);
    CHECK_EQ(why, std::string("over_points"));
    CHECK_EQ(creationCost(c, Json{{"sex", "f"}, {"look", {{"beard", "yes"}}}}, &why), -1);
    CHECK_EQ(creationCost(c, Json{{"age", 5}}, &why), -1);
}

TEST(lr_sim_charcreate_makes_the_person) {
    World w = realWorld();
    const Json choice = {{"backstory", "hunter"},
                         {"race", "elf"},
                         {"sex", "f"},
                         {"age", 30},
                         {"name", "Ялина"},
                         {"lang", "ru"},
                         {"traits", {"gifted", "curious"}},
                         {"attrs", {{"perception", 0.6}, {"strength", 0.4}}},
                         {"look", {{"hair", "braids"}, {"hair_colour", "#d8a848"}, {"cloth", "#4a5a3a"}}}};
    const EntityId who = creationCandidate(w, 'f');
    CHECK(who != kNone);
    const Command done = w.applyNow(create(choice));
    CHECK(done.result.rfind("rejected", 0) != 0);
    const Character& c = *w.character(who);
    CHECK(c.controller.isPlayer());
    CHECK_EQ(c.race, std::string("elf"));
    CHECK_EQ(c.body.sex, 'f');
    CHECK_EQ(c.backstory, std::string("hunter"));
    CHECK_EQ(c.name, std::string("Ялина"));
    CHECK(c.hasTrait("gifted"));
    CHECK(c.hasTrait("curious"));
    CHECK(c.hasTrait("keen_eyed")); // the hunter's own
    CHECK_EQ(c.traits.size(), std::size_t(3));
    CHECK(c.body.lifeYears() >= 29 && c.body.lifeYears() <= 31);
    CHECK(c.body.ageDays / kDaysPerYear > 30); // an elf of 30 in human years is older in years
    // the backstory's shift on top of the chosen value
    CHECK(std::fabs(c.attrs.at("perception") - 0.75) < 1e-6);
    CHECK(std::fabs(c.attrs.at("strength") - 0.4) < 1e-6);
    CHECK_EQ(c.look.at("hair"), std::string("braids"));
    // the household takes the kind
    if (const Family* f = w.familyOf(c))
        for (EntityId m : f->members)
            if (const Character* o = w.character(m)) CHECK_EQ(o->race, std::string("elf"));
    // the look and the culture save
    World back(real(), 1);
    back.loadJson(w.toJson());
    CHECK_EQ(back.character(who)->look.at("cloth"), std::string("#4a5a3a"));
    CHECK_EQ(mapVillage(back).culture, std::string("fantasy_european"));
    // a bad choice is refused and nobody is led
    World w2 = realWorld();
    const Command bad = w2.applyNow(create(Json{{"race", "dragon"}}));
    CHECK(bad.result.rfind("rejected:", 0) == 0);
    // the world goes on with them
    w.run(kTicksPerDay / 4);
    CHECK(w.character(who)->body.alive);
}

TEST(lr_sim_charcreate_stranger) {
    // a troll among humans starts cold with the village; a human does not
    auto villageTrust = [](const World& w, EntityId id) {
        const Character& c = *w.character(id);
        double sum = 0;
        int n = 0;
        for (const auto& [oid, o] : w.characters())
            if (oid != id && !o.isAnimal() && o.status.settlement == c.status.settlement && o.status.family != c.status.family) {
                auto it = o.relations.find(id);
                sum += it == o.relations.end() ? 0.0 : it->second.trust;
                ++n;
            }
        return n ? sum / n : 0.0;
    };
    World a = realWorld();
    const EntityId ida = creationCandidate(a, 'm');
    CHECK(a.applyNow(create(Json{{"race", "human"}})).result.rfind("rejected", 0) != 0);
    World b = realWorld();
    const EntityId idb = creationCandidate(b, 'm');
    CHECK(b.applyNow(create(Json{{"race", "troll"}})).result.rfind("rejected", 0) != 0);
    CHECK(villageTrust(b, idb) < villageTrust(a, ida) - 0.05);
}
