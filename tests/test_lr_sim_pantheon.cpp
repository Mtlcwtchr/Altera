// LR-169: the pantheon (sim/pantheon.hpp): Powers and liks from data, a person's gods by the
// belt of the culture, the gods' standing from worship, prayer answered by the rule (grant,
// refuse with a reason, defer and fulfil, silence and the god's debt), the world laws over
// mortals and gods (facts, memories, reported breaches, retribution, chains and lost domain),
// identification of liks and heresy.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/culture.hpp"
#include "sim/pantheon.hpp"
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

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement != kNone) out.push_back(&c);
    return out;
}

void nextDay(World& w) { w.run(kTicksPerDay); }

bool hasMood(const Character& c, const std::string& id, Tick now) {
    return std::any_of(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == id && m.until > now; });
}

int chronicled(const World& w, const std::string& code) {
    int n = 0;
    for (const auto& e : w.chronicleLog())
        if (e.code == code) ++n;
    return n;
}

} // namespace

TEST(lr_sim_pantheon_rules_load_and_validate) {
    const PantheonRules& r = pantheonRules(realContent());
    CHECK(r.loaded);
    for (const auto& e : r.errors) std::printf("  %s\n", e.c_str());
    CHECK(r.errors.empty());
    CHECK(r.powers.size() >= 16);
    CHECK(r.liks.size() >= 60);
    CHECK_EQ(r.laws.size(), static_cast<std::size_t>(8));
    CHECK(r.lik("zeus") && r.lik("zeus")->power == "sky_storm" && r.lik("zeus")->belt == "mediterranean");
    CHECK(r.lik("thor") && r.lik("perun") && r.lik("tengri"));
    ValidationReport rep;
    validatePantheon(realContent(), rep);
    validateCulture(realContent(), rep);
    for (const auto& e : rep.errors) std::printf("  %s\n", e.c_str());
    CHECK(rep.errors.empty());
    // every lik has a name, the laws a name and a description in the base language
    const auto keys = pantheonLocKeys(realContent());
    CHECK(std::find(keys.begin(), keys.end(), "god.zeus.name") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "law.hospitality.desc") != keys.end());
    // the faiths of the pantheon: a power and liks for any belt
    const CultureRules& cr = cultureRules(realContent());
    for (const char* f : {"hearth_faith", "sea_lord", "sun_oracle", "road_trickster", "wine_mysteries", "knowledge_runes", "fate_weavers"}) {
        CHECK(cr.faithDef(f) != nullptr);
        CHECK(!faithLiks(realContent(), f, "").empty());
    }
}

TEST(lr_sim_pantheon_gods_by_belt_and_aim) {
    const Content& c = realContent();
    // one faith, other gods in another belt
    CHECK_EQ(faithLiks(c, "hearth_faith", "forest").front(), std::string("domovoi"));
    CHECK_EQ(faithLiks(c, "hearth_faith", "mediterranean").front(), std::string("hestia"));
    CHECK_EQ(faithLiks(c, "hearth_faith", "nile").front(), std::string("hestia")); // no Nile list: "*"
    CHECK(likServesAim(c, "poseidon", "safe_sea"));
    CHECK(!likServesAim(c, "hestia", "safe_sea"));
    World w = realWorld();
    CHECK_EQ(likForAim(w, "sea_lord", "safe_sea", "north"), std::string("aegir"));
    CHECK_EQ(likForAim(w, "hearth_faith", "rain", "forest"), std::string("perun")); // the patron does not serve rain
    // the window's gods: every lik of the faith, named by the pantheon
    const FaithDef* f = cultureRules(c).faithDef("sun_oracle");
    CHECK(f && std::any_of(f->deities.begin(), f->deities.end(), [](const FaithDef::Deity& d) { return d.id == "ra"; }));
    CHECK(f->power == "sun");
}

TEST(lr_sim_pantheon_worship_feeds_standing) {
    World w = realWorld();
    auto ppl = people(w);
    CHECK(!ppl.empty());
    nextDay(w);
    nextDay(w);
    const auto gods = godsOf(w, *ppl.front());
    CHECK(!gods.empty());
    const GodState g = godState(w, gods.front());
    CHECK(g.worshippers > 0);
    // a god nobody worships fades below one that is worshipped
    std::string lonely;
    for (const auto& id : pantheonRules(w.content()).likOrder)
        if (godState(w, id).worshippers == 0) {
            lonely = id;
            break;
        }
    CHECK(!lonely.empty());
    for (int i = 0; i < 20; ++i) nextDay(w);
    CHECK(godState(w, lonely).standing < godState(w, gods.front()).standing);
}

TEST(lr_sim_pantheon_prayer_answers) {
    World w = realWorld();
    nextDay(w);
    Character* c = people(w).front();
    const std::string god = godsOf(w, *c).front();
    // outside the god's domain: refused with that reason
    std::string offAim;
    for (const char* a : {"safe_sea", "rain", "rest_dead", "victory", "wisdom", "heal", "good_hunt"})
        if (!likServesAim(w.content(), god, a)) {
            offAim = a;
            break;
        }
    CHECK(!offAim.empty());
    PrayerAsk p;
    p.who = c->id;
    p.lik = god;
    p.aim = offAim;
    DivineAnswer a = answerPrayer(w, p);
    CHECK(a.kind == DivineAnswerKind::Refused);
    CHECK_EQ(a.reason, std::string("pantheon.refuse.outside_domain"));
    // a favoured petitioner with an offering is mostly granted
    std::string aim = pantheonRules(w.content()).power(pantheonRules(w.content()).lik(god)->power)->aims.front();
    w.applyNow({0, 0, "test", "set_favor", {{"id", c->id}, {"lik", god}, {"favor", 0.9}}});
    int granted = 0, deferred = 0;
    for (int i = 0; i < 20; ++i) {
        w.run(1);
        PrayerAsk q = p;
        q.aim = aim;
        q.offering = "bread";
        q.weight = 1.5;
        const DivineAnswer b = answerPrayer(w, q);
        granted += b.kind == DivineAnswerKind::Granted;
        deferred += b.kind == DivineAnswerKind::Deferred;
        if (b.kind == DivineAnswerKind::Granted) CHECK(b.strength > 0.0 && b.strength <= 1.0);
    }
    CHECK(granted >= 1);
    CHECK(granted + deferred >= 12);
    CHECK(chronicled(w, "chronicle.pantheon.granted") >= 1);
    // the empty lik is the faith's lik for the aim
    PrayerAsk e;
    e.who = c->id;
    e.aim = aim;
    CHECK(!answerPrayer(w, e).lik.empty());
}

TEST(lr_sim_pantheon_deferred_is_fulfilled_or_owed) {
    World w = realWorld();
    nextDay(w);
    Character* c = people(w).front();
    const std::string god = godsOf(w, *c).front();
    const std::string aim = pantheonRules(w.content()).power(pantheonRules(w.content()).lik(god)->power)->aims.front();
    int fulfilled = 0;
    setPrayerFulfill([&](World&, const PrayerAsk& p, double s) {
        CHECK(p.who == c->id && s > 0.0);
        ++fulfilled;
    });
    // ask until the god defers (budget spent today pushes it there)
    int deferred = 0;
    for (int i = 0; i < 60 && deferred == 0; ++i) {
        PrayerAsk q;
        q.who = c->id;
        q.lik = god;
        q.aim = aim;
        q.rite = "test_rite";
        deferred += answerPrayer(w, q).kind == DivineAnswerKind::Deferred;
    }
    CHECK(deferred == 1);
    for (int d = 0; d < 7; ++d) nextDay(w);
    CHECK(fulfilled >= 1);
    CHECK(chronicled(w, "chronicle.pantheon.fulfilled") >= 1);
    setPrayerFulfill(nullptr);
}

TEST(lr_sim_pantheon_bound_god_is_silent_and_owes) {
    World w = realWorld();
    nextDay(w);
    Character* c = people(w).front();
    const std::string god = godsOf(w, *c).front();
    w.moduleState("pantheon")["gods"][god]["bound"] = w.day() + 10;
    const double before = godState(w, god).debt;
    PrayerAsk q;
    q.who = c->id;
    q.lik = god;
    q.aim = "protect";
    q.offering = "bread";
    const DivineAnswer a = answerPrayer(w, q);
    CHECK(a.kind == DivineAnswerKind::Silent);
    CHECK_EQ(a.reason, std::string("pantheon.refuse.bound"));
    CHECK(godState(w, god).debt > before);
    // a plea not made by the rule (no offering, no rite) owes nothing
    const double owed = godState(w, god).debt;
    PrayerAsk bare = q;
    bare.offering.clear();
    answerPrayer(w, bare);
    CHECK(std::abs(godState(w, god).debt - owed) < 1e-9);
}

TEST(lr_sim_pantheon_law_breach_curses_a_mortal) {
    World w = realWorld();
    nextDay(w);
    Character* c = people(w).front();
    Character* neighbour = nullptr;
    for (Character* o : people(w))
        if (o != c && o->status.settlement == c->status.settlement) neighbour = o;
    CHECK(neighbour != nullptr);
    const double trust = neighbour->relations[c->id].trust;
    Command cmd{0, 0, "test", "law_breach", {{"id", c->id}, {"law", "hospitality"}, {"weight", 1.0}}};
    cmd = w.applyNow(cmd);
    CHECK(openBreaches(w, c->id) == 1);
    CHECK(chronicled(w, "chronicle.pantheon.breach") == 1);
    // the plea of a breaker to a watcher of the law is refused until it is answered
    const auto& r = pantheonRules(w.content());
    std::string watcher;
    for (const auto& g : godsOf(w, *c))
        if (r.lik(g)->hasPower("sky_storm") || r.lik(g)->hasPower("hearth") || r.lik(g)->hasPower("road")) watcher = g;
    if (!watcher.empty()) {
        PrayerAsk q;
        q.who = c->id;
        q.lik = watcher;
        q.aim = r.power(r.lik(watcher)->power)->aims.front();
        const DivineAnswer a = answerPrayer(w, q);
        CHECK(a.kind == DivineAnswerKind::Refused && a.reason == "pantheon.refuse.breaks_law");
    }
    for (int d = 0; d < 8; ++d) nextDay(w);
    CHECK(openBreaches(w, c->id) == 0);
    CHECK(hasMood(*c, "cursed", w.now()));
    CHECK(neighbour->relations[c->id].trust < trust); // the community turned away
    CHECK(chronicled(w, "chronicle.pantheon.punished") == 1);
    // an unknown law is rejected
    Command bad{0, 0, "test", "law_breach", {{"id", c->id}, {"law", "no_such_law"}}};
    bad = w.applyNow(bad);
    CHECK_EQ(bad.result, std::string("rejected:unknown_law"));
}

TEST(lr_sim_pantheon_facts_and_memories_are_breaches) {
    World w = realWorld();
    nextDay(w);
    auto ppl = people(w);
    // the murder of kin (a deed in the world's record) is a breach of Kin Blood
    Character* doer = nullptr;
    Character* kin = nullptr;
    for (Character* a : ppl)
        for (Character* b : ppl)
            if (a != b && a->status.family != kNone && a->status.family == b->status.family) doer = a, kin = b;
    CHECK(doer && kin);
    Fact f;
    f.pred = "murder";
    f.roles["doer"] = doer->id;
    f.roles["victim"] = kin->id;
    f.place = doer->pos;
    w.addFact(f);
    // a raised dead (the necromancer's memory) breaks the Rest of the Dead
    Character* necro = ppl.back() == doer ? ppl.front() : ppl.back();
    MemoryEntry m;
    m.type = "necromancy";
    m.salience = 0.6;
    w.remember(*necro, m);
    nextDay(w);
    bool kinBlood = false, restDead = false;
    for (const Json& b : breachLog(w)) {
        kinBlood |= b.value("law", std::string()) == "kin_blood" && b.value("doer", kNone) == doer->id;
        restDead |= b.value("law", std::string()) == "rest_dead" && b.value("doer", kNone) == necro->id;
    }
    CHECK(kinBlood);
    CHECK(restDead);
    // facts are read once
    const auto n = breachLog(w).size();
    nextDay(w);
    std::size_t kb = 0;
    for (const Json& b : breachLog(w)) kb += b.value("law", std::string()) == "kin_blood";
    CHECK_EQ(kb, static_cast<std::size_t>(1));
    CHECK(breachLog(w).size() >= n);
}

TEST(lr_sim_pantheon_gods_answer_to_the_laws) {
    World w = realWorld();
    nextDay(w);
    const std::string god = "poseidon";
    const double st = godState(w, god).standing;
    // a debt past the line is a breach of the Exchange by the god itself
    w.moduleState("pantheon")["gods"][god]["debt"] = 3.0;
    nextDay(w);
    CHECK(openBreaches(w, kNone, god) == 1);
    CHECK(chronicled(w, "chronicle.pantheon.god_breach") == 1);
    for (int d = 0; d < 8; ++d) nextDay(w);
    CHECK(openBreaches(w, kNone, god) == 0);
    const GodState g = godState(w, god);
    CHECK(g.standing < st);
    CHECK(g.bound(w.day()));              // heavy: in chains
    CHECK(g.domain < 1.0);                // and a share of the Sea went to another lik
    CHECK(chronicled(w, "chronicle.pantheon.domain_lost") == 1);
    double others = 0.0;
    for (const char* o : {"aegir", "ran", "nun", "vodyanoy", "manawydan", "lake_spirit"}) others = std::max(others, godState(w, o).domain);
    CHECK(others > 1.0);
    // a reported breach of a god (any law that binds gods)
    Command cmd{0, 0, "test", "law_breach", {{"god", "zeus"}, {"law", "oath"}}};
    cmd = w.applyNow(cmd);
    CHECK_EQ(cmd.result, std::string("ok"));
    CHECK(openBreaches(w, kNone, "zeus") == 1);
}

TEST(lr_sim_pantheon_identification_and_heresy) {
    World w = realWorld();
    auto ppl = people(w);
    CHECK(ppl.size() >= 4);
    // half the place keeps the faith of the Sky: Zeus ("*" of hearth_faith) meets Thor (of sky_blood)
    for (size_t i = 0; i < ppl.size(); i += 2) {
        ppl[i]->faith = "sky_blood";
        ppl[i]->piety = 0.5;
    }
    auto& R = const_cast<PantheonRules&>(pantheonRules(w.content()));
    const auto saved = R.ident;
    R.ident.base = 10.0; // every day a step (the chance is capped by the roll)
    R.ident.step = 0.25;
    R.ident.heresyChance = 0.0;
    for (int d = 0; d < 5; ++d) {
        for (Character* c : ppl) c->piety = 0.5; // keep the zealots out of it
        nextDay(w);
    }
    CHECK(identified(w, "zeus", "thor"));
    CHECK(chronicled(w, "chronicle.pantheon.identified") >= 1);
    // a shared favour: Thor's favour shows in Zeus's
    w.applyNow({0, 0, "test", "set_favor", {{"id", ppl[1]->id}, {"lik", "thor"}, {"favor", 0.8}}});
    CHECK(favorOf(w, ppl[1]->id, "zeus") > favorOf(w, ppl[1]->id, "zeus", false));
    // zealots reject it
    R.ident.heresyChance = 1.0;
    R.ident.heresyTolerance = 1.0;
    R.ident.heresyPiety = 0.0;
    const double was = identification(w, "zeus", "thor");
    nextDay(w);
    CHECK(identification(w, "zeus", "thor") < was);
    CHECK(chronicled(w, "chronicle.pantheon.heresy") >= 1);
    R.ident = saved;
}

TEST(lr_sim_pantheon_rites_feed_favor) {
    World w = realWorld();
    nextDay(w);
    Character* c = people(w).front();
    const std::string god = godsOf(w, *c).front();
    const double f0 = favorOf(w, c->id, god, false);
    const double s0 = godState(w, god).standing;
    onRiteDone(w, c->id, "", "any_rite", 2.0);
    CHECK(favorOf(w, c->id, god, false) > f0);
    CHECK(godState(w, god).standing > s0);
}
