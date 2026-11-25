// Memory -> meaning (AL-49, AL-38): a remembered deed becomes a Prop with its
// Epistemic, word of it carries the chain of tellers and the fact, recall
// ranks what a person holds against another, the context facts make the
// AL-38 rules win in a greeting / refusal / bargain, the line renders, and
// the wronged are slower to say yes.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/dialogue_memory.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

struct Scene {
    Character* doer = nullptr;
    WorldObject* store = nullptr;
    Character* bystander = nullptr;
    Character* owner = nullptr;
};

Scene scene(World& w) {
    Scene s;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") {
            s.doer = &c;
            break;
        }
    if (!s.doer) return s;
    for (auto& [id, o] : w.objects())
        if (o.type == "storage" && o.ownerFamily != kNone && o.ownerFamily != s.doer->status.family) {
            s.store = &o;
            break;
        }
    if (!s.store) return s;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        if (!s.bystander && c.status.family != s.doer->status.family && c.status.family != s.store->ownerFamily)
            s.bystander = &c;
        if (!s.owner && c.status.family == s.store->ownerFamily) s.owner = &c;
    }
    return s;
}

void clearAround(World& w, Vec2 at, const std::vector<EntityId>& keep) {
    for (auto& [id, c] : w.characters())
        if (std::find(keep.begin(), keep.end(), id) == keep.end() && dist(c.pos, at) < 400.0)
            c.pos = {at.x + 900.0, at.y + 900.0};
}

// A seen theft: the bystander saw it, the owners were told.
bool seenTheft(World& w, Scene& s) {
    s = scene(w);
    if (!s.doer || !s.store || !s.bystander || !s.owner) return false;
    s.store->stock["meal"] = 3;
    clearAround(w, s.store->pos, {s.doer->id, s.bystander->id});
    s.doer->pos = s.store->pos;
    s.bystander->pos = {s.store->pos.x + 5.0, s.store->pos.y};
    InteractionDef pub = *w.content().interaction("take_meal@foreign");
    pub.visibility = "public";
    applyEffects(w, *s.doer, pub, s.store, nullptr);
    return true;
}

// The most specific rule whose criteria all hold (what the rule base, AL-48, does without weights).
const DialogueRuleDef* pick(const DialogueGenData& d, const ContextFacts& f) {
    const DialogueRuleDef* best = nullptr;
    for (const auto& r : d.rules) {
        if (!std::all_of(r.when.begin(), r.when.end(), [&](const Criterion& c) { return c.match(f); })) continue;
        if (!best || r.specificity() > best->specificity()) best = &r;
    }
    return best;
}

} // namespace

TEST(lr_dialogue_memory_seen_theft_to_prop) {
    World w = realWorld();
    Scene s;
    CHECK(seenTheft(w, s));
    if (!s.doer) return;
    const DialogueGenData& d = dialogueGenData(w.content());
    const MemoryEntry* saw = nullptr;
    for (const auto& m : s.bystander->memory)
        if (m.type == "saw_crime") saw = &m;
    CHECK(saw != nullptr);
    if (!saw) return;
    auto p = propFromMemory(w, *s.bystander, *saw, d);
    CHECK(p.has_value());
    if (!p) return;
    CHECK_EQ(p->pred, std::string("theft"));
    CHECK(p->role("doer") && p->role("doer")->id == s.doer->id);
    // AL-58: the object is what was taken (meal), the storage only names the owners
    CHECK(p->role("object") && p->role("object")->kind == "item" && p->role("object")->key == "meal");
    CHECK(p->role("victim_family") && p->role("victim_family")->id == s.store->ownerFamily);
    CHECK(checkProp(d, *p));
    const Epistemic e = epistemicFromMemory(*saw, w.now(), d.forMemory("saw_crime"));
    CHECK_EQ(e.source, std::string("seen"));
    CHECK(e.confidence > 0.99);
    CHECK(e.chain.empty());

    const auto rec = recall(w, *s.bystander, s.doer->id);
    CHECK(!rec.empty());
    if (rec.empty()) return;
    CHECK_EQ(rec.front().kind, std::string("deed"));
    CHECK(!rec.front().mine);
    CHECK(rec.front().weight > 0.3);
    // the owners were told: theirs, so it weighs more, and the teller is the chain
    const auto own = recall(w, *s.owner, s.doer->id);
    CHECK(!own.empty());
    if (own.empty()) return;
    CHECK(own.front().mine);
    CHECK(own.front().weight > rec.front().weight);
    CHECK_EQ(own.front().epi.source, std::string("told"));
    CHECK(own.front().epi.chain.size() == 1 && own.front().epi.chain[0] == s.bystander->id);
    CHECK(deedWariness(w, *s.owner, s.doer->id) > deedWariness(w, *s.bystander, s.doer->id));
    CHECK(deedWariness(w, *s.owner, s.owner->id) == 0.0);
    CHECK(priceFactor(w, *s.owner, s.doer->id) > 1.0);
}

TEST(lr_dialogue_memory_rumor_keeps_chain_and_fact) {
    World w = realWorld();
    Scene s;
    CHECK(seenTheft(w, s));
    if (!s.doer) return;
    // pretend the registry (AL-55) had given the seen memory its fact
    for (auto& m : s.bystander->memory)
        if (m.type == "saw_crime") m.fact = 42;
    Character* listener = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.id != s.doer->id && c.id != s.bystander->id &&
            c.status.family != s.store->ownerFamily && c.status.family != s.doer->status.family) {
            listener = &c;
            break;
        }
    CHECK(listener != nullptr);
    if (!listener) return;
    listener->pos = s.bystander->pos;
    applyEffects(w, *s.bystander, *w.content().interaction("chat"), nullptr, listener);
    const MemoryEntry* heard = nullptr;
    for (const auto& m : listener->memory)
        if (m.type == "rumor_crime") heard = &m;
    CHECK(heard != nullptr);
    if (!heard) return;
    CHECK(heard->chain.size() == 1 && heard->chain[0] == s.bystander->id);
    CHECK_EQ(heard->fact, EntityId(42));
    const auto rec = recall(w, *listener, s.doer->id);
    CHECK(!rec.empty());
    if (rec.empty()) return;
    CHECK_EQ(rec.front().epi.source, std::string("rumor"));
    CHECK_EQ(rec.front().prop.fact, EntityId(42));
    CHECK_EQ(rec.front().topic, std::string("fact:42"));
    // saves keep chain and fact; an old save without them loads with defaults
    Json j = *listener;
    Character back = j.get<Character>();
    const MemoryEntry* kept = nullptr;
    for (const auto& m : back.memory)
        if (m.type == "rumor_crime") kept = &m;
    CHECK(kept && kept->chain == heard->chain && kept->fact == heard->fact);
    for (auto& m : j["memory"]) {
        m.erase("chain");
        m.erase("fact");
    }
    back = j.get<Character>();
    for (const auto& m : back.memory) CHECK(m.chain.empty() && m.fact == kNone);
}

TEST(lr_dialogue_memory_facts_pick_al38_rules) {
    World w = realWorld();
    Scene s;
    CHECK(seenTheft(w, s));
    if (!s.doer) return;
    const DialogueGenData& d = dialogueGenData(w.content());
    ContextFacts f;
    addMemoryFacts(w, *s.owner, *s.doer, f);
    CHECK_EQ(f.str["memory.deed"], std::string("theft"));
    CHECK(f.num.count("memory.deed.mine"));
    CHECK(f.num["memory.wariness"] > 0.15);
    CHECK(f.num.count("listener.reputation.honesty"));
    auto said = [&](const std::string& act) {
        ContextFacts g = f;
        g.set("act", act);
        const DialogueRuleDef* r = pick(d, g);
        return r ? r->id : std::string();
    };
    CHECK_EQ(said("greet"), std::string("greet.recall.theft_mine"));
    CHECK_EQ(said("refuse"), std::string("refuse.recall.theft_mine"));
    CHECK_EQ(said("bargain"), std::string("bargain.recall.mine"));
    // somebody who knows nothing greets plainly
    ContextFacts none;
    Character* stranger = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.id != s.doer->id && recall(w, c, s.doer->id).empty()) {
            stranger = &c;
            break;
        }
    CHECK(stranger != nullptr);
    if (!stranger) return;
    addMemoryFacts(w, *stranger, *s.doer, none);
    CHECK(!none.has("memory.deed"));
    none.set("act", "greet");
    const DialogueRuleDef* plain = pick(d, none);
    CHECK(plain && plain->id.rfind("greet.recall", 0) != 0);

    // the line renders with no placeholders or missing keys left, ru and en
    const auto rec = recall(w, *s.owner, s.doer->id);
    CHECK(!rec.empty());
    if (rec.empty()) return;
    const Utterance u = recallUtterance(w, *s.owner, *s.doer, "refuse", rec.front(), 1);
    CHECK(u.content && u.content->pred == "theft");
    CHECK(u.stance.valence < 0.0);
    for (const char* lang : {"ru", "en"})
        for (const char* key : {"recall.refuse.theft_mine", "recall.greet.seen", "memory.saw_crime.text"}) {
            const std::string text = renderUtterance(realLoc(), lang, w, u, key);
            CHECK(!text.empty());
            CHECK(text.find('{') == std::string::npos);
            CHECK(text.find('[') == std::string::npos);
        }
}

TEST(lr_dialogue_memory_wronged_say_no_more) {
    World w = realWorld();
    Scene s;
    CHECK(seenTheft(w, s));
    if (!s.doer) return;
    InteractionDef ask;
    ask.id = "ask_favor";
    ask.consent = "target";
    // same relation numbers, with and without the memory of the theft
    Character owner = *s.owner;
    const double with = consentChance(w, *s.doer, owner, ask, "");
    owner.memory.clear();
    const double without = consentChance(w, *s.doer, owner, ask, "");
    CHECK(with < without - 0.1);
}

// AL-58: the menu says "о краже еды", not "о краже кладовой"; without goods
// (an old save) the storage stays the object.
TEST(lr_dialogue_memory_theft_names_the_goods) {
    World w = realWorld();
    Scene s;
    CHECK(seenTheft(w, s));
    if (!s.doer) return;
    const DialogueGenData& d = dialogueGenData(w.content());
    // every memory of the deed knows what was taken, the rumour too
    Character* listener = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.id != s.doer->id && c.id != s.bystander->id &&
            c.status.family != s.store->ownerFamily && c.status.family != s.doer->status.family) {
            listener = &c;
            break;
        }
    CHECK(listener != nullptr);
    if (!listener) return;
    listener->pos = s.bystander->pos;
    applyEffects(w, *s.bystander, *w.content().interaction("chat"), nullptr, listener);
    int checked = 0;
    for (Character* c : {s.bystander, s.owner, listener})
        for (const auto& m : c->memory) {
            if (m.type != "saw_crime" && m.type != "told_crime" && m.type != "rumor_crime") continue;
            if (!m.prop.is_null()) continue; // a distorted rumour may have blurred the thing on purpose
            CHECK(m.goods.count("meal"));
            CHECK_EQ(m.object, s.store->id);
            auto p = propFromMemory(w, *c, m, d);
            CHECK(p && p->role("object") && p->role("object")->key == "meal");
            CHECK(p && p->role("victim_family") && p->role("victim_family")->id == s.store->ownerFamily);
            ++checked;
        }
    CHECK(checked >= 2);
    auto topics = dialogueTopics(w, *s.bystander, *s.owner);
    CHECK(!topics.empty());
    if (topics.empty()) return;
    const std::string ru = menuLabel(realLoc(), "ru", w, *s.bystander, *s.owner, topics.front());
    CHECK(ru.find("еды") != std::string::npos);
    CHECK(ru.find("кладов") == std::string::npos);
    if (ru.find("еды") == std::string::npos) ::testing::fail(__FILE__, __LINE__, ru.c_str());
    const std::string en = menuLabel(realLoc(), "en", w, *s.bystander, *s.owner, topics.front());
    CHECK(en.find("storage") == std::string::npos);
    // an old memory without goods: the storage it was
    MemoryEntry old;
    old.type = "saw_crime";
    old.who = {s.doer->id};
    old.object = s.store->id;
    auto p = propFromMemory(w, *s.bystander, old, d);
    CHECK(p && p->role("object") && p->role("object")->kind == "object" && p->role("object")->id == s.store->id);
}
