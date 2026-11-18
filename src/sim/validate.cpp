// Content validators (AI_DATA_SCHEMA.md §12, AI_ARCHITECTURE §10).

#include <algorithm>
#include <functional>
#include <set>

#include "sim/content.hpp"

namespace altera::sim {
namespace {

const std::set<std::string> kCondSubjects = {"has", "at", "need", "knows", "skill", "trait", "age", "body", "time",
                                             "known", "home", "target_stock", "target_owner", "rel", "moodlet",
                                             "reputation", "status_pair", "status_pair_any", "right", "verdict"};
// t.<subject>: conditions on the target character.
const std::set<std::string> kTargetCondSubjects = {"need", "skill", "body", "age", "moodlet", "has", "trait",
                                                   "knows", "reputation", "status_pair_any", "right", "verdict"};
// w.<subject>: effects on every witness of the actor (schema v0.3 §1).
const std::set<std::string> kWitnessEffSubjects = {"memory", "moodlet", "rel"};
const std::set<std::string> kEffSubjects = {"has",        "need",           "target_stock",   "home",
                                            "skill",      "body",           "moodlet",        "memory",
                                            "rel",        "rel_back",       "spawn",          "target_need",
                                            "reputation", "status_pair",    "household_join", "household_adopt",
                                            "household_leave", "household_found", "commitment", "death"};
const std::set<std::string> kTargetEffSubjects = {"need", "skill", "body", "moodlet", "has", "reputation", "death"};
// Flag effects: subject:key with no operator.
const std::set<std::string> kFlagEffects = {"moodlet",         "memory",          "spawn",           "household_join",
                                            "household_adopt", "household_leave", "household_found", "commitment",
                                            "death"};
// Effects and conditions that need a target character.
const std::set<std::string> kNeedsCharTarget = {"rel", "rel_back", "target_need", "status_pair", "household_join",
                                                "household_adopt", "commitment"};
const std::set<std::string> kParamTypes = {"item", "skill", "place", "need", "trait"};

bool contains(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Where a condition/effect sits: what it may refer to.
struct Scope {
    bool charTarget = false;                     // the interaction/dialogue has a target character
    const std::vector<ParamDef>* params = nullptr; // declared $params (parametric interaction)
    std::set<std::string>* used = nullptr;       // collects the $params used
};

// The parameter type a subject's key expects.
std::string keyType(const std::string& subject) {
    if (subject == "has" || subject == "home" || subject == "target_stock") return "item";
    if (subject == "skill" || subject == "knows") return "skill";
    if (subject == "at" || subject == "known" || subject == "spawn") return "place";
    if (subject == "need" || subject == "target_need") return "need";
    if (subject == "trait") return "trait";
    return "";
}

struct V {
    const Content& c;
    ValidationReport& r;

    void err(const std::string& s) { r.errors.push_back(s); }
    void warn(const std::string& s) { r.warnings.push_back(s); }

    // A $param key: declared, of a matching type. Returns false when the key
    // is a parameter (id checks are then done on its domain, not the key).
    bool param(const std::string& subject, const std::string& key, const std::string& text, const std::string& where,
               const Scope& sc) {
        if (key.empty() || key[0] != '$') return true;
        const std::string name = key.substr(1);
        if (!sc.params) {
            err(where + ": parameter '" + key + "' outside a parametric interaction in '" + text + "'");
            return false;
        }
        const ParamDef* pd = nullptr;
        for (const auto& p : *sc.params)
            if (p.name == name) pd = &p;
        if (!pd) {
            err(where + ": undeclared parameter '" + key + "' in '" + text + "'");
            return false;
        }
        if (sc.used) sc.used->insert(name);
        const std::string want = keyType(subject);
        if (!want.empty() && want != pd->type)
            err(where + ": parameter '" + key + "' is a " + pd->type + ", '" + subject + ":' wants a " + want);
        return false;
    }

    void cond(const Condition& k, const std::string& where, const Scope& sc = {}) {
        if (k.onTarget) {
            if (!kTargetCondSubjects.count(k.subject))
                return err(where + ": t." + k.subject + " is not a target condition ('" + k.text + "')");
            if (!sc.charTarget) err(where + ": '" + k.text + "' needs a character target");
        } else if (!kCondSubjects.count(k.subject)) {
            return err(where + ": unknown condition subject '" + k.subject + "'");
        }
        if ((k.subject == "rel" || k.subject == "status_pair") && !sc.charTarget) err(where + ": '" + k.text + "' needs a character target");
        if (!param(k.subject, k.key, k.text, where, sc)) return;
        if ((k.subject == "has" || k.subject == "home" || k.subject == "target_stock") && !c.items.count(k.key))
            err(where + ": unknown item '" + k.key + "' in '" + k.text + "'");
        if ((k.subject == "at" || k.subject == "known") && !c.objects.count(k.key))
            err(where + ": unknown object type '" + k.key + "' in '" + k.text + "'");
        if (k.subject == "need" && !c.need(k.key)) err(where + ": unknown need '" + k.key + "'");
        if (k.subject == "trait" && !c.traits.count(k.key)) err(where + ": unknown trait '" + k.key + "'");
        if (k.subject == "moodlet" && !c.moodlets.count(k.key)) err(where + ": unknown moodlet '" + k.key + "'");
        if (k.subject == "age" && k.key != "child" && k.key != "adult" && k.key != "elder")
            err(where + ": bad age stage '" + k.key + "'");
        if (k.subject == "body" && k.key != "health" && k.key != "fatigue")
            err(where + ": bad body key '" + k.key + "'");
        if (k.subject == "time" && k.key != "night" && k.key != "day") err(where + ": bad time '" + k.key + "'");
        if (k.subject == "target_owner" && k.key != "self" && k.key != "family" && k.key != "other" && k.key != "none")
            err(where + ": bad owner '" + k.key + "'");
        if (k.subject == "rel" && !contains(relAxes(), k.key)) err(where + ": unknown rel axis '" + k.key + "'");
        if (k.subject == "reputation" && !contains(c.config.reputationAxes, k.key))
            err(where + ": unknown reputation axis '" + k.key + "'");
        if ((k.subject == "status_pair" || k.subject == "status_pair_any") && !contains(c.config.statusPairs, k.key))
            err(where + ": unknown status pair '" + k.key + "'");
        if (k.subject == "right" && !contains(c.config.rights, k.key)) err(where + ": unknown right '" + k.key + "'");
        if (k.subject == "verdict" && !contains(c.config.verdicts, k.key))
            err(where + ": unknown verdict '" + k.key + "'");
    }

    void eff(const Effect& e, const std::string& where, const Scope& sc = {}) {
        if (e.onWitnesses) {
            if (!kWitnessEffSubjects.count(e.subject))
                return err(where + ": w." + e.subject + " is not a witness effect ('" + e.text + "')");
            if (e.subject == "memory" && e.op != 0) err(where + ": effect '" + e.text + "' has the wrong form");
            if (e.subject == "moodlet" && !c.moodlets.count(e.key)) err(where + ": unknown moodlet '" + e.key + "'");
            if (e.subject == "rel" && !contains(relAxes(), e.key)) err(where + ": unknown rel axis '" + e.key + "'");
            return;
        }
        if (e.onTarget) {
            if (!kTargetEffSubjects.count(e.subject))
                return err(where + ": t." + e.subject + " is not a target effect ('" + e.text + "')");
            if (!sc.charTarget) err(where + ": '" + e.text + "' needs a character target");
        } else if (!kEffSubjects.count(e.subject)) {
            return err(where + ": unknown effect subject '" + e.subject + "'");
        }
        if (kNeedsCharTarget.count(e.subject) && !sc.charTarget)
            err(where + ": '" + e.text + "' needs a character target");
        // Form: flag effects have no operator; status_pair is a flag (set) or =0/=1.
        if (e.subject == "status_pair") {
            if (e.op != 0 && !(e.op == '=' && (e.value == 0.0 || e.value == 1.0)))
                err(where + ": effect '" + e.text + "' has the wrong form (status_pair:<pair> or =0/=1)");
        } else if (kFlagEffects.count(e.subject) != (e.op == 0)) {
            err(where + ": effect '" + e.text + "' has the wrong form");
        }
        if (!param(e.subject, e.key, e.text, where, sc)) return;
        if ((e.subject == "has" || e.subject == "home" || e.subject == "target_stock") && !c.items.count(e.key))
            err(where + ": unknown item '" + e.key + "' in '" + e.text + "'");
        if ((e.subject == "need" || e.subject == "target_need") && !c.need(e.key))
            err(where + ": unknown need '" + e.key + "'");
        if (e.subject == "body" && e.key != "health" && e.key != "fatigue")
            err(where + ": bad body key '" + e.key + "'");
        if (e.subject == "moodlet" && !c.moodlets.count(e.key)) err(where + ": unknown moodlet '" + e.key + "'");
        if (e.subject == "spawn" && !c.objects.count(e.key)) err(where + ": unknown object '" + e.key + "'");
        if ((e.subject == "rel" || e.subject == "rel_back") && !contains(relAxes(), e.key))
            err(where + ": unknown rel axis '" + e.key + "'");
        if (e.subject == "reputation" && !contains(c.config.reputationAxes, e.key))
            err(where + ": unknown reputation axis '" + e.key + "'");
        if (e.subject == "status_pair" && !contains(c.config.statusPairs, e.key))
            err(where + ": unknown status pair '" + e.key + "'");
        if ((e.subject == "household_join" || e.subject == "household_adopt") &&
            !contains(c.config.householdRoles, e.key))
            err(where + ": unknown household role '" + e.key + "'");
        if (e.subject == "commitment" && !c.config.commitments.count(e.key))
            err(where + ": unknown commitment '" + e.key + "'");
        if (e.subject == "death" && !contains(c.config.deathCauses, e.key))
            err(where + ": unknown death cause '" + e.key + "'");
    }

    // Values allowed for a parameter type.
    bool paramValueOk(const std::string& type, const std::string& v) const {
        if (type == "item") return c.items.count(v) > 0;
        if (type == "place") return c.objects.count(v) > 0;
        if (type == "need") return c.need(v) != nullptr;
        if (type == "trait") return c.traits.count(v) > 0;
        return !v.empty(); // skill: knowledge ids are free-form
    }

    void interaction(const InteractionDef& d) {
        const std::string where = "interaction " + d.id;
        std::set<std::string> used;
        Scope sc{d.targetsCharacter(), d.parametric() ? &d.params : nullptr, &used};
        std::set<std::string> names;
        for (const auto& p : d.params) {
            if (!names.insert(p.name).second) err(where + ": duplicate parameter '" + p.name + "'");
            if (!kParamTypes.count(p.type)) err(where + ": parameter '" + p.name + "' has bad type '" + p.type + "'");
            for (const auto& v : p.of)
                if (!paramValueOk(p.type, v))
                    err(where + ": parameter '" + p.name + "' allows unknown " + p.type + " '" + v + "'");
        }
        if (d.parametric() && d.goap)
            warn(where + ": parametric interactions are not planned by GOAP yet (bound by dialogue/player only)");
        for (const auto& k : d.pre) cond(k, where, sc);
        for (const auto& e : d.eff) eff(e, where, sc);
        if (d.hasUntil) cond(d.until, where + " until", sc);
        if (d.continuous && !d.hasUntil) err(where + ": continuous without until");
        // chance / on_fail / risk
        if (d.chance.set) {
            if (d.continuous) err(where + ": chance on a continuous interaction");
            if (d.chance.base < 0.0 || d.chance.base > 1.0) err(where + ": chance base out of [0, 1]");
            if (d.chance.min < 0.0 || d.chance.max > 1.0 || d.chance.min > d.chance.max)
                err(where + ": chance min/max out of [0, 1]");
            if (!d.chance.skill.empty() && d.chance.skill[0] == '$') param("skill", d.chance.skill, "chance", where, sc);
            static const std::set<std::string> inputs = {"rel", "need", "body", "skill", "reputation"};
            for (const auto& in : d.chance.add) {
                if (!inputs.count(in.input.subject))
                    err(where + ": chance.add input '" + in.input.text + "' must be rel:/need:/body:/skill:/reputation:");
                else cond(in.input, where + " chance.add", sc);
            }
        }
        for (const auto* rules : {&d.legalIf, &d.legalUnless})
            for (const auto& lr : *rules) {
                for (const auto& any : lr.all)
                    for (const auto& k : any) cond(k, where + " legal rule " + lr.tag, sc);
                if (lr.mult >= 0.0 && rules == &d.legalIf) warn(where + ": legal_if " + lr.tag + ": mult is ignored");
            }
        if (!d.onFail.empty() && !d.chance.set) warn(where + ": on_fail without chance never applies");
        for (const auto& e : d.onFail) eff(e, where + " on_fail", sc);
        std::set<std::string> riskIds;
        for (const auto& rk : d.risk) {
            if (rk.id.empty()) err(where + ": risk without id");
            else if (!riskIds.insert(rk.id).second) err(where + ": duplicate risk '" + rk.id + "'");
            if (rk.p < 0.0 || rk.p > 1.0) err(where + ": risk " + rk.id + " p out of [0, 1]");
            if (rk.eff.empty()) err(where + ": risk " + rk.id + " without eff");
            for (const auto& e : rk.eff) eff(e, where + " risk " + rk.id, sc);
        }
        // combat
        if (d.combat.set) {
            const CombatDef& cb = d.combat;
            if (!d.targetsCharacter()) err(where + ": combat needs target: character");
            if (cb.damageLo < 0.0 || cb.damageHi > 1.0 || cb.damageLo > cb.damageHi)
                err(where + ": combat damage must be [lo, hi] within [0, 1]");
            static const std::set<std::string> outs = {"win", "lose", "draw", "flee"};
            for (const auto& [o, effs] : cb.outcomes) {
                if (!outs.count(o)) err(where + ": unknown combat outcome '" + o + "'");
                for (const auto& e : effs) eff(e, where + " combat." + o, sc);
            }
            if (cb.lethal && !contains(c.config.deathCauses, "combat"))
                err(where + ": lethal combat but 'combat' is not in config.death_causes");
        }
        // terms of a deal
        if (d.terms.set) {
            const TermsDef& t = d.terms;
            if (!d.targetsCharacter()) err(where + ": terms need target: character");
            auto items = [&](const std::map<std::string, int>& m, const char* side) {
                for (const auto& [it, n] : m) {
                    if (n <= 0) err(where + ": terms." + side + " count for '" + it + "' must be > 0");
                    if (!it.empty() && it[0] == '$') param("has", it, std::string("terms.") + side, where, sc);
                    else if (!c.items.count(it)) err(where + ": terms." + side + " unknown item '" + it + "'");
                }
            };
            items(t.give, "give");
            items(t.take, "take");
            if (t.give.empty() && t.take.empty()) err(where + ": terms with nothing to give or take");
            if (t.due < 0) err(where + ": terms due < 0");
            if (t.due > 0 && t.commitment.empty()) err(where + ": deferred terms (due > 0) need a commitment");
            if (!t.commitment.empty() && !c.config.commitments.count(t.commitment))
                err(where + ": unknown commitment '" + t.commitment + "'");
            for (const auto& e : t.onBreach) eff(e, where + " terms.on_breach", sc);
        }
        // speech acts the interaction can be offered with
        for (const auto& sa : d.speech)
            if (!c.speechActs.empty() && !contains(c.speechActs, sa))
                err(where + ": unknown speech act '" + sa + "'");
        for (const auto& p : d.params)
            if (!used.count(p.name)) warn(where + ": parameter '" + p.name + "' is never used");
    }
};

void checkCurve(const Curve& cv, const std::string& where, V& v) {
    static const std::set<std::string> types = {"linear", "inverse", "quadratic", "logistic",
                                                "inverse_logistic", "step", "const"};
    if (!types.count(cv.type)) return v.err(where + ": unknown curve type '" + cv.type + "'");
    if ((cv.type == "logistic" || cv.type == "inverse_logistic") && (cv.k < 0.1 || cv.k > 50.0))
        v.err(where + ": curve k out of [0.1, 50]");
    if ((cv.type == "logistic" || cv.type == "inverse_logistic" || cv.type == "step") && (cv.x0 < 0.0 || cv.x0 > 1.0))
        v.err(where + ": curve x0 out of [0, 1]");
    if (cv.type == "quadratic" && (cv.p < 0.1 || cv.p > 8.0)) v.err(where + ": curve p out of [0.1, 8]");
}

} // namespace

ValidationReport validate(const Content& c) {
    ValidationReport r;
    V v{c, r};

    // Needs / traits.
    for (const auto& n : c.needs) {
        if (n.decayPerDay < 0.0 || n.decayPerDay > 10.0) v.err("need " + n.id + ": decay_per_day out of range");
        if (n.critical < 0.0 || n.critical > 1.0) v.err("need " + n.id + ": critical out of [0,1]");
        if (n.startLo > n.startHi) v.err("need " + n.id + ": start range inverted");
    }
    for (const auto& [id, t] : c.traits) {
        for (const auto& [g, m] : t.goalMult)
            if (!c.goal(g)) v.err("trait " + id + ": goal_mult names unknown goal '" + g + "'");
        for (const auto& [n, m] : t.needMult)
            if (!c.need(n)) v.err("trait " + id + ": need_mult names unknown need '" + n + "'");
    }
    for (const auto& [a, b] : c.exclusiveTraits)
        if (!c.traits.count(a) || !c.traits.count(b)) v.err("traits: exclusive pair names unknown trait " + a + "/" + b);

    // Objects.
    for (const auto& [id, o] : c.objects) {
        for (const auto& [it, n] : o.stock)
            if (!c.items.count(it)) v.err("object " + id + ": unknown item in stock '" + it + "'");
        for (const auto& [it, n] : o.regrow)
            if (!c.items.count(it)) v.err("object " + id + ": unknown item in regrow '" + it + "'");
        if (!o.regrow.empty() && o.regrowEvery <= 0) v.err("object " + id + ": regrow without regrow_every");
    }

    // FSM.
    for (const auto& [id, s] : c.fsm) {
        if (!s.parent.empty() && !c.fsm.count(s.parent)) v.err("fsm " + id + ": unknown parent '" + s.parent + "'");
        std::string p = s.parent;
        for (int guard = 0; !p.empty(); ++guard) {
            if (guard > 16 || p == id) {
                v.err("fsm " + id + ": parent cycle");
                break;
            }
            auto it = c.fsm.find(p);
            p = it == c.fsm.end() ? "" : it->second.parent;
        }
    }

    // Interactions.
    std::set<std::string> referenced;
    for (const auto& [id, d] : c.interactions) {
        const std::string where = "interaction " + id;
        if (d.category.empty()) v.err(where + ": missing category");
        if (d.target != "self" && d.target != "character" && !d.targetsObject())
            v.err(where + ": bad target '" + d.target + "'");
        if (d.targetsObject() && !c.objects.count(d.targetType))
            v.err(where + ": unknown target object type '" + d.targetType + "'");
        if (d.duration < 1) v.err(where + ": duration < 1");
        if (d.cost < 0.0) v.err(where + ": negative cost");
        if (!c.fsm.empty() && !c.fsm.count(d.fsm)) v.err(where + ": unknown fsm state '" + d.fsm + "'");
        v.interaction(d);
    }

    // Dialogue (dialogue.json): speech acts and templates.
    {
        std::set<std::string> acts;
        for (const auto& sa : c.speechActs)
            if (!acts.insert(sa).second) v.err("dialogue: duplicate speech act '" + sa + "'");
        for (const auto& [id, t] : c.dialogue) {
            const std::string where = "dialogue " + id;
            const Scope sc{true, nullptr, nullptr};
            if (!acts.count(t.speech)) v.err(where + ": unknown speech act '" + t.speech + "'");
            for (const auto& k : t.pre) v.cond(k, where, sc);
            for (const auto& e : t.eff) v.eff(e, where, sc);
            if (t.variants.empty()) v.err(where + ": no variants (texts are dialogue." + id + ".<variant>)");
            std::set<std::string> vids;
            for (const auto& var : t.variants) {
                if (!vids.insert(var.id).second) v.err(where + ": duplicate variant '" + var.id + "'");
                if (var.weight < 0.0) v.err(where + "/" + var.id + ": negative weight");
                for (const auto& k : var.pre) v.cond(k, where + "/" + var.id, sc);
            }
            for (const auto& rp : t.replies)
                if (!c.dialogue.count(rp)) v.err(where + ": reply names unknown template '" + rp + "'");
        }
    }

    // Commitments (config.json).
    for (const auto& [id, cm] : c.config.commitments) {
        if (cm.due <= 0) v.err("commitment " + id + ": due must be > 0");
        for (const auto& e : cm.onBreach) v.eff(e, "commitment " + id, Scope{true, nullptr, nullptr});
    }

    // HTN.
    for (const auto& [id, t] : c.tasks) {
        if (t.methods.empty()) v.err("task " + id + ": no methods");
        for (const auto& m : t.methods) {
            const std::string where = "task " + id + "/" + m.id;
            for (const auto& k : m.pre) v.cond(k, where);
            if (m.steps.empty()) v.err(where + ": no steps");
            for (const auto& s : m.steps) {
                if (s.kind == HtnStep::Kind::Do) {
                    if (!c.interactions.count(s.ref)) v.err(where + ": unknown interaction '" + s.ref + "'");
                    referenced.insert(s.ref);
                } else if (s.kind == HtnStep::Kind::Task) {
                    if (!c.tasks.count(s.ref)) v.err(where + ": unknown task '" + s.ref + "'");
                } else {
                    if (s.achieve.empty()) v.err(where + ": empty achieve");
                    for (const auto& k : s.achieve) {
                        v.cond(k, where);
                        if (k.subject != "has" && k.subject != "at")
                            v.err(where + ": achieve supports has:/at: only, got '" + k.text + "'");
                    }
                    for (const auto& a : s.allow) {
                        if (!c.interactions.count(a)) v.err(where + ": unknown allowed interaction '" + a + "'");
                        referenced.insert(a);
                    }
                }
            }
        }
    }
    // Task recursion cycles.
    {
        std::map<std::string, int> state; // 0 new, 1 on stack, 2 done
        std::function<void(const std::string&)> dfs = [&](const std::string& id) {
            state[id] = 1;
            const HtnTask* t = c.task(id);
            if (t)
                for (const auto& m : t->methods)
                    for (const auto& s : m.steps) {
                        if (s.kind != HtnStep::Kind::Task || !c.task(s.ref)) continue;
                        if (state[s.ref] == 1) v.err("htn: task cycle through " + id + " -> " + s.ref);
                        else if (state[s.ref] == 0) dfs(s.ref);
                    }
            state[id] = 2;
        };
        for (const auto& [id, t] : c.tasks)
            if (state[id] == 0) dfs(id);
    }

    // Goals.
    for (const auto& g : c.goals) {
        const std::string where = "goal " + g.id;
        if (g.task.empty()) v.err(where + ": no task");
        else if (!c.task(g.task)) v.err(where + ": unknown task '" + g.task + "'");
        if (g.aggregate != "product" && g.aggregate != "sum") v.err(where + ": bad aggregate '" + g.aggregate + "'");
        if (g.considerations.empty()) v.err(where + ": no considerations");
        if (g.hasInterrupt) v.cond(g.interrupt, where + " interrupt");
        for (const auto& k : g.considerations) {
            checkCurve(k.curve, where, v);
            const auto colon = k.input.find(':');
            const std::string subj = k.input.substr(0, colon);
            const std::string key = colon == std::string::npos ? "" : k.input.substr(colon + 1);
            if (subj == "need" && !c.need(key)) v.err(where + ": unknown need input '" + k.input + "'");
            else if ((subj == "has" || subj == "home") && !c.items.count(key))
                v.err(where + ": unknown item input '" + k.input + "'");
            else if (subj != "need" && subj != "has" && subj != "home" && subj != "time" && subj != "body" &&
                     subj != "mood" && subj != "const" && subj != "trait" && subj != "age")
                v.err(where + ": unknown input '" + k.input + "'");
        }
        // Score identically zero over the whole range?
        bool any = false;
        for (double x = 0.0; x <= 1.0001 && !any; x += 0.05) {
            double s = 1.0;
            for (const auto& k : g.considerations) s *= k.curve.eval(x);
            if (s > 1e-6) any = true;
        }
        if (!any) v.warn(where + ": score is zero over the sampled range");
    }

    // Physical chain from an empty start: nothing appears out of thin air
    // (AI_ARCHITECTURE §5.2, v0.28 §7). Items obtainable from world objects, then
    // the fixpoint of GOAP interactions whose has: preconditions are satisfied.
    std::set<std::string> obtainable;
    std::map<std::string, std::set<std::string>> objectItems;
    for (const auto& [id, o] : c.objects) {
        for (const auto& [it, n] : o.stock) objectItems[id].insert(it);
        for (const auto& [it, n] : o.regrow) objectItems[id].insert(it);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [id, d] : c.interactions) {
            if (!d.goap) continue;
            bool ok = true;
            if (d.parametric()) continue; // bound at run time, not part of the static chain
            for (const auto& k : d.pre) {
                if (k.negate || k.onTarget) continue;
                if (k.subject == "has" && !obtainable.count(k.key)) ok = false;
                if (k.subject == "target_stock" && d.targetsObject() && !objectItems[d.targetType].count(k.key))
                    ok = false;
            }
            if (!ok) continue;
            for (const auto& e : d.eff) {
                if (e.onTarget) continue;
                if (e.subject == "has" && e.op == '+' && obtainable.insert(e.key).second) changed = true;
                if (e.subject == "target_stock" && e.op == '+' && d.targetsObject() &&
                    objectItems[d.targetType].insert(e.key).second)
                    changed = true;
            }
        }
    }
    for (const auto& [id, t] : c.tasks)
        for (const auto& m : t.methods)
            for (const auto& s : m.steps) {
                if (s.kind != HtnStep::Kind::Achieve) continue;
                for (const auto& k : s.achieve)
                    if (k.subject == "has" && !k.negate && !obtainable.count(k.key))
                        v.err("task " + id + "/" + m.id + ": '" + k.text +
                              "' is not obtainable from an empty start (no physical chain)");
            }
    for (const auto& [id, d] : c.interactions)
        for (const auto& k : d.pre)
            if (k.subject == "has" && !k.negate && !k.onTarget && !k.isParam() && !obtainable.count(k.key))
                v.warn("interaction " + id + ": requires '" + k.key + "' which no chain produces");

    for (const auto& [id, d] : c.interactions)
        // Character-targeted interactions are reached through dialogue and the player.
        if (!d.goap && !d.targetsCharacter() && !referenced.count(id)) v.warn("interaction " + id + ": not reachable by any plan");
    return r;
}

} // namespace altera::sim
