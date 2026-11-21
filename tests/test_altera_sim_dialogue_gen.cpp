// Dialogue generation contract (AL-46): the meaning types survive JSON,
// criteria parse and match, the shipped predicates/rules validate against the
// content and loc, and the validator catches broken data.

#include "framework.hpp"
#include "sim/dialogue_gen.hpp"
#include "sim/loc.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

Ref ch(EntityId id) {
    Ref r;
    r.kind = "char";
    r.id = id;
    return r;
}

Prop theft() {
    Prop p;
    p.pred = "theft";
    p.roles["doer"] = ch(12);
    Ref item;
    item.kind = "item";
    item.key = "flour";
    p.roles["object"] = item;
    Ref fam;
    fam.kind = "family";
    fam.id = 4;
    p.roles["victim_family"] = fam;
    p.qty = 1;
    p.unit = "sack";
    p.tick = 1440;
    p.fact = 9;
    return p;
}

ContextFacts facts() {
    ContextFacts f;
    f.set("act", "inform");
    f.set("pred", "theft");
    f.set("epi.source", "rumor");
    f.set("epi.conf", 0.49);
    f.set("epi.hops", 2.0);
    f.add("speaker.trait", "gossip");
    f.add("speaker.trait", "greedy");
    return f;
}

bool crit(const std::string& s, const ContextFacts& f) {
    Criterion c;
    std::string err;
    if (!Criterion::parse(s, c, &err)) {
        ::testing::fail(__FILE__, __LINE__, ("parse failed: " + s + ": " + err).c_str());
        return false;
    }
    return c.match(f);
}

} // namespace

TEST(altera_dialogue_gen_utterance_roundtrip) {
    Utterance u;
    u.speaker = 3;
    u.listener = 5;
    u.act = "inform";
    u.tone = "familiar";
    u.intent = "share_news";
    // "Брана говорит, что Олег украл муку": say(Брана, theft)
    Prop say;
    say.pred = "say";
    say.roles["speaker"] = ch(7);
    Ref inner;
    inner.kind = "prop";
    inner.sub.push_back(theft());
    say.roles["content"] = inner;
    u.content = say;
    u.epi.source = "rumor";
    u.epi.confidence = 0.49;
    u.epi.chain = {7, 2};
    u.epi.age = 300;
    u.stance.valence = -0.6;
    u.seed = 42;

    const Json j = toJson(u);
    Utterance back;
    std::string err;
    CHECK(fromJson(j, back, &err));
    CHECK(back == u);
    CHECK_EQ(back.content->role("content")->sub.front().fact, 9u);
    // defaults are left out of the JSON
    CHECK(!j["content"].contains("modality"));
    CHECK(!j["content"]["roles"]["content"]["prop"].contains("tense"));
    CHECK(checkProp(dialogueGenData(realContent()), *back.content, &err));
}

TEST(altera_dialogue_gen_bad_json_does_not_throw) {
    Prop p;
    std::string err;
    CHECK(!fromJson(Json::parse(R"({"roles": {}})"), p, &err));
    CHECK(!fromJson(Json::parse(R"({"pred": "theft", "roles": {"doer": {"kind": "char"}}})"), p, &err));
    CHECK(err.find("without id") != std::string::npos);
    CHECK(!fromJson(Json::parse(R"({"pred": "theft", "modality": "maybe"})"), p, &err));
    Epistemic e;
    CHECK(!fromJson(Json::parse(R"({"source": "dreamt"})"), e, &err));
    CHECK(fromJson(Json::parse(R"({"source": "told", "chain": [1, 2, 3, 4, 5]})"), e, &err));
    CHECK_EQ(e.chain.size(), Epistemic::kMaxChain);
    Utterance u;
    CHECK(!fromJson(Json::parse(R"({"speaker": 1})"), u, &err));
}

TEST(altera_dialogue_gen_criteria) {
    const ContextFacts f = facts();
    CHECK(crit("act=inform", f));
    CHECK(crit("act=ask|inform", f));
    CHECK(!crit("act!=inform", f));
    CHECK(crit("epi.conf<0.6", f));
    CHECK(!crit("epi.conf>=0.6", f));
    CHECK(crit("epi.hops=2", f));
    CHECK(crit("speaker.trait=gossip", f));
    CHECK(!crit("speaker.trait=honest", f));
    CHECK(crit("speaker.trait!=honest", f));
    CHECK(crit("pred", f));
    CHECK(crit("!listener.is_victim", f));
    CHECK(!crit("rel.affinity>0", f)); // absent number: a comparison fails
    Criterion c;
    CHECK(!Criterion::parse("epi.conf<abc", c));
    CHECK(!Criterion::parse("act=", c));
    CHECK(!Criterion::parse("bad key=1", c));
    CHECK(!Criterion::parse("", c));
}

TEST(altera_dialogue_gen_shipped_data_valid) {
    const Content& c = realContent();
    const Loc loc = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    const DialogueGenData& d = dialogueGenData(c);
    const ValidationReport v = validateDialogueGen(d, &c, &loc);
    for (const auto& e : v.errors) ::testing::fail(__FILE__, __LINE__, e.c_str());
    CHECK(v.ok());
    CHECK(d.predicate("theft") != nullptr);
    CHECK(d.forMemory("rumor_crime") != nullptr);
    CHECK_EQ(d.forMemory("suspects")->modality, std::string("guess"));
    CHECK(!d.rules.empty());
    CHECK(d.noise.contains("operators"));
}

TEST(altera_dialogue_gen_validator_catches) {
    const Json preds = Json::parse(R"({
      "predicates": [
        {"id": "theft", "roles": {"doer": "char", "object": ["thing"]}, "required": ["doer", "victim"], "scale": ["robbery"]}
      ],
      "memory": [
        {"type": "saw_crime", "pred": "theft", "roles": {"doer": "who7"}},
        {"type": "saw_crime", "pred": "nope", "roles": {}}
      ]})");
    const Json rules = Json::parse(R"({"_file": "t.json", "rules": [
        {"id": "a", "when": ["act=inform", "pred=nope"], "say": "x.y"},
        {"id": "a", "when": ["act=inform"], "say": "", "cooldown": "forever", "once_per": "year"},
        {"id": "b", "when": ["epi.conf<abc"], "say": "x.z"}
      ]})");
    const DialogueGenData d = DialogueGenData::fromJson(preds, {rules});
    const ValidationReport v = validateDialogueGen(d);
    auto has = [&](const std::string& s) {
        for (const auto& e : v.errors)
            if (e.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(has("unknown kind thing"));
    CHECK(has("required role victim is not declared"));
    CHECK(has("scale names unknown predicate robbery"));
    CHECK(has("unknown source who7"));
    CHECK(has("unknown predicate nope"));
    CHECK(has("duplicate"));
    CHECK(has("duplicate id"));
    CHECK(has("no say"));
    CHECK(has("bad cooldown"));
    CHECK(has("bad once_per"));
    CHECK(has("not a number"));
    // a Prop against its predicate
    std::string err;
    Prop p;
    p.pred = "theft";
    CHECK(!checkProp(d, p, &err));
    p.roles["doer"] = ch(1);
    Ref r;
    r.kind = "char";
    r.id = 2;
    p.roles["victim"] = r;
    CHECK(!checkProp(d, p, &err)); // victim is not a declared role here
}
