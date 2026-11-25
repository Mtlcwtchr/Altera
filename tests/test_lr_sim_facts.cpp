// The registry of truth (AL-55, DIALOGUE_GEN §6.4): memories point at the
// fact; a true witness refutes a false accusation (vouch), the accused can
// prove his innocence through one or be caught lying, a rumour about a deed
// nobody did falls apart, and the truth report tells truth from rumour.

#include <algorithm>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/facts.hpp"
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

// Grown people of different households: the wronged, the blamed, the real thief, a witness, a gossip.
struct Cast {
    Character* accuser = nullptr;
    Character* innocent = nullptr;
    Character* thief = nullptr;
    Character* witness = nullptr;
    Character* gossip = nullptr;
    WorldObject* store = nullptr;
};

Cast cast(World& w) {
    Cast k;
    std::vector<Character*> others;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult" || c.status.family == kNone) continue;
        if (!k.accuser) k.accuser = &c;
        else if (c.status.family != k.accuser->status.family) others.push_back(&c);
    }
    if (!k.accuser || others.size() < 4) {
        k.accuser = nullptr;
        return k;
    }
    k.innocent = others[0];
    k.thief = others[1];
    k.witness = others[2];
    k.gossip = others[3];
    k.store = w.object(w.families().at(k.accuser->status.family).storage);
    return k;
}

void believe(World& w, Character& c, const std::string& type, EntityId doer, EntityId object, double conf,
             std::vector<EntityId> chain, EntityId fact) {
    MemoryEntry m;
    m.type = type;
    m.who = {doer};
    m.object = object;
    m.source = type == "saw_crime" ? "seen" : "told";
    m.confidence = conf;
    m.chain = std::move(chain);
    m.fact = fact;
    m.salience = 0.9;
    w.remember(c, std::move(m));
}

// The truth: `thief` took a meal from the store, `witness` saw it.
EntityId theft(World& w, const Cast& k) {
    Fact f;
    f.pred = "theft";
    f.roles = {{"doer", k.thief->id}, {"victim_family", k.accuser->status.family}, {"object", k.store->id}};
    f.goods = {{"meal", 1}};
    f.witnesses = {k.witness->id};
    const EntityId id = w.addFact(f).id;
    believe(w, *k.witness, "saw_crime", k.thief->id, k.store->id, 1.0, {}, id);
    return id;
}

const MemoryEntry* belief(const Character& c, const std::string& type, EntityId doer) {
    for (const auto& m : c.memory)
        if (m.type == type && !m.who.empty() && m.who[0] == doer) return &m;
    return nullptr;
}

bool chronicled(const World& w, const std::string& type) {
    for (const auto& e : w.chronicleLog())
        if (e.type == type) return true;
    return false;
}

} // namespace

TEST(lr_sim_facts_memories_point_at_the_truth) {
    World w = realWorld();
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    k.store->stock["meal"] = 3;
    for (auto& [id, c] : w.characters())
        if (dist(c.pos, k.store->pos) < 400.0) c.pos = {k.store->pos.x + 900.0, k.store->pos.y + 900.0};
    k.thief->pos = k.store->pos;
    k.witness->pos = {k.store->pos.x + 5.0, k.store->pos.y};
    InteractionDef pub = *w.content().interaction("take_meal@foreign");
    pub.visibility = "public";
    applyEffects(w, *k.thief, pub, k.store, nullptr);
    CHECK(!w.facts().empty());
    if (w.facts().empty()) return;
    const Fact& f = w.facts().back();
    CHECK_EQ(f.role("doer"), k.thief->id);
    CHECK_EQ(f.pred, std::string("theft"));
    bool seenEv = false, goodsEv = false;
    for (const auto& e : f.evidence) {
        seenEv |= e.value("kind", "") == "seen" && e.value("who", kNone) == k.witness->id;
        goodsEv |= e.value("kind", "") == "goods" && e.value("item", "") == "meal";
    }
    CHECK(seenEv && goodsEv);
    const MemoryEntry* saw = belief(*k.witness, "saw_crime", k.thief->id);
    CHECK(saw && saw->fact == f.id);
    // the owners were told by the witness: the same fact, the witness in the chain
    const MemoryEntry* told = belief(*k.accuser, "told_crime", k.thief->id);
    CHECK(told && told->fact == f.id);
    // and a rumour of it carries the fact on
    Character* listener = k.gossip;
    listener->pos = k.witness->pos;
    applyEffects(w, *k.witness, *w.content().interaction("chat"), nullptr, listener);
    const MemoryEntry* r = belief(*listener, "rumor_crime", k.thief->id);
    CHECK(r && r->fact == f.id);
}

TEST(lr_sim_facts_false_accusation_refuted_by_witness) {
    World w = realWorld(3);
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    const EntityId f = theft(w, k);
    // the gossip told the accuser it was the innocent one, and he accused him
    believe(w, *k.accuser, "rumor_crime", k.innocent->id, k.store->id, 0.8, {k.gossip->id}, f);
    applyEffects(w, *k.accuser, *w.content().interaction("accuse_publicly"), nullptr, k.innocent);
    CHECK(casePending(*k.innocent, k.accuser->id, k.store->id));
    const double trustInn = k.accuser->relations[k.innocent->id].trust;
    const double trustGossip = k.accuser->relations[k.gossip->id].trust;
    const double honesty0 = k.innocent->reputation["reputation:honesty"];
    // one who really saw it tells him how it was
    applyEffects(w, *k.witness, *w.content().interaction("vouch"), nullptr, k.accuser);
    const MemoryEntry* wrong = belief(*k.accuser, "rumor_crime", k.innocent->id);
    CHECK(!wrong || wrong->confidence < 0.2);
    const MemoryEntry* right = belief(*k.accuser, "told_crime", k.thief->id);
    CHECK(right && right->fact == f && !right->chain.empty() && right->chain[0] == k.witness->id);
    CHECK(k.accuser->relations[k.innocent->id].trust > trustInn);
    CHECK(k.accuser->relations[k.gossip->id].trust < trustGossip);
    CHECK(k.innocent->reputation["reputation:honesty"] > honesty0);
    CHECK(chronicled(w, "fact_vouched"));
    CHECK(chronicled(w, "case_cleared"));
    CHECK(!casePending(*k.innocent, k.accuser->id, k.store->id));
    // nothing left to refute: a second vouch changes nothing
    CHECK_EQ(vouch(w, *k.witness, *k.accuser), 0);
    // one who did not see it cannot vouch
    believe(w, *k.accuser, "suspects", k.innocent->id, k.store->id, 0.7, {}, f);
    CHECK_EQ(vouch(w, *k.gossip, *k.accuser), 0);
}

TEST(lr_sim_facts_prove_innocence_or_caught_lying) {
    World w = realWorld(5);
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    const EntityId f = theft(w, k);
    // the innocent points to the witness
    believe(w, *k.accuser, "suspects", k.innocent->id, k.store->id, 0.7, {}, f);
    CHECK(proveInnocence(w, *k.innocent, *k.accuser) == 1);
    CHECK(chronicled(w, "fact_proved"));
    CHECK(belief(*k.accuser, "told_crime", k.thief->id) != nullptr);
    // the thief claims innocence before a true witness: caught lying
    believe(w, *k.gossip, "suspects", k.thief->id, k.store->id, 0.6, {}, f);
    const double trust0 = k.gossip->relations[k.thief->id].trust;
    CHECK_EQ(proveInnocence(w, *k.thief, *k.gossip), -1);
    CHECK(chronicled(w, "lie_exposed"));
    CHECK(k.gossip->relations[k.thief->id].trust < trust0);
    const MemoryEntry* still = belief(*k.gossip, "suspects", k.thief->id);
    CHECK(still && still->confidence >= 0.9);
}

TEST(lr_sim_facts_rumour_of_nothing_falls_apart) {
    // a lie: a theft from a store where nothing was ever taken (no fact)
    World w = realWorld(9);
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    CHECK(w.facts().empty());
    believe(w, *k.accuser, "rumor_crime", k.innocent->id, k.store->id, 0.6, {k.gossip->id}, kNone);
    const double trustGossip = k.accuser->relations[k.gossip->id].trust;
    CHECK_EQ(proveInnocence(w, *k.innocent, *k.accuser), 1);
    const MemoryEntry* m = belief(*k.accuser, "rumor_crime", k.innocent->id);
    CHECK(!m || m->confidence < 0.2);
    CHECK(k.accuser->relations[k.gossip->id].trust < trustGossip);
}

TEST(lr_sim_facts_truth_report) {
    World w = realWorld(11);
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    const EntityId f = theft(w, k);
    believe(w, *k.accuser, "rumor_crime", k.innocent->id, k.store->id, 0.6, {k.gossip->id}, f);
    believe(w, *k.gossip, "rumor_crime", k.thief->id, k.store->id, 0.6, {k.witness->id}, f);
    believe(w, *k.gossip, "suspects", k.innocent->id, k.store->id, 0.4, {}, kNone);
    const Json r = truthReport(w);
    CHECK_EQ(r["facts"].size(), size_t(1));
    const Json& b = r["facts"][0]["beliefs"];
    CHECK_EQ(b.value("true", 0), 2); // the witness and the gossip's rumour
    CHECK_EQ(b.value("wrong", 0), 1);
    CHECK_EQ(r.value("unlinked_beliefs", 0), 1);
}

TEST(lr_sim_facts_witness_speaks_up_for_the_accused) {
    // the innocent denies it, and the one who really saw it goes to tell the accuser
    World w = realWorld(13);
    Cast k = cast(w);
    CHECK(k.accuser && k.store);
    if (!k.accuser || !k.store) return;
    const EntityId f = theft(w, k);
    believe(w, *k.accuser, "suspects", k.innocent->id, k.store->id, 0.7, {}, f);
    applyEffects(w, *k.accuser, *w.content().interaction("accuse_publicly"), nullptr, k.innocent);
    w.run(static_cast<Tick>(caseRules(w.content()).answerHours * kTicksPerDay / 24.0) + 2);
    bool queued = false;
    for (const auto& q : k.witness->ai.queue) queued |= q.task == "vouch" && q.target == k.accuser->id;
    for (const auto& q : k.innocent->ai.queue) queued |= q.task == "prove_innocence" && q.target == k.accuser->id;
    CHECK(queued || chronicled(w, "fact_vouched") || chronicled(w, "fact_proved"));
}
