// Rumour noise (AL-50): retelling distorts the meaning deterministically,
// keeps the truth reference, grows the chain, and the rare operators stay rare.

#include "framework.hpp"
#include "sim/rumor_noise.hpp"

using namespace altera::sim;

namespace {

const DialogueGenData& data() {
    static const DialogueGenData d = DialogueGenData::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return d;
}

Ref ch(EntityId id) {
    Ref r;
    r.kind = "char";
    r.id = id;
    return r;
}

// Oleg (12) stole a sack of flour from the miller (4), night of tick 1440; fact 9.
Prop truth() {
    Prop p;
    p.pred = "theft";
    p.roles["doer"] = ch(12);
    p.roles["victim"] = ch(4);
    Ref item;
    item.kind = "item";
    item.key = "flour";
    p.roles["object"] = item;
    p.qty = 1;
    p.unit = "sack";
    p.tick = 1440;
    p.fact = 9;
    return p;
}

NoiseContext ctx(EntityId teller) {
    NoiseContext c;
    c.teller = teller;
    c.listener = 99;
    c.agentCandidates = {{30, 1.0}, {31, 0.5}, {12, 5.0}};
    c.victimCandidates = {{40, 1.0}};
    c.itemCategory = {{"flour", "food"}};
    return c;
}

NoiseParams only(const std::string& id, double base, double belowConf = 1.0) {
    NoiseParams p;
    NoiseParams::Op op;
    op.base = base;
    op.belowConf = belowConf;
    op.roles = {"tick", "qty", "victim"};
    p.ops[id] = op;
    return p;
}

} // namespace

TEST(altera_rumor_noise_deterministic_and_truth_kept) {
    const NoiseParams p = NoiseParams::fromJson(data().noise);
    Epistemic e;
    e.source = "seen";
    Rng a(77), b(77);
    const NoiseResult x = retell(data(), p, truth(), e, ctx(7), a);
    const NoiseResult y = retell(data(), p, truth(), e, ctx(7), b);
    CHECK(x.prop == y.prop);
    CHECK(x.applied == y.applied);
    CHECK_EQ(x.prop.fact, 9u);
    CHECK_EQ(x.epi.source, std::string("rumor"));
    CHECK(std::abs(x.epi.confidence - (0.7 + (std::find(x.applied.begin(), x.applied.end(), "exaggerate") != x.applied.end() ? 0.05 : 0.0))) < 1e-9);
    CHECK_EQ(x.epi.chain.size(), 1u);
    CHECK_EQ(x.epi.chain[0], 7u);
}

TEST(altera_rumor_noise_chain_grows_and_caps) {
    NoiseParams p; // no operators: only decay and the chain
    Epistemic e;
    Prop cur = truth();
    for (EntityId teller : {1u, 2u, 3u, 4u, 5u}) {
        Rng r(teller);
        const NoiseResult n = retell(data(), p, cur, e, ctx(teller), r);
        cur = n.prop;
        e = n.epi;
    }
    CHECK(cur == truth());
    CHECK_EQ(e.chain.size(), Epistemic::kMaxChain);
    CHECK_EQ(e.chain[0], 5u);
    CHECK(std::abs(e.confidence - std::pow(0.7, 5)) < 1e-9);
}

TEST(altera_rumor_noise_operators) {
    Epistemic e;
    e.confidence = 0.5;
    e.chain = {3, 2};
    // vague_theme: flour -> food -> something
    {
        Rng r(1);
        NoiseResult n = retell(data(), only("vague_theme", 1.0), truth(), e, ctx(7), r);
        CHECK_EQ(n.prop.role("object")->key, std::string("category.food"));
        CHECK_EQ(n.prop.qty, 0.0);
        n = retell(data(), only("vague_theme", 1.0), n.prop, e, ctx(7), r);
        CHECK_EQ(n.prop.role("object")->key, std::string("thing.something"));
        const NoiseResult again = retell(data(), only("vague_theme", 1.0), n.prop, e, ctx(7), r);
        CHECK(again.applied.empty());
    }
    // exaggerate: theft -> banditry, a sack -> two
    {
        Rng r(1);
        const NoiseResult n = retell(data(), only("exaggerate", 1.0), truth(), e, ctx(7), r);
        CHECK_EQ(n.prop.pred, std::string("banditry"));
        CHECK_EQ(n.prop.qty, 2.0);
    }
    // minimize only from a friend of the doer
    {
        NoiseParams p = only("minimize", 1.0);
        p.ops["minimize"].relOver = 0.4;
        Rng r(1);
        CHECK(retell(data(), p, truth(), e, ctx(7), r).applied.empty());
        NoiseContext friendly = ctx(7);
        friendly.tellerAffinityToDoer = 0.8;
        const NoiseResult n = retell(data(), p, truth(), e, friendly, r);
        CHECK_EQ(n.prop.pred, std::string("take"));
    }
    // swap_agent: never the same doer, never teller/listener, only below its confidence
    {
        Rng r(5);
        const NoiseResult n = retell(data(), only("swap_agent", 1.0, 0.45), truth(), e, ctx(7), r);
        CHECK(n.applied.size() == 1);
        const EntityId to = n.prop.role("doer")->id;
        CHECK(to == 30 || to == 31);
        Epistemic sure = e;
        sure.confidence = 0.9; // 0.9 * 0.7 = 0.63 >= 0.45
        CHECK(retell(data(), only("swap_agent", 1.0, 0.45), truth(), sure, ctx(7), r).applied.empty());
        NoiseContext none = ctx(7);
        none.agentCandidates = {{12, 1.0}};
        CHECK(retell(data(), only("swap_agent", 1.0, 0.45), truth(), e, none, r).applied.empty());
    }
    // invert swaps doer and victim; attribute_shift forgets the origin
    {
        Rng r(1);
        const NoiseResult n = retell(data(), only("invert", 1.0), truth(), e, ctx(7), r);
        CHECK_EQ(n.prop.role("doer")->id, 4u);
        CHECK_EQ(n.prop.role("victim")->id, 12u);
        const NoiseResult s = retell(data(), only("attribute_shift", 1.0), truth(), e, ctx(7), r);
        CHECK(s.epi.chain == (std::vector<EntityId>{7, 3}));
    }
    // drop_detail never drops a required role
    {
        Prop bare;
        bare.pred = "theft";
        bare.roles["doer"] = ch(12);
        NoiseParams p = only("drop_detail", 1.0);
        p.ops["drop_detail"].roles = {"doer"};
        Rng r(1);
        CHECK(retell(data(), p, bare, e, ctx(7), r).applied.empty());
    }
}

TEST(altera_rumor_noise_rates_shipped) {
    // With the shipped parameters a doer is swapped rarely, and only once the
    // story is unsure; the truth reference is always kept.
    const NoiseParams p = NoiseParams::fromJson(data().noise);
    int swapped = 0, drifted = 0;
    const int n = 4000;
    for (int i = 0; i < n; ++i) {
        Rng r(1000 + static_cast<std::uint64_t>(i));
        Epistemic e;
        Prop cur = truth();
        for (int hop = 0; hop < 3; ++hop) {
            const NoiseResult res = retell(data(), p, cur, e, ctx(static_cast<EntityId>(50 + hop)), r);
            cur = res.prop;
            e = res.epi;
            CHECK_EQ(cur.fact, 9u);
        }
        if (cur.role("doer") && cur.role("doer")->id != 12) ++swapped;
        if (drift(truth(), cur) > 0.0) ++drifted;
    }
    const double swapRate = static_cast<double>(swapped) / n;
    CHECK(swapRate > 0.0);
    CHECK(swapRate < 0.08);
    CHECK(drifted > n / 4); // three retellings change most stories a little
}

TEST(altera_rumor_noise_reconcile_and_drift) {
    const NoiseParams p = NoiseParams::fromJson(data().noise);
    Prop wrong = truth();
    wrong.roles["doer"] = ch(30);
    Epistemic saw;
    saw.source = "seen";
    Epistemic heard;
    heard.source = "rumor";
    bool contra = false;
    CHECK(reconcile(p, wrong, truth(), saw, &contra));
    CHECK(contra);
    CHECK(!reconcile(p, wrong, truth(), heard, &contra));
    CHECK(contra);
    CHECK(!reconcile(p, truth(), truth(), saw, &contra));
    CHECK(!contra);
    CHECK_EQ(drift(truth(), truth()), 0.0);
    CHECK(drift(truth(), wrong) > 0.3);
    Prop bigger = truth();
    bigger.pred = "banditry";
    CHECK(drift(truth(), bigger) >= 0.5);
}
