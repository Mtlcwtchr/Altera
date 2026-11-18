// Layer 1 - Utility: "what do I want now?" Scores every goal type from its
// considerations (curves over needs, time, stock...), trait multipliers,
// weight modifiers (player hints, dialogue) and the hold bonus of the current
// goal. Selection (hysteresis, top-K weighted pick) lives in ai_mind.cpp.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

double inputValue(const World& w, const Character& c, const Consideration& k) {
    const auto colon = k.input.find(':');
    const std::string subj = k.input.substr(0, colon);
    const std::string key = colon == std::string::npos ? "" : k.input.substr(colon + 1);
    const double norm = k.norm > 0.0 ? k.norm : 1.0;
    if (subj == "need") return c.need(key);
    if (subj == "time") return (key == "night") == w.isNight() ? 1.0 : 0.0;
    if (subj == "body") return key == "health" ? c.body.health : c.body.fatigue;
    if (subj == "has") return clamp01(c.has(key) / norm);
    if (subj == "home") {
        const WorldObject* h = w.object(c.home);
        if (!h) return 0.0;
        auto it = h->stock.find(key);
        return clamp01((it == h->stock.end() ? 0 : it->second) / norm);
    }
    if (subj == "mood") return (c.mood(w.now()) + 1.0) * 0.5;
    if (subj == "trait") return c.hasTrait(key) ? 1.0 : 0.0;
    if (subj == "age") return c.body.stage() == key ? 1.0 : 0.0;
    return 1.0; // const
}

double modifierSum(const Character& c, const std::string& target, Tick now) {
    double s = 0.0;
    for (const auto& m : c.ai.mods)
        if (m.target == target) s += m.at(now);
    return s;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

} // namespace

std::vector<GoalScore> scoreGoals(const World& w, const Character& c) {
    const Content& ct = w.content();
    const AiConfig& cfg = ct.config;
    std::vector<GoalScore> out;
    for (const GoalDef& g : ct.goals) {
        GoalScore gs;
        gs.goal = g.id;
        gs.task = g.task;
        std::string why;
        if (!anyMethodFeasible(w, c, g.task, &why)) continue;
        // Considerations.
        double score = g.aggregate == "sum" ? 0.0 : 1.0;
        double wsum = 0.0;
        Json inputs = Json::object();
        for (const auto& k : g.considerations) {
            const double x = inputValue(w, c, k);
            const double u = k.curve.eval(x);
            inputs[k.input] = round3(x);
            if (g.aggregate == "sum") {
                score += u * k.weight;
                wsum += k.weight;
            } else {
                score *= std::pow(u, k.weight);
            }
        }
        if (g.aggregate == "sum") score = wsum > 0.0 ? score / wsum : 0.0;
        else if (g.considerations.size() > 1) {
            // Compensation (Dave Mark): many factors in (0,1) should not sink the
            // product just for being many.
            const double mod = 1.0 - 1.0 / static_cast<double>(g.considerations.size());
            score += (1.0 - score) * mod * score;
        }
        score *= g.base;
        // Traits.
        double tm = 1.0;
        for (const auto& t : c.traits) {
            auto td = ct.traits.find(t);
            if (td == ct.traits.end()) continue;
            auto m = td->second.goalMult.find(g.id);
            if (m != td->second.goalMult.end()) {
                tm *= m->second;
                gs.why["trait:" + t] = m->second;
            }
        }
        score *= tm;
        // Weight modifiers (player hints, dialogue, hold after a switch).
        const double mod = modifierSum(c, "goal:" + g.id, w.now());
        if (mod != 0.0) gs.why["mod"] = round3(mod);
        score += mod;
        // Failure cooldown.
        auto cd = c.ai.goalCooldown.find(g.id);
        if (cd != c.ai.goalCooldown.end() && cd->second > w.now()) {
            score *= cfg.failPenalty;
            gs.why["cooldown"] = true;
        }
        // Hold bonus of the current goal (hysteresis).
        if (c.ai.goal.id == g.id && c.ai.goal.source != "queued") {
            double hm = 1.0;
            for (const auto& t : c.traits) {
                auto td = ct.traits.find(t);
                if (td != ct.traits.end()) hm *= td->second.hysteresisMult;
            }
            score += cfg.hysteresisBonus * hm;
            gs.why["hold"] = round3(cfg.hysteresisBonus * hm);
        }
        gs.why["in"] = inputs;
        gs.score = std::max(0.0, score);
        if (gs.score > 1e-4) out.push_back(gs);
    }
    // Queued tasks (player layer-2 orders, dialogue promises): ordinary
    // candidates, weighed like everything else - the character may refuse.
    for (size_t i = 0; i < c.ai.queue.size(); ++i) {
        const QueuedTask& q = c.ai.queue[i];
        if (q.ttl > 0 && w.now() >= q.created + q.ttl) continue;
        GoalScore gs;
        gs.goal = "queued";
        gs.task = q.task;
        gs.source = "queued";
        gs.target = q.target;
        const bool isInteraction = ct.interaction(q.task) != nullptr;
        if (!isInteraction && !anyMethodFeasible(w, c, q.task, nullptr)) continue;
        gs.score = q.priority + modifierSum(c, "task:" + q.task, w.now());
        if (c.ai.goal.source == "queued" && c.ai.goal.task == q.task) gs.score += cfg.hysteresisBonus;
        gs.why["reason"] = q.reason;
        gs.why["source"] = q.source;
        if (gs.score > 1e-4) out.push_back(gs);
    }
    std::stable_sort(out.begin(), out.end(), [](const GoalScore& a, const GoalScore& b) {
        return a.score != b.score ? a.score > b.score : a.goal < b.goal;
    });
    return out;
}

} // namespace altera::sim
