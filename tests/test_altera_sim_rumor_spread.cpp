// Rumour noise in the sim (AL-50 part 2): a retold misdeed reaches the
// listener decayed and sometimes distorted, the truth reference always kept;
// a listener who saw it himself rejects a wrong version and remembers the
// teller lied; the spread report measures the drift from the world's facts.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/dialogue_memory.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

std::vector<Character*> adults(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") out.push_back(&c);
    return out;
}

WorldObject* storeOf(World& w, EntityId notFamily) {
    for (auto& [id, o] : w.objects())
        if (o.type == "storage" && o.ownerFamily != kNone && o.ownerFamily != notFamily) return &o;
    return nullptr;
}

MemoryEntry deedMemory(const std::string& type, EntityId doer, EntityId object, EntityId fact, const std::string& source,
                       double conf) {
    MemoryEntry m;
    m.type = type;
    m.who = {doer};
    m.object = object;
    m.fact = fact;
    m.source = source;
    m.confidence = conf;
    m.valence = -0.4;
    return m;
}

const MemoryEntry* rumorOf(const Character& c, EntityId fact) {
    for (const auto& m : c.memory)
        if (m.type == "rumor_crime" && m.fact == fact) return &m;
    return nullptr;
}

} // namespace

TEST(altera_rumor_spread_noise_in_retelling) {
    World w = realWorld();
    auto people = adults(w);
    CHECK(people.size() >= 8);
    if (people.size() < 8) return;
    Character& doer = *people[0];
    Character& teller = *people[1];
    WorldObject* store = storeOf(w, doer.status.family);
    CHECK(store != nullptr);
    if (!store) return;
    Fact f;
    f.pred = "theft";
    f.roles = {{"doer", doer.id}, {"object", store->id}, {"victim_family", store->ownerFamily}};
    f.witnesses = {teller.id};
    const EntityId fact = w.addFact(f).id;
    w.remember(teller, deedMemory("saw_crime", doer.id, store->id, fact, "seen", 1.0));
    const ConsequenceRules& R = consequenceRules(w.content());

    // the teller tells everyone in turn; each listener tells the next
    Character* from = &teller;
    int heard = 0, distorted = 0;
    for (size_t i = 2; i < people.size(); ++i) {
        Character& to = *people[i];
        if (to.id == doer.id) continue;
        passRumor(w, teller, to, R);
        const MemoryEntry* m = rumorOf(to, fact);
        if (!m) continue;
        ++heard;
        CHECK_EQ(m->fact, fact);
        CHECK(!m->chain.empty() && m->chain[0] == teller.id);
        CHECK(m->confidence < 1.0);
        if (!m->prop.is_null()) {
            ++distorted;
            Prop p;
            CHECK(fromJson(m->prop, p));
            CHECK_EQ(p.fact, fact);
        }
        from = &to;
    }
    (void)from;
    CHECK(heard >= 5);

    // the second hop: further decayed, chain of two, still the same fact
    Character* first = nullptr;
    Character* next = nullptr;
    for (size_t i = 2; i < people.size() && !next; ++i) {
        if (!rumorOf(*people[i], fact)) continue;
        if (!first) first = people[i];
    }
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && !rumorOf(c, fact) && c.id != doer.id && c.id != teller.id &&
            c.body.stage() != "child") {
            next = &c;
            break;
        }
    if (first && next) {
        passRumor(w, *first, *next, R);
        if (const MemoryEntry* m = rumorOf(*next, fact)) {
            CHECK_EQ(m->chain.size(), 2u);
            CHECK_EQ(m->chain[0], first->id);
            CHECK(m->confidence < rumorOf(*first, fact)->confidence);
        }
    }

    const Json rep = rumorReport(w);
    CHECK_EQ(rep["facts"].get<int>(), 1);
    CHECK(rep["memories"].get<int>() >= heard + 1);
    CHECK(rep["distorted"].get<int>() >= distorted);
    CHECK(rep["mean_drift"].get<double>() > 0.0);
    CHECK(rep["max_hops"].get<int>() >= 1);

    // drop_detail alone fires about one time in four: eight listeners may all be lucky, so a few
    // more villages (the same one, a tick or two on) tell it too, until some retelling is distorted
    for (int k = 1; k <= 4 && distorted == 0; ++k) {
        World v = realWorld();
        for (int i = 0; i < k; ++i) v.step();
        auto vp = adults(v);
        if (vp.size() < 3) break;
        WorldObject* vs = storeOf(v, vp[0]->status.family);
        if (!vs) break;
        Fact g;
        g.pred = "theft";
        g.roles = {{"doer", vp[0]->id}, {"object", vs->id}, {"victim_family", vs->ownerFamily}};
        g.witnesses = {vp[1]->id};
        const EntityId gf = v.addFact(g).id;
        v.remember(*vp[1], deedMemory("saw_crime", vp[0]->id, vs->id, gf, "seen", 1.0));
        for (size_t i = 2; i < vp.size(); ++i) {
            passRumor(v, *vp[1], *vp[i], consequenceRules(v.content()));
            if (const MemoryEntry* m = rumorOf(*vp[i], gf); m && !m->prop.is_null()) ++distorted;
        }
    }
    CHECK(distorted >= 1);
}

TEST(altera_rumor_spread_eyewitness_rejects_lie) {
    World w = realWorld();
    auto people = adults(w);
    CHECK(people.size() >= 4);
    if (people.size() < 4) return;
    Character& doer = *people[0];
    Character& framed = *people[1];
    Character& teller = *people[2];
    Character& witness = *people[3];
    WorldObject* store = storeOf(w, doer.status.family);
    if (!store) return;
    Fact f;
    f.pred = "theft";
    f.roles = {{"doer", doer.id}, {"object", store->id}, {"victim_family", store->ownerFamily}};
    f.witnesses = {witness.id};
    const EntityId fact = w.addFact(f).id;
    w.remember(witness, deedMemory("saw_crime", doer.id, store->id, fact, "seen", 1.0));
    // the teller believes (wrongly) that `framed` did it
    MemoryEntry wrong = deedMemory("rumor_crime", framed.id, store->id, fact, "rumor", 0.9);
    Prop p;
    p.pred = "theft";
    Ref d;
    d.kind = "char";
    d.id = framed.id;
    p.roles["doer"] = d;
    p.fact = fact;
    wrong.prop = toJson(p);
    w.remember(teller, wrong);

    const ConsequenceRules& R = consequenceRules(w.content());
    const double trustBefore = witness.relations[teller.id].trust;
    passRumor(w, teller, witness, R);
    int caught = 0;
    for (const auto& m : witness.memory) caught += m.type == "caught_lying" && m.fact == fact && m.who[0] == teller.id;
    CHECK_EQ(caught, 1);
    CHECK(witness.relations[teller.id].trust < trustBefore);
    CHECK(rumorOf(witness, fact) == nullptr); // the wrong version is not taken in
    // and he is not told the same lie twice
    passRumor(w, teller, witness, R);
    caught = 0;
    for (const auto& m : witness.memory) caught += m.type == "caught_lying" && m.fact == fact;
    CHECK_EQ(caught, 1);
    // the distorted memory is what recall brings up: the framed one, same fact
    const auto rec = recall(w, teller, framed.id);
    CHECK(!rec.empty());
    if (!rec.empty()) CHECK_EQ(rec.front().prop.fact, fact);
}
