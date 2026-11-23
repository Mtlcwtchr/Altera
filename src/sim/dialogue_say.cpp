// Saying a line in the sim (AL-53, AL-36). See dialogue_say.hpp.

#include "sim/dialogue_say.hpp"

#include <algorithm>

#include "sim/ai.hpp"
#include "sim/dialogue_corpus.hpp"
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
             Epistemic epi, const std::string& tone, const std::string& intent, std::map<std::string, Ref> args) {
    SaidLine out;
    Utterance& u = out.utterance;
    u.speaker = speaker.id;
    u.listener = listener.id;
    u.act = act;
    u.tone = tone;
    u.intent = intent;
    u.content = std::move(content);
    u.epi = std::move(epi);
    u.args = std::move(args);
    u.seed = w.rng(speaker.id, "say:" + act).next();
    const DialogueGenData& D = dialogueGenData(w.content());
    ContextFacts facts;
    const RuleChoice choice = chooseLine(D, w, u, &w.said(), false, &facts);
    if (!choice.rule) return out;
    out.key = choice.say();
    out.rule = choice.rule->id;
    // the parts around the line (DIALOGUE_GEN §5.4, slots.json): one of each group, the
    // first that has a rule; which of tone / voice goes first alternates by the seed
    const bool swap = (u.seed & 1) != 0;
    auto part = [&](std::vector<std::string> group, std::vector<std::string>& into) {
        if (swap && group.size() > 1) std::swap(group.front(), group.back());
        for (const auto& slot : group) {
            const RuleChoice c = chooseSlot(D, facts, u, slot, w.now(), &w.said());
            if (!c.rule || c.repeat) continue; // a part on cooldown is left out
            into.push_back(c.say());
            return;
        }
    };
    part({"noise.open"}, out.pre);
    part({"tone.open", "voice.open"}, out.pre);
    part({"voice.tail", "tone.tail"}, out.post);
    Json params = {{"speaker", speaker.id}, {"listener", listener.id}, {"key", out.key},
                   {"rule", out.rule},      {"act", act},              {"utt", toJson(u)}};
    if (!out.pre.empty()) params["pre"] = out.pre;
    if (!out.post.empty()) params["post"] = out.post;
    trace(w, speaker, 'S', "say.line", params);
    if (listener.id != speaker.id) trace(w, listener, 'S', "say.line", params);
    return out;
}

SaidLine sayRefusal(World& w, Character& refuser, Character& asker, const std::string& why,
                    const std::string& actionKey) {
    std::optional<Prop> content;
    Epistemic epi;
    for (const auto& rc : recall(w, refuser, asker.id))
        if (rc.kind == "deed") {
            content = rc.prop;
            epi = rc.epi;
            break;
        }
    std::map<std::string, Ref> args;
    if (!actionKey.empty()) args["action"] = Ref{"text", kNone, actionKey, 0.0, {}};
    return say(w, refuser, asker, "refuse", std::move(content), std::move(epi), "", why, std::move(args));
}

SaidLine sayFailReaction(World& w, Character& target, Character& actor, const InteractionDef& d) {
    std::map<std::string, Ref> args;
    if (!d.id.empty()) args["action"] = Ref{"text", kNone, "interaction." + d.id + ".name", 0.0, {}};
    return say(w, target, actor, "ignore", std::nullopt, Epistemic{}, "", "fail", std::move(args));
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

bool addressedVy(const World& w, EntityId speaker, EntityId listener) {
    if (speaker == listener) return false;
    const Character* l = w.character(listener);
    return l && std::find(l->traits.begin(), l->traits.end(), "noble") != l->traits.end();
}

namespace {
// A person as a loc argument: the name's forms plus id (so the line says "ты/вы" to the
// listener and "я" for the speaker); toward the nobility reg=vy and the address word.
Json personArg(const Loc& loc, const std::string& lang, const World& w, EntityId id, bool vy) {
    const Character* c = w.character(id);
    if (!c) return Json::object();
    Json n = loc.personNoun(lang, *c);
    n["id"] = id;
    if (vy) {
        n["reg"] = "vy";
        const std::string key = c->body.sex == 'm' ? "addr.lord" : "addr.lady";
        if (loc.has(lang, key)) n["addr"] = loc.noun(lang, key, "");
    }
    return n;
}
} // namespace

Json needArg(const Loc& loc, const std::string& lang, const std::string& need) {
    const std::string want = "need." + need + ".want";
    if (loc.has(lang, want)) return Json(loc.text(lang, want));
    return loc.noun(lang, "need." + need + ".name", need);
}

Json utteranceArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u) {
    Json params = u.content ? propParams(w, *u.content) : Json::object();
    if (!u.epi.chain.empty()) params["from"] = u.epi.chain.front();
    const Character* self = w.character(u.speaker);
    Json args = locArgs(loc, lang, w, params, self);
    const bool vy = addressedVy(w, u.speaker, u.listener);
    if (u.speaker != kNone) args["speaker"] = args["self"] = personArg(loc, lang, w, u.speaker, false);
    if (u.listener != kNone) args["listener"] = personArg(loc, lang, w, u.listener, vy);
    // the teller of a rumour: {src} of say.json (v0.2), {from} of the v0.1 lines
    if (!u.epi.chain.empty()) args["src"] = personArg(loc, lang, w, u.epi.chain.front(), false);
    // {reason} of the old refuse lines: dialogue.reason.<intent> when there is one
    if (!args.contains("reason"))
        args["reason"] = loc.has(lang, "dialogue.reason." + u.intent) ? loc.text(lang, "dialogue.reason." + u.intent, args)
                                                                     : std::string();
    auto refNoun = [&](const Ref& r) -> Json {
        if (r.kind == "char") {
            if (w.character(r.id)) return personArg(loc, lang, w, r.id, vy && r.id == u.listener);
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
                        return loc.personNoun(lang, *c);
            return Json("?");
        }
        if (r.kind == "text") return loc.noun(lang, r.key, r.key);
        if (r.kind == "num") return Json(r.num);
        return Json(r.key);
    };
    // LOC v0.2: the content as a proposition argument ({>prop:clause} etc.)
    if (u.content)
        args["prop"] = propLocArg(*u.content, refNoun,
                                  [&](const std::string& unit) { return loc.noun(lang, "unit." + unit, unit); });
    // the moment's own arguments (v0.1 lines)
    std::vector<std::pair<std::string, std::string>> phrases; // phrases with arguments ("дать {item:acc}"), at the end
    for (const auto& [k, r] : u.args) {
        // an act in a sentence: its phrase ("дать хлеба") over the menu caption ("Дать")
        if (r.kind == "text" && r.key.size() > 5 && r.key.compare(r.key.size() - 5, 5, ".name") == 0) {
            const std::string phrase = r.key.substr(0, r.key.size() - 5) + ".phrase";
            if (loc.has(lang, phrase)) {
                if (loc.text(lang, phrase).find('{') == std::string::npos) {
                    args[k] = loc.noun(lang, phrase, "");
                    continue;
                }
                phrases.emplace_back(k, phrase);
            }
        }
        args[k] = refNoun(r);
    }
    // what a v0.1 line may still ask for, from the moment itself, so no "{action}" is left raw
    const Json something = loc.noun(lang, "thing.something", "");
    if (!args.contains("topic")) {
        std::string key = "thing.something";
        if (u.content)
            if (const Json* fr = loc.find(lang, "frame." + u.content->pred); fr && fr->is_object() && fr->contains("noun") &&
                                                                              (*fr)["noun"].is_string())
                key = (*fr)["noun"].get<std::string>();
        args["topic"] = loc.noun(lang, key, "");
    }
    if (!args.contains("text"))
        args["text"] = u.content ? tidyLine(realizeProp(loc, lang, args["prop"], "clause", "", args, u.seed) + ".", true)
                                 : tidyLine(something.value("nom", std::string()) + ".", true);
    for (const char* k : {"action", "give", "get", "item"})
        if (!args.contains(k)) args[k] = something;
    if (!args.contains("need") && self && !self->needs.empty()) {
        // the speaker's most pressing need
        auto low = std::min_element(self->needs.begin(), self->needs.end(),
                                    [](const auto& a, const auto& b) { return a.second < b.second; });
        args["need"] = needArg(loc, lang, low->first);
    }
    if (!args.contains("need")) args["need"] = something;
    if (!args.contains("person")) {
        // whom the content is about (the doer), else the teller
        const Ref* doer = u.content ? u.content->role("doer") : nullptr;
        if (doer && doer->kind == "char") args["person"] = refNoun(*doer);
        else if (args.contains("src")) args["person"] = args["src"];
        else args["person"] = something;
    }
    if (!args.contains("deadline")) args["deadline"] = loc.noun(lang, "hud.daypart.evening", "");
    if (!args.contains("price")) args["price"] = 0; // AL-75: coins of a deal (trade lines)
    for (const auto& [k, phrase] : phrases)
        if (const std::string t = loc.text(lang, phrase, args); t.find('{') == std::string::npos) args[k] = t;
    return args;
}

std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key) {
    return sayLoc(loc, lang, key, utteranceArgs(loc, lang, w, u), u.seed);
}

std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key, const std::vector<std::string>& pre,
                          const std::vector<std::string>& post) {
    const Json args = utteranceArgs(loc, lang, w, u);
    std::vector<std::string> pieces;
    for (const auto& k : pre) pieces.push_back(sayLoc(loc, lang, k, args, u.seed));
    pieces.push_back(sayLoc(loc, lang, key, args, u.seed));
    for (const auto& k : post) pieces.push_back(sayLoc(loc, lang, k, args, u.seed));
    if (pieces.size() == 1) return pieces.front();
    return joinDialogueParts(pieces, dialogueNameWords(args, {"speaker", "listener", "src", "person"}));
}

std::string lineText(const Loc& loc, const std::string& lang, const World& w, const Json& params) {
    Utterance u;
    if (!params.contains("utt") || !fromJson(params["utt"], u)) return loc.text(lang, "trace.say.line", params);
    auto keys = [&](const char* k) {
        std::vector<std::string> v;
        if (params.contains(k) && params[k].is_array())
            for (const auto& e : params[k])
                if (e.is_string()) v.push_back(e.get<std::string>());
        return v;
    };
    const std::string line = utteranceText(loc, lang, w, u, params.value("key", std::string()), keys("pre"), keys("post"));
    Json args = locArgs(loc, lang, w, {{"speaker", u.speaker}, {"listener", u.listener}}, w.character(u.speaker));
    args["line"] = line;
    return loc.text(lang, "trace.say.line", args);
}

} // namespace altera::sim
