// LR-141: stats and skills side by side (gdd/stats/STATS_SKILLS_v0.3.md). 16 stats in
// data, base + current (kind, traits, age, state), growth by divisions past 1, forgetting,
// old saves filled and skills renamed; an act's check in data (factors -> quality ->
// each listener's outcome), odds without a roll, the validator catching unknown ids.

#include <algorithm>
#include <cmath>

#include "framework.hpp"
#include "sim/act_check.hpp"
#include "sim/ai.hpp"
#include "sim/stats.hpp"
#include "sim/species.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json base = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(real(), base, seed);
    installAi(w);
    return w;
}

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (c.body.alive && !c.isAnimal()) out.push_back(&c);
    return out;
}

void setAll(Character& c, double v) {
    for (const auto& id : statRules(real())->ids()) c.attrs[id] = v;
}

double oddsOf(const std::vector<std::pair<std::string, double>>& odds, const std::string& id) {
    for (const auto& [k, p] : odds)
        if (k == id) return p;
    return 0.0;
}

} // namespace

TEST(lr_stats_content_loads) {
    const Content& c = real();
    const StatRules* r = statRules(c);
    CHECK(r != nullptr);
    CHECK_EQ(r->stats.size(), size_t(16));
    CHECK(r->errors.empty());
    CHECK_EQ(c.expansion.attributes.size(), size_t(16));
    for (const char* s : {"agility", "dexterity", "voice", "composure", "willpower", "appearance", "attunement"})
        CHECK(r->has(s));
    const RaceDef* dragon = c.raceOf("dragon");
    CHECK(dragon != nullptr);
    CHECK(dragon->statMods.at("strength") > 1.0); // superhuman by kind
    const RaceDef* half = c.raceOf("half_orc");
    CHECK(half != nullptr);
    CHECK(std::abs(half->statMods.at("strength") - 0.09) < 1e-9); // the blend halves the orc's shift
    // the social skills are their own skills now
    for (const char* k : {"eloquence", "persuasion", "guile", "menace", "courtship", "insight", "empathy", "leadership",
                          "trade", "etiquette", "tricks"})
        CHECK(c.knowledge.count(k));
    for (const char* k : {"storytelling", "deception", "intimidation", "diplomacy"}) CHECK(!c.knowledge.count(k));
}

TEST(lr_stats_growth_slows_by_divisions_past_soft_start) {
    GrowthCurve g; // 0.8, 0.1, x0.5
    CHECK(std::abs(g.mult(0.5) - 1.0) < 1e-12);
    CHECK(std::abs(g.mult(0.8) - 1.0) < 1e-12);
    CHECK(std::abs(g.mult(0.9) - 0.5) < 1e-9);
    CHECK(std::abs(g.mult(1.0) - 0.25) < 1e-9);
    CHECK(std::abs(g.mult(1.3) - 1.0 / 32.0) < 1e-9);
    CHECK(g.mult(0.95) < g.mult(0.9) && g.mult(0.95) > g.mult(1.0)); // smooth inside a division
}

TEST(lr_stats_practice_passes_one_ever_slower) {
    World w = realWorld();
    Character& c = *people(w).front();
    c.body.ageDays = 30 * kDaysPerYear;
    auto stepsToGain = [&](double from, double gain) {
        c.skills["eloquence"] = from;
        int n = 0;
        while (c.skills["eloquence"] < from + gain && n < 1000000) {
            practiceSkill(w, c, "eloquence", 0.01);
            ++n;
        }
        return n;
    };
    const int low = stepsToGain(0.3, 0.1);
    const int high = stepsToGain(1.0, 0.1);
    const int higher = stepsToGain(1.2, 0.1);
    CHECK(c.skills["eloquence"] > 1.29); // no cap at 1
    CHECK(high > low * 4);
    CHECK(higher > high * 3);
    // a stat's base trains the same way
    c.attrs["strength"] = 0.95;
    const double before = c.attrs["strength"];
    trainStat(w, c, "strength", 0.01);
    CHECK(c.attrs["strength"] > before && c.attrs["strength"] - before < 0.005);
}

TEST(lr_stats_old_save_filled_skills_renamed_state_shifts) {
    World w = realWorld();
    Character& c = *people(w).front();
    c.attrs = {{"strength", 0.6},  {"agility", 0.5},    {"endurance", 0.7}, {"intellect", 0.4},
               {"perception", 0.6}, {"willpower", 0.8}, {"charisma", 0.5}, {"dexterity", 0.5}};
    c.skills.erase("eloquence");
    c.skills["storytelling"] = 0.42;
    c.body.ageDays = 30 * kDaysPerYear;
    c.attrsNow.clear();
    w.step();
    CHECK_EQ(c.attrs.size(), size_t(16));
    CHECK(!c.skills.count("storytelling"));
    CHECK(std::abs(c.skills["eloquence"] - 0.42) < 1e-9);
    CHECK(std::abs(c.attrs["composure"] - 0.8) < 0.35); // filled from willpower with noise
    CHECK(std::abs(c.attrs["vitality"] - 0.7) < 0.3);
    // deterministic: the same person in the same world gets the same fill
    World w2 = realWorld();
    Character& c2 = *w2.character(c.id);
    c2.attrs = {{"strength", 0.6},  {"agility", 0.5},    {"endurance", 0.7}, {"intellect", 0.4},
                {"perception", 0.6}, {"willpower", 0.8}, {"charisma", 0.5}, {"dexterity", 0.5}};
    w2.step();
    CHECK(std::abs(c2.attrs["composure"] - c.attrs["composure"]) < 1e-12);

    // the current value: tipsy hands shake, a tipsy tongue loosens
    refreshStats(real(), c);
    const double hands = c.attrsNow["dexterity"], cha = c.attrsNow["charisma"];
    c.moodlets.push_back({"tipsy", 0.0, w.now() + 100});
    refreshStats(real(), c);
    CHECK(c.attrsNow["dexterity"] < hands);
    CHECK(c.attrsNow["charisma"] > cha);
    // age: a child is weaker than a grown person with the same base
    const double grown = c.attrsNow["strength"];
    c.body.ageDays = 6 * kDaysPerYear;
    refreshStats(real(), c);
    CHECK(c.attrsNow["strength"] < grown * 0.8);
}

TEST(lr_stats_kind_shifts_current_value) {
    World w = realWorld();
    Character& c = *people(w).front();
    c.body.ageDays = 30 * kDaysPerYear;
    setAll(c, 0.5);
    c.traits.clear();
    c.moodlets.clear();
    c.body.fatigue = 0.0;
    c.body.health = 1.0;
    for (auto& [k, v] : c.needs) v = 1.0;
    setRace(real(), c, "human");
    refreshStats(real(), c);
    const double human = c.attrsNow["strength"];
    setRace(real(), c, "ogre");
    c.body.ageDays = (c.body.adultYears + (c.body.elderYears - c.body.adultYears) / 2) * kDaysPerYear;
    refreshStats(real(), c);
    CHECK(c.attrsNow["strength"] > 1.0); // a grown ogre is past human
    CHECK(c.attrsNow["strength"] > human + 0.5);
}

TEST(lr_stats_unused_skill_fades_used_one_stays) {
    World w = realWorld();
    Character& c = *people(w).front();
    c.body.ageDays = 30 * kDaysPerYear;
    c.skills["tricks"] = 1.1;
    c.skills["eloquence"] = 1.1;
    c.skills["menace"] = 0.1; // below the floor: kept
    c.skillDay["tricks"] = c.body.ageDays - 1000;
    for (int d = 0; d < 10; ++d) {
        c.skillDay["eloquence"] = c.body.ageDays; // practised every day
        w.run(kTicksPerDay);
        if (!c.body.alive) break;
    }
    CHECK(c.skills["tricks"] < 1.1);
    CHECK(std::abs(c.skills["eloquence"] - 1.1) < 1e-9 || c.skills["eloquence"] > 1.1);
    CHECK(c.skills["menace"] >= 0.1 - 1e-9);
}

TEST(lr_stats_tell_stories_lifts_only_with_a_good_teller) {
    World w = realWorld();
    auto ps = people(w);
    CHECK(ps.size() >= 3);
    Character& teller = *ps[0];
    Character& listener = *ps[1];
    const InteractionDef& d = real().interactions.at("tell_stories");
    CHECK(d.check.set);
    CHECK(d.check.audienceRadius > 0.0);
    for (Character* p : {&teller, &listener}) {
        p->body.ageDays = 30 * kDaysPerYear;
        setAll(*p, 0.5);
        p->traits.clear();
        p->moodlets.clear();
        p->body.fatigue = 0.0;
        for (auto& [k, v] : p->needs) v = 0.6;
        setRace(real(), *p, "human");
    }
    auto odds = [&](double cha, double voice, double skill, double imagination) {
        setAll(teller, 0.5);
        teller.attrs["charisma"] = cha;
        teller.attrs["voice"] = voice;
        teller.attrs["creativity"] = imagination;
        teller.skills["eloquence"] = skill;
        refreshStats(real(), teller);
        refreshStats(real(), listener);
        return actCheckOdds(w, teller, d, listener);
    };
    const auto avg = odds(0.5, 0.5, 0.2, 0.5);
    const auto bard = odds(0.8, 0.75, 0.8, 0.7);
    const auto noob = odds(0.85, 0.5, 0.1, 0.5);
    double sum = 0.0;
    for (const auto& [k, p] : bard) sum += p;
    CHECK(std::abs(sum - 1.0) < 1e-9);
    const double avgLift = oddsOf(avg, "spellbound") + oddsOf(avg, "good");
    const double bardLift = oddsOf(bard, "spellbound") + oddsOf(bard, "good");
    CHECK(bardLift > 0.9);
    CHECK(avgLift < 0.5);
    CHECK(oddsOf(bard, "spellbound") > 0.3);
    CHECK(oddsOf(avg, "spellbound") < 0.05);
    CHECK(oddsOf(avg, "bored") > oddsOf(bard, "bored"));
    // the charismatic novice is uneven: more of both ends than the average teller
    CHECK(oddsOf(noob, "spellbound") > oddsOf(avg, "spellbound"));
    CHECK(oddsOf(noob, "bored") > 0.05);

    // the roll: the audience around the teller each get their own outcome
    odds(0.8, 0.75, 0.8, 0.7);
    for (size_t i = 1; i < ps.size(); ++i) ps[i]->pos = {teller.pos.x + 1.0 + 0.1 * double(i), teller.pos.y};
    ActCheckRoll roll;
    const bool ok = resolveActCheck(w, teller, d, nullptr, nullptr, nullptr, &roll);
    CHECK(ok);
    CHECK(!roll.outcomes.empty());
    CHECK(roll.outcomes.size() <= size_t(d.check.audienceMax));
    CHECK(roll.succeeded > 0);
}

TEST(lr_stats_joke_and_flirt_use_checks) {
    const Content& c = real();
    for (const char* id : {"joke", "tell_story", "compliment", "flirt", "comfort", "calm_down", "boast", "recite_poem",
                           "serenade", "give_counsel"}) {
        const auto it = c.interactions.find(id);
        CHECK(it != c.interactions.end());
        if (it == c.interactions.end()) continue;
        CHECK(it->second.check.set);
        CHECK(!it->second.chance.set);
        CHECK(it->second.check.outcomes.size() >= size_t(2));
    }
    // a template's parameter reached the skill name
    const auto& j = c.interactions.at("comfort").check;
    bool readsEmpathy = false;
    for (const auto& [n, e] : j.factors)
        for (const auto& name : e.names()) readsEmpathy = readsEmpathy || name == "skill.empathy";
    CHECK(readsEmpathy);
}

TEST(lr_stats_validator_catches_unknown_ids) {
    Content c = real();
    std::vector<std::string> errs;
    InteractionDef& d = c.interactions.at("joke");
    d.check = parseActCheck(Json::parse(R"J({
        "factors": {"a": {"attr.charm": 0.5, "skill.no_such_skill": 0.5}},
        "need": "f.no_such_formula",
        "outcomes": [{"id": "ok", "over": 0, "eff": []}, {"id": "no", "else": true, "eff": []}]})J"),
                            "joke check", errs);
    CHECK(errs.empty());
    ValidationReport r;
    validateActChecks(c, r);
    auto has = [&](const std::string& s) {
        return std::any_of(r.errors.begin(), r.errors.end(), [&](const std::string& e) { return e.find(s) != std::string::npos; });
    };
    CHECK(has("charm"));
    CHECK(has("no_such_skill"));
    CHECK(has("no_such_formula"));
    // a malformed check is a parse error
    errs.clear();
    parseActCheck(Json::parse(R"J({"factors": {"a": {"attr.charisma": "x"}}, "outcomes": []})J"), "bad", errs);
    CHECK(errs.size() >= size_t(2));
}
