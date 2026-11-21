// Saying lines in the sim (AL-53) and no silent "no" (AL-36): a refusal is
// said aloud with the deed the refuser holds against the asker, a failed
// social act gets a reaction, both sides' traces carry the line, the text is
// assembled at output, and the world remembers what was said across a save.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    return l;
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

const TraceEntry* lastLine(const Character& c) {
    for (auto it = c.ai.trace.rbegin(); it != c.ai.trace.rend(); ++it)
        if (it->code == "say.line") return &*it;
    return nullptr;
}

bool clean(const std::string& s) {
    return !s.empty() && s.find('[') == std::string::npos && s.find('{') == std::string::npos;
}

} // namespace

TEST(altera_dialogue_say_refusal_with_reason) {
    World w = realWorld();
    auto people = adults(w);
    CHECK(people.size() >= 3);
    if (people.size() < 3) return;
    Character& asker = *people[0];
    Character* other = nullptr;
    for (Character* c : people)
        if (c->status.family != asker.status.family) other = c;
    CHECK(other != nullptr);
    if (!other) return;
    Character& refuser = *other;
    // a plain no first: nothing held against the asker
    SaidLine plain = sayRefusal(w, refuser, asker);
    CHECK(plain.ok());
    CHECK(plain.key.rfind("recall.", 0) != 0);
    // the refuser's household was robbed by the asker, and he saw it
    WorldObject* store = nullptr;
    for (auto& [id, o] : w.objects())
        if (o.type == "storage" && o.ownerFamily == refuser.status.family) store = &o;
    if (!store) {
        for (auto& [id, o] : w.objects())
            if (o.type == "storage" && o.ownerFamily != kNone && o.ownerFamily != asker.status.family) {
                store = &o;
                refuser.status.family = o.ownerFamily;
                break;
            }
    }
    CHECK(store != nullptr);
    if (!store) return;
    MemoryEntry m;
    m.type = "saw_crime";
    m.who = {asker.id};
    m.object = store->id;
    m.source = "seen";
    m.confidence = 1.0;
    m.valence = -0.6;
    w.remember(refuser, m);
    const SaidLine line = sayRefusal(w, refuser, asker);
    CHECK(line.ok());
    if (line.key.rfind("recall.refuse.", 0) != 0) {
        ContextFacts f;
        chooseLine(dialogueGenData(w.content()), w, line.utterance, nullptr, false, &f);
        std::string dump;
        for (const auto& [k, v] : f.str) dump += k + "=" + v + " ";
        for (const auto& [k, v] : f.num) dump += k + "=" + std::to_string(v) + " ";
        ::testing::fail(__FILE__, __LINE__, "key " + line.key + " facts: " + dump);
    }
    CHECK(line.utterance.content.has_value());
    CHECK_EQ(line.utterance.act, std::string("refuse"));
    // both sides hear it
    const TraceEntry* a = lastLine(asker);
    const TraceEntry* r = lastLine(refuser);
    CHECK(a && r);
    if (!a || !r) return;
    CHECK_EQ(a->params["key"].get<std::string>(), line.key);
    for (const std::string lang : {"ru", "en"}) {
        const std::string text = traceText(realLoc(), lang, w, asker, *a);
        CHECK(clean(text));
        CHECK(text.find(refuser.name) != std::string::npos);
    }
}

TEST(altera_dialogue_say_fail_reaction_and_said_memory) {
    World w = realWorld();
    auto people = adults(w);
    if (people.size() < 2) return;
    Character& actor = *people[0];
    Character& target = *people[1];
    const InteractionDef* joke = w.content().interaction("joke");
    CHECK(joke != nullptr);
    if (!joke) return;
    const SaidLine first = sayFailReaction(w, target, actor, *joke);
    CHECK(first.ok());
    CHECK_EQ(first.utterance.intent, std::string("fail"));
    CHECK(clean(traceText(realLoc(), "ru", w, actor, *lastLine(actor))));
    // the rule's cooldown holds right after: the next reaction is another line
    const SaidLine second = sayFailReaction(w, target, actor, *joke);
    CHECK(second.ok());
    if (second.rule == first.rule && first.rule != "ignore.any")
        ::testing::fail(__FILE__, __LINE__, "rules " + first.rule + " / " + second.rule);
    CHECK(w.said().count(first.rule, target.id) >= 1);
    // the said memory survives a save
    const Json j = w.toJson();
    World back(w.content(), w.seed());
    back.loadJson(j);
    CHECK(back.said() == w.said());
}
