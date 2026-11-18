// Controllers (CORE_SPEC §2-3). A Character is one type for the player and for
// NPCs; the controller only decides where layer 1's goal comes from. Layers
// 2-4 (HTN, GOAP, FSM) and the interaction catalog are the same for both.
//
// Switching, as in The Sims: the released character is picked up by its
// AiController in the same tick without losing its goal or plan (a player goal
// becomes an ordinary Utility goal with a hold bonus for a while, player hints
// become decaying weights); the newly taken character's autonomous plan is
// paused, not deleted, and resumes when the AI gets it back - or, with free
// will on, when the player leaves it without orders.

#include <algorithm>
#include <stdexcept>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

int playerOf(const Command& cmd) {
    if (cmd.args.contains("player")) return cmd.args["player"].get<int>();
    if (cmd.source.rfind("player:", 0) == 0) return std::stoi(cmd.source.substr(7));
    return 0;
}

Character* controlledBy(World& w, int player) {
    for (auto& [id, c] : w.characters())
        if (c.controller.isPlayer() && c.controller.playerId == player) return &c;
    return nullptr;
}

void resumeSuspended(World& w, Character& c, const char* code) {
    AiState& ai = c.ai;
    if (!ai.suspended) return;
    ai.suspended = false;
    if (!ai.suspendedGoal.valid()) return;
    ai.goal = ai.suspendedGoal;
    ai.plan = ai.suspendedPlan;
    ai.fsm = FsmState{};
    ai.suspendedGoal = GoalInstance{};
    ai.suspendedPlan = Plan{};
    trace(w, c, 'C', code, {{"goal", ai.goal.id}, {"methods", ai.plan.methods}, {"step", ai.plan.stepIndex},
                            {"action", ai.plan.actionIndex}});
}

} // namespace

// The released character: AI takes over now, continuing what was going on.
void handToAi(World& w, Character& c) {
    AiState& ai = c.ai;
    const AiConfig& cfg = w.content().config;
    c.controller.kind = Controller::Kind::Ai;
    c.controller.since = w.now();
    if (ai.goal.valid() && ai.goal.source == "player") {
        if (w.content().goal(ai.goal.id)) {
            // An ordinary Utility goal now, held for a while by a bonus.
            ai.goal.source = "utility";
            ai.mods.push_back(WeightMod{"goal:" + ai.goal.id, cfg.playerGoalHoldBonus, w.now(), cfg.playerGoalHoldTicks,
                                        "linear", "player_goal_handover", "control"});
        } else {
            // A direct order (perform / player task): it stays as a queued task
            // the AI weighs like any other.
            ai.queue.push_back(QueuedTask{ai.goal.task, ai.goal.target, 0.6 + cfg.playerGoalHoldBonus, w.now(),
                                          cfg.playerGoalHoldTicks, "player_goal_handover", "control"});
            ai.goal.id = "queued";
            ai.goal.source = "queued";
        }
    }
    // Player hints turn into decaying Utility considerations.
    for (auto& m : ai.mods)
        if (m.source == "player") {
            const Tick left = std::max<Tick>(0, m.created + m.ttl - w.now());
            m.source = "player_hint";
            m.delta = m.at(w.now());
            m.created = w.now();
            m.ttl = std::min<Tick>(left, static_cast<Tick>(cfg.hintDecayTicks));
            m.decay = "linear";
        }
    if (ai.goal.valid()) {
        if (ai.suspended) {
            trace(w, c, 'C', "control.suspended_dropped", {{"goal", ai.suspendedGoal.id}});
            ai.suspended = false;
            ai.suspendedGoal = GoalInstance{};
            ai.suspendedPlan = Plan{};
        }
    } else {
        resumeSuspended(w, c, "control.resumed");
    }
    ai.nextThink = w.now();
    trace(w, c, 'C', "control.to_ai", {{"goal", ai.goal.id}, {"step", ai.plan.stepIndex}});
}

// The taken character: autonomous plan paused, the player sees it.
void handToPlayer(World& w, Character& c, int player) {
    AiState& ai = c.ai;
    c.controller.kind = Controller::Kind::Player;
    c.controller.playerId = player;
    c.controller.since = w.now();
    c.controller.idleSince = w.now();
    if (ai.goal.valid() && ai.goal.source != "player") {
        abortAction(w, c, "control.paused");
        releaseReservations(w, c);
        ai.suspended = true;
        ai.suspendedGoal = ai.goal;
        ai.suspendedPlan = ai.plan;
        ai.goal = GoalInstance{};
        ai.plan = Plan{};
        trace(w, c, 'C', "control.paused",
              {{"goal", ai.suspendedGoal.id}, {"methods", ai.suspendedPlan.methods}, {"step", ai.suspendedPlan.stepIndex}});
    }
    trace(w, c, 'C', "control.to_player", {{"player", player}});
}

bool switchControl(World& w, int player, EntityId to, std::string& why) {
    Character* next = w.character(to);
    if (!next) return why = "no_such_character", false;
    if (!next->body.alive) return why = "dead", false;
    if (next->controller.isPlayer() && next->controller.playerId != player) return why = "controlled_by_other", false;
    Character* prev = controlledBy(w, player);
    if (prev == next) return true;
    // Atomic within the command: release first, then take.
    if (prev) handToAi(w, *prev);
    handToPlayer(w, *next, player);
    return true;
}

void playerThink(World& w, Character& c) {
    AiState& ai = c.ai;
    const AiConfig& cfg = w.content().config;
    if (ai.goal.valid()) return; // the player's order (or its queued task) runs
    // Player-queued tasks, in order.
    // A copy: adopting (or failing) a goal edits the queue.
    const std::vector<QueuedTask> queue = ai.queue;
    for (const auto& q : queue) {
        if (q.source != "player") continue;
        GoalScore gs;
        gs.goal = "queued";
        gs.task = q.task;
        gs.target = q.target;
        gs.score = q.priority;
        if (adoptGoal(w, c, gs, "queued")) return;
    }
    if (!c.controller.freeWill) return;
    if (w.now() - c.controller.idleSince < cfg.playerIdleResumeTicks) return;
    // Free will: left without orders, the character goes back to its own life.
    if (ai.suspended) {
        resumeSuspended(w, c, "control.free_will_resume");
        return;
    }
    aiThink(w, c);
}

namespace {

bool requireControl(World& w, const Command& cmd, Character*& c) {
    c = w.character(cmd.args.at("id").get<EntityId>());
    if (!c) throw std::runtime_error("no such character");
    const int p = playerOf(cmd);
    if (!c->controller.isPlayer() || c->controller.playerId != p) throw std::runtime_error("not controlled by player");
    return true;
}

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    const AiConfig& cfg = w.content().config;
    if (t == "take_control") {
        std::string why;
        if (!switchControl(w, playerOf(cmd), cmd.args.at("id").get<EntityId>(), why)) cmd.result = "rejected:" + why;
        return true;
    }
    if (t == "release_control") {
        if (Character* c = controlledBy(w, playerOf(cmd))) handToAi(w, *c);
        else cmd.result = "rejected:nothing_controlled";
        return true;
    }
    if (t == "set_goal" || t == "perform" || t == "queue_task") {
        Character* c = nullptr;
        requireControl(w, cmd, c);
        GoalScore gs;
        gs.score = cmd.args.value("priority", 0.7);
        gs.target = cmd.args.value("target", kNone);
        std::string why;
        if (t == "set_goal") {
            const GoalDef* g = w.content().goal(cmd.args.at("goal").get<std::string>());
            if (!g) throw std::runtime_error("unknown goal");
            if (!anyMethodFeasible(w, *c, g->task, &why)) {
                cmd.result = "rejected:cannot:" + why; // "не может": shown to the player with the reason
                trace(w, *c, 'C', "control.order_refused", {{"goal", g->id}, {"why", why}});
                return true;
            }
            gs.goal = g->id;
            gs.task = g->task;
        } else if (t == "perform") {
            const std::string x = cmd.args.at("interaction").get<std::string>();
            const InteractionDef* d = w.content().interaction(x);
            if (!d) throw std::runtime_error("unknown interaction");
            gs.goal = "player_action";
            gs.task = x;
        } else {
            QueuedTask q{cmd.args.at("task").get<std::string>(), gs.target, gs.score, w.now(),
                         std::min<Tick>(cmd.args.value("ttl", cfg.modMaxTtl), cfg.modMaxTtl),
                         cmd.args.value("reason", std::string("player_order")), "player"};
            if (!w.content().task(q.task) && !w.content().interaction(q.task))
                throw std::runtime_error("unknown task");
            c->ai.queue.push_back(q);
            trace(w, *c, 'C', "control.queued", {{"task", q.task}});
            return true;
        }
        if (!adoptGoal(w, *c, gs, "player")) cmd.result = "rejected:plan_failed:" + c->ai.lastFailReason;
        else trace(w, *c, 'C', "control.order", {{"goal", gs.goal}, {"task", gs.task}});
        c->controller.idleSince = w.now();
        return true;
    }
    if (t == "hint") {
        // "Хочу / не хочу" (Sims): a bounded, decaying weight - never an order.
        Character* c = w.character(cmd.args.at("id").get<EntityId>());
        if (!c) throw std::runtime_error("no such character");
        const double delta = std::clamp(cmd.args.at("delta").get<double>(), -cfg.modMaxAbsDelta, cfg.modMaxAbsDelta);
        const Tick ttl = std::clamp<Tick>(cmd.args.value("ttl", cfg.modMaxTtl), 1, cfg.modMaxTtl);
        c->ai.mods.push_back(WeightMod{"goal:" + cmd.args.at("goal").get<std::string>(), delta, w.now(), ttl, "linear",
                                       cmd.args.value("reason", std::string("player_hint")), "player"});
        trace(w, *c, 'C', "control.hint", {{"goal", cmd.args["goal"]}, {"delta", delta}, {"ttl", ttl}});
        return true;
    }
    if (t == "confirm_plan") {
        Character* c = nullptr;
        requireControl(w, cmd, c);
        if (c->ai.goal.valid() || !c->ai.suspended) cmd.result = "rejected:no_paused_plan";
        else resumeSuspended(w, *c, "control.confirmed");
        return true;
    }
    if (t == "cancel_goal") {
        Character* c = nullptr;
        requireControl(w, cmd, c);
        dropGoal(w, *c, "cancelled_by_player", false);
        c->controller.idleSince = w.now();
        return true;
    }
    if (t == "set_free_will") {
        Character* c = w.character(cmd.args.at("id").get<EntityId>());
        if (!c) throw std::runtime_error("no such character");
        c->controller.freeWill = cmd.args.at("on").get<bool>();
        return true;
    }
    return false;
}

} // namespace

void installControl(World& w) { w.addCommandHandler(handle); }

} // namespace altera::sim
