// "Why" report (AI_ARCHITECTURE §8): per character and aggregated - why
// hungry, why idle, which plans broke and on what. Codes and parameters only;
// display text is assembled through localization at output time.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/consequences.hpp"

namespace altera::sim {

namespace {
double r3(double v) { return std::round(v * 1000.0) / 1000.0; }
} // namespace

Json aiReport(const World& w) {
    Json chars = Json::array();
    std::map<std::string, std::int64_t> idleAll, failAll, methodAll, goalAll;
    Json hunger = Json::array();
    int alive = 0;
    int population = 0;
    std::map<std::string, std::pair<int, int>> fauna; // species -> groups, heads
    std::map<std::string, std::map<std::string, int>> faunaLost; // species -> cause -> heads (AL-82)
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) {
            if (c.body.alive) {
                ++fauna[c.species].first;
                fauna[c.species].second += c.group;
            }
            for (const auto& [k, v] : c.needs)
                if (k.rfind("lost_", 0) == 0) faunaLost[c.species][k.substr(5)] += static_cast<int>(v);
            continue;
        }
        ++population;
        const AiStats& s = c.ai.stats;
        Json needs = Json::object();
        for (const auto& [n, v] : c.needs) needs[n] = r3(v);
        Json cj = {{"id", id},
                   {"name", c.name},
                   {"family", c.status.family},
                   {"alive", c.body.alive},
                   {"health", r3(c.body.health)},
                   {"controller", c.controller.isPlayer() ? "player" : "ai"},
                   {"needs", needs},
                   {"goal", c.ai.goal.id},
                   {"fsm", c.ai.fsm.state},
                   {"goal_ticks", s.goalTicks},
                   {"idle_ticks", s.idleTicks},
                   {"idle_reasons", s.idleReasons},
                   {"starving_ticks", s.starvingTicks},
                   {"fail_reasons", s.failReasons},
                   {"last_goal_fail", s.lastGoalFail},
                   {"methods", s.methodUse},
                   {"replans", s.replans},
                   {"plan_failures", s.planFailures},
                   {"goal_switches", s.goalSwitches}};
        chars.push_back(cj);
        alive += c.body.alive ? 1 : 0;
        for (const auto& [k, v] : s.idleReasons) idleAll[k] += v;
        for (const auto& [k, v] : s.failReasons) failAll[k] += v;
        for (const auto& [k, v] : s.methodUse) methodAll[k] += v;
        for (const auto& [k, v] : s.goalTicks) goalAll[k] += v;
        if (s.starvingTicks > 0 || c.need("hunger") < 0.15) {
            Json why = Json::array();
            auto it = s.lastGoalFail.find("eat");
            if (it != s.lastGoalFail.end()) why.push_back(it->second);
            for (const auto& [k, v] : s.failReasons)
                if (k.find("meal") != std::string::npos || k.find("berr") != std::string::npos ||
                    k.find("obtain_food") != std::string::npos)
                    why.push_back(k);
            hunger.push_back({{"id", id}, {"name", c.name}, {"starving_ticks", s.starvingTicks},
                              {"hunger", r3(c.need("hunger"))}, {"why", why}});
        }
    }
    auto top = [](const std::map<std::string, std::int64_t>& m, size_t n) {
        std::vector<std::pair<std::string, std::int64_t>> v(m.begin(), m.end());
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        Json out = Json::array();
        for (size_t i = 0; i < v.size() && i < n; ++i) out.push_back({{"code", v[i].first}, {"count", v[i].second}});
        return out;
    };
    Json deaths = Json::array();
    for (const auto& e : w.chronicleLog())
        if (e.type == "death") deaths.push_back({{"tick", e.tick}, {"who", e.who}, {"code", e.code}, {"params", e.params}});
    return Json{{"tick", w.now()},
                {"day", w.day()},
                {"seed", w.seed()},
                {"alive", alive},
                {"population", population},
                {"fauna", [&] {
                     Json f = Json::object();
                     for (const auto& [sp, gh] : fauna) f[sp] = {{"groups", gh.first}, {"heads", gh.second}};
                     for (const auto& [sp, lost] : faunaLost) f[sp]["lost"] = lost;
                     return f;
                 }()},
                {"summary",
                 {{"why_hunger", hunger},
                  {"why_idle", top(idleAll, 10)},
                  {"plan_failures", top(failAll, 15)},
                  {"methods", top(methodAll, 30)},
                  {"goal_time", top(goalAll, 20)},
                  {"deaths", deaths},
                  {"rumors", rumorReport(w)}}},
                {"characters", chars}};
}

} // namespace altera::sim
