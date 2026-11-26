// LR-166: regional peoples of the mythic world (gdd/setting/LIVELYREALMS_SETTING_v0.3_MYTHIC_WORLD_PROPOSAL.md
// §5, §8): peoples built on a base kind, kin, one-sex kinds, belts in worldgen and the realm,
// old saves moved to today's ids, adults-only intimacy in the cultures.

#include <algorithm>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/charcreate.hpp"
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

const Settlement& mapVillage(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return s;
    return w.settlements().begin()->second;
}

} // namespace

TEST(lr_sim_peoples_base_and_kin) {
    const Content& c = realContent();
    const RaceDef* he = c.raceOf("hellene");
    const RaceDef* hu = c.raceOf("human");
    CHECK(he && hu);
    // the base's numbers where the people says nothing, its own where it does
    CHECK_EQ(he->adultYears, hu->adultYears);
    CHECK_EQ(he->lifespanYears, hu->lifespanYears);
    CHECK(he->aptitudes.count("eloquence") && he->aptitudes.at("eloquence").learn > 1.0);
    CHECK(he->interbreed.count("elf")); // the human's interbreed table, merged
    CHECK_EQ(he->kin, std::string("human"));
    CHECK_EQ(c.raceOf("northman")->kin, std::string("human"));
    CHECK_EQ(c.raceOf("elf")->kin, std::string("elf"));
    CHECK_EQ(c.raceOf("alv")->kin, std::string("elf"));     // alv is built on elf
    CHECK_EQ(c.raceOf("vila")->base, std::string("nymph"));
    CHECK(!hu->settles);
    CHECK(he->settles);
    CHECK(std::find(he->region.begin(), he->region.end(), "mediterranean") != he->region.end());
    // kin: a hellene and a northman have the father's people's child, an elf and a hellene a half-elf
    CHECK_EQ(childRace(c, "hellene", "northman"), std::string("northman"));
    CHECK_EQ(childRace(c, "northman", "hellene"), std::string("hellene"));
    CHECK_EQ(childRace(c, "elf", "woodfolk"), std::string("half_elf"));
    CHECK_EQ(childRace(c, "woodfolk", "elf"), std::string("half_elf"));
    CHECK_EQ(childRace(c, "steppe_folk", "orc"), std::string("half_orc"));
    CHECK(childRace(c, "hellene", "troll").empty());
    // relations written for the kin hold for its peoples
    CHECK_EQ(raceBias(c, "elf", "hellene"), raceBias(c, "elf", "human"));
    CHECK(raceBias(c, "dwarf", "orc") < -0.3);
    CHECK(raceBias(c, "hellene", "cyclops") < -0.2);
    // and a culture's welcome for "human"
    CHECK_EQ(cultureWelcome(c.peoples, "forest_elf", "hellene"), cultureWelcome(c.peoples, "forest_elf", "human"));
}

TEST(lr_sim_peoples_one_sex_kinds) {
    const Content& c = realContent();
    CHECK_EQ(c.raceOf("nymph")->sexes, std::string("f"));
    CHECK_EQ(c.raceOf("satyr")->sexes, std::string("m"));
    CHECK_EQ(raceForSex(c, "nymph", 'm'), std::string("satyr"));
    CHECK_EQ(raceForSex(c, "satyr", 'f'), std::string("nymph"));
    CHECK_EQ(raceForSex(c, "nymph", 'f'), std::string("nymph"));
    CHECK_EQ(raceForSex(c, "vila", 'm'), std::string("woodfolk"));
    // a nymph's daughter is a nymph; her son is his father's people (a satyr's son a satyr)
    CHECK_EQ(childRace(c, "nymph", "hellene", 'f'), std::string("nymph"));
    CHECK_EQ(childRace(c, "nymph", "hellene", 'm'), std::string("hellene"));
    CHECK_EQ(childRace(c, "nymph", "satyr", 'm'), std::string("satyr"));
    CHECK_EQ(childRace(c, "nymph", "satyr", 'f'), std::string("nymph"));
    // a satyr's daughter by a woman is her mother's people
    CHECK_EQ(childRace(c, "hellene", "satyr", 'f'), std::string("hellene"));
    CHECK_EQ(childRace(c, "hellene", "satyr", 'm'), std::string("satyr"));
    CHECK_EQ(childRace(c, "vila", "woodfolk", 'm'), std::string("woodfolk"));
    // setRace keeps a person of the wrong sex out of a one-sex kind
    World w = makeTestWorld(c, worldCfg(), 3);
    for (auto& [id, p] : w.characters())
        if (!p.isAnimal()) {
            setRace(c, p, "nymph");
            CHECK_EQ(raceId(c, p), std::string(p.body.sex == 'f' ? "nymph" : "satyr"));
        }
    // character creation does not let a man be a nymph
    std::string why;
    CHECK(creationCost(c, Json{{"race", "nymph"}, {"sex", "m"}}, &why) < 0);
    CHECK_EQ(why, std::string("sex_not_for_race"));
    CHECK_EQ(creationCost(c, Json{{"race", "nymph"}, {"sex", "f"}}, &why), 0);
}

TEST(lr_sim_peoples_belt_villages) {
    const Content& c = realContent();
    CHECK(c.peoples.belts.size() >= 5);
    CHECK_EQ(c.peoples.homeBelt, std::string("mediterranean"));
    // every culture has a belt, every belt its cultures
    for (const auto& [id, cu] : c.peoples.cultures) CHECK(!cu.belt.empty() && c.peoples.belts.count(cu.belt));
    // a new game in a belt starts a village of that belt's people
    for (const auto& [belt, people] : std::vector<std::pair<std::string, std::string>>{
             {"mediterranean", "hellene"}, {"north", "northman"}, {"forest", "woodfolk"}, {"steppe", "steppe_folk"}, {"nile", "nilote"}}) {
        Json cfg = worldCfg();
        cfg["belt"] = belt;
        const World w = makeTestWorld(c, cfg, 5);
        CHECK_EQ(cultureBelt(c.peoples, mapVillage(w).culture), belt);
        int own = 0, n = 0;
        for (const auto& [id, p] : w.characters())
            if (!p.isAnimal()) ++n, own += raceId(c, p) == people;
        CHECK(n > 0);
        CHECK(own * 2 >= n); // mostly the belt's people
        for (const auto& [id, p] : w.characters()) CHECK(p.isAnimal() || raceId(c, p) != "human");
    }
}

TEST(lr_sim_peoples_realm_by_belts) {
    const Content& c = realContent();
    World w = makeTestWorld(c, worldCfg(), 7);
    installRealm(w);
    realmDaily(w);
    std::map<std::string, int> byBelt;
    std::map<std::string, Vec2> centre;
    for (const auto& [id, p] : w.settlements()) {
        const std::string b = cultureBelt(c.peoples, p.culture);
        CHECK(!b.empty());
        ++byBelt[b];
        centre[b].x += p.km.x, centre[b].y += p.km.y;
    }
    for (const char* b : {"mediterranean", "forest", "north", "steppe"}) CHECK(byBelt[b] >= 3);
    for (auto& [b, v] : centre) v.x /= byBelt[b], v.y /= byBelt[b];
    // the North lies north of the Middle Sea, the Steppe east of the Woods
    CHECK(centre["north"].y > centre["mediterranean"].y + 50);
    CHECK(centre["steppe"].x > centre["forest"].x + 50);
    // a realm settlement's people are its culture's
    for (const auto& [id, f] : w.offstage()) {
        if (!f.body.alive || f.isAnimal()) continue;
        const auto st = w.settlements().find(f.status.settlement);
        if (st == w.settlements().end() || st->second.culture.empty()) continue;
        CHECK(raceId(c, f) != "human");
        if (st->second.culture == "dwarf_hold") CHECK(raceId(c, f) == "dwarf" || raceId(c, f) == "northman");
        if (raceId(c, f) == "nymph") CHECK_EQ(f.body.sex, 'f');
        if (raceId(c, f) == "satyr") CHECK_EQ(f.body.sex, 'm');
    }
}

TEST(lr_sim_peoples_old_save_migrates) {
    const Content& c = realContent();
    World w = makeTestWorld(c, worldCfg(), 9);
    // make it look like a save from before LR-166: humans of fantasy_european, an orcish settlement
    EntityId someone = kNone;
    for (auto& [id, p] : w.characters())
        if (!p.isAnimal()) {
            p.race = "";
            p.culture = "fantasy_european";
            if (someone == kNone) someone = id;
        }
    for (auto& [id, s] : w.settlements()) {
        s.culture = "fantasy_european";
        s.peoples = {{"human", 0.7}, {"dwarf", 0.3}};
    }
    const Json snap = w.toJson();
    World back(c, 1);
    back.loadJson(snap);
    const Character* p = back.character(someone);
    CHECK(p != nullptr);
    CHECK_EQ(p->culture, std::string("slav_village"));
    CHECK_EQ(raceId(c, *p), std::string("woodfolk"));
    CHECK_EQ(p->body.ageDays, w.character(someone)->body.ageDays);
    for (const auto& [id, s] : back.settlements()) {
        CHECK_EQ(s.culture, std::string("slav_village"));
        CHECK(!s.peoples.count("human"));
        CHECK(s.peoples.count("woodfolk") && s.peoples.count("dwarf"));
    }
    // a save of today does not change on a load
    World again(c, 1);
    again.loadJson(back.toJson());
    CHECK(migratePeoples(again) == 0);
}

TEST(lr_sim_peoples_cultures_adults_only) {
    const Content& c = realContent();
    ValidationReport r;
    validateSpecies(c, r);
    for (const auto& e : r.errors) std::printf("  peoples error: %s\n", e.c_str());
    CHECK(r.ok());
    for (const auto& [id, cu] : c.peoples.cultures) {
        CHECK(cu.data.contains("intimacy"));
        CHECK(cu.data["intimacy"].value("adults_only", false));
        CHECK(cu.data["intimacy"].value("min_age_years", 0.0) >= 18.0);
    }
    // a culture that let children in is refused
    Json bad = Json{{"id", "bad"}, {"species", "human"}, {"intimacy", {{"adults_only", false}}}};
    const SpeciesCatalog s = parseSpecies({Json{{"id", "human"}, {"glyph", "h"}}}, {bad}, Json::object());
    Content tmp;
    tmp.peoples = s;
    ValidationReport r2;
    validateSpecies(tmp, r2);
    CHECK(!r2.ok());
}
