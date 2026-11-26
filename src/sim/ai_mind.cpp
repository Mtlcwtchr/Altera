// The mind loop: Utility selection (hysteresis, interrupts, weighted top-K),
// HTN decomposition, GOAP planning per step, FSM execution - and the way
// failures climb back up (FSM -> GOAP replan -> HTN method switch -> Utility
// cooldown), each with a reason in the trace.

#include <algorithm>
#include <cmath>

#include "sim/dialogue_say.hpp"
#include "sim/ai.hpp"
#include "sim/craft_sim.hpp"
#include "sim/memory_budget.hpp"
#include "sim/building.hpp"
#include "sim/captivity.hpp"
#include "sim/consequences.hpp"
#include "sim/gear.hpp"
#include "sim/social_chain.hpp"
#include "sim/trade.hpp"
#include "sim/village_life.hpp"
#include "sim/realm.hpp"
#include "sim/worldmap.hpp"
#include "sim/magic.hpp"
#include "sim/magic_society.hpp"
#include "sim/culture.hpp"
#include "sim/wounds.hpp"
#include "sim/firefight.hpp"
#include "sim/world_fields.hpp"
#include "sim/world_reactions.hpp"
#include "sim/transport.hpp"

namespace lr::sim {

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
    // AL-73: the player's own order that cannot be done as asked (what it
    // needs, no room for what it gives) is refused at once, with the reason -
    // not tried again for minutes while the person stands still.
    if (c.ai.goal.source == "player" && (why.rfind("gear:", 0) == 0 || why.rfind("pre:", 0) == 0)) {
        dropGoal(w, c, why, true);
        return;
    }
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
    // AL-73: the player's own act, done the one way it can be done: another
    // try comes to the same - refused now, with the reason
    if (ai.goal.source == "player" && method.rfind("direct/", 0) == 0) {
        dropGoal(w, c, why, true);
        return;
    }
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
        // A moving target (a person, a herd): follow it, stop at the reach of
        // the act that comes next; give up after a long chase.
        if (Character* tc = w.character(f.target); tc && tc->id != c.id) {
            const Plan& pl = ai.plan;
            const InteractionDef* nd = pl.actionIndex + 1 < pl.actions.size()
                                           ? w.content().interaction(pl.actions[pl.actionIndex + 1].interaction)
                                           : nullptr;
            // AL-108: up close for an act done by hand, a throw away for a hunt
            const double reach = nd ? approachOf(w, *nd) : cfg.contactM;
            const double dd = dist(c.pos, tc->pos);
            if (!tc->body.alive) {
                replanStep(w, c, "fsm.lost:target_gone");
                return false;
            }
            if (dd <= reach) {
                trace(w, c, 'F', "fsm.arrived", {{"target", f.target}});
                c.ai.fsm = FsmState{};
                ai.plan.actionIndex++;
                return true;
            }
            if (w.now() - f.started > 48) {
                replanStep(w, c, "fsm.lost:lost_track");
                return false;
            }
            f.dest = {tc->pos.x + (c.pos.x - tc->pos.x) * reach / dd, tc->pos.y + (c.pos.y - tc->pos.y) * reach / dd};
            f.timer = std::max<Tick>(f.timer, 2);
        }
        const double d = dist(c.pos, f.dest);
        // AL-115: mud, ice and steep ground slow the step; the walk takes the ticks it needs
        // LR-159: a long walk on one's own beast; riding or driving, the step is the mount's / the team's
        if (w.now() - f.started <= 1) autoRideStart(w, c, f.dest);
        const double step = cfg.walkMPerTick * stepMoveFactor(w, c.pos, f.dest, cfg.walkMPerTick) * transportSpeedMult(w, c);
        if (d <= step || (f.timer <= 1 && step >= cfg.walkMPerTick)) {
            c.pos = f.dest;
        } else {
            c.pos.x += (f.dest.x - c.pos.x) * step / d;
            c.pos.y += (f.dest.y - c.pos.y) * step / d;
            if (f.timer <= 1) f.timer = 2;
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
    Character* creature = (d->targetsCreature() || d->targetsCharacter()) ? w.character(f.target) : nullptr;
    // AL-108: a person or a herd that moves off during the act is followed
    // (one keeps by the goats being milked); a ranged act keeps its range.
    if (creature && creature->body.alive && !isRanged(w, *d) && creature->id != c.id) {
        const double dd = dist(c.pos, creature->pos);
        const double keep = approachOf(w, *d);
        if (dd > workReachOf(w, *d) && dd > 1e-9) {
            const double go = std::min(dd - keep, cfg.walkMPerTick * stepMoveFactor(w, c.pos, creature->pos, cfg.walkMPerTick));
            c.pos.x += (creature->pos.x - c.pos.x) * go / dd;
            c.pos.y += (creature->pos.y - c.pos.y) * go / dd;
            f.dest = creature->pos;
        }
    }
    --f.timer;
    if (d->continuous) {
        applyEffects(w, c, *d, target, creature);
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
    // Game moves: a group that bolted out of reach during the act means a
    // miss (on_fail), not a lost plan.
    const bool bolted = creature && creature->isAnimal() && creature->body.alive &&
                        dist(creature->pos, c.pos) > reachOf(w, *d);
    if (!canPerform(w, c, *d, target, &why, !(creature && creature->isAnimal()), creature)) {
        replanStep(w, c, "fsm.lost:" + why);
        return false;
    }
    if (bolted) {
        applyEffectList(w, c, d->onFail, nullptr, creature);
        trace(w, c, 'F', "fsm.done", {{"action", d->id}});
        finishAction(w, c);
        return true;
    }
    applyEffects(w, c, *d, target, creature);
    tradeOnAct(w, c, *d, creature); // AL-75: a trade act makes its offer when done
    if (!d->craft.empty()) {
        const Plan& p = ai.plan;
        craftAct(w, c, *d, target, creature, p.stepIndex < p.steps.size() ? p.steps[p.stepIndex].params : Json::object());
    }
    if (d->id == "start_dialog" && creature && !creature->isAnimal()) {
        const Plan& p = ai.plan;
        const Json params = p.stepIndex < p.steps.size() ? p.steps[p.stepIndex].params : Json::object();
        runConversation(w, c, *creature, params);
    }
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
        EntityId t = kNone;
        // AL-108: a goto put before an act that found itself out of reach
        // leads to that act's own target
        const WorldObject* pinned = next.target != kNone ? w.object(next.target) : nullptr;
        const Character* pinnedC = next.target != kNone ? w.character(next.target) : nullptr;
        if (pinned && d->targetsObject() && pinned->type == d->targetType && !pinned->reservedFor(c.id, w.now()))
            t = next.target;
        else if (pinnedC && d->targetsCreature() && pinnedC->body.alive && pinnedC->species == d->targetType)
            t = next.target;
        else if (d->targetsCharacter()) {
            const Character* pc = w.character(next.target);
            t = pc && pc->body.alive ? next.target : findPerson(w, c, *d, c.ai.goal.target, &fw, c.ai.goal.source == "player");
        } else {
            // the player picked this very thing or herd (AL-23): no nearer stand-in
            t = pickedTarget(w, c, *d);
            if (t == kNone) t = findTarget(w, c, *d, c.pos, &fw);
        }
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
        if (o && d->reserve) {
            o->reservedBy = c.id;
            o->reservedUntil = w.now() + cfg.reserveTicks;
        }
        Vec2 tp;
        targetPos(w, t, tp);
        // Stop at the act's reach (a hunter stays a spear throw away; AL-108:
        // one pets the goats, talks to a neighbour, from a step away).
        const double reach = approachOf(w, *d, o);
        const double dd = dist(c.pos, tp);
        if (dd <= workReachOf(w, *d, o)) {
            p.actionIndex++;
            return Start::Instant;
        }
        if ((d->targetsCreature() || d->targetsCharacter()) && dd > reach) {
            tp.x = c.pos.x + (tp.x - c.pos.x) * (dd - reach) / dd;
            tp.y = c.pos.y + (tp.y - c.pos.y) * (dd - reach) / dd;
        }
        // AL-78: a thing is worked from beside it, on the side one comes from
        // (at the well's rim, by the trunk), not from its middle
        if (o && !d->targetsCreature()) {
            const auto od = w.content().objects.find(o->type);
            const double stand = od != w.content().objects.end() ? od->second.standM : 0.0;
            if (stand > 0.0 && dd > stand) {
                tp.x = tp.x + (c.pos.x - tp.x) * stand / dd;
                tp.y = tp.y + (c.pos.y - tp.y) * stand / dd;
            }
        }
        const double walk = dist(c.pos, tp);
        ai.fsm = FsmState{"Move", "goto", t, tp, static_cast<Tick>(std::ceil(walk / cfg.walkMPerTick)), w.now()};
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
    Character* creature = nullptr;
    if (d->targetsCharacter()) {
        if (!w.character(a.target)) {
            std::string fw;
            a.target = findPerson(w, c, *d, c.ai.goal.target, &fw, c.ai.goal.source == "player");
        }
        creature = w.character(a.target); // the other person (same slot as a creature target)
        // AL-40: an act with a social chain asks the other, who answers with
        // an act of their own; the pair plays it out (social_chain.cpp).
        if (creature && !creature->isAnimal() && socialChain(w.content(), d->id)) {
            if (!canPerform(w, c, *d, nullptr, &why, true, creature)) return Start::Failed;
            return beginSocial(w, c, *creature, *d, why) ? Start::Started : Start::Failed;
        }
        if (creature && !consents(w, c, *creature, *d)) {
            w.remember(c, MemoryEntry{0, "was_refused", {creature->id}, kNone, -0.2, 0.0, "seen", 1.0});
            why = "refused:" + d->id;
            // AL-36: the "no" is said aloud, with the reason the other holds
            if (!creature->isAnimal()) {
                const SaidLine line = sayRefusal(w, *creature, c, "consent", "interaction." + d->id + ".name");
                if (c.controller.isPlayer()) {
                    Json o = {{"action", d->id}, {"success", false}, {"refused", true}, {"by", creature->id}};
                    if (line.ok()) o["said"] = line.key;
                    trace(w, c, 'S', "act.outcome", o);
                }
            }
            return Start::Failed;
        }
    } else if (d->targetsObject() || d->targetsCreature()) {
        if (a.target == kNone || (d->targetsCreature() && !w.character(a.target))) {
            std::string fw;
            a.target = pickedTarget(w, c, *d);
            if (a.target == kNone) a.target = findTarget(w, c, *d, c.pos, &fw);
        }
        if (d->targetsObject()) target = w.object(a.target);
        else creature = w.character(a.target);
    }
    if (!canPerform(w, c, *d, target, &why, true, creature)) return Start::Failed;
    if (d->reserve && target) {
        target->reservedBy = c.id;
        target->reservedUntil = w.now() + d->duration + 2;
    }
    const EntityId tid = target ? target->id : (creature ? creature->id : kNone);
    ai.fsm = FsmState{d->fsm, d->id, tid, target ? target->pos : (creature ? creature->pos : c.pos), d->duration, w.now()};
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
    bool healed = false;
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
        Start s = startAction(w, c, why);
        // AL-108: an act whose target is out of reach (the plan thought it near,
        // the person stepped off) walks there first instead of failing.
        if (s == Start::Failed && why.rfind("out_of_range:", 0) == 0 && !healed && p.actionIndex < p.actions.size() &&
            p.actions[p.actionIndex].interaction != "goto" && p.actions[p.actionIndex].target != kNone) {
            healed = true;
            const PlanAction act = p.actions[p.actionIndex];
            p.actions.insert(p.actions.begin() + static_cast<std::ptrdiff_t>(p.actionIndex),
                             PlanAction{"goto", act.target, act.targetType, 0.0});
            trace(w, c, 'F', "fsm.approach", {{"action", act.interaction}, {"target", act.target}});
            why.clear();
            s = startAction(w, c, why);
        }
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
    if (c.body.downUntil > w.now()) return; // knocked out or carried off after a fight
    if (undeadHeld(w, c)) return;           // AL-118: a mindless body follows its master or the living (undeadTick)
    if (firefighting(w, c)) return;          // AL-115: the village's fire comes first
    AiState& ai = c.ai;
    // Expire modifiers and stale queued tasks.
    ai.mods.erase(std::remove_if(ai.mods.begin(), ai.mods.end(),
                                 [&](const WeightMod& m) { return w.now() >= m.created + m.ttl; }),
                  ai.mods.end());
    ai.queue.erase(std::remove_if(ai.queue.begin(), ai.queue.end(),
                                  [&](const QueuedTask& q) { return q.ttl > 0 && w.now() >= q.created + q.ttl; }),
                   ai.queue.end());
    // AL-40: a social chain holds the body (the actor drives the pair, the
    // target and a follower wait on it); its end hands the act back to the plan.
    if (c.ai.social.active()) {
        std::string why;
        const bool actor = c.ai.social.role == "actor";
        // AL-73: one talked to over their work hears it and works on
        const bool aside = c.ai.social.role == "target" && c.ai.social.aside;
        const SocialStep st = tickSocial(w, c, why);
        if (st == SocialStep::Running && !aside) return;
        if (actor && st == SocialStep::Done && ai.goal.valid()) finishAction(w, c);
        if (actor && st == SocialStep::Failed && ai.goal.valid()) {
            if (why.rfind("refused:", 0) == 0 && ai.goal.source == "player") dropGoal(w, c, why, true); // asked once
            else replanStep(w, c, why);
        }
        if (actor) return; // the next act starts at the next tick
    }
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

void faceAll(World& w) {
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal()) {   // a herd looks the way it goes
            const Vec2 was = c.seenPos;
            c.seenPos = c.pos;
            const double d = dist(c.pos, was);
            if (d > 1e-3) c.facing = {(c.pos.x - was.x) / d, (c.pos.y - was.y) / d};
            continue;
        }
        const Vec2 was = c.seenPos;
        c.seenPos = c.pos;
        if (!c.body.alive) continue;
        const FsmState& f = c.ai.fsm;
        Vec2 to;
        bool have = false;
        // at work: toward what one works at, or the one one talks to
        const bool acting = f.state != "Idle" && f.state != "Move" && f.state != "Wait" && f.interaction != "goto";
        if (acting && f.target != kNone && f.target != c.id) {
            have = targetPos(w, f.target, to);
            // one keeps by a person or a herd that moved after one's own turn this tick
            const InteractionDef* d = w.content().interaction(f.interaction);
            const Character* t = w.character(f.target);
            if (d && t && t->body.alive && !isRanged(w, *d) && !c.captivity.held()) {
                const double dd = dist(c.pos, t->pos), keep = approachOf(w, *d);
                if (dd > workReachOf(w, *d) && dd > keep) {
                    const double walk = w.content().config.walkMPerTick;
                    const double go = std::min(dd - keep, walk * stepMoveFactor(w, c.pos, t->pos, walk));
                    c.pos.x += (t->pos.x - c.pos.x) * go / dd;
                    c.pos.y += (t->pos.y - c.pos.y) * go / dd;
                    c.seenPos = c.pos;
                }
            }
        }
        // walking: the way one goes
        if (!have && dist(c.pos, was) > 1e-3) {
            to = {c.pos.x + (c.pos.x - was.x), c.pos.y + (c.pos.y - was.y)};
            have = true;
        }
        if (!have && f.state == "Move" && dist(f.dest, c.pos) > 1e-3) {
            to = f.dest;
            have = true;
        }
        if (!have) continue;
        const double d = dist(to, c.pos);
        if (d > 1e-3) c.facing = {(to.x - c.pos.x) / d, (to.y - c.pos.y) / d};
    }
}

void installAi(World& w) {
    installConsequences(w);
    installVillageLife(w);
    w.setThinkHook([](World& world) {
        // Animals may be born while the loop runs (animalsDaily adds groups),
        // so the daily pass runs before the per-tick loop.
        if (world.now() % kTicksPerDay == 0 && world.now() > 0) {
            animalsDaily(world);
            spoilFood(world);
            cropsDaily(world);
            villageDaily(world); // AL-39: courtship, births, illness, quarrels, strangers
            realmDaily(world);   // AL-85: the region beyond the map: politics, wars, wanderers, quests
            magicDaily(world);   // AL-116: the gift, healers, the undead, paths out of grief
            societyDaily(world); // AL-118: laws and witch trials, guilds, magic at work
            cultureDaily(world); // LR-155: bonds to culture, piety and conversion, the settlements' traditions
            budgetDaily(world);  // AL-83: each person keeps what his share of the memory budget allows
        }
        fieldsTick(world);  // AL-115: the cells' fields, fire, the integrity of things
        healWounds(world); // wounds in the profile heal in the ordinary ticks
        buildingTick(world);  // finished sites leave, a dead chief's mandate passes on
        captivityTick(world); // captives follow, escape, meet their fate; a dead player's heir takes over
        undeadTick(world);    // AL-118: mindless undead follow their master, the unbound go for the living
        tradeTick(world);     // AL-75: roles, the tavern's hungry customers, treats among friends
        dressTick(world);     // AL-109: people put on what they carry and can wear
        AnimalTick tick;
        for (const auto& [id, c] : world.characters()) {
            if (!c.body.alive) continue;
            if (!c.isAnimal()) tick.people.push_back(&c);
            else if (const SpeciesDef* sp = world.content().speciesOf(c.species); sp && sp->diet == "prey")
                tick.predators.push_back(&c);
        }
        for (auto& [id, c] : world.characters()) {
            if (c.isAnimal()) animalThink(world, c, tick);
            else thinkCharacter(world, c);
        }
        transportTick(world); // LR-159: mounts, teams and vehicles go with their rider / driver
        faceAll(world); // AL-108
    });
    installTransport(w); // LR-159: before control, it takes the transport.* performs
    installControl(w);
    installSocial(w);
    installBuilding(w);
    installCrafting(w);
    installGear(w);
    installTrade(w);
    installRealm(w);
    installWorldMap(w);   // LR-153: the world map (fealty at the start, battlefields)
    installFields(w);
    installMagic(w);
    installMagicSociety(w);
    installCulture(w);
}

} // namespace lr::sim
