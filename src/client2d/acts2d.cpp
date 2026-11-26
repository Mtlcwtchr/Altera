#include "client2d/acts2d.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>

#include "sim/act_info.hpp"
#include "sim/gear.hpp"
#include <set>
#include "sim/ai.hpp"
#include "sim/transport.hpp"

namespace lr::client2d {

using namespace lr::sim;

namespace {

std::string num(double v) {
    char b[32];
    if (std::fabs(v - std::round(v)) < 1e-6) std::snprintf(b, sizeof b, "%d", int(std::round(v)));
    else std::snprintf(b, sizeof b, "%.2g", v);
    return b;
}

std::string opText(Condition::Op op) {
    switch (op) {
    case Condition::Op::Ge: return ">=";
    case Condition::Op::Le: return "<=";
    case Condition::Op::Gt: return ">";
    case Condition::Op::Lt: return "<";
    case Condition::Op::Eq: return "=";
    case Condition::Op::Ne: return "!=";
    default: return "";
    }
}

struct Words {
    const ViewText& t;
    const Character* self = nullptr;   // LR-123: {self} in names (Польщён{self:g||а}) reads as the actor
    std::string text(const std::string& key, const Json& args = Json::object()) const {
        if (!t.loc || !t.loc->has(t.lang, key)) return key;
        Json a = args;
        if (self && !a.contains("self")) a["self"] = t.loc->personNoun(t.lang, *self);
        return t.loc->text(t.lang, key, a);
    }
    std::string name(const std::string& kind, const std::string& id) const {
        // <id>@foreign and <id>#param=value read as the act
        const std::string bare = id.substr(0, std::min(id.find('@'), id.find('#')));
        if (t.loc && self)
            for (const std::string k : {kind + "." + bare + ".name", kind + "." + bare})
                if (t.loc->has(t.lang, k)) return text(k);
        return t.name(kind, bare);
    }
    std::string condition(const Condition& k) const {
        const std::string who = k.onTarget ? text("hud.req.target") + " " : "";
        const bool counted = k.op == Condition::Op::Ge || k.op == Condition::Op::Gt;
        std::string out;
        if (k.subject == "has") {
            out = text("hud.req.has", {{"item", name("item", k.key)}});
            if (counted && k.value > 1) out += " x" + num(k.value);
        } else if (k.subject == "knows") out = text("hud.req.knows", {{"skill", name("knowledge", k.key)}});
        else if (k.subject == "skill")
            out = text("hud.req.skill", {{"skill", name("knowledge", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        else if (k.subject == "target_stock")
            out = text("hud.req.stock", {{"item", name("item", k.key)}, {"op", opText(k.op)}, {"v", num(k.value)}});
        else if (k.subject == "age" && k.op == Condition::Op::Truthy)   // LR-123: age:adult is a stage, not a number
            out = text("hud.req.age_stage", {{"stage", name("life_stage", k.key)}});
        else if (k.subject == "age") out = text("hud.req.age", {{"op", opText(k.op)}, {"v", num(k.value)}});
        else if (k.subject == "kin") out = text("hud.req.kin." + k.key);
        else if (k.subject == "status_pair" || k.subject == "status_pair_any")
            out = text("hud.req." + k.subject + "." + k.key);
        else if (k.subject == "trait") out = text("hud.req.trait", {{"what", name("trait", k.key)}});
        else if (k.subject == "moodlet") out = text("hud.req.moodlet", {{"what", name("moodlet", k.key)}});
        else if (k.subject == "right") out = text("hud.req.right", {{"what", text("hud.right." + k.key)}});
        else if (k.subject == "verdict") out = text("hud.req.verdict", {{"what", name("interaction", k.key)}});
        else if (k.subject == "body")
            out = text("hud.req.body." + k.key, {{"op", opText(k.op)}, {"v", num(k.value * 100)}});
        else if (k.subject == "need")
            out = text("hud.req.need", {{"need", name("need", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        else if (k.subject == "home") out = text("hud.req.home", {{"what", name("item", k.key)}});
        else if (k.subject == "rel" && k.key == "debt") out = text("hud.req.owed");   // LR-131
        else if (k.subject == "rel")
            out = text("hud.req.rel", {{"axis", text("hud.axis." + k.key)}, {"op", opText(k.op)}, {"v", num(k.value)}});
        else out = k.text;
        if (k.negate) out = text("hud.req.not", {{"what", out}});
        return who + out;
    }
    std::string conditionWhy(const Condition& k, const WorldObject* obj) const {
        if (k.subject == "target_owner") {
            if (k.key == "family" || k.key == "self") return text("hud.why.not_ours");
            if (k.key == "other") return text("hud.why.ours");
            return text("hud.why.owner");
        }
        if (k.subject == "time")
            return text(k.negate ? "hud.why.not_at_night" : k.key == "night" ? "hud.why.night_only" : "hud.why.time");
        if (k.subject == "herd") return text("hud.why.herd");
        if (k.subject == "charity") return text("hud.why.charity");
        if (k.subject == "target_stock") {
            const std::string what = obj ? name("object", obj->type) : "";
            if (k.op == Condition::Op::Ge || k.op == Condition::Op::Gt)
                return text("hud.why.stock_low", {{"what", what}, {"item", name("item", k.key)}, {"v", num(k.value)}});
            return text("hud.why.stock_full", {{"what", what}, {"item", name("item", k.key)}});
        }
        return condition(k);
    }
    std::string why(const std::string& why, const InteractionDef& d, const WorldObject* obj, const Character* tc) const {
        if (why.empty()) return "";
        const std::size_t colon = why.find(':');
        const std::string head = why.substr(0, colon), rest = colon == std::string::npos ? "" : why.substr(colon + 1);
        const std::string what = obj ? name("object", obj->type) : tc ? tc->name : name("object", rest);
        if (head == "pre") {
            for (const Condition& k : d.pre)
                if (k.text == rest) return conditionWhy(k, obj);
            return "";
        }
        if (head == "reserved") return text("hud.why.reserved", {{"what", what}});
        if (head == "season") return text("hud.why.season", {{"what", what}});
        if (head == "depleted") return text("hud.why.depleted", {{"what", what}});
        if (head == "out_of_range") return text("hud.why.far", {{"what", what}});
        if (head == "gear") return text(rest == "too_heavy" ? "hud.why.too_heavy" : "hud.why.no_room");
        if (head == "dead") return text("hud.why.dead");
        if (head == "no_person") return text("hud.why.no_person");
        if (head == "no_target" || head == "wrong_target") return text("hud.why.no_target", {{"what", name("object", rest)}});
        return "";
    }
    std::string effect(const Effect& e) const {
        const std::string sign = e.op == '-' ? "-" : "+";
        if (e.subject == "has") return sign + num(e.value) + " " + name("item", e.key);
        if (e.subject == "skill") return text(e.op == '-' ? "hud.gain.skill_down" : "hud.gain.skill", {{"skill", name("knowledge", e.key)}});
        if (e.subject == "memory" || e.subject == "status_pair" || e.subject == "commitment") return "";   // LR-123: no raw keys
        if (e.subject == "need") return text(e.op == '-' ? "hud.gain.need_down" : "hud.gain.need", {{"need", name("need", e.key)}});
        if (e.subject == "body" && e.key == "fatigue") return text(e.op == '-' ? "hud.gain.rest" : "hud.gain.tired");
        if (e.subject == "body" && e.key == "health") return text(e.op == '-' ? "hud.gain.hurt" : "hud.gain.heal");
        if (e.subject == "moodlet") return text("hud.gain.moodlet", {{"what", name("moodlet", e.key)}});
        if (e.subject == "rel" || e.subject == "rel_back")
            return text(e.signedDelta() >= 0 ? "hud.gain.rel_up" : "hud.gain.rel_down", {{"axis", text("hud.axis." + e.key)}});
        if (e.subject == "reputation") return text("hud.gain.reputation", {{"what", name("reputation", e.key)}, {"sign", sign}});
        if (e.subject == "spawn") return text("hud.gain.spawn", {{"what", name("object", e.key)}});
        if (e.subject == "help") return text("hud.gain.help");     // LR-135
        if (e.subject == "exile") return text("hud.gain.exile");   // LR-127
        return "";
    }
};

Json actJson(const World& w, const Words& W, const Character& me, const InteractionDef& d, const WorldObject* obj,
             const Character* tc);

// LR-123: a parametric act (lend, give, trade, extort, learn...) offers its choices: one bound
// act per thing the actor (or for t.has:$x, the target) has / knows, each with its own can/why;
// the act can be done when any choice can. The client performs the choice's id (lend#item=bread).
Json choicesJson(const World& w, const Words& W, const Character& me, const InteractionDef& d, const Character* tc) {
    std::vector<std::pair<std::string, std::vector<std::string>>> options;   // param -> values
    for (const auto& p : d.params) {
        bool onTarget = false;
        for (const auto& k : d.pre)
            if (k.isParam() && k.param() == p.name) onTarget = k.onTarget;
        const Character* src = onTarget ? tc : &me;
        std::vector<std::string> vals;
        auto allowed = [&](const std::string& v) { return p.of.empty() || std::find(p.of.begin(), p.of.end(), v) != p.of.end(); };
        if (src && p.type == "item") {
            for (const auto& [item, n] : src->inventory)
                if (n > 0 && allowed(item)) vals.push_back(item);
        } else if (src && p.type == "skill") {
            for (const auto& [sk, v] : src->skills)
                if (v > 0 && allowed(sk)) vals.push_back(sk);
        }
        if (vals.size() > 16) vals.resize(16);
        options.push_back({p.name, vals});
    }
    // a trade of two items: what I give x what I want from the target (bounded)
    std::vector<std::string> ids = {d.id + "#"};
    std::vector<std::string> labels = {""};
    for (const auto& [name, vals] : options) {
        bool skill = false;
        for (const auto& p : d.params)
            if (p.name == name) skill = p.type == "skill";
        std::vector<std::string> nid, nlab;
        for (std::size_t i = 0; i < ids.size(); ++i)
            for (const auto& v : vals) {
                nid.push_back(ids[i] + (ids[i].back() == '#' ? "" : ",") + name + "=" + v);
                nlab.push_back(labels[i] + (labels[i].empty() ? "" : " ↔ ") + W.name(skill ? "knowledge" : "item", v));
            }
        ids.swap(nid);
        labels.swap(nlab);
        if (ids.size() > 24) { ids.resize(24); labels.resize(24); }
    }
    Json out = Json::array();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const InteractionDef* b = w.content().interaction(ids[i]);
        if (!b) continue;
        Json o = actJson(w, W, me, *b, nullptr, tc);
        out.push_back({{"id", ids[i]}, {"name", labels[i]}, {"can", o["can"]}, {"why", o["why"]}, {"gains", o["gains"]}});
    }
    return out;
}

Json actJson(const World& w, const Words& W, const Character& me, const InteractionDef& d, const WorldObject* obj,
             const Character* tc) {
    if (d.parametric()) {
        Json o = {{"id", d.id}, {"name", W.name("interaction", d.id)}, {"can", false}, {"why", ""}, {"needs", Json::array()},
                  {"gains", Json::array()}, {"to_target", Json::array()}, {"legal", Json::array()}, {"minutes", int(d.duration) * int(24 * 60 / kTicksPerDay)}};
        Json ch = choicesJson(w, W, me, d, tc);
        for (const auto& c : ch)
            if (c["can"].get<bool>()) o["can"] = true;
        if (!o["can"].get<bool>())
            o["why"] = ch.empty() ? W.text("hud.why.no_choice") : ch.front()["why"].get<std::string>();
        o["choices"] = ch;
        return o;
    }
    const ActInfo a = actInfo(w, me, d, obj, tc);
    Json o = {{"id", d.id}, {"name", W.name("interaction", d.id)}, {"can", a.can}};
    std::string why;
    Json needs = Json::array();
    for (const auto& r : a.pre) {
        const std::string s = W.condition(r.cond);
        needs.push_back(Json::array({s, r.ok}));
        if (!r.ok && why.empty()) {
            why = s;
            // LR-135: where such a thing comes from, when the data names it
            const std::string hint = "hud.hint.source." + r.cond.key;
            if (r.cond.subject == "has" && !r.cond.onTarget && W.t.loc && W.t.loc->has(W.t.lang, hint)) why += ". " + W.text(hint);
        }
    }
    if (!a.can) {
        if (std::string s = W.why(a.why, d, obj, tc); !s.empty()) why = s;
        // LR-135: name who holds the spot
        if (obj && a.why.rfind("reserved", 0) == 0)
            if (const Character* by = w.character(obj->reservedBy))
                why = W.text("hud.why.reserved_by", {{"what", W.name("object", obj->type)}, {"who", by->name}});
        if (why.empty()) why = W.text("hud.why.unknown", {{"code", a.why}});
    }
    o["why"] = why;
    o["needs"] = needs;
    Json gains = Json::array(), toT = Json::array(), legal = Json::array();
    for (const auto& e : a.gains)
        if (std::string g = W.effect(e); !g.empty()) gains.push_back(g);
    for (const auto& e : a.onTarget)
        if (std::string g = W.effect(e); !g.empty()) toT.push_back(g);
    for (const auto& l : a.legal) legal.push_back(W.name("legal", l));
    o["gains"] = gains;
    o["to_target"] = toT;
    o["legal"] = legal;
    o["minutes"] = int(a.duration) * int(24 * 60 / kTicksPerDay);
    return o;
}

} // namespace

ActCatalog ActCatalog::make(const World& w, const Json& wheel) {
    ActCatalog c;
    if (wheel.is_object())
        for (const auto& sp : wheel.value("spheres", Json::array()))
            for (const auto& sub : sp.value("subs", Json::array()))
                for (const auto& id : sub.value("interactions", Json::array())) c.personActs.push_back(id.get<std::string>());
    for (const auto& [id, d] : w.content().interactions)
        if (d.targetsCreature() && d.baseId.empty()) c.beastActs[d.target.substr(9)].push_back(id);
    return c;
}

Json actsJson(const World& w, const ViewText& t, ActCatalog& cat, EntityId meId, EntityId target) {
    Json out = Json::array();
    const Character* me = w.character(meId);
    if (!me) return out;
    const Words W{t, me};
    const WorldObject* obj = w.object(target);
    const Character* tc = obj ? nullptr : w.character(target);
    if (target != kNone && !obj && !tc) return out;
    std::vector<std::string> ids;
    if (obj) {
        auto it = cat.objectActs.find(obj->type);
        if (it == cat.objectActs.end()) it = cat.objectActs.emplace(obj->type, actsOnObject(w.content(), obj->type)).first;
        ids = it->second;
    } else if (tc && tc->isAnimal()) {
        auto it = cat.beastActs.find(tc->species);
        if (it != cat.beastActs.end()) ids = it->second;
    } else if (tc) {
        ids = cat.personActs;
    } else {
        for (const auto& [id, d] : w.content().interactions)
            if (d.target == "self" && d.baseId.empty()) ids.push_back(id);
    }
    for (const auto& id : ids) {
        const InteractionDef* d = w.content().interaction(id);
        if (!d) continue;
        Json o = actJson(w, W, *me, *d, obj, tc);
        // on oneself: only what can be done now or lacks one thing (the rest is noise)
        if (target == kNone && !o["can"].get<bool>()) continue;
        out.push_back(std::move(o));
    }
    // LR-159: mount, hitch, drive, load... (sim/transport): the same perform, interaction "transport.<act>"
    for (const auto& a : sim::transportActs(w, *me, target)) {
        const std::string id = a["id"].get<std::string>(), why = a["why"].get<std::string>();
        const bool can = a["can"].get<bool>();
        if (target == kNone && !can) continue;
        out.push_back({{"id", id}, {"name", W.name("interaction", id)}, {"can", can}, {"why", can ? "" : resultText(t, why)},
                       {"needs", Json::array()}, {"gains", Json::array()}, {"to_target", Json::array()}, {"minutes", 1},
                       {"legal", Json::array()}});
    }
    // LR-122: on oneself, putting on what one carries and taking off what one wears
    // (the sim's wear / take_off commands: "cmd" + "item" instead of an act)
    if (target == kNone)
        if (const GearRules* G = gearRules(w.content())) {
            std::set<std::string> worn;
            for (const auto& [slot, item] : me->worn) worn.insert(item);
            for (const auto& [item, n] : me->inventory) {
                if (n <= 0 || worn.count(item) || G->item(item).slot.empty()) continue;
                const std::string why = canWear(*G, *me, item);
                if (!why.empty()) continue;
                out.push_back({{"id", "wear:" + item}, {"cmd", "wear"}, {"item", item}, {"can", true}, {"why", ""},
                               {"name", W.text("hud.act.wear", {{"what", W.name("item", item)}})}, {"needs", Json::array()},
                               {"gains", Json::array()}, {"to_target", Json::array()}, {"legal", Json::array()}, {"minutes", 0}});
            }
            for (const auto& item : worn)
                out.push_back({{"id", "take_off:" + item}, {"cmd", "take_off"}, {"item", item}, {"can", true}, {"why", ""},
                               {"name", W.text("hud.act.take_off", {{"what", W.name("item", item)}})}, {"needs", Json::array()},
                               {"gains", Json::array()}, {"to_target", Json::array()}, {"legal", Json::array()}, {"minutes", 0}});
        }
    return out;
}

Json barJson(const World& w, const ViewText& t, EntityId meId, EntityId target, const Json& slots) {
    Json out = Json::array();
    const Character* me = w.character(meId);
    const Words W{t, me};
    const Character* tc = target != kNone ? w.character(target) : nullptr;
    for (const auto& s : slots) {
        const std::string kind = s.value("kind", std::string()), id = s.value("id", std::string());
        Json o = {{"kind", kind}, {"id", id}, {"can", false}, {"why", ""}};
        if (kind == "goal") {
            const GoalDef* g = w.content().goal(id);
            o["name"] = W.name("goal", id);
            if (me && g) {
                std::string why;
                const bool can = anyMethodFeasible(w, *me, g->task, &why);
                o["can"] = can;
                if (!can) o["why"] = W.text("hud.why.unknown", {{"code", why}});
            }
        } else if (kind == "act") {
            const InteractionDef* d = w.content().interaction(id);
            o["name"] = W.name("interaction", id);
            if (me && d) {
                if (d->targetsCharacter() && !tc) {
                    o["why"] = W.text("hud.why.no_person");
                } else {
                    const Json a = actJson(w, W, *me, *d, nullptr, d->targetsCharacter() ? tc : nullptr);
                    o["can"] = a["can"];
                    o["why"] = a["why"];
                    o["needs_target"] = d->targetsCharacter();
                }
            }
        }
        out.push_back(std::move(o));
    }
    return out;
}

Json outcomesJson(const World& w, const ViewText& t, EntityId meId) {
    Json out = Json::array();
    const Character* me = w.character(meId);
    if (!me) return out;
    const Words W{t, me};
    auto items = [&](const Json& m) {
        std::string s;
        if (!m.is_object()) return s;
        for (auto it = m.begin(); it != m.end(); ++it) {
            if (!it.value().is_number_integer()) continue;
            s += (s.empty() ? "" : ", ") + std::to_string(it.value().get<int>()) + " " + W.name("item", it.key());
        }
        return s;
    };
    const Tick from = w.now() > kTicksPerDay ? w.now() - kTicksPerDay : 0;
    std::map<Tick, int> seen;   // the n-th entry of a tick (keys stay put as the trace scrolls)
    for (const auto& e : me->ai.trace) {
        const int n = seen[e.tick]++;
        if (e.tick < from) continue;
        std::string text;
        bool ok = true;
        const std::string act = e.params.value("action", std::string());
        if (e.code == "act.result") {
            ok = e.params.value("success", true);
            const std::string g = items(e.params.value("gain", Json::object())), l = items(e.params.value("lost", Json::object()));
            text = W.text(ok ? "hud.outcome.done" : "hud.outcome.failed", {{"act", W.name("interaction", act)}});
            if (!g.empty()) text += " " + W.text("hud.outcome.got", {{"what", g}});
            if (!l.empty()) text += " " + W.text("hud.outcome.spent", {{"what", l}});
        } else if (e.code == "act.outcome" && e.params.value("refused", false)) {
            ok = false;
            const Character* by = w.character(e.params.value("by", kNone));
            text = W.text("hud.outcome.refused", {{"act", W.name("interaction", act)}, {"who", by ? by->name : std::string()}});
        } else if (e.code == "goal.failed" && e.params.value("source", std::string()) == "player") {
            ok = false;
            const std::string task = e.params.value("task", std::string());
            const InteractionDef* d = w.content().interaction(task);
            const std::string what = d ? W.name("interaction", task) : W.name("goal", e.params.value("goal", task));
            std::string why = resultText(t, "rejected:" + e.params.value("why", std::string()));
            text = W.text("hud.outcome.gave_up", {{"act", what}, {"why", why}});
        } else continue;
        out.push_back({{"at", e.tick}, {"key", std::to_string(e.tick) + ":" + e.code + ":" + act + ":" + std::to_string(n)}, {"ok", ok}, {"text", text}});
    }
    while (out.size() > 8) out.erase(out.begin());
    return out;
}

std::string resultText(const ViewText& t, const std::string& result) {
    if (result.empty() || result == "ok" || result.rfind("ok:", 0) == 0) return "";
    const Words W{t};
    std::string r = result;
    if (r.rfind("rejected:", 0) == 0) r = r.substr(9);
    if (r.rfind("cannot:", 0) == 0) r = r.substr(7);
    if (r.rfind("target:", 0) == 0) r = r.substr(7);
    const std::string head = r.substr(0, r.find(':'));
    const std::string rest = r.size() > head.size() ? r.substr(head.size() + 1) : "";
    // LR-130: no raw codes: wrong_target:hearth, missing:mat:garlic
    if (head == "wrong_target" || head == "no_target") return W.text("hud.why.no_target", {{"what", W.name("object", rest)}});
    if (head == "gear") return W.text(rest == "too_heavy" ? "hud.why.too_heavy" : "hud.why.no_room");
    if (head == "missing" && rest.rfind("mat:", 0) == 0)
        return W.text("ui2d.refused.missing_mat", {{"what", W.name("material", rest.substr(4))}});
    for (const std::string key : {"hud.why." + head, "ui2d.refused." + head})
        if (t.loc && t.loc->has(t.lang, key)) return W.text(key, {{"what", ""}});
    // LR-130: a workshop refusal (no_station:rack, missing_input:work) reads as the forecast does
    if (t.loc && t.loc->has(t.lang, "craft.reason." + head)) {
        const std::string arg = r.size() > head.size() ? r.substr(head.size() + 1) : "";
        auto named = [&](const std::string& key, const std::string& fallback) {
            return t.loc->has(t.lang, key) ? W.text(key) : fallback;
        };
        const std::string station = named("station." + arg + ".name", arg);
        const std::string slot = named("hud.craft.slot." + arg, arg);
        return W.text("craft.reason." + head, {{"station", station}, {"slot", slot}, {"tool", arg}, {"need", arg},
                                               {"level", arg}, {"mode", arg}, {"shape", named("shape." + arg + ".name", arg)}});
    }
    return W.text("ui2d.refused", {{"code", r}});
}

} // namespace lr::client2d
