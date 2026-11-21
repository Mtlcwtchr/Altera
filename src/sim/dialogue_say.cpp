// Saying a line in the sim (AL-53, AL-36). See dialogue_say.hpp.

#include "sim/dialogue_say.hpp"

#include "sim/ai.hpp"
#include "sim/dialogue_memory.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/loc.hpp"
#include "sim/loc_gen.hpp"
#include "sim/world.hpp"

namespace altera::sim {

void installDialogueFacts() {
    // AL-49's memory facts (memory.deed*, memory.kind*, listener.reputation.*) for every line
    registerFactProvider(
        "memory",
        [](ContextFacts& f, const World& w, const Utterance& u) {
            const Character* s = w.character(u.speaker);
            const Character* l = w.character(u.listener);
            if (!s || !l) return;
            // only memory.*: listener.reputation.* is the world facts' own (dialogue_rules)
            ContextFacts m;
            addMemoryFacts(w, *s, *l, m);
            auto mine = [](const std::string& k) { return k.rfind("memory.", 0) == 0; };
            for (auto& [k, v] : m.str)
                if (mine(k)) f.str[k] = v;
            for (auto& [k, v] : m.num)
                if (mine(k)) f.num[k] = v;
            for (auto& [k, v] : m.list)
                if (mine(k)) f.list[k] = v;
        },
        {"memory."});
    // the speaker's own state: speaker.need.<id> (1 = satisfied), speaker.fsm (barks)
    registerFactProvider(
        "self",
        [](ContextFacts& f, const World& w, const Utterance& u) {
            const Character* s = w.character(u.speaker);
            if (!s) return;
            for (const auto& [id, v] : s->needs) f.set("speaker.need." + id, v);
            f.set("speaker.fsm", s->ai.fsm.state);
        },
        {"speaker.need.", "speaker.fsm"});
}

namespace {
// Registered before any rule is checked or chosen (validation asks the providers' prefixes).
[[maybe_unused]] const bool kFactsInstalled = (installDialogueFacts(), true);
} // namespace

SaidLine say(World& w, Character& speaker, Character& listener, const std::string& act, std::optional<Prop> content,
             Epistemic epi, const std::string& tone, const std::string& intent) {
    SaidLine out;
    Utterance& u = out.utterance;
    u.speaker = speaker.id;
    u.listener = listener.id;
    u.act = act;
    u.tone = tone;
    u.intent = intent;
    u.content = std::move(content);
    u.epi = std::move(epi);
    u.seed = w.rng(speaker.id, "say:" + act).next();
    const DialogueGenData& D = dialogueGenData(w.content());
    const RuleChoice choice = chooseLine(D, w, u, &w.said());
    if (!choice.rule) return out;
    out.key = choice.say();
    out.rule = choice.rule->id;
    const Json params = {{"speaker", speaker.id}, {"listener", listener.id}, {"key", out.key},
                         {"rule", out.rule},      {"act", act},              {"utt", toJson(u)}};
    trace(w, speaker, 'S', "say.line", params);
    if (listener.id != speaker.id) trace(w, listener, 'S', "say.line", params);
    return out;
}

SaidLine sayRefusal(World& w, Character& refuser, Character& asker, const std::string& why) {
    std::optional<Prop> content;
    Epistemic epi;
    for (const auto& rc : recall(w, refuser, asker.id))
        if (rc.kind == "deed") {
            content = rc.prop;
            epi = rc.epi;
            break;
        }
    return say(w, refuser, asker, "refuse", std::move(content), std::move(epi), "", why);
}

SaidLine sayFailReaction(World& w, Character& target, Character& actor, const InteractionDef& d) {
    (void)d;
    return say(w, target, actor, "ignore", std::nullopt, Epistemic{}, "", "fail");
}

std::vector<MenuTopic> dialogueTopics(const World& w, const Character& speaker, const Character& listener,
                                      size_t max) {
    std::vector<MenuTopic> out;
    for (const auto& rc : recall(w, speaker, kNone)) {
        const Ref* doer = rc.prop.role("doer");
        if (!doer || doer->kind != "char") continue;
        MenuTopic t;
        if (doer->id == listener.id) t.act = rc.kind == "deed" ? "accuse" : "thank";
        else if (rc.kind == "deed") t.act = "inform";
        else continue; // somebody else's kindness is not news worth a menu line
        t.prop = rc.prop;
        t.epi = rc.epi;
        t.topic = rc.topic;
        t.weight = rc.weight;
        out.push_back(std::move(t));
        if (out.size() >= max) break;
    }
    return out;
}

std::string menuLabel(const Loc& loc, const std::string& lang, const World& w, const Character& speaker,
                      const Character& listener, const MenuTopic& t) {
    Utterance u;
    u.speaker = speaker.id;
    u.listener = listener.id;
    u.act = t.act;
    u.content = t.prop;
    u.epi = t.epi;
    return utteranceText(loc, lang, w, u, "dialogue.menu." + t.act);
}

SaidLine chooseBark(const World& w, const Character& c) {
    SaidLine out;
    if (!c.body.alive || c.isAnimal() || c.ai.fsm.state == "Sleep" || c.ai.fsm.state == "Incapacitated") return out;
    Utterance& u = out.utterance;
    u.speaker = c.id;
    u.listener = c.id;
    u.act = "bark";
    u.seed = w.rng(c.id, "bark").next();
    const DialogueGenData& D = dialogueGenData(w.content());
    const ContextFacts f = dialogueFacts(D, &w, nullptr, u);
    SayContext ctx;
    ctx.speaker = c.id;
    ctx.listener = c.id;
    ctx.now = w.now();
    ctx.seed = u.seed;
    const RuleChoice choice = chooseRule(D, f, nullptr, ctx);
    if (!choice.rule) return out;
    out.key = choice.say();
    out.rule = choice.rule->id;
    return out;
}

Json agentContent(const Utterance& u) {
    if (!u.content) return {{"kind", "none"}};
    const Prop& p = *u.content;
    Json j = {{"kind", "fact"}, {"params", toJson(p)}, {"epi", toJson(u.epi)}};
    j["ref"] = p.fact != kNone ? "fact:" + std::to_string(p.fact) : topicOf(p);
    return j;
}

std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key) {
    Json params = u.content ? propParams(w, *u.content) : Json::object();
    if (u.speaker != kNone) params["speaker"] = u.speaker;
    if (u.listener != kNone) params["listener"] = u.listener;
    if (!u.epi.chain.empty()) params["from"] = u.epi.chain.front();
    const Character* self = w.character(u.speaker);
    Json args = locArgs(loc, lang, w, params, self);
    // {reason} of the old refuse lines: dialogue.reason.<intent> when there is one
    if (!args.contains("reason"))
        args["reason"] = loc.has(lang, "dialogue.reason." + u.intent) ? loc.text(lang, "dialogue.reason." + u.intent, args)
                                                                     : std::string();
    // LOC v0.2: the content as a proposition argument ({>prop:clause} etc.)
    if (u.content) {
        auto refNoun = [&](const Ref& r) -> Json {
            if (r.kind == "char") {
                if (const Character* c = w.character(r.id)) {
                    Json n = loc.personNoun(lang, c->name, c->body.sex);
                    n["id"] = r.id;
                    return n;
                }
                return Json(std::to_string(r.id));
            }
            if (r.kind == "object") {
                if (const WorldObject* o = w.object(r.id)) return loc.noun(lang, "object." + o->type + ".name", o->type);
                return Json("?");
            }
            if (r.kind == "item") return loc.noun(lang, "item." + r.key + ".name", r.key);
            if (r.kind == "family") {
                auto it = w.families().find(r.id);
                if (it != w.families().end())
                    for (EntityId id : it->second.members)
                        if (const Character* c = w.character(id); c && c->body.alive && !c->isAnimal())
                            return loc.personNoun(lang, c->name, c->body.sex);
                return Json("?");
            }
            if (r.kind == "text") return loc.noun(lang, r.key, r.key);
            if (r.kind == "num") return Json(r.num);
            return Json(r.key);
        };
        args["prop"] = propLocArg(*u.content, refNoun,
                                  [&](const std::string& unit) { return loc.noun(lang, "unit." + unit, unit); });
    }
    return sayLoc(loc, lang, key, args, u.seed);
}

std::string lineText(const Loc& loc, const std::string& lang, const World& w, const Json& params) {
    Utterance u;
    if (!params.contains("utt") || !fromJson(params["utt"], u)) return loc.text(lang, "trace.say.line", params);
    const std::string line = utteranceText(loc, lang, w, u, params.value("key", std::string()));
    Json args = locArgs(loc, lang, w, {{"speaker", u.speaker}, {"listener", u.listener}}, w.character(u.speaker));
    args["line"] = line;
    return loc.text(lang, "trace.say.line", args);
}

} // namespace altera::sim
