// LR-121…135: the bugs of the AL-120 QA playtest, checked headless.
// LR-126: no wedding with one's spouse or blood kin; LR-123: parametric acts
// are bound by id (lend#item=bread) and carry out their terms (a loan is a debt).

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& qaContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World qaWorld() {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(qaContent(), cfg, 7);
    installAi(w);
    return w;
}

// A married adult with a child, or null.
Character* parentWithSpouse(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.spouse != kNone && !c.status.children.empty() && w.character(c.status.spouse))
            return &c;
    return nullptr;
}

} // namespace

TEST(lr_qa_marry_not_spouse_or_kin) {
    World w = qaWorld();
    Character* c = parentWithSpouse(w);
    CHECK(c != nullptr);
    if (!c) return;
    Character* spouse = w.character(c->status.spouse);
    Character* child = w.character(c->status.children.front());
    CHECK(spouse && child);
    if (!spouse || !child) return;
    // make them fond of each other: only kinship may refuse
    c->relations[spouse->id].affinity = 0.9;
    c->relations[child->id].affinity = 0.9;
    for (const char* act : {"marry", "propose_marriage"}) {
        const InteractionDef* d = w.content().interaction(act);
        CHECK(d != nullptr);
        if (!d) continue;
        std::string why;
        CHECK(!canPerform(w, *c, *d, nullptr, &why, false, spouse));
        why.clear();
        CHECK(!canPerform(w, *c, *d, nullptr, &why, false, child));
    }
}

TEST(lr_qa_bound_act_ids) {
    const Content& ct = qaContent();
    const InteractionDef* d = ct.interaction("lend#item=bread");
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(!d->parametric());
    CHECK(d->baseId == "lend");
    bool bound = false;
    for (const auto& k : d->pre)
        if (k.subject == "has" && k.key == "bread") bound = true;
    CHECK(bound);
    CHECK(d->terms.give.count("bread") == 1);
    CHECK(ct.interaction("lend#item=no_such_thing") == nullptr);
    CHECK(ct.interaction("lend#color=red") == nullptr);
    CHECK(ct.interaction("give#item=log") != nullptr);
    CHECK(ct.interaction("trade#give=bread,take=coin") != nullptr);
    CHECK(ct.interaction("trade#give=bread") == nullptr);   // take unbound
}

TEST(lr_qa_lend_is_a_debt) {
    World w = qaWorld();
    Character* c = parentWithSpouse(w);
    CHECK(c != nullptr);
    if (!c) return;
    Character* t = w.character(c->status.spouse);
    c->inventory["bread"] = 3;
    const int had = t->inventory.count("bread") ? t->inventory["bread"] : 0;
    const InteractionDef* lend = w.content().interaction("lend#item=bread");
    CHECK(lend != nullptr);
    if (!lend) return;
    applyEffects(w, *c, *lend, nullptr, t);
    CHECK(c->inventory["bread"] == 2);
    CHECK(t->inventory["bread"] == had + 1);
    CHECK(c->relations[t->id].debt >= 1.0);
    // collect_debt wants a debt; the borrower giving back pays it off
    const InteractionDef* collect = w.content().interaction("collect_debt");
    std::string why;
    CHECK(canPerform(w, *c, *collect, nullptr, &why, false, t));
    const InteractionDef* repay = w.content().interaction("repay_debt#item=bread");
    CHECK(repay != nullptr);
    applyEffects(w, *t, *repay, nullptr, c);
    CHECK(c->relations[t->id].debt < 0.5);
    CHECK(!canPerform(w, *c, *collect, nullptr, &why, false, t));
}
