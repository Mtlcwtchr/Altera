// Layer 2 - HTN: "how, in broad strokes?" A goal's root task is decomposed by
// methods from htn.json; a method needs its preconditions (knowledge, access,
// physical possibility) and is ranked by cost + trait bias + legal price +
// failure penalty. The law raises the price of a method, never removes it.

#include <algorithm>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

std::string methodKey(const std::string& task, const std::string& m) { return task + "/" + m; }

bool methodBlocked(const World& w, const Character& c, const std::string& task, const HtnMethod& m) {
    auto it = c.ai.methodFails.find(methodKey(task, m.id));
    return it != c.ai.methodFails.end() && it->second.until > w.now();
}

bool methodPre(const World& w, const Character& c, const HtnMethod& m, std::string* why) {
    const EvalCtx x{w, c, nullptr, nullptr};
    for (const auto& k : m.pre)
        if (!evalCondition(x, k)) {
            if (why) *why = "pre:" + k.text;
            return false;
        }
    return true;
}

bool expand(const World& w, const Character& c, const std::string& taskId, int depth, Plan& plan, std::string& why,
            Json& log) {
    const Content& ct = w.content();
    if (depth > ct.config.htnMaxDepth) {
        why = "htn.depth";
        return false;
    }
    const HtnTask* t = ct.task(taskId);
    if (!t) {
        // A catalog interaction queued directly (player/dialogue order).
        if (ct.interaction(taskId)) {
            plan.methods.push_back("direct/" + taskId);
            plan.steps.push_back(PlanStep{"do", taskId, {}, {}, "direct/" + taskId});
            log = Json{{"task", taskId}, {"chosen", "direct"}};
            return true;
        }
        why = "htn.unknown_task:" + taskId;
        return false;
    }
    struct Cand {
        const HtnMethod* m;
        double score;
    };
    std::vector<Cand> cands;
    Json rejected = Json::array();
    for (const auto& m : t->methods) {
        std::string r;
        if (methodBlocked(w, c, taskId, m)) {
            rejected.push_back({{"m", m.id}, {"why", "blocked_after_failures"}});
            continue;
        }
        if (!methodPre(w, c, m, &r)) {
            rejected.push_back({{"m", m.id}, {"why", r}});
            continue;
        }
        cands.push_back({&m, methodScore(w, c, taskId, m)});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    for (const Cand& cd : cands) {
        Plan sub;
        bool ok = true;
        Json subLog = Json::array();
        for (const auto& s : cd.m->steps) {
            if (s.kind == HtnStep::Kind::Task) {
                Json inner;
                if (!expand(w, c, s.ref, depth + 1, sub, why, inner)) {
                    ok = false;
                    break;
                }
                subLog.push_back(inner);
                continue;
            }
            PlanStep ps;
            ps.kind = s.kind == HtnStep::Kind::Do ? "do" : "achieve";
            ps.ref = s.ref;
            for (const auto& k : s.achieve) ps.achieve.push_back(k.text);
            ps.allow = s.allow;
            ps.params = s.params;
            ps.method = methodKey(taskId, cd.m->id);
            sub.steps.push_back(ps);
        }
        if (!ok) {
            rejected.push_back({{"m", cd.m->id}, {"why", "subtask:" + why}});
            continue;
        }
        plan.methods.push_back(methodKey(taskId, cd.m->id));
        plan.methods.insert(plan.methods.end(), sub.methods.begin(), sub.methods.end());
        plan.steps.insert(plan.steps.end(), sub.steps.begin(), sub.steps.end());
        Json alts = Json::array();
        for (const Cand& o : cands)
            if (o.m != cd.m) alts.push_back({{"m", o.m->id}, {"score", o.score}});
        log = Json{{"task", taskId}, {"chosen", cd.m->id}, {"score", cd.score}, {"rejected", rejected},
                   {"alternatives", alts}};
        if (!subLog.empty()) log["sub"] = subLog;
        return true;
    }
    why = "htn.no_method:" + taskId;
    log = Json{{"task", taskId}, {"rejected", rejected}};
    return false;
}

} // namespace

double methodScore(const World& w, const Character& c, const std::string& task, const HtnMethod& m) {
    const Content& ct = w.content();
    double s = m.cost;
    for (const auto& t : c.traits) {
        auto td = ct.traits.find(t);
        if (td == ct.traits.end()) continue;
        auto b = td->second.methodBias.find(m.id);
        if (b != td->second.methodBias.end()) s += b->second;
    }
    for (const auto& tag : m.legal) s += legalPrice(w, c, tag);
    auto it = c.ai.methodFails.find(task + "/" + m.id);
    if (it != c.ai.methodFails.end()) s += it->second.count * 1.5;
    return s;
}

bool anyMethodFeasible(const World& w, const Character& c, const std::string& task, std::string* why) {
    const Content& ct = w.content();
    const HtnTask* t = ct.task(task);
    if (!t) return ct.interaction(task) != nullptr;
    for (const auto& m : t->methods)
        if (!methodBlocked(w, c, task, m) && methodPre(w, c, m, why)) return true;
    return false;
}

bool decompose(const World& w, const Character& c, const std::string& task, Plan& out, std::string& why, Json& log) {
    out = Plan{};
    return expand(w, c, task, 0, out, why, log);
}

} // namespace altera::sim
