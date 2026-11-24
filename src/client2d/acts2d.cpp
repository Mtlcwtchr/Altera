#include "client2d/acts2d.hpp"

#include <cmath>
#include <cstdio>

#include "sim/act_info.hpp"
#include "sim/ai.hpp"

namespace altera::client2d {

using namespace altera::sim;

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
    std::string text(const std::string& key, const Json& args = Json::object()) const {
        return t.loc && t.loc->has(t.lang, key) ? t.loc->text(t.lang, key, args) : key;
    }
    std::string name(const std::string& kind, const std::string& id) const {
        return t.name(kind, id.substr(0, id.find('@')));   // <id>@foreign reads as the act
    }
    std::string condition(const Condition& k) const {
        const std::string who = k.onTarget ? text("hud.req.target") + " " : "";
        const bool counted = k.op == Condition::Op::Ge || k.op == Condition::Op::Gt;
        std::string out;
        if (k.subject == "has") {
            out = text("hud.req.has", {{"item", name("item", k.key)}});
            if (counted && k.value > 1) out += " x" + num(k.value);
        } else if (k.subject == "knows") out = text("hud.req.knows", {{"skill", name("skill", k.key)}});
        else if (k.subject == "skill")
            out = text("hud.req.skill", {{"skill", name("skill", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        else if (k.subject == "target_stock")
            out = text("hud.req.stock", {{"item", name("item", k.key)}, {"op", opText(k.op)}, {"v", num(k.value)}});
        else if (k.subject == "age") out = text("hud.req.age", {{"op", opText(k.op)}, {"v", num(k.value)}});
        else if (k.subject == "need")
            out = text("hud.req.need", {{"need", name("need", k.key)}, {"op", opText(k.op)}, {"v", num(k.value * 100)}});
        else if (k.subject == "home") out = text("hud.req.home", {{"what", name("item", k.key)}});
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
        if (e.subject == "skill") return text(e.op == '-' ? "hud.gain.skill_down" : "hud.gain.skill", {{"skill", name("skill", e.key)}});
        if (e.subject == "need") return text(e.op == '-' ? "hud.gain.need_down" : "hud.gain.need", {{"need", name("need", e.key)}});
        if (e.subject == "body" && e.key == "fatigue") return text(e.op == '-' ? "hud.gain.rest" : "hud.gain.tired");
        if (e.subject == "body" && e.key == "health") return text(e.op == '-' ? "hud.gain.hurt" : "hud.gain.heal");
        if (e.subject == "moodlet") return text("hud.gain.moodlet", {{"what", name("moodlet", e.key)}});
        if (e.subject == "rel" || e.subject == "rel_back")
            return text(e.signedDelta() >= 0 ? "hud.gain.rel_up" : "hud.gain.rel_down", {{"axis", text("hud.axis." + e.key)}});
        if (e.subject == "reputation") return text("hud.gain.reputation", {{"what", name("reputation", e.key)}, {"sign", sign}});
        if (e.subject == "spawn") return text("hud.gain.spawn", {{"what", name("object", e.key)}});
        return "";
    }
};

Json actJson(const World& w, const Words& W, const Character& me, const InteractionDef& d, const WorldObject* obj,
             const Character* tc) {
    const ActInfo a = actInfo(w, me, d, obj, tc);
    Json o = {{"id", d.id}, {"name", W.name("interaction", d.id)}, {"can", a.can}};
    std::string why;
    Json needs = Json::array();
    for (const auto& r : a.pre) {
        const std::string s = W.condition(r.cond);
        needs.push_back(Json::array({s, r.ok}));
        if (!r.ok && why.empty()) why = s;
    }
    if (!a.can) {
        if (std::string s = W.why(a.why, d, obj, tc); !s.empty()) why = s;
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
    const Words W{t};
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
    return out;
}

Json barJson(const World& w, const ViewText& t, EntityId meId, EntityId target, const Json& slots) {
    Json out = Json::array();
    const Character* me = w.character(meId);
    const Words W{t};
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

std::string resultText(const ViewText& t, const std::string& result) {
    if (result.empty() || result == "ok") return "";
    const Words W{t};
    std::string r = result;
    if (r.rfind("rejected:", 0) == 0) r = r.substr(9);
    if (r.rfind("cannot:", 0) == 0) r = r.substr(7);
    const std::string head = r.substr(0, r.find(':'));
    for (const std::string key : {"hud.why." + head, "ui2d.refused." + head})
        if (t.loc && t.loc->has(t.lang, key)) return W.text(key, {{"what", ""}});
    return W.text("ui2d.refused", {{"code", r}});
}

} // namespace altera::client2d
