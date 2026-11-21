// Dialogue rules (AL-48): context facts from the utterance and the world, the
// most specific rule wins, ties by weight from the seed and independent of file
// order, cooldown / once_per from the memory of what was said, slots for parts
// of a line, fact providers, and the rule checks.

#include <algorithm>

#include "framework.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/world.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

DialogueGenData rulesOf(const char* text) {
    Json preds = Json::parse(R"({"predicates": [
        {"id": "theft", "roles": {"doer": "char", "object": ["item", "object"], "victim_family": "family"},
         "required": ["doer"], "tags": ["theft", "crime"]}]})");
    Json f = Json::parse(text);
    f["_file"] = "test.json";
    return DialogueGenData::fromJson(preds, {f});
}

Ref ch(EntityId id) {
    Ref r;
    r.kind = "char";
    r.id = id;
    return r;
}

Utterance theftTalk(EntityId speaker, EntityId listener, EntityId doer, const std::string& source = "rumor") {
    Utterance u;
    u.speaker = speaker;
    u.listener = listener;
    u.act = "inform";
    Prop p;
    p.pred = "theft";
    p.roles["doer"] = ch(doer);
    Ref fam;
    fam.kind = "family";
    fam.id = 40;
    p.roles["victim_family"] = fam;
    p.fact = 9;
    u.content = p;
    u.epi.source = source;
    u.epi.confidence = 0.45;
    u.epi.chain = {7, 8};
    u.epi.age = kTicksPerDay * 3;
    u.seed = 5;
    return u;
}

const char* kRules = R"({"rules": [
    {"id": "inform.any", "when": ["act=inform"], "say": "l.any"},
    {"id": "inform.theft", "when": ["act=inform", "pred=theft"], "say": "l.theft"},
    {"id": "inform.theft.rumor.a", "when": ["act=inform", "pred=theft", "epi.source=rumor"], "say": "l.a", "weight": 1},
    {"id": "inform.theft.rumor.b", "when": ["act=inform", "pred=theft", "epi.source=rumor"], "say": "l.b", "weight": 3},
    {"id": "inform.theft.gossip", "when": ["act=inform", "pred.tag=crime", "epi.source=rumor", "speaker.trait=gossip"],
     "say": "l.gossip", "cooldown": "speaker:1d"},
    {"id": "inform.theft.to_doer", "when": ["act=inform", "pred=theft", "listener.is_doer", "epi.conf<0.5"],
     "say": "l.to_doer", "once_per": "listener+topic"},
    {"id": "wrap.epi.rumor", "when": ["slot=wrap.epi", "epi.source=rumor"], "say": "w.rumor"},
    {"id": "wrap.epi.rumor.far", "when": ["slot=wrap.epi", "epi.source=rumor", "epi.hops>=2"], "say": "w.far"}
]})";

SayContext ctxOf(const Utterance& u, Tick now = 0, std::uint64_t seed = 1) {
    SayContext c;
    c.speaker = u.speaker;
    c.listener = u.listener;
    c.topic = dialogueTopic(u);
    c.now = now;
    c.seed = seed;
    return c;
}

} // namespace

TEST(altera_dialogue_rules_duration_and_topic) {
    CHECK_EQ(parseDialogueDuration("2d"), 2 * kTicksPerDay);
    CHECK_EQ(parseDialogueDuration("3h"), 18);
    CHECK_EQ(parseDialogueDuration("40"), 40);
    CHECK_EQ(parseDialogueDuration("d"), -1);
    CHECK_EQ(parseDialogueDuration("2w"), -1);
    Utterance u = theftTalk(1, 2, 3);
    CHECK_EQ(dialogueTopic(u), std::string("fact:9"));
    u.content->fact = kNone;
    CHECK_EQ(dialogueTopic(u), std::string("pred:theft|doer=char:3|victim_family=family:40"));
    u.content.reset();
    CHECK(dialogueTopic(u).empty());
}

TEST(altera_dialogue_rules_utterance_facts) {
    const DialogueGenData d = rulesOf(kRules);
    ContextFacts f;
    addUtteranceFacts(f, d, theftTalk(1, 2, 2));
    CHECK_EQ(f.str["act"], std::string("inform"));
    CHECK_EQ(f.str["pred"], std::string("theft"));
    CHECK_EQ(f.list["pred.tag"].size(), size_t{2});
    CHECK_EQ(f.str["role.doer"], std::string("char"));
    CHECK_EQ(f.num["listener.is_doer"], 1.0);
    CHECK(!f.has("speaker.is_doer"));
    CHECK_EQ(f.num["epi.hops"], 2.0);
    CHECK_EQ(f.num["epi.age_days"], 3.0);
    CHECK_EQ(f.num["fact"], 1.0);
    CHECK_EQ(f.str["epi.source"], std::string("rumor"));
}

TEST(altera_dialogue_rules_most_specific_wins) {
    const DialogueGenData d = rulesOf(kRules);
    // Told by someone not the doer, no gossip trait: the rumour pair (3 criteria) wins over 2 and 1.
    Utterance u = theftTalk(1, 2, 3);
    ContextFacts f = dialogueFacts(d, nullptr, nullptr, u);
    RuleChoice c = chooseRule(d, f, nullptr, ctxOf(u));
    CHECK(c.rule != nullptr);
    CHECK_EQ(c.specificity, 3);
    CHECK_EQ(c.tied.size(), size_t{2});
    CHECK(c.rule->id == "inform.theft.rumor.a" || c.rule->id == "inform.theft.rumor.b");
    CHECK_EQ(c.fitting.size(), size_t{4});
    // A gossip speaker: 4 criteria.
    f.add("speaker.trait", "gossip");
    CHECK_EQ(chooseRule(d, f, nullptr, ctxOf(u)).rule->id, std::string("inform.theft.gossip"));
    // Plain inform without content: the fallback.
    Utterance g;
    g.act = "inform";
    CHECK_EQ(chooseRule(d, dialogueFacts(d, nullptr, nullptr, g), nullptr, ctxOf(g)).say(), std::string("l.any"));
    // An act nobody wrote rules for: no rule.
    g.act = "praise";
    CHECK(chooseRule(d, dialogueFacts(d, nullptr, nullptr, g), nullptr, ctxOf(g)).rule == nullptr);
}

TEST(altera_dialogue_rules_ties_by_weight_and_seed) {
    const DialogueGenData d = rulesOf(kRules);
    Utterance u = theftTalk(1, 2, 3);
    const ContextFacts f = dialogueFacts(d, nullptr, nullptr, u);
    int b = 0;
    for (std::uint64_t s = 0; s < 400; ++s)
        if (chooseRule(d, f, nullptr, ctxOf(u, 0, s)).rule->id == "inform.theft.rumor.b") ++b;
    CHECK(b > 240 && b < 360); // weight 3 : 1 -> about 300 of 400
    // Same seed, same line; reversed rule order, same line.
    Json files = Json::parse(kRules);
    std::reverse(files["rules"].begin(), files["rules"].end());
    files["_file"] = "rev.json";
    const DialogueGenData rev = DialogueGenData::fromJson(Json::parse(R"({"predicates": [
        {"id": "theft", "roles": {"doer": "char", "victim_family": "family"}, "tags": ["theft", "crime"]}]})"), {files});
    for (std::uint64_t s = 0; s < 50; ++s) {
        const std::string a = chooseRule(d, f, nullptr, ctxOf(u, 0, s)).rule->id;
        CHECK_EQ(a, chooseRule(d, f, nullptr, ctxOf(u, 0, s)).rule->id);
        CHECK_EQ(a, chooseRule(rev, f, nullptr, ctxOf(u, 0, s)).rule->id);
    }
}

TEST(altera_dialogue_rules_cooldown_once_per_repeat) {
    const DialogueGenData d = rulesOf(kRules);
    SaidMemory said;
    // Gossip cooldown speaker:1d.
    Utterance u = theftTalk(1, 2, 3);
    ContextFacts f = dialogueFacts(d, nullptr, &said, u);
    f.add("speaker.trait", "gossip");
    RuleChoice c = chooseRule(d, f, &said, ctxOf(u, 100));
    CHECK_EQ(c.rule->id, std::string("inform.theft.gossip"));
    said.record(c.rule->id, 1, 2, ctxOf(u).topic, 100);
    c = chooseRule(d, f, &said, ctxOf(u, 150));
    CHECK(c.rule->id != "inform.theft.gossip");
    CHECK_EQ(c.blocked.size(), size_t{1});
    CHECK(!c.repeat);
    // To another listener: still the speaker's cooldown.
    Utterance other = theftTalk(1, 5, 3);
    CHECK(chooseRule(d, f, &said, ctxOf(other, 150)).rule->id != "inform.theft.gossip");
    CHECK_EQ(chooseRule(d, f, &said, ctxOf(u, 100 + kTicksPerDay)).rule->id, std::string("inform.theft.gossip"));

    // once_per listener+topic: telling the doer to his face once.
    SaidMemory s2;
    Utterance face = theftTalk(1, 3, 3);
    ContextFacts ff = dialogueFacts(d, nullptr, &s2, face);
    CHECK_EQ(chooseRule(d, ff, &s2, ctxOf(face)).rule->id, std::string("inform.theft.to_doer"));
    s2.record("inform.theft.to_doer", 1, 3, dialogueTopic(face), 0);
    CHECK(chooseRule(d, ff, &s2, ctxOf(face, 9999)).rule->id != "inform.theft.to_doer");
    Utterance otherTopic = face;
    otherTopic.content->fact = 10;
    CHECK_EQ(chooseRule(d, ff, &s2, ctxOf(otherTopic, 9999)).rule->id, std::string("inform.theft.to_doer"));

    // Everything blocked: the best blocked one comes back as a repeat.
    const DialogueGenData one = rulesOf(R"({"rules": [{"id": "greet.once", "when": ["act=greet"], "say": "g", "once_per": "listener"}]})");
    Utterance g;
    g.act = "greet";
    g.speaker = 1;
    g.listener = 2;
    SaidMemory s3;
    s3.record("greet.once", 1, 2, "", 0);
    RuleChoice r = chooseRule(one, dialogueFacts(one, nullptr, &s3, g), &s3, ctxOf(g, 10));
    CHECK(r.rule != nullptr);
    CHECK(r.repeat);
}

TEST(altera_dialogue_rules_slots) {
    const DialogueGenData d = rulesOf(kRules);
    Utterance u = theftTalk(1, 2, 3);
    const ContextFacts f = dialogueFacts(d, nullptr, nullptr, u);
    // A whole line never takes a part rule.
    for (const auto* r : chooseRule(d, f, nullptr, ctxOf(u)).fitting) CHECK(r->id.rfind("wrap.", 0) != 0);
    SayContext sc = ctxOf(u);
    sc.slot = "wrap.epi";
    RuleChoice c = chooseRule(d, f, nullptr, sc);
    CHECK_EQ(c.say(), std::string("w.far"));
    CHECK_EQ(c.fitting.size(), size_t{2});
    // Seen with own eyes: no part rule fits, the part stays empty (no fallback from whole lines).
    Utterance seen = theftTalk(1, 2, 3, "seen");
    CHECK(chooseRule(d, dialogueFacts(d, nullptr, nullptr, seen), nullptr, sc).rule == nullptr);
    sc.slot = "wrap.voice";
    CHECK(chooseRule(d, f, nullptr, sc).rule == nullptr);
}

TEST(altera_dialogue_rules_world_facts) {
    World w(realContent(), 3);
    Character a;
    a.name = "a";
    a.body.ageDays = 70 * kDaysPerYear;
    a.body.sex = 'm';
    a.traits = {"gossip", "noble"};
    a.status.family = 40;
    a.moodlets.push_back(Moodlet{"merry", 0.2, 1000});
    a.body.fatigue = 0.9;
    Character& sa = w.addCharacter(a);
    Character b;
    b.name = "b";
    b.body.ageDays = 20 * kDaysPerYear;
    b.status.mother = 77;
    b.reputation["honesty"] = -0.6;
    Character& sb = w.addCharacter(b);
    Character c;
    c.name = "c";
    c.status.mother = 77;
    Character& sc = w.addCharacter(c);
    Character& speaker = *w.character(sa.id);
    speaker.relations[sb.id].affinity = 0.7;
    speaker.relations[sb.id].trust = 0.2;
    w.character(sb.id)->relations[speaker.id].fear = 0.4;
    MemoryEntry m;
    m.type = "saw_crime";
    m.who = {sb.id};
    speaker.memory.push_back(m);
    speaker.memory.push_back(m);

    const DialogueGenData& d = dialogueGenData(realContent());
    Utterance u = theftTalk(sa.id, sb.id, sb.id);
    ContextFacts f = dialogueFacts(d, &w, nullptr, u);
    CHECK_EQ(f.str["speaker.age"], std::string("old"));
    CHECK_EQ(f.str["listener.age"], std::string("young"));
    CHECK_EQ(f.num["speaker.age_years"], 70.0);
    CHECK_EQ(f.str["speaker.sex"], std::string("m"));
    CHECK_EQ(f.str["speaker.status"], std::string("noble"));
    CHECK_EQ(f.str["listener.status"], std::string("commoner"));
    Criterion k;
    CHECK(Criterion::parse("speaker.state=drunk", k));
    CHECK(k.match(f));
    CHECK(Criterion::parse("speaker.state=tired", k));
    CHECK(k.match(f));
    CHECK(Criterion::parse("listener.state=drunk", k));
    CHECK(!k.match(f));
    CHECK(Criterion::parse("speaker.trait=gossip", k));
    CHECK(k.match(f));
    CHECK_EQ(f.num["rel.affinity"], 0.7);
    CHECK_EQ(f.num["rel_back.fear"], 0.4);
    CHECK_EQ(f.num["memory.saw_crime"], 2.0);
    CHECK_EQ(f.num["reputation.honesty"], -0.6);
    CHECK_EQ(f.num["listener.reputation.honesty"], -0.6);
    CHECK_EQ(f.num["listener.is_doer"], 1.0);
    CHECK_EQ(f.num["speaker.is_victim_family"], 1.0);
    CHECK_EQ(f.str["time"], std::string("night")); // tick 0 = midnight
    CHECK(f.has("season"));
    // Siblings by mother.
    Utterance sib;
    sib.act = "greet";
    sib.speaker = sb.id;
    sib.listener = sc.id;
    ContextFacts fs = dialogueFacts(d, &w, nullptr, sib);
    CHECK(Criterion::parse("rel.kin=sibling", k));
    CHECK(k.match(fs));
}

TEST(altera_dialogue_rules_said_memory) {
    SaidMemory m;
    m.record("a", 1, 2, "fact:9", 10);
    m.record("a", 1, 3, "fact:9", 20);
    m.record("b", 4, 2, "fact:9", 30);
    CHECK_EQ(m.count("a", 1), 2);
    CHECK_EQ(m.count("a", 1, 3), 1);
    CHECK_EQ(m.topicCount(1, 2, "fact:9"), 1);
    CHECK_EQ(m.topicCount(kNone, 2, "fact:9"), 2);
    Utterance u = theftTalk(1, 2, 3);
    ContextFacts f;
    addSaidFacts(f, m, u);
    CHECK_EQ(f.num["said.a"], 2.0);
    CHECK_EQ(f.num["said_to.a"], 1.0);
    CHECK_EQ(f.num["said.topic"], 1.0);
    CHECK_EQ(f.num["heard.topic"], 2.0);
    SaidMemory back;
    std::string err;
    CHECK(fromJson(toJson(m), back, &err));
    CHECK(back == m);
    CHECK(!fromJson(Json::parse(R"({"said": [{"s": 1}]})"), back, &err));
    m.prune(45, 30);
    CHECK_EQ(m.log.size(), size_t{2});
    m.prune(45, 1000, 1);
    CHECK_EQ(m.log.size(), size_t{1});
    CHECK_EQ(m.log[0].rule, std::string("b"));
}

TEST(altera_dialogue_rules_providers) {
    CHECK(!knownDialogueFact("weather.rain"));
    CHECK(knownDialogueFact("speaker.trait"));
    CHECK(knownDialogueFact("speaker.reputation.honesty"));
    CHECK(knownDialogueFact("listener.is_victim"));
    CHECK(!knownDialogueFact("speaker.favourite_colour"));
    CHECK(knownDialogueFact("said.greet.any"));
    CHECK(knownDialogueFact("topic.kind"));
    CHECK(knownDialogueFact("reply_to"));
    CHECK(knownDialogueFact("commitment"));
    CHECK(knownDialogueFact("mode"));
    registerFactProvider("test_weather", [](ContextFacts& f, const World&, const Utterance&) { f.set("weather.rain", "heavy"); },
                         {"weather."});
    CHECK(knownDialogueFact("weather.rain"));
    World w(realContent(), 1);
    Utterance u;
    u.act = "greet";
    ContextFacts f = dialogueFacts(dialogueGenData(realContent()), &w, nullptr, u);
    CHECK_EQ(f.str["weather.rain"], std::string("heavy"));
    unregisterFactProvider("test_weather");
    CHECK(!knownDialogueFact("weather.rain"));
}

TEST(altera_dialogue_rules_choose_line_real_content) {
    World w(realContent(), 2);
    Character a, b, c;
    a.name = "a";
    b.name = "b";
    c.name = "c";
    const EntityId ia = w.addCharacter(a).id, ib = w.addCharacter(b).id, ic = w.addCharacter(c).id;
    const DialogueGenData& d = dialogueGenData(realContent());
    SaidMemory said;
    Utterance u = theftTalk(ia, ib, ic);
    RuleChoice r = chooseLine(d, w, u, &said);
    CHECK_EQ(r.rule->id, std::string("inform.rumor.mid")); // rumour, confidence 0.4..0.6 (AL-51 lines.json)
    CHECK_EQ(said.log.size(), size_t{1});
    // Second time to the same listener about the same theft: once_per listener+topic, the fallback.
    ContextFacts f;
    r = chooseLine(d, w, u, &said, false, &f);
    CHECK_EQ(r.rule->id.rfind("port.inform.", 0), size_t{0}); // an old dialogue.inform.* line (port.json)
    CHECK_EQ(f.num["said.topic"], 1.0);
    CHECK(explainChoice(r, f).find("inform.rumor.mid (5) blocked") != std::string::npos);
    // The real rules pass the rule checks.
    const ValidationReport v = validateDialogueRules(d, &realContent());
    for (const auto& e : v.errors) ::testing::fail(__FILE__, __LINE__, e.c_str());
    for (const auto& wn : v.warnings) ::testing::fail(__FILE__, __LINE__, wn.c_str());
}

TEST(altera_dialogue_rules_checks_catch_broken_rules) {
    const DialogueGenData d = rulesOf(R"({"rules": [
        {"id": "ask.x", "when": ["act=ask", "speaker.favourite_colour=red"], "say": "a"},
        {"id": "greet.any", "when": ["act=greet"], "say": "g", "cooldown": "speaker:1d"},
        {"id": "greet.twin1", "when": ["act=greet", "tone=rude"], "say": "r"},
        {"id": "greet.twin2", "when": ["tone=rude", "act=greet"], "say": "r"}
    ]})");
    const ValidationReport v = validateDialogueRules(d);
    auto has = [&](const std::string& s) {
        for (const auto& w : v.warnings)
            if (w.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(has("speaker.favourite_colour"));
    CHECK(has("act ask has no fallback"));
    CHECK(has("act greet has no fallback"));
    CHECK(has("fallback with cooldown"));
    CHECK(has("same criteria and say as greet.twin1"));
    std::vector<std::string> errs;
    const auto st = parseDialogueStates({Json::parse(R"({"states": {"x": {"moodlets": "notalist"}}})")}, &errs);
    CHECK(st.empty());
    CHECK_EQ(errs.size(), size_t{1});
}
