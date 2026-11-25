// After an accusation (AL-37): the accused gives back, pays off, denies or
// runs; a denial goes to a quarrel, a fight or the chief's court; the court
// convicts and sentences or acquits, and a false accusation costs the
// accuser his name. Every end of a case is in the chronicle.

#include <algorithm>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/justice.hpp"
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

// Three grown people of three households: the accused, the accuser, a judge.
struct Parties {
    Character* accused = nullptr;
    Character* accuser = nullptr;
    Character* judge = nullptr;
};

Parties parties(World& w) {
    Parties p;
    std::set<EntityId> families;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult" || c.status.family == kNone) continue;
        if (families.count(c.status.family)) continue;
        if (!p.accused) p.accused = &c;
        else if (!p.accuser && c.status.settlement == p.accused->status.settlement) p.accuser = &c;
        else if (p.accuser && !p.judge && c.status.settlement == p.accused->status.settlement) p.judge = &c;
        else continue;
        families.insert(c.status.family);
    }
    return p;
}

const CaseOption* option(const std::vector<CaseOption>& v, const std::string& id) {
    for (const auto& o : v)
        if (o.id == id) return &o;
    return nullptr;
}

std::string best(const std::vector<CaseOption>& v, const Character& a, const Character& b, const CaseCtx& x) {
    std::string id;
    double top = -1e18;
    for (const auto& o : v)
        if (const double s = caseScore(o, a, b, x); s > top) {
            top = s;
            id = o.id;
        }
    return id;
}

bool chronicled(const World& w, const std::string& type) {
    for (const auto& e : w.chronicleLog())
        if (e.type == type) return true;
    return false;
}

int memories(const Character& c, const std::string& type, EntityId about) {
    int n = 0;
    for (const auto& m : c.memory) n += m.type == type && !m.who.empty() && m.who[0] == about;
    return n;
}

void remember(World& w, Character& c, const std::string& type, std::vector<EntityId> who, EntityId object,
              const std::string& source, double confidence) {
    MemoryEntry m;
    m.type = type;
    m.who = std::move(who);
    m.object = object;
    m.source = source;
    m.confidence = confidence;
    m.salience = 0.9;
    w.remember(c, std::move(m));
}

// A case already at the court stage, sitting at the next tick.
void atCourt(World& w, const Parties& p, EntityId object, double certainty) {
    remember(w, *p.accused, "case_pending", {p.accuser->id, p.judge->id}, object, "court", certainty);
    w.schedule({w.now() + 1, 0, "case", p.accuser->id, p.accused->id, object,
                {{"stage", "court"}, {"judge", p.judge->id}, {"confidence", certainty}, {"seen", false}}});
}

} // namespace

TEST(lr_sim_justice_rules_loaded) {
    const CaseRules& R = caseRules(realContent());
    for (const char* a : {"return", "compensate", "deny", "flee"}) CHECK(option(R.answers, a) != nullptr);
    for (const char* a : {"let_go", "quarrel", "fight", "court"}) CHECK(option(R.pursuits, a) != nullptr);
    for (const char* a : {"fine", "shame", "custody", "exile"}) CHECK(option(R.sentences, a) != nullptr);
    CHECK(R.worth.count("meal") > 0);
    CHECK(realContent().interaction(R.judgeTask) && realContent().interaction(R.quarrelTask) &&
          realContent().interaction(R.fightTask));
}

TEST(lr_sim_justice_answer_scores) {
    World w = realWorld();
    Parties p = parties(w);
    CHECK(p.accused && p.accuser);
    if (!p.accused || !p.accuser) return;
    const CaseRules& R = caseRules(w.content());
    p.accused->traits = {"honest"};
    CaseCtx x;
    x.guilty = x.canReturn = x.canPay = x.seen = true;
    x.evidence = 1.0;
    CHECK_EQ(best(R.answers, *p.accused, *p.accuser, x), std::string("return"));
    // caught but with nothing left to give back: pays off
    x.canReturn = false;
    CHECK_EQ(best(R.answers, *p.accused, *p.accuser, x), std::string("compensate"));
    // an innocent man denies it and does not run
    CaseCtx inn;
    inn.canPay = true;
    inn.evidence = 0.4;
    CHECK_EQ(best(R.answers, *p.accused, *p.accuser, inn), std::string("deny"));
    CHECK(caseScore(*option(R.answers, "flee"), *p.accused, *p.accuser, inn) < -5.0);
    // a denied accuser sure of it goes to the judge when there is one, never when there is none
    CaseCtx d;
    d.seen = d.hasJudge = true;
    d.evidence = 1.0;
    p.accuser->traits = {"dutiful"};
    CHECK_EQ(best(R.pursuits, *p.accuser, *p.accused, d), std::string("court"));
    d.hasJudge = false;
    CHECK(best(R.pursuits, *p.accuser, *p.accused, d) != "court");
    // a hot head fights
    p.accuser->traits = {"hot_tempered", "unforgiving"};
    p.accuser->relations[p.accused->id].enmity = 0.8;
    CHECK_EQ(best(R.pursuits, *p.accuser, *p.accused, d), std::string("fight"));
}

TEST(lr_sim_justice_accused_gives_back) {
    World w = realWorld(5);
    Parties p = parties(w);
    CHECK(p.accused && p.accuser);
    if (!p.accused || !p.accuser) return;
    WorldObject* store = w.object(w.families().at(p.accuser->status.family).storage);
    CHECK(store != nullptr);
    if (!store) return;
    store->stock["cheese"] = 3;
    p.accused->traits = {"honest"};
    p.accused->pos = store->pos;
    p.accuser->pos = {store->pos.x + 4.0, store->pos.y};
    // a theft the owner sees (cheese: nobody eats it in the next hours)
    InteractionDef take = *w.content().interaction("take_meal@foreign");
    take.visibility = "public";
    for (auto& e : take.eff)
        if (e.key == "meal") e.key = "cheese";
    take.pre.clear();
    applyEffects(w, *p.accused, take, store, nullptr);
    CHECK_EQ(store->stock["cheese"], 2);
    // the truth is kept by the world, with what was taken and who saw it
    CHECK(!w.facts().empty());
    if (!w.facts().empty()) {
        CHECK_EQ(w.facts().back().role("doer"), p.accused->id);
        CHECK_EQ(w.facts().back().goods.at("cheese"), 1);
        CHECK(std::find(w.facts().back().witnesses.begin(), w.facts().back().witnesses.end(), p.accuser->id) !=
              w.facts().back().witnesses.end());
    }
    applyEffects(w, *p.accuser, *w.content().interaction("accuse_publicly"), nullptr, p.accused);
    CHECK(chronicled(w, "case_opened"));
    CHECK_EQ(memories(*p.accused, "case_pending", p.accuser->id), 1);
    const double honesty0 = p.accused->reputation["reputation:honesty"];
    w.run(kTicksPerDay / 4);
    CHECK(chronicled(w, "case_returned"));
    CHECK_EQ(store->stock["cheese"], 3);
    CHECK_EQ(memories(*p.accused, "case_pending", p.accuser->id), 0);
    CHECK_EQ(memories(*p.accused, "case_closed", p.accuser->id), 1);
    CHECK_EQ(memories(*p.accuser, "case_closed", p.accused->id), 1);
    CHECK(p.accused->reputation["reputation:honesty"] > honesty0);
    CHECK_EQ(w.facts().back().outcome, std::string("returned"));
    // the same matter is not opened twice
    const size_t n = w.chronicleLog().size();
    applyEffects(w, *p.accuser, *w.content().interaction("accuse_publicly"), nullptr, p.accused);
    int opened = 0;
    for (size_t i = n; i < w.chronicleLog().size(); ++i) opened += w.chronicleLog()[i].type == "case_opened";
    CHECK_EQ(opened, 0);
}

TEST(lr_sim_justice_false_accusation) {
    World w = realWorld(3);
    Parties p = parties(w);
    CHECK(p.accused && p.accuser && p.judge);
    if (!p.accused || !p.accuser || !p.judge) return;
    p.accused->reputation["reputation:honesty"] = 0.0;
    p.judge->relations.erase(p.accused->id);
    const EntityId object = 999999;
    remember(w, *p.accuser, "suspects", {p.accused->id}, object, "seen", 0.3);
    const double honesty0 = p.accuser->reputation["reputation:honesty"];
    const double enmity0 = p.accused->relations[p.accuser->id].enmity;
    atCourt(w, p, object, 0.3);
    w.run(2);
    CHECK(chronicled(w, "case_acquitted"));
    CHECK(!chronicled(w, "case_convicted"));
    CHECK(p.accuser->reputation["reputation:honesty"] < honesty0);
    CHECK(p.accused->relations[p.accuser->id].enmity > enmity0);
    CHECK_EQ(memories(*p.accused, "case_pending", p.accuser->id), 0);
}

TEST(lr_sim_justice_conviction) {
    World w = realWorld(9);
    Parties p = parties(w);
    CHECK(p.accused && p.accuser && p.judge);
    if (!p.accused || !p.accuser || !p.judge) return;
    WorldObject* store = w.object(w.families().at(p.accuser->status.family).storage);
    CHECK(store != nullptr);
    if (!store) return;
    const EntityId object = store->id;
    const int cheese0 = store->stock["cheese"];
    p.accused->inventory["cheese"] = 2;
    // the truth: he took two cheeses, the accuser and two more saw it
    Fact truth;
    truth.roles["doer"] = p.accused->id;
    truth.roles["victim_family"] = p.accuser->status.family;
    truth.roles["object"] = object;
    truth.pred = "theft";
    truth.goods = {{"cheese", 2}};
    truth.witnesses = {p.accuser->id};
    for (auto& [id, c] : w.characters())
        if (truth.witnesses.size() < 3 && !c.isAnimal() && c.body.alive && id != p.accused->id && id != p.accuser->id &&
            id != p.judge->id)
            truth.witnesses.push_back(id);
    w.addFact(truth);
    remember(w, *p.accuser, "saw_crime", {p.accused->id}, object, "seen", 1.0);
    const double honesty0 = p.accused->reputation["reputation:honesty"];
    atCourt(w, p, object, 1.0);
    w.run(2);
    CHECK(chronicled(w, "case_convicted"));
    CHECK(store->stock["cheese"] >= cheese0 + 2);
    CHECK(p.accused->reputation["reputation:honesty"] < honesty0);
    bool sentenced = false;
    for (const auto& v : p.accused->status.verdicts) sentenced |= v.rfind("case_", 0) == 0;
    CHECK(sentenced);
    std::string sentence;
    for (const auto& e : w.chronicleLog())
        if (e.type == "case_convicted") sentence = e.params.value("sentence", std::string());
    CHECK(sentence == "fine" || sentence == "shame" || sentence == "custody" || sentence == "exile");
    CHECK_EQ(w.facts().back().outcome, std::string("convicted"));
}

TEST(lr_sim_justice_witness_clears_the_accused) {
    // the accuser is sure, but one who really saw it knows it was someone else
    World w = realWorld(17);
    Parties p = parties(w);
    CHECK(p.accused && p.accuser && p.judge);
    if (!p.accused || !p.accuser || !p.judge) return;
    Character* thief = nullptr;
    Character* witness = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || id == p.accused->id || id == p.accuser->id || id == p.judge->id) continue;
        if (c.status.family == p.accuser->status.family) continue;
        if (!thief) thief = &c;
        else if (!witness) witness = &c;
    }
    CHECK(thief && witness);
    if (!thief || !witness) return;
    WorldObject* store = w.object(w.families().at(p.accuser->status.family).storage);
    if (!store) return;
    Fact truth;
    truth.roles["doer"] = thief->id;
    truth.roles["victim_family"] = p.accuser->status.family;
    truth.roles["object"] = store->id;
    truth.pred = "theft";
    truth.goods = {{"meal", 1}};
    truth.witnesses = {witness->id};
    w.addFact(truth);
    remember(w, *p.accuser, "suspects", {p.accused->id}, store->id, "seen", 0.9);
    atCourt(w, p, store->id, 0.9);
    w.run(2);
    CHECK(chronicled(w, "case_acquitted"));
    CHECK(chronicled(w, "case_cleared"));
    CHECK(memories(*p.accuser, "told_crime", thief->id) > 0);
    CHECK(w.facts().back().outcome.empty()); // the real doer's deed is still open
}

TEST(lr_sim_justice_truth_saved) {
    World w = realWorld();
    Fact d;
    d.roles = {{"doer", 5}, {"object", 6}};
    d.pred = "theft";
    d.goods = {{"meal", 2}};
    d.witnesses = {7};
    w.addFact(d);
    World copy(w.content(), 1);
    copy.loadJson(w.toJson());
    CHECK_EQ(copy.facts().size(), size_t(1));
    CHECK_EQ(copy.facts()[0].goods.at("meal"), 2);
    CHECK(copy.stateHash() == w.stateHash());
}

TEST(lr_sim_justice_denial_goes_on) {
    // a denial is never the end: the accuser lets go, quarrels, fights or goes to court
    World w = realWorld(13);
    Parties p = parties(w);
    CHECK(p.accused && p.accuser);
    if (!p.accused || !p.accuser) return;
    p.accused->traits = {"unscrupulous", "proud"};
    const EntityId object = 777777;
    remember(w, *p.accuser, "suspects", {p.accused->id}, object, "seen", 0.7);
    applyEffects(w, *p.accuser, *w.content().interaction("accuse_publicly"), nullptr, p.accused);
    w.run(kTicksPerDay / 2);
    CHECK(chronicled(w, "case_denied"));
    const bool next = chronicled(w, "case_let_go") || chronicled(w, "case_quarrel") || chronicled(w, "case_fight") ||
                      chronicled(w, "case_to_court");
    CHECK(next);
    if (chronicled(w, "case_to_court")) {
        w.run(static_cast<Tick>(caseRules(w.content()).courtWaitDays * kTicksPerDay) + 2);
        CHECK(chronicled(w, "case_acquitted") || chronicled(w, "case_convicted") || chronicled(w, "case_dropped"));
    }
}
