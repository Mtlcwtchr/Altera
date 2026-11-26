// LR-168: things of the belts (sim/regional.hpp, content/ai/regional): a person of a belt
// starts in its clothes and knows its crafts, a centaur is not given trousers, a carried
// shield goes into the fight, belt-bound sources grow only where their belt lives, and the
// pack's things are whole (loc, gear, pictures, crafts that make them).

#include <algorithm>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/gear.hpp"
#include "sim/regional.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& regContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World regWorld() {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(regContent(), cfg, 7);
    installAi(w);
    return w;
}

Character fresh(World& w, const std::string& culture, char sex, const std::string& race = "") {
    Character* base = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") base = &c;
    Character c = *base;
    c.id = 0;
    c.worn.clear();
    c.inventory.clear();
    c.knowledge.clear();
    c.culture = culture;
    c.body.sex = sex;
    if (!race.empty()) c.race = race;
    return c;
}

} // namespace

TEST(lr_sim_regional_belt_of_culture) {
    const Content& c = regContent();
    CHECK_EQ(regionalBelt(c, "polis_hellene"), std::string("mediterranean"));
    CHECK_EQ(regionalBelt(c, "north_folk"), std::string("north"));
    CHECK_EQ(regionalBelt(c, "steppe_rider"), std::string("steppe"));
    CHECK_EQ(regionalBelt(c, "no_such_culture"), std::string());
}

TEST(lr_sim_regional_clothes_and_crafts_of_the_belt) {
    World w = regWorld();
    Character m = fresh(w, "polis_hellene", 'm');
    giveBaseClothes(w, m);
    CHECK_EQ(m.worn["torso:under"], std::string("chiton"));
    CHECK_EQ(m.worn["feet:shoes"], std::string("sandals"));
    CHECK(m.knowledge.count("craft_mediterranean") == 1);
    Character f = fresh(w, "polis_hellene", 'f');
    giveBaseClothes(w, f);
    CHECK_EQ(f.worn["torso:under"], std::string("peplos"));
    Character s = fresh(w, "steppe_clan", 'm');
    giveBaseClothes(w, s);
    CHECK_EQ(s.worn["legs:under"], std::string("riding_trousers"));
    CHECK_EQ(s.worn["feet:shoes"], std::string("soft_boots"));
    CHECK(s.knowledge.count("craft_steppe") == 1);
    CHECK(s.knowledge.count("craft_mediterranean") == 0);
    // a culture's own over its belt's
    Character c = fresh(w, "celtic_grove", 'm');
    giveBaseClothes(w, c);
    CHECK_EQ(c.worn["torso:over"], std::string("plaid_cloak"));
}

TEST(lr_sim_regional_centaur_has_no_trousers) {
    World w = regWorld();
    Character k = fresh(w, "steppe_rider", 'm', "steppe_centaur");
    giveBaseClothes(w, k);
    CHECK(!k.worn.count("legs:under"));
    CHECK(!k.worn.count("feet:shoes"));
    CHECK(k.inventory.count("riding_trousers") == 0);
    CHECK(k.worn.count("torso:under") == 1);
}

TEST(lr_sim_regional_shield_goes_into_the_fight) {
    World w = regWorld();
    const CombatBridge* b = combatBridge(w.content());
    if (!b) return;
    Character c = fresh(w, "polis_hellene", 'm');
    c.inventory["dory"] = 1;
    c.inventory["hoplon"] = 1;
    const auto s = fighterOf(*b, c, "fighting");
    CHECK_EQ(s.shield, std::string("hoplon"));
    CHECK_EQ(s.weapon, std::string("spear"));
    c.inventory.erase("hoplon");
    CHECK(fighterOf(*b, c, "fighting").shield.empty());
}

TEST(lr_sim_regional_sources_grow_in_their_belt) {
    World w = regWorld();   // the test world's culture is of the forest belt
    std::set<std::string> belted;
    for (const auto& [id, d] : w.content().objects)
        if (!d.belts.empty()) belted.insert(id);
    CHECK(belted.count("olive_grove") == 1);
    for (const auto& [id, o] : w.objects()) CHECK(!belted.count(o.type));
}

TEST(lr_sim_regional_things_are_whole) {
    const Content& c = regContent();
    const Json items = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/ai/regional/items.json");
    const GearRules* g = gearRules(c);
    CHECK(g != nullptr);
    std::set<std::string> made;
    for (const auto& [id, a] : c.interactions)
        for (const auto& e : a.eff)
            if (e.subject == "has" && e.op == '+' && !e.onTarget) made.insert(e.key);
    int n = 0;
    for (const auto& it : items["items"]) {
        const std::string id = it["id"].get<std::string>();
        ++n;
        CHECK(c.items.count(id) == 1);
        CHECK(g->items.count(id) == 1);
        if (!made.count(id)) {
            // only what a source in the world gives or a caravan brings is not made
            static const std::set<std::string> found = {"olives", "grapes", "figs", "dates", "natron", "wormwood", "silphium",
                                                        "frankincense", "myrrh", "amber", "sable_fur", "purple_dye", "papyrus",
                                                        "mandrake", "mistletoe", "henbane"};
            if (!found.count(id)) {
                std::printf("  not made: %s\n", id.c_str());
                CHECK(!"a thing of the pack nobody makes");
            }
        }
    }
    CHECK(n > 60);
    // offerings carry their kind
    CHECK(std::count(c.items.at("wine").tags.begin(), c.items.at("wine").tags.end(), "offering_libation") == 1);
}
