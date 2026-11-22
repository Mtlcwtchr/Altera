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
#include "sim/loc_gen.hpp"
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
        CHECK(text.find(realLoc().personNoun(lang, refuser)["nom"].get<std::string>()) != std::string::npos);
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

TEST(altera_dialogue_say_menu_topics_and_labels) {
    World w = realWorld();
    auto people = adults(w);
    if (people.size() < 3) return;
    Character& speaker = *people[0];
    Character* listener = nullptr;
    Character* thief = nullptr;
    for (Character* c : people)
        if (c != &speaker) (listener ? thief : listener) = (listener ? (thief ? thief : c) : c);
    CHECK(listener && thief && listener != thief);
    if (!listener || !thief) return;
    WorldObject* store = nullptr;
    for (auto& [id, o] : w.objects())
        if (o.type == "storage" && o.ownerFamily != kNone && o.ownerFamily != thief->status.family) store = &o;
    if (!store) return;
    MemoryEntry m;
    m.type = "saw_crime";
    m.who = {thief->id};
    m.object = store->id;
    m.source = "seen";
    m.confidence = 1.0;
    m.valence = -0.6;
    w.remember(speaker, m);
    // to a third person: news; to the thief himself: an accusation
    auto news = dialogueTopics(w, speaker, *listener);
    CHECK(!news.empty());
    if (!news.empty()) CHECK_EQ(news.front().act, std::string("inform"));
    auto acc = dialogueTopics(w, speaker, *thief);
    CHECK(!acc.empty());
    if (acc.empty() || news.empty()) return;
    CHECK_EQ(acc.front().act, std::string("accuse"));
    for (const std::string lang : {"ru", "en"}) {
        const std::string a = menuLabel(realLoc(), lang, w, speaker, *listener, news.front());
        const std::string b = menuLabel(realLoc(), lang, w, speaker, *thief, acc.front());
        CHECK(clean(a));
        CHECK(clean(b));
        CHECK(a != b);
    }
    // the agent gets the meaning, not words
    Utterance u;
    u.speaker = speaker.id;
    u.listener = listener->id;
    u.act = "inform";
    u.content = news.front().prop;
    const Json c = agentContent(u);
    CHECK_EQ(c["kind"].get<std::string>(), std::string("fact"));
    CHECK_EQ(c["params"]["pred"].get<std::string>(), std::string("theft"));
    CHECK(c.contains("ref") && c.contains("epi"));
    u.content.reset();
    CHECK_EQ(agentContent(u)["kind"].get<std::string>(), std::string("none"));
}

TEST(altera_dialogue_say_barks_are_presentation_only) {
    World w = realWorld();
    auto people = adults(w);
    if (people.empty()) return;
    Character& c = *people[0];
    c.needs["hunger"] = 0.05;
    const std::uint64_t before = w.stateHash();
    const SaidLine a = chooseBark(w, c);
    const SaidLine b = chooseBark(w, c);
    CHECK(a.ok());
    CHECK_EQ(a.key, b.key); // the same tick, the same line
    CHECK(a.rule.rfind("bark.", 0) == 0);
    CHECK(a.rule == "bark.starving"); // the most specific: hunger < 0.1 beats < 0.3
    CHECK_EQ(w.stateHash(), before);  // nothing written into the world
    CHECK(clean(utteranceText(realLoc(), "ru", w, a.utterance, a.key)));
    c.ai.fsm.state = "Sleep";
    CHECK(!chooseBark(w, c).ok());
}

// AL-58 (from the AL-54 corpus): the game's lines get every v0.1 argument (no raw
// "{Action}"), the teller as {src}, the listener inside the content as "ты" / "вы"
// toward the nobility, and the parts around a line glued into one sentence.
TEST(altera_dialogue_say_v01_args_and_address) {
    World w = realWorld();
    auto people = adults(w);
    CHECK(people.size() >= 3);
    if (people.size() < 3) return;
    Character& a = *people[0];
    Character& b = *people[1];
    Character& teller = *people[2];
    const Loc& loc = realLoc();
    const DialogueGenData& D = dialogueGenData(w.content());

    Prop theft;
    theft.pred = "theft";
    theft.roles["doer"] = Ref{"char", b.id, "", 0.0, {}};
    theft.roles["victim"] = Ref{"char", a.id, "", 0.0, {}};
    theft.roles["object"] = Ref{"item", kNone, "flour", 0.0, {}};
    theft.tense = "past";
    Utterance u;
    u.speaker = a.id;
    u.listener = b.id;
    u.act = "accuse";
    u.content = theft;
    u.epi.source = "told";
    u.epi.chain = {teller.id};
    u.seed = 11;
    u.args["action"] = Ref{"text", kNone, "interaction.chat.name", 0.0, {}};

    // every ported v0.1 line and every say.* line renders with nothing raw left
    int bad = 0;
    for (const std::string lang : {"ru", "en"})
        for (const auto& r : D.rules) {
            if (r.say.rfind("dialogue.", 0) != 0 && r.say.rfind("say.", 0) != 0) continue;
            for (std::uint64_t seed = 1; seed <= 3; ++seed) {
                u.seed = seed;
                const std::string t = utteranceText(loc, lang, w, u, r.say);
                if (!clean(t)) {
                    if (bad < 5) std::printf("  raw %s %s: %s\n", lang.c_str(), r.say.c_str(), t.c_str());
                    ++bad;
                }
            }
        }
    CHECK_EQ(bad, 0);

    // the teller is {src}, the listener carries his id
    Json args = utteranceArgs(loc, "ru", w, u);
    CHECK(args.contains("src") && args["src"].value("id", kNone) == teller.id);
    CHECK(args["listener"].value("id", kNone) == b.id);
    CHECK(!args["listener"].contains("reg"));
    // the listener's own deed is said to him: "ты"
    std::string clause = realizeProp(loc, "ru", args["prop"], "clause", "", args, 1);
    CHECK(clause.find("ты") != std::string::npos);
    // toward the nobility: "вы"
    b.traits.push_back("noble");
    CHECK(addressedVy(w, a.id, b.id));
    args = utteranceArgs(loc, "ru", w, u);
    CHECK_EQ(args["listener"].value("reg", std::string()), std::string("vy"));
    clause = realizeProp(loc, "ru", args["prop"], "clause", "", args, 1);
    CHECK(clause.find("вы") != std::string::npos);
    b.traits.pop_back();

    // parts: a noise opener and a tail glue onto the line
    std::string open, tail;
    for (const auto& r : D.rules)
        for (const auto& c : r.when) {
            if (c.key != "slot" || c.values.empty()) continue;
            if (open.empty() && c.values.front() == "noise.open") open = r.say;
            if (tail.empty() && c.values.front() == "voice.tail") tail = r.say;
        }
    CHECK(!open.empty() && !tail.empty());
    const std::string line = utteranceText(loc, "ru", w, u, "say.inform.told");
    const std::string whole = utteranceText(loc, "ru", w, u, "say.inform.told", {open}, {tail});
    CHECK(clean(whole));
    CHECK(whole.size() > line.size());
    Json params = {{"key", "say.inform.told"}, {"utt", toJson(u)}, {"pre", Json::array({open})},
                   {"post", Json::array({tail})}};
    const std::string traced = lineText(loc, "ru", w, params);
    CHECK(traced.find(whole) != std::string::npos);
    // the args survive the trace
    Utterance back;
    CHECK(fromJson(toJson(u), back));
    CHECK(back == u);
}
