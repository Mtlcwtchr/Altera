// LR-155: cultures and faiths (sim/culture.hpp): what people are given, how a settlement's
// customs grow and fade with practice, newcomers taking the place's ways, conversion,
// the goals bent by culture and faith, the save.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/culture.hpp"
#include "sim/magic_society.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

std::vector<Character*> villagers(World& w, EntityId sid) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement == sid) out.push_back(&c);
    return out;
}

EntityId firstSettlement(const World& w) { return w.settlements().empty() ? kNone : w.settlements().begin()->first; }

void remember(World& w, Character& c, const std::string& type) {
    MemoryEntry m;
    m.type = type;
    m.salience = 0.5;
    w.remember(c, m);
}

} // namespace

TEST(lr_sim_culture_rules_load_and_validate) {
    const CultureRules& r = cultureRules(realContent());
    CHECK(r.loaded);
    CHECK(r.errors.empty());
    CHECK(r.faiths.size() >= 8);
    CHECK(r.traditions.count("daily_prayer") == 1);
    CHECK(r.faithDef("pale_mourners") && r.faithDef("pale_mourners")->heresy);
    ValidationReport rep;
    validateCulture(realContent(), rep);
    for (const auto& e : rep.errors) std::printf("  %s\n", e.c_str());
    CHECK(rep.errors.empty());
    // every culture keeps a faith
    for (const auto& [id, cd] : realContent().peoples.cultures) CHECK(!cultureFaith(realContent(), id).empty());
}

TEST(lr_sim_culture_people_get_culture_and_faith) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    CHECK(sid != kNone);
    const std::string sc = settlementCulture(w, sid);
    CHECK(!sc.empty());
    auto people = villagers(w, sid);
    CHECK(!people.empty());
    // before the first day: derived
    for (Character* c : people) CHECK(!cultureOf(w, *c).empty() && !faithOf(w, *c).empty());
    w.run(kTicksPerDay + 1);
    int own = 0;
    for (Character* c : villagers(w, sid)) {
        CHECK(!c->culture.empty() && !c->faith.empty());
        CHECK(c->piety >= 0.0 && c->piety <= 1.0);
        own += c->culture == sc;
    }
    CHECK(own > 0);
    // the settlement starts with its culture's customs
    const auto tr = settlementTraditions(w, sid);
    CHECK(!tr.empty());
    const auto vals = settlementValues(w, sid);
    CHECK(!vals.empty());
    const SettlementBelief b = settlementBelief(w, sid);
    CHECK(b.residents > 0 && !b.faiths.empty());
}

TEST(lr_sim_culture_customs_grow_with_practice_and_fade_without) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    w.run(kTicksPerDay + 1);   // the state is set
    auto people = villagers(w, sid);
    const double s0 = settlementTraditions(w, sid).at("daily_prayer").strength;
    // a day when nobody prays (the minds are not run by cultureDaily itself)
    for (Character* c : people) c->memory.clear();
    cultureDaily(w);
    const double s1 = settlementTraditions(w, sid).at("daily_prayer").strength;
    CHECK(s1 < s0);
    // a day when half the village prays
    for (std::size_t i = 0; i < people.size(); i += 2) remember(w, *people[i], "prayed");
    cultureDaily(w);
    const auto t2 = settlementTraditions(w, sid).at("daily_prayer");
    CHECK(t2.strength > s1);
    CHECK(t2.practised > 0.3);
    // a custom nobody here had takes root when practised
    CHECK(settlementTraditions(w, sid).count("blood_feud") == 0);
    for (Character* c : people) {
        c->memory.clear();
        remember(w, *c, "duel");
    }
    cultureDaily(w);
    CHECK(settlementTraditions(w, sid).count("blood_feud") == 1);
}

TEST(lr_sim_culture_newcomer_takes_the_place_ways) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    w.run(kTicksPerDay + 1);
    const std::string sc = settlementCulture(w, sid);
    Character* c = villagers(w, sid).front();
    c->culture = sc == "steppe_rider" ? "dwarf_hold" : "steppe_rider";
    c->cultureBond = cultureRules(w.content()).person.switchBelow + 0.0001;
    cultureDaily(w);
    CHECK_EQ(c->culture, sc);
    CHECK(c->cultureBond > 0.3);
    CHECK(std::any_of(w.chronicleLog().begin(), w.chronicleLog().end(), [](const auto& e) { return e.code == "chronicle.culture.adopted"; }));
}

TEST(lr_sim_culture_grief_calls_the_pale_mourners) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    w.run(kTicksPerDay + 1);
    Character* c = villagers(w, sid).front();
    CHECK(c->faith != "pale_mourners");
    bool called = false;
    for (int i = 0; i < 40 && !called; ++i) {
        remember(w, *c, "necromancy");
        w.run(kTicksPerDay);
        called = c->faith == "pale_mourners";
    }
    CHECK(called);
}

TEST(lr_sim_culture_lukewarm_take_the_spouse_faith) {
    World w = realWorld();
    w.run(kTicksPerDay + 1);
    Character* a = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.spouse != kNone && w.character(c.status.spouse)) {
            a = &c;
            break;
        }
    CHECK(a != nullptr);
    if (!a) return;
    Character* b = w.character(a->status.spouse);
    b->faith = "sky_blood";
    b->piety = 0.9;
    a->faith = "hearth_faith";
    bool turned = false;
    for (int i = 0; i < 600 && !turned; ++i) {
        a->piety = 0.0;
        w.run(1);   // a roll of its own each time
        cultureDaily(w);
        turned = a->faith == "sky_blood";
    }
    CHECK(turned);
}

TEST(lr_sim_culture_bends_goals_and_saves) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    w.run(kTicksPerDay + 1);
    Character* c = villagers(w, sid).front();
    const bool bent = std::any_of(c->ai.mods.begin(), c->ai.mods.end(), [](const WeightMod& m) { return m.source == "culture" && m.target == "goal:pray"; });
    CHECK(bent);
    // one mod per goal: a second day does not stack them
    cultureDaily(w);
    const auto n = std::count_if(c->ai.mods.begin(), c->ai.mods.end(), [](const WeightMod& m) { return m.source == "culture" && m.target == "goal:pray"; });
    CHECK_EQ(n, 1);
    Json j;
    to_json(j, *c);
    Character back;
    from_json(j, back);
    CHECK_EQ(back.culture, c->culture);
    CHECK_EQ(back.faith, c->faith);
    CHECK(std::abs(back.piety - c->piety) < 1e-9);
}
