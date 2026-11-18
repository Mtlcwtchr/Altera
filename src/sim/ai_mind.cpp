// The mind loop: Utility selection (hysteresis, interrupts, weighted top-K),
// HTN decomposition, GOAP planning per step, FSM execution - and the way
// failures climb back up (FSM -> GOAP replan -> HTN method switch -> Utility
// cooldown), each with a reason in the trace.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

constexpr int kMaxStepReplans = 3;
constexpr int kMaxHtnReplans = 4;
constexpr Tick kThinkEvery = 3;

Json goalTop(const std::vector<GoalScore>& s, size_t k) {
    Json a = Json::array();
    for (size_t i = 0; i < s.size() && i < k; ++i)
        a.push_back({{"goal", s[i].goal == "queued" ? "queued:" + s[i].task : s[i].goal},
                     {"score", std::round(s[i].score * 1000.0) / 1000.0}});
    return a;
}

void release(World& w, const Character& c, EntityId obj) {
    WorldObject* o = w.object(obj);
    if (o && o->reservedBy == c.id) o->reservedBy = kNone;
}

void finishAction(World& w, Character& c) {
    release(w, c, c.ai.fsm.target);
    c.ai.fsm = FsmState{};
    c.ai.plan.actionIndex++;
}

void failStep(World& w, Character& c, const std::string& why);

void replanStep(World& w, Character& c, const std::string& why) {
    Plan& p = c.ai.plan;
    abortAction(w, c, why);
    p.actions.clear();
    p.actionIndex = 0;
    ++c.ai.stats.replans;
    if (++p.stepReplans > kMaxStepReplans) failStep(w, c, why);
    else trace(w, c, 'G', "goap.replan", {{"why", why}, {"attempt", p.stepReplans}});
}

} // namespace

void trace(World& w, Character& c, char layer, std::string code, Json params) {
    c.ai.trace.push_back(TraceEntry{w.now(), layer, std::move(code), std::move(params)});
    while (static_cast<int>(c.ai.trace.size()) > std::max(8, w.content().config.traceSize)) c.ai.trace.pop_front();
}

void releaseReservations(World& w, const Character& c) {
    for (EntityId id : c.knownObjects) release(w, c, id);
}

void abortAction(World& w, Character& c, const std::string& reason) {
    if (c.ai.fsm.state != "Idle" || c.ai.fsm.target != kNone) {
        if (c.ai.fsm.state != "Idle")
            trace(w, c, 'F', "fsm.abort", {{"state", c.ai.fsm.state}, {"action", c.ai.fsm.interaction}, {"why", reason}});
        release(w, c, c.ai.fsm.target);
    }
    c.ai.fsm = FsmState{};
    // A goto reserved the target of the action after it.
    const Plan& p = c.ai.plan;
    if (p.actionIndex + 1 < p.actions.size()) release(w, c, p.actions[p.actionIndex + 1].target);
}

void dropGoal(World& w, Character& c, const std::string& reason, bool failed) {
    AiState& ai = c.ai;
    if (!ai.goal.valid()) return;
    abortAction(w, c, reason);
    releaseReservations(w, c);
    const GoalDef* g = w.content().goal(ai.goal.id);
    if (failed) {
        ai.goalCooldown[ai.goal.id] = w.now() + (g ? g->cooldownOnFail : 12);
        ai.stats.lastGoalFail[ai.goal.id] = reason;
        ai.lastFailReason = reason;
        ++ai.stats.failReasons[reason];
        if (g && g->priorityClass == "need") w.remember(c, MemoryEntry{0, "goal_failed", {}, kNone, -0.2, 0.0, "seen", 1.0});
    }
    if (ai.goal.source == "queued") {
        auto& q = ai.queue;
        q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedTask& t) { return t.task == ai.goal.task; }),
                q.end());
    }
    trace(w, c, 'U', failed ? "goal.failed" : (reason == "done" ? "goal.done" : "goal.dropped"),
          {{"goal", ai.goal.id}, {"task", ai.goal.task}, {"why", reason}});
    // A finished player order starts the free-will countdown again.
    if (c.controller.isPlayer() && ai.goal.source != "utility") c.controller.idleSince = w.now();
    ai.goal = GoalInstance{};
    ai.plan = Plan{};
}

bool adoptGoal(World& w, Character& c, const GoalScore& gs, const std::string& source) {
    AiState& ai = c.ai;
    if (ai.goal.valid()) {
        ++ai.stats.goalSwitches;
        dropGoal(w, c, "switched_to:" + gs.goal, false);
    }
    GoalInstance g;
    g.id = gs.goal;
    g.source = source;
    g.score = gs.score;
    g.started = w.now();
    g.task = gs.task;
    g.target = gs.target;
    Plan plan;
    std::string why;
    Json log;
    if (!decompose(w, c, g.task, plan, why, log)) {
        ai.goal = g;
        trace(w, c, 'H', "htn.no_method", {{"task", g.task}, {"log", log}});
        dropGoal(w, c, why, true);
        return false;
    }
    ai.goal = g;
    ai.plan = plan;
    for (const auto& m : plan.methods) ++ai.stats.methodUse[m];
    trace(w, c, 'H', "htn.plan", {{"task", g.task}, {"methods", plan.methods}, {"steps", plan.steps.size()}, {"log", log}});
    return true;
}

namespace {

void failStep(World& w, Character& c, const std::string& why) {
    AiState& ai = c.ai;
    Plan& p = ai.plan;
    const std::string method = p.stepIndex < p.steps.size() ? p.steps[p.stepIndex].method : "";
    MethodFail& mf = ai.methodFails[method];
    ++mf.count;
    mf.until = w.now() + (mf.count >= w.content().config.methodMaxFails ? w.content().config.methodFailTicks : 6);
    ++ai.stats.planFailures;
    ++ai.stats.failReasons[why];
    trace(w, c, 'H', "htn.method_failed", {{"method", method}, {"why", why}, {"fails", mf.count}});
    abortAction(w, c, why);
    if (p.htnReplans >= kMaxHtnReplans) {
        dropGoal(w, c, why, true);
        return;
    }
    Plan next;
    std::string why2;
    Json log;
    const int replans = p.htnReplans + 1;
    if (!decompose(w, c, ai.goal.task, next, why2, log)) {
        trace(w, c, 'H', "htn.no_method", {{"task", ai.goal.task}, {"log", log}});
        dropGoal(w, c, why, true); // the goal fails for the step's reason
        return;
    }
    next.htnReplans = replans;
    ai.plan = next;
    for (const auto& m : next.methods) ++ai.stats.methodUse[m];
    trace(w, c, 'H', "htn.replan", {{"task", ai.goal.task}, {"methods", next.methods}, {"log", log}});
}

// One FSM tick. Returns true when the running action finished this tick.
bool tickFsm(World& w, Character& c) {
    AiState& ai = c.ai;
    FsmState& f = ai.fsm;
    if (f.state == "Idle") return false;
    const AiConfig& cfg = w.content().config;
    if (f.state == "Wait") {
        if (--f.timer <= 0) c.ai.fsm = FsmState{};
        return f.timer <= 0;
    }
    if (f.state == "Move") {
        const double d = dist(c.pos, f.dest);
        if (d <= cfg.walkMPerTick || f.timer <= 1) {
            c.pos = f.dest;
        } else {
            c.pos.x += (f.dest.x - c.pos.x) * cfg.walkMPerTick / d;
            c.pos.y += (f.dest.y - c.pos.y) * cfg.walkMPerTick / d;
        }
        --f.timer;
        if (dist(c.pos, f.dest) < 1e-6) {
            trace(w, c, 'F', "fsm.arrived", {{"target", f.target}});
            c.ai.fsm = FsmState{};
            ai.plan.actionIndex++;
            return true;
        }
        return false;
    }
    const InteractionDef* d = w.content().interaction(f.interaction);
    if (!d) {
        replanStep(w, c, "fsm.unknown_action:" + f.interaction);
        return false;
    }
    WorldObject* target = w.object(f.target);
    --f.timer;
    if (d->continuous) {
        applyEffects(w, c, *d, target);
        const EvalCtx x{w, c, target, nullptr};
        if ((d->hasUntil && evalCondition(x, d->until)) || f.timer <= 0) {
            trace(w, c, 'F', "fsm.done", {{"action", d->id}});
            finishAction(w, c);
            return true;
        }
        return false;
    }
    if (f.timer > 0) return false;
    std::string why;
    if (!canPerform(w, c, *d, target, &why)) {
        replanStep(w, c, "fsm.lost:" + why);
        return false;
    }
    applyEffects(w, c, *d, target);
    trace(w, c, 'F', "fsm.done", {{"action", d->id}});
    finishAction(w, c);
    return true;
}

enum class Start { Started, Instant, Failed, Wait };
constexpr int kMaxWaits = 12;

Start startAction(World& w, Character& c, std::string& why) {
    AiState& ai = c.ai;
    Plan& p = ai.plan;
    PlanAction& a = p.actions[p.actionIndex];
    const AiConfig& cfg = w.content().config;
    if (a.interaction == "goto") {
        if (p.actionIndex + 1 >= p.actions.size()) {
            why = "goto.dangling";
            return Start::Failed;
        }
        PlanAction& next = p.actions[p.actionIndex + 1];
        const InteractionDef* d = w.content().interaction(next.interaction);
        if (!d) {
            why = "goto.unknown_action";
            return Start::Failed;
        }
        std::string fw;
        const EntityId t = findTarget(w, c, *d, c.pos, &fw);
        if (t == kNone) {
            why = "goto.no_target:" + fw;
            // Someone else holds it: wait a little instead of giving up.
            if (fw.rfind("reserved:", 0) == 0 && p.waits < kMaxWaits) {
                ++p.waits;
                ai.fsm = FsmState{"Wait", "wait", kNone, c.pos, 2, w.now()};
                if (p.waits == 1) trace(w, c, 'F', "fsm.wait", {{"for", fw}});
                return Start::Wait;
            }
            return Start::Failed;
        }
        WorldObject* o = w.object(t);
        next.target = t;
        if (d->reserve) {
            o->reservedBy = c.id;
            o->reservedUntil = w.now() + cfg.reserveTicks;
        }
        const double dd = dist(c.pos, o->pos);
        if (dd <= cfg.interactionRangeM) {
            p.actionIndex++;
            return Start::Instant;
        }
        ai.fsm = FsmState{"Move", "goto", t, o->pos, static_cast<Tick>(std::ceil(dd / cfg.walkMPerTick)), w.now()};
        trace(w, c, 'F', "fsm.enter", {{"state", "Move"}, {"to", a.targetType}, {"target", t},
                                       {"ticks", ai.fsm.timer}});
        return Start::Started;
    }
    const InteractionDef* d = w.content().interaction(a.interaction);
    if (!d) {
        why = "fsm.unknown_action:" + a.interaction;
        return Start::Failed;
    }
    WorldObject* target = nullptr;
    if (d->targetsObject()) {
        if (a.target == kNone) {
            std::string fw;
            a.target = findTarget(w, c, *d, c.pos, &fw);
        }
        target = w.object(a.target);
    }
    if (!canPerform(w, c, *d, target, &why)) return Start::Failed;
    if (d->reserve && target) {
        target->reservedBy = c.id;
        target->reservedUntil = w.now() + d->duration + 2;
    }
    ai.fsm = FsmState{d->fsm, d->id, target ? target->id : kNone, target ? target->pos : c.pos, d->duration, w.now()};
    trace(w, c, 'F', "fsm.enter", {{"state", d->fsm}, {"root", w.content().fsmRoot(d->fsm)}, {"action", d->id},
                                   {"target", ai.fsm.target}, {"ticks", d->duration}});
    return Start::Started;
}

bool achieved(const World& w, const Character& c, const PlanStep& s) {
    if (s.kind != "achieve") return false;
    const EvalCtx x{w, c, nullptr, nullptr};
    for (const auto& t : s.achieve)
        if (!evalCondition(x, parseCondition(t))) return false;
    return true;
}

} // namespace

void executePlan(World& w, Character& c) {
    AiState& ai = c.ai;
    if (ai.fsm.state != "Idle") {
        if (!tickFsm(w, c)) return;
    }
    if (!ai.goal.valid()) return;
    for (int guard = 0; guard < 6 && ai.goal.valid(); ++guard) {
        Plan& p = ai.plan;
        if (p.stepIndex >= p.steps.size()) {
            dropGoal(w, c, "done", false);
            return;
        }
        const PlanStep& step = p.steps[p.stepIndex];
        if (p.actions.empty()) {
            if (achieved(w, c, step)) {
                p.stepIndex++;
                p.stepReplans = 0;
                continue;
            }
            GoapResult r = planStep(w, c, step);
            if (!r.ok) {
                trace(w, c, 'G', "goap.fail", {{"step", step.kind == "do" ? step.ref : step.achieve.front()},
                                               {"why", r.why}, {"nodes", r.nodes}});
                failStep(w, c, r.why);
                return;
            }
            Json acts = Json::array();
            for (const auto& a : r.actions) acts.push_back(a.interaction == "goto" ? "goto:" + a.targetType : a.interaction);
            trace(w, c, 'G', "goap.plan", {{"step", step.kind == "do" ? step.ref : step.achieve.front()},
                                           {"actions", acts}, {"cost", std::round(r.cost * 10.0) / 10.0},
                                           {"nodes", r.nodes}});
            p.actions = std::move(r.actions);
            p.actionIndex = 0;
        }
        if (p.actionIndex >= p.actions.size()) {
            p.actions.clear();
            p.actionIndex = 0;
            p.stepIndex++;
            p.stepReplans = 0;
            p.waits = 0;
            continue;
        }
        std::string why;
        const Start s = startAction(w, c, why);
        if (s == Start::Started || s == Start::Wait) return;
        if (s == Start::Failed) {
            replanStep(w, c, why);
            return;
        }
    }
}

// Layer 1 under AiController.
void aiThink(World& w, Character& c) {
    AiState& ai = c.ai;
    const Content& ct = w.content();
    const bool busy = ai.fsm.state != "Idle";
    const bool interruptible = !busy || ct.fsmInterruptible(ai.fsm.state);
    // Interrupts: a critical need breaks any goal (if the body can stop).
    const GoalDef* interrupter = nullptr;
    {
        const EvalCtx x{w, c, nullptr, nullptr};
        for (const auto& g : ct.goals)
            if (g.hasInterrupt && g.id != ai.goal.id && evalCondition(x, g.interrupt)) {
                const GoalDef* cur = ct.goal(ai.goal.id);
                if (cur && cur->hasInterrupt && evalCondition(x, cur->interrupt)) continue; // already handling a crisis
                if (!interrupter) interrupter = &g;
            }
    }
    const bool due = !ai.goal.valid() || w.now() >= ai.nextThink || interrupter;
    if (!due) return;
    ai.nextThink = w.now() + kThinkEvery;
    std::vector<GoalScore> scores = scoreGoals(w, c);
    if (scores.empty()) {
        if (!ai.goal.valid()) ++ai.stats.idleReasons["no_feasible_goal"];
        return;
    }
    if (interrupter && interruptible) {
        for (const auto& s : scores)
            if (s.goal == interrupter->id) {
                trace(w, c, 'U', "utility.interrupt", {{"goal", s.goal}, {"was", ai.goal.id},
                                                       {"cond", interrupter->interrupt.text}});
                adoptGoal(w, c, s, "utility");
                return;
            }
    }
    double current = 0.0;
    if (ai.goal.valid()) {
        const GoalDef* g = ct.goal(ai.goal.id);
        const Tick age = w.now() - ai.goal.started;
        if (!interruptible || (g && age < g->minTicks)) return;
        for (const auto& s : scores)
            if ((s.goal == ai.goal.id && s.goal != "queued") || (s.goal == "queued" && ai.goal.source == "queued" &&
                                                                  s.task == ai.goal.task))
                current = s.score;
        if (scores.front().score <= current * (1.0 + ct.config.switchMargin) + 1e-9) return;
    }
    // Weighted pick among the top K (seeded per agent and tick, not argmax).
    const size_t k = std::min(scores.size(), static_cast<size_t>(std::max(1, ct.config.topK)));
    const double best = scores.front().score;
    std::vector<double> wts(k);
    double sum = 0.0;
    for (size_t i = 0; i < k; ++i) {
        wts[i] = std::max(0.0, scores[i].score - 0.6 * best);
        if (scores[i].score <= current * (1.0 + ct.config.switchMargin)) wts[i] = 0.0; // no worse than staying
        sum += wts[i];
    }
    size_t pick = 0;
    if (sum > 0.0) {
        Rng r = w.rng(c.id, "utility");
        double roll = r.uniform() * sum;
        for (size_t i = 0; i < k; ++i) {
            if (roll < wts[i]) {
                pick = i;
                break;
            }
            roll -= wts[i];
        }
    }
    const GoalScore& chosen = scores[pick];
    if (ai.goal.valid() &&
        ((chosen.goal == ai.goal.id && chosen.goal != "queued") || (chosen.goal == "queued" && chosen.task == ai.goal.task)))
        return;
    trace(w, c, 'U', "utility.choose", {{"top", goalTop(scores, k)},
                                        {"chosen", chosen.goal == "queued" ? "queued:" + chosen.task : chosen.goal},
                                        {"why", chosen.why}});
    adoptGoal(w, c, chosen, chosen.source);
}

void thinkCharacter(World& w, Character& c) {
    if (!c.body.alive) return;
    AiState& ai = c.ai;
    // Expire modifiers and stale queued tasks.
    ai.mods.erase(std::remove_if(ai.mods.begin(), ai.mods.end(),
                                 [&](const WeightMod& m) { return w.now() >= m.created + m.ttl; }),
                  ai.mods.end());
    ai.queue.erase(std::remove_if(ai.queue.begin(), ai.queue.end(),
                                  [&](const QueuedTask& q) { return q.ttl > 0 && w.now() >= q.created + q.ttl; }),
                   ai.queue.end());
    if (c.controller.isPlayer()) playerThink(w, c);
    else aiThink(w, c);
    // Statistics for the report (what this tick was spent on).
    if (ai.goal.valid()) ++ai.stats.goalTicks[ai.goal.source == "queued" ? "queued:" + ai.goal.task : ai.goal.id];
    if (!ai.goal.valid() || ai.goal.id == "idle") {
        ++ai.stats.idleTicks;
        std::string why = ai.goal.id == "idle" ? "chose_idle" : "no_goal";
        if (c.controller.isPlayer() && !ai.goal.valid()) why = ai.suspended ? "player_paused_plan" : "player_no_order";
        ++ai.stats.idleReasons[why];
    }
    executePlan(w, c);
}

void installAi(World& w) {
    w.setThinkHook([](World& world) {
        for (auto& [id, c] : world.characters()) thinkCharacter(world, c);
    });
    installControl(w);
}

} // namespace altera::sim
