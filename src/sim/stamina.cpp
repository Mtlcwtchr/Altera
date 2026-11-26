#include "sim/stamina.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>

#include "sim/char_expr.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

std::map<std::string, double> numbers(const Json& j) {
    std::map<std::string, double> m;
    if (!j.is_object()) return m;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_number() && it.key().rfind('_', 0) != 0) m[it.key()] = it.value().get<double>();
    return m;
}

} // namespace

const StaminaRules* staminaRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<StaminaRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<StaminaRules> r;
    const fs::path p = fs::path(c.dir) / "stamina.json";
    if (data::exists(p)) {
        const Json j = data::readJson(p);
        r = std::make_unique<StaminaRules>();
        r->enabled = j.value("enabled", true);
        r->maxFormula = j.value("max_formula", r->maxFormula);
        r->regenFormula = j.value("regen_formula", r->regenFormula);
        r->fallbackMax = j.value("fallback_max", r->fallbackMax);
        r->fallbackRegen = j.value("fallback_regen_per_s", r->fallbackRegen);
        r->tickSeconds = j.value("tick_seconds", r->tickSeconds);
        r->spentBelow = j.value("spent_below", r->spentBelow);
        r->stallShare = std::clamp(j.value("stall_share", r->stallShare), 0.0, 1.0);
        const Json costs = j.value("cost_per_s", Json::object());
        r->defaultCost = costs.value("default", r->defaultCost);
        r->byAct = numbers(costs.value("act", Json::object()));
        r->byCategory = numbers(costs.value("category", Json::object()));
        r->byFsm = numbers(costs.value("fsm", Json::object()));
        const Json mv = j.value("move", Json::object());
        r->move.walkPerS = mv.value("walk_per_s", r->move.walkPerS);
        r->move.runPerS = mv.value("run_per_s", r->move.runPerS);
        r->move.walkRegenMult = mv.value("walk_regen_mult", r->move.walkRegenMult);
        r->move.runMinShare = mv.value("run_min_share", r->move.runMinShare);
    }
    return (cache[key] = std::move(r)).get();
}

double staminaCap(const Content& c, const Character& ch) {
    const StaminaRules* r = staminaRules(c);
    const double fb = r ? r->fallbackMax : 100.0;
    if (!r) return fb;
    const CharExprCtx x(ch);
    return std::max(1.0, c.formulas.eval(r->maxFormula, x, fb));
}

double staminaRegen(const Content& c, const Character& ch) {
    const StaminaRules* r = staminaRules(c);
    const double fb = r ? r->fallbackRegen : 1.0;
    if (!r) return fb;
    const CharExprCtx x(ch);
    return std::max(0.0, c.formulas.eval(r->regenFormula, x, fb));
}

double staminaCost(const StaminaRules& r, const InteractionDef* d, const std::string& fsmState) {
    if (d) {
        if (auto it = r.byAct.find(d->id); it != r.byAct.end()) return it->second;
        if (auto it = r.byCategory.find(d->category); it != r.byCategory.end()) return it->second;
    }
    if (auto it = r.byFsm.find(fsmState); it != r.byFsm.end()) return it->second;
    return d ? r.defaultCost : 0.0;
}

namespace {

void settle(const StaminaRules& r, Character& c) {
    c.body.stamina = std::clamp(c.body.stamina, 0.0, c.body.staminaMax);
    const double share = c.body.staminaMax > 0 ? c.body.stamina / c.body.staminaMax : 1.0;
    if (share <= r.spentBelow) c.body.winded = true;
    else if (share >= r.move.runMinShare) c.body.winded = false;
}

} // namespace

void staminaTick(World& w) {
    const Content& cont = w.content();
    const StaminaRules* r = staminaRules(cont);
    if (!r || !r->enabled) return;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive) continue;
        c.body.staminaMax = staminaCap(cont, c);
        if (c.body.stamina < 0) c.body.stamina = c.body.staminaMax;
        // the player's hand walk counts its own real seconds (staminaSteer)
        if (c.body.steeredAt >= 0 && w.now() - c.body.steeredAt <= 1) {
            settle(*r, c);
            continue;
        }
        // the player's own body lives in the client's real seconds (when the client says them)
        const double dt = c.controller.isPlayer() && w.client.realSecPerTick > 0 ? w.client.realSecPerTick : r->tickSeconds;
        const FsmState& f = c.ai.fsm;
        const InteractionDef* d = f.interaction.empty() || f.interaction == "goto" ? nullptr : cont.interaction(f.interaction);
        double cost = staminaCost(*r, d, f.state);
        double regen = staminaRegen(cont, c);
        if (f.state == "Move") regen *= r->move.walkRegenMult;
        // a spent body's act stalls this tick (tickFsm reads stalledAt) and it catches its breath
        if (d && staminaStalls(w, c)) c.body.stalledAt = w.now();
        if (c.body.downUntil > w.now() || c.body.stalledAt == w.now()) cost = 0.0;
        c.body.stamina += (regen - cost) * dt;
        settle(*r, c);
    }
}

bool staminaSteer(World& w, Character& c, double metres, double dt, bool run) {
    const Content& cont = w.content();
    const StaminaRules* r = staminaRules(cont);
    if (!r || !r->enabled) return true;
    if (c.body.staminaMax <= 0) c.body.staminaMax = staminaCap(cont, c);
    if (c.body.stamina < 0) c.body.stamina = c.body.staminaMax;
    c.body.steeredAt = w.now();
    dt = std::clamp(dt, 0.0, 1.0);
    const bool ran = run && !c.body.winded && metres > 1e-6;
    const bool moved = metres > 1e-6;
    const double regen = staminaRegen(cont, c) * (moved ? r->move.walkRegenMult : 1.0);
    const double cost = ran ? r->move.runPerS : (moved ? r->move.walkPerS : 0.0);
    c.body.stamina += (ran ? -cost : regen - cost) * dt;
    settle(*r, c);
    return ran || !run;
}

bool staminaSpent(const Content& cont, const Character& c) {
    const StaminaRules* r = staminaRules(cont);
    if (!r || !r->enabled || c.body.staminaMax <= 0 || c.body.stamina < 0) return false;
    return c.body.stamina <= r->spentBelow * c.body.staminaMax;
}

bool staminaStalls(const World& w, const Character& c) {
    const StaminaRules* r = staminaRules(w.content());
    if (!r || r->stallShare <= 0 || !staminaSpent(w.content(), c)) return false;
    // deterministic: a fixed share of ticks, spread by the id
    const std::uint64_t h = (static_cast<std::uint64_t>(w.now()) * 2654435761u) ^ static_cast<std::uint64_t>(c.id) * 40503u;
    return static_cast<double>(h % 1000) < r->stallShare * 1000.0;
}

bool staminaCanRun(const Content& cont, const Character& c) {
    const StaminaRules* r = staminaRules(cont);
    if (!r || !r->enabled) return true;
    return !c.body.winded;
}

} // namespace lr::sim
