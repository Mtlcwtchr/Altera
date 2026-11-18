// Content validators (AI_DATA_SCHEMA.md §12, AI_ARCHITECTURE §10).

#include <functional>
#include <set>

#include "sim/content.hpp"

namespace altera::sim {
namespace {

const std::set<std::string> kCondSubjects = {"has", "at", "need", "knows", "skill", "trait", "age", "body", "time",
                                             "known", "home", "target_stock", "target_owner", "rel"};
const std::set<std::string> kEffSubjects = {"has", "need", "target_stock", "home", "skill", "body",
                                            "moodlet", "memory", "rel", "rel_back", "spawn", "target_need"};

struct V {
    const Content& c;
    ValidationReport& r;

    void err(const std::string& s) { r.errors.push_back(s); }
    void warn(const std::string& s) { r.warnings.push_back(s); }

    void cond(const Condition& k, const std::string& where) {
        if (!kCondSubjects.count(k.subject)) return err(where + ": unknown condition subject '" + k.subject + "'");
        if ((k.subject == "has" || k.subject == "home" || k.subject == "target_stock") && !c.items.count(k.key))
            err(where + ": unknown item '" + k.key + "' in '" + k.text + "'");
        if ((k.subject == "at" || k.subject == "known") && !c.objects.count(k.key))
            err(where + ": unknown object type '" + k.key + "' in '" + k.text + "'");
        if (k.subject == "need" && !c.need(k.key)) err(where + ": unknown need '" + k.key + "'");
        if (k.subject == "trait" && !c.traits.count(k.key)) err(where + ": unknown trait '" + k.key + "'");
        if (k.subject == "age" && k.key != "child" && k.key != "adult" && k.key != "elder")
            err(where + ": bad age stage '" + k.key + "'");
        if (k.subject == "time" && k.key != "night" && k.key != "day") err(where + ": bad time '" + k.key + "'");
        if (k.subject == "target_owner" && k.key != "self" && k.key != "family" && k.key != "other" && k.key != "none")
            err(where + ": bad owner '" + k.key + "'");
    }

    void eff(const Effect& e, const std::string& where) {
        if (!kEffSubjects.count(e.subject)) return err(where + ": unknown effect subject '" + e.subject + "'");
        if ((e.subject == "has" || e.subject == "home" || e.subject == "target_stock") && !c.items.count(e.key))
            err(where + ": unknown item '" + e.key + "' in '" + e.text + "'");
        if ((e.subject == "need" || e.subject == "target_need") && !c.need(e.key))
            err(where + ": unknown need '" + e.key + "'");
        if (e.subject == "moodlet" && !c.moodlets.count(e.key)) err(where + ": unknown moodlet '" + e.key + "'");
        if (e.subject == "spawn" && !c.objects.count(e.key)) err(where + ": unknown object '" + e.key + "'");
        const bool flag = e.subject == "moodlet" || e.subject == "memory" || e.subject == "spawn";
        if (flag != (e.op == 0)) err(where + ": effect '" + e.text + "' has the wrong form");
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
        for (const auto& k : d.pre) v.cond(k, where);
        for (const auto& e : d.eff) v.eff(e, where);
        if (d.hasUntil) v.cond(d.until, where + " until");
        if (d.continuous && !d.hasUntil) v.err(where + ": continuous without until");
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
            for (const auto& k : d.pre) {
                if (k.negate) continue;
                if (k.subject == "has" && !obtainable.count(k.key)) ok = false;
                if (k.subject == "target_stock" && d.targetsObject() && !objectItems[d.targetType].count(k.key))
                    ok = false;
            }
            if (!ok) continue;
            for (const auto& e : d.eff) {
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
            if (k.subject == "has" && !k.negate && !obtainable.count(k.key))
                v.warn("interaction " + id + ": requires '" + k.key + "' which no chain produces");

    for (const auto& [id, d] : c.interactions)
        if (!d.goap && !referenced.count(id)) v.warn("interaction " + id + ": not reachable by any plan");
    return r;
}

} // namespace altera::sim
