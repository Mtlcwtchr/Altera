// Controllers (CORE_SPEC §2-3). A Character is one type for the player and for
// NPCs; the controller only decides where layer 1's goal comes from. Layers
// 2-4 (HTN, GOAP, FSM) and the interaction catalog are the same for both.
//
// Switching, as in The Sims: the released character is picked up by its
// AiController in the same tick without losing its goal or plan (a player goal
// becomes an ordinary Utility goal with a hold bonus for a while, player hints
// become decaying weights); the newly taken character's autonomous plan is
// paused, not deleted, and resumes when the AI gets it back - or, with free
// will on, when the player leaves it without orders. LR-157: free will is off
// unless the player turns it on: the led person lives by itself only once the
// player lets go of it ("release_control", the Release button).

#include <algorithm>
#include <stdexcept>

#include "sim/stamina.hpp"
#include "sim/charcreate.hpp"
#include "sim/backstory.hpp"
#include "sim/ai.hpp"
#include "sim/gear.hpp"
#include "sim/social_chain.hpp"
#include "sim/transport.hpp"

namespace lr::sim {

namespace {

constexpr double kSteerMaxStepM = 25.0; // a hard sprint for a few seconds of real time

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

// --- LR-138: one plays one dynasty -------------------------------------------
// As in an RPG, a player leads one house: the person they lead, and when they
// switch, only their own kin (household, dynasty, spouse, parents, children -
// the same circle an heir comes from). The house is the last person the player
// led, kept in the world (module "player_house") so a release (watching, the
// colony mode) does not open the door to anyone. Only when no one of the house
// is left alive (or the player never led anyone) may the player pick freely.
namespace {

constexpr const char* kHouseKey = "player_house";

bool kinOfHouse(const Character& a, const Character& b) {
    if (a.id == b.id) return true;
    if (a.status.family != kNone && a.status.family == b.status.family) return true;
    if (a.status.dynasty != kNone && a.status.dynasty == b.status.dynasty) return true;
    if (a.status.spouse == b.id || b.status.spouse == a.id) return true;
    return a.status.mother == b.id || a.status.father == b.id || b.status.mother == a.id || b.status.father == a.id;
}

const Json* houseOf(const World& w, int player) {
    const Json* h = w.moduleState(kHouseKey);
    if (!h || !h->is_object()) return nullptr;
    const auto it = h->find(std::to_string(player));
    return it == h->end() || !it->is_object() ? nullptr : &*it;
}

void rememberHouse(World& w, int player, const Character& c) {
    Json& h = w.moduleState(kHouseKey);
    if (!h.is_object()) h = Json::object();
    h[std::to_string(player)] = {{"who", c.id}, {"family", c.status.family}, {"dynasty", c.status.dynasty}};
}

// Whether `c` belongs to the house `h` (the anchor person, else its ids).
bool inHouse(const World& w, const Json& h, const Character& c) {
    if (const Character* a = w.character(h.value("who", kNone))) return kinOfHouse(*a, c);
    const EntityId fam = h.value("family", kNone), dyn = h.value("dynasty", kNone);
    return (fam != kNone && c.status.family == fam) || (dyn != kNone && c.status.dynasty == dyn);
}

// LR-160: another player's house holds `c` (each player leads a dynasty of their own).
bool heldByOtherHouse(const World& w, int player, const Character& c) {
    const Json* all = w.moduleState(kHouseKey);
    if (!all || !all->is_object()) return false;
    const std::string mine = std::to_string(player);
    for (const auto& [k, h] : all->items())
        if (k != mine && h.is_object() && !h.value("free", false) && inHouse(w, h, c)) return true;
    return false;
}

} // namespace

bool mayLead(const World& w, int player, const Character& c, std::string* why) {
    auto no = [&](const char* r) {
        if (why) *why = r;
        return false;
    };
    if (!c.body.alive) return no("dead");
    if (c.isAnimal()) return no("animal"); // AL-25: people only
    if (c.controller.isPlayer() && c.controller.playerId != player) return no("controlled_by_other");
    const Json* h = houseOf(w, player);
    if (!(h && !h->value("free", false) && inHouse(w, *h, c)) && heldByOtherHouse(w, player, c)) return no("dynasty_of_other");
    if (!h) {
        // a world from before LR-138: the person the player leads is the house
        for (const auto& [id, o] : w.characters())
            if (o.controller.isPlayer() && o.controller.playerId == player && o.body.alive && !kinOfHouse(o, c))
                return no("not_your_dynasty");
        return true;
    }
    if (h->value("free", false) || inHouse(w, *h, c)) return true;
    // The house died out: the player may go on with anyone (GDD: or end the game).
    for (const auto& [id, o] : w.characters())
        if (o.body.alive && !o.isAnimal() && inHouse(w, *h, o)) return no("not_your_dynasty");
    return true;
}

void forgetHouse(World& w, int player) {
    Json& h = w.moduleState(kHouseKey);
    if (!h.is_object()) h = Json::object();
    h[std::to_string(player)] = {{"free", true}}; // the next one led is the house
}

bool switchControl(World& w, int player, EntityId to, std::string& why) {
    Character* next = w.character(to);
    if (!next) return why = "no_such_character", false;
    if (!mayLead(w, player, *next, &why)) return false;
    Character* prev = controlledBy(w, player);
    if (prev == next) {
        rememberHouse(w, player, *next);
        return true;
    }
    // Atomic within the command: release first, then take.
    if (prev) handToAi(w, *prev);
    handToPlayer(w, *next, player);
    rememberHouse(w, player, *next);
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
    if (!c.controller.autonomy) return;
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
    if (handleBackstoryCommand(w, cmd, playerOf(cmd))) return true;
    if (handleCreationCommand(w, cmd, playerOf(cmd))) return true; // AL-136
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
        // AL-40: a new order breaks the joint act the player's person is in (a follow stays).
        if (t != "queue_task" && c->ai.social.active() && c->ai.social.role != "follower")
            endSocial(w, *c, "player_order");
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
            std::string x = cmd.args.at("interaction").get<std::string>();
            // LR-123: {"params": {"item": "bread"}} binds a parametric act (the same as id lend#item=bread)
            if (cmd.args.contains("params") && cmd.args["params"].is_object() && x.find('#') == std::string::npos) {
                std::string tail;
                for (const auto& [k, v] : cmd.args["params"].items())
                    if (v.is_string()) tail += (tail.empty() ? "" : ",") + k + "=" + v.get<std::string>();
                if (!tail.empty()) x += "#" + tail;
            }
            const InteractionDef* d = w.content().interaction(x);
            if (d && d->parametric()) {
                cmd.result = "rejected:needs_choice";   // what to lend / give / trade is not said
                return true;
            }
            if (!d) throw std::runtime_error("unknown interaction");
            // The tone the act is done in (tones.json); stays until another order names one.
            if (cmd.args.contains("tone")) c->controller.tone = cmd.args["tone"].get<std::string>();
            // AL-23: the named target must be one the act can be done to - a
            // refusal with the reason now, never a silent stand-in later.
            if (gs.target != kNone) {
                std::string tw;
                if (const std::string bad = orderTargetProblem(w, *c, *d, gs.target, tw); !bad.empty()) {
                    cmd.result = "rejected:" + bad;
                    trace(w, *c, 'C', "control.order_refused", {{"task", x}, {"why", bad}});
                    return true;
                }
                // AL-32: another household's thing or beast - the act is not
                // forbidden, it is done as what it is (theft, trespass).
                if (const InteractionDef* f = foreignVariant(w, *c, *d, gs.target)) d = f;
            }
            gs.goal = "player_action";
            gs.task = d->id;
        } else {
            QueuedTask q{cmd.args.at("task").get<std::string>(), gs.target, gs.score, w.now(),
                         std::min<Tick>(cmd.args.value("ttl", cfg.modMaxTtl), cfg.modMaxTtl),
                         cmd.args.value("reason", std::string("player_order")), "player"};
            if (!w.content().task(q.task) && !w.content().interaction(q.task))
                throw std::runtime_error("unknown task");
            c->ai.queue.push_back(q);
            trace(w, *c, 'C', "control.queued", {{"task", q.task}});
            // LR-138: nothing running - the queued task is taken up now
            if (!c->ai.goal.valid() && !c->ai.social.active() && c->body.alive && c->body.downUntil <= w.now()) {
                playerThink(w, *c);
                if (c->ai.goal.valid()) executePlan(w, *c);
            }
            return true;
        }
        if (!adoptGoal(w, *c, gs, "player")) cmd.result = "rejected:plan_failed:" + c->ai.lastFailReason;
        else {
            trace(w, *c, 'C', "control.order", {{"goal", gs.goal}, {"task", gs.task}});
            // LR-138: the order is set about at once (the first act starts now,
            // the walk goes on from the next tick), not a tick later.
            if (c->body.alive && c->body.downUntil <= w.now() && !c->ai.social.active()) executePlan(w, *c);
        }
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
    // AL-80: a task the player queued, taken back (the quick bars' queue, RMB)
    if (t == "unqueue") {
        Character* c = nullptr;
        requireControl(w, cmd, c);
        const int i = cmd.args.value("index", -1);
        if (i < 0 || i >= int(c->ai.queue.size())) cmd.result = "rejected:no_such_task";
        else c->ai.queue.erase(c->ai.queue.begin() + i);
        return true;
    }
    if (t == "cancel_goal") {
        Character* c = nullptr;
        requireControl(w, cmd, c);
        if (c->ai.social.active()) endSocial(w, *c, "cancelled_by_player");
        dropGoal(w, *c, "cancelled_by_player", false);
        c->controller.idleSince = w.now();
        return true;
    }
    if (t == "steer") {
        // Direct control (WASD in the game): the client walks the player's
        // person in real time and sends where it is now. A running order or
        // plan gives way, as when the player clicks elsewhere in The Sims, and
        // free will waits as after any order.
        Character* c = nullptr;
        requireControl(w, cmd, c);
        if (!c->body.alive) return cmd.result = "rejected:dead", true;
        if (c->body.downUntil > w.now()) return cmd.result = "rejected:down", true;
        if (c->captivity.held()) return cmd.result = "rejected:captive", true;
        if (c->ai.fsm.state == "Fight") return cmd.result = "rejected:fighting", true;
        if (c->ai.goal.valid()) dropGoal(w, *c, "steered_by_player", false);
        // AL-40: walking off breaks a joint act; following someone the player may walk freely.
        if (c->ai.social.active() && c->ai.social.role != "follower") endSocial(w, *c, "steered_by_player");
        const double s = w.map().sizeM;
        const Vec2 was = c->pos;
        Vec2 to{std::clamp(cmd.args.at("x").get<double>(), 0.0, s), std::clamp(cmd.args.at("y").get<double>(), 0.0, s)};
        // AL-25: a step, not a teleport. The client sends steer every few
        // frames; anything farther than a run in that time is cut to it.
        const double step = dist(was, to);
        const double maxStep = kSteerMaxStepM * std::max(1.0, transportSpeedMult(w, *c)); // LR-159: a horse goes farther
        if (step > maxStep) {
            to = {was.x + (to.x - was.x) * maxStep / step, was.y + (to.y - was.y) * maxStep / step};
            trace(w, *c, 'C', "control.steer_clamped", {{"asked_m", step}});
        }
        c->pos = to;
        c->controller.idleSince = w.now();
        // LR-152: walking costs a little stamina, running much; a spent body cannot run
        const bool ran = staminaSteer(w, *c, dist(was, c->pos), cmd.args.value("dt", 0.1),
                                      cmd.args.value("run", false));
        pullFollowers(w, *c); // AL-43: whoever was invited along keeps a step behind
        // Walking tires, running more, uphill and under a load more still (gear.json).
        if (const GearRules* R = gearRules(w.content())) {
            const double km = std::min(dist(was, c->pos), 60.0) / 1000.0;
            const double climb = std::max(0.0, cmd.args.value("climb", 0.0));
            const double tire = (km * (cmd.args.value("run", false) && ran ? R->runPerKm : R->walkPerKm) +
                                 std::min(climb, 30.0) / 100.0 * R->climbPer100m) *
                                loadFatigueMult(*R, *c) * transportTireMult(w, *c); // LR-159: the beast does the walking
            c->body.fatigue = std::clamp(c->body.fatigue + tire, 0.0, 1.0);
        }
        return true;
    }
    if (t == "set_free_will") {
        Character* c = w.character(cmd.args.at("id").get<EntityId>());
        if (!c) throw std::runtime_error("no such character");
        c->controller.autonomy = cmd.args.at("on").get<bool>();
        // `now`: the player has been idle long enough already (the game counts
        // its own seconds): free will takes over at the next think.
        if (c->controller.autonomy && cmd.args.value("now", false)) {
            c->controller.idleSince = w.now() - cfg.playerIdleResumeTicks;
            c->ai.nextThink = w.now();
        }
        return true;
    }
    return false;
}

} // namespace

void installControl(World& w) { w.addCommandHandler(handle); }

} // namespace lr::sim
