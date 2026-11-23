// Altera life-sim core: controllers, Sims-style switching, player/NPC equality.

#include <map>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& content() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/ai");
    return c;
}

World aiWorld(std::uint64_t seed = 0) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/test_world.json");
    World w = makeTestWorld(content(), cfg, seed);
    installAi(w);
    return w;
}

Command cmdOf(const std::string& type, Json args, int player = 1) {
    Command c;
    c.source = "player:" + std::to_string(player);
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = player;
    return c;
}

// Two adults from different families.
std::pair<EntityId, EntityId> twoAdults(const World& w) {
    EntityId a = kNone, b = kNone;
    for (const auto& [id, c] : w.characters()) {
        if (c.body.stage() != "adult") continue;
        if (a == kNone) a = id;
        else if (c.status.family != w.character(a)->status.family) {
            b = id;
            break;
        }
    }
    return {a, b};
}

// Runs n ticks; counts position jumps faster than walking (there must be none)
// and the ticks `watch` spends without any goal.
int runSmooth(World& w, Tick n, EntityId watch, int& watchNoGoal) {
    const double maxStep = w.content().config.walkMPerTick + 1e-6;
    int jumps = 0;
    for (Tick t = 0; t < n; ++t) {
        std::map<EntityId, Vec2> before;
        for (const auto& [id, c] : w.characters()) before[id] = c.pos;
        w.step();
        for (const auto& [id, c] : w.characters())
            if (dist(before[id], c.pos) > maxStep) ++jumps;
        if (!w.character(watch)->ai.goal.valid()) ++watchNoGoal;
    }
    return jumps;
}

const std::string kOk = "ok";

} // namespace

TEST(altera_sim_switch_a_b_a) {
    World w = aiWorld();
    w.run(kTicksPerDay + 40);
    const auto [A, B] = twoAdults(w);
    CHECK(A != kNone && B != kNone);

    // 1. Take A: its autonomous plan is paused, not deleted.
    const GoalInstance aAuto = w.character(A)->ai.goal;
    const Plan aAutoPlan = w.character(A)->ai.plan;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}})).result, kOk);
    {
        const Character& a = *w.character(A);
        CHECK(a.controller.isPlayer());
        if (aAuto.valid()) {
            CHECK(a.ai.suspended);
            CHECK_EQ(a.ai.suspendedGoal.id, aAuto.id);
            CHECK(a.ai.suspendedPlan.methods == aAutoPlan.methods);
            CHECK_EQ(a.ai.suspendedPlan.stepIndex, aAutoPlan.stepIndex);
            CHECK(!a.ai.goal.valid());
        }
    }
    // The player gives A a goal; A works on it.
    CHECK_EQ(w.applyNow(cmdOf("set_goal", {{"id", A}, {"goal", "stock_food"}})).result, kOk);
    w.run(8);
    const GoalInstance aGoal = w.character(A)->ai.goal;
    const Plan aPlan = w.character(A)->ai.plan;
    CHECK_EQ(aGoal.id, std::string("stock_food"));
    CHECK_EQ(aGoal.source, std::string("player"));
    const GoalInstance bAuto = w.character(B)->ai.goal;
    const Plan bAutoPlan = w.character(B)->ai.plan;

    // 2. Switch to B: in the same tick A is the AI's, its goal and plan untouched.
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", B}})).result, kOk);
    // Free will off: B's paused plan must wait for the AI, not resume by itself.
    w.applyNow(cmdOf("set_free_will", {{"id", B}, {"on", false}}));
    {
        const Character& a2 = *w.character(A);
        CHECK(!a2.controller.isPlayer());
        CHECK_EQ(a2.ai.goal.id, aGoal.id);
        CHECK(a2.ai.plan.methods == aPlan.methods);
        CHECK_EQ(a2.ai.plan.stepIndex, aPlan.stepIndex);
        CHECK_EQ(a2.ai.plan.actionIndex, aPlan.actionIndex);
        bool hold = false;
        for (const auto& m : a2.ai.mods) hold |= m.target == "goal:stock_food" && m.delta > 0.0;
        CHECK(hold);
        const Character& b2 = *w.character(B);
        CHECK(b2.controller.isPlayer());
        if (bAuto.valid()) {
            CHECK(b2.ai.suspended);
            CHECK_EQ(b2.ai.suspendedGoal.id, bAuto.id);
            CHECK(b2.ai.suspendedPlan.methods == bAutoPlan.methods);
        }
    }
    // A keeps living without a hitch: no jumps, (almost) never without a goal.
    int aNoGoal = 0;
    CHECK_EQ(runSmooth(w, 60, A, aNoGoal), 0);
    CHECK(aNoGoal <= 6);
    {
        const Character& a3 = *w.character(A);
        bool finished = false;
        for (const auto& e : a3.ai.trace)
            finished |= e.code == "goal.done" && e.params.value("goal", "") == "stock_food";
        CHECK(finished || a3.ai.goal.id == "stock_food");
    }

    // 3. Back to A: B's paused plan resumes under the AI from where it stopped.
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}})).result, kOk);
    {
        const Character& b4 = *w.character(B);
        CHECK(!b4.controller.isPlayer());
        if (bAuto.valid()) {
            CHECK(!b4.ai.suspended);
            CHECK_EQ(b4.ai.goal.id, bAuto.id);
            CHECK(b4.ai.plan.methods == bAutoPlan.methods);
            CHECK_EQ(b4.ai.plan.stepIndex, bAutoPlan.stepIndex);
        }
        CHECK(w.character(A)->controller.isPlayer());
    }
    int bNoGoal = 0;
    CHECK_EQ(runSmooth(w, 40, B, bNoGoal), 0);
    CHECK(bNoGoal <= 6);
}

TEST(altera_sim_free_will_resumes_paused_plan) {
    // Taking a character pauses its autonomous plan (it is not deleted); with
    // free will on and no orders for playerIdleResumeTicks, the AI picks the
    // paused plan up again and the character lives on by itself.
    World w = aiWorld();
    w.run(kTicksPerDay / 2);
    const EntityId A = twoAdults(w).first;
    for (int i = 0; i < 64 && !w.character(A)->ai.goal.valid(); ++i) w.run(1);
    CHECK(w.character(A)->ai.goal.valid());
    if (!w.character(A)->ai.goal.valid()) return;
    const std::string pausedGoal = w.character(A)->ai.goal.id;
    const size_t pausedSteps = w.character(A)->ai.plan.steps.size();
    const Tick resume = w.content().config.playerIdleResumeTicks;
    w.applyNow(cmdOf("take_control", {{"id", A}}));
    {
        const Character& a = *w.character(A);
        CHECK(a.controller.isPlayer());
        CHECK(a.ai.suspended);
        CHECK(!a.ai.goal.valid());
        CHECK_EQ(a.ai.suspendedGoal.id, pausedGoal);
        CHECK_EQ(a.ai.suspendedPlan.steps.size(), pausedSteps);
    }
    // Before the idle timeout the paused plan waits for the player.
    w.run(resume - 2);
    CHECK(w.character(A)->ai.suspended);
    CHECK(!w.character(A)->ai.goal.valid());
    // Timeout passed: free will resumes exactly the paused plan.
    w.run(4);
    const Character& a = *w.character(A);
    CHECK(a.controller.isPlayer());
    CHECK(!a.ai.suspended);
    bool resumed = false;
    for (const auto& e : a.ai.trace)
        if (e.code == "control.free_will_resume" && e.params.value("goal", std::string()) == pausedGoal) resumed = true;
    CHECK(resumed);
    // It keeps living by itself (one tick may pass between two goals).
    bool lives = false;
    for (int i = 0; i < 3; ++i) {
        if (w.character(A)->ai.goal.valid()) lives = true;
        w.run(1);
    }
    CHECK(lives);
    // Free will off: once the current goal is cancelled it waits for the player.
    w.applyNow(cmdOf("set_free_will", {{"id", A}, {"on", false}}));
    w.applyNow(cmdOf("cancel_goal", {{"id", A}}));
    for (Tick i = 0; i < resume + 4; ++i) {
        w.run(1);
        CHECK(!w.character(A)->ai.goal.valid());
    }
}

TEST(altera_sim_control_rules) {
    World w = aiWorld();
    const auto [A, B] = twoAdults(w);
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}}, 1)).result, kOk);
    // Another player cannot take a character someone controls.
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}}, 2)).result, std::string("rejected:controlled_by_other"));
    // Orders only for the character you control.
    CHECK(w.applyNow(cmdOf("set_goal", {{"id", B}, {"goal", "eat"}}, 1)).result.rfind("rejected:", 0) == 0);
    // An order the character cannot fulfil is refused, with the reason.
    Character& a = *w.character(A);
    for (auto& [id, o] : w.objects())
        if (o.type == "storage") o.stock.clear();
    a.knownObjects.clear();
    a.inventory.clear();
    CHECK(w.applyNow(cmdOf("set_goal", {{"id", A}, {"goal", "eat"}}, 1)).result.rfind("rejected:cannot", 0) == 0);
    // Hints are bounded.
    w.applyNow(cmdOf("hint", {{"id", B}, {"goal", "sleep"}, {"delta", 9.0}, {"ttl", 100000}}, 1));
    const WeightMod& m = w.character(B)->ai.mods.back();
    CHECK(m.delta <= w.content().config.modMaxAbsDelta + 1e-12);
    CHECK(m.ttl <= w.content().config.modMaxTtl);
}

TEST(altera_sim_steer_moves_the_player_person) {
    World w = aiWorld();
    const auto [A, B] = twoAdults(w);
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}}, 1)).result, kOk);
    CHECK_EQ(w.applyNow(cmdOf("set_goal", {{"id", A}, {"goal", "eat"}}, 1)).result, kOk);
    CHECK(w.character(A)->ai.goal.valid());
    const Vec2 to{w.character(A)->pos.x + 6.0, w.character(A)->pos.y - 4.0};
    // Steering: the order gives way, the person stands where the client walked it.
    CHECK_EQ(w.applyNow(cmdOf("steer", {{"id", A}, {"x", to.x}, {"y", to.y}}, 1)).result, kOk);
    CHECK(!w.character(A)->ai.goal.valid());
    CHECK(dist(w.character(A)->pos, to) < 1e-9);
    // Free will waits as after any order: the person stays put for a while.
    for (int k = 0; k < 2; ++k) w.step();
    CHECK(dist(w.character(A)->pos, to) < 1e-9);
    // Only the person you control; never off the map.
    CHECK(w.applyNow(cmdOf("steer", {{"id", B}, {"x", 1.0}, {"y", 1.0}}, 1)).result.rfind("rejected:", 0) == 0);
    // AL-25: a far point is a step towards it, not a teleport
    const Vec2 before = w.character(A)->pos;
    w.applyNow(cmdOf("steer", {{"id", A}, {"x", -50.0}, {"y", 1e9}}, 1));
    CHECK(dist(before, w.character(A)->pos) <= 25.0 + 1e-6);
    CHECK(w.character(A)->pos.x >= 0.0 && w.character(A)->pos.y <= w.map().sizeM);
}

TEST(altera_sim_player_and_npc_share_the_catalog) {
    // Every interaction: the same availability verdict and the same effects
    // whether the character is driven by its AI or by a player.
    World ai = aiWorld();
    ai.run(30);
    const EntityId X = twoAdults(ai).first;
    World pl(content());
    pl.loadJson(ai.toJson());
    installAi(pl);
    pl.applyNow(cmdOf("take_control", {{"id", X}}));
    CHECK(pl.character(X)->controller.isPlayer());
    CHECK(!ai.character(X)->controller.isPlayer());
    int checked = 0;
    for (const auto& [iid, d] : content().interactions) {
        EntityId tgt[2] = {kNone, kNone};
        World* worlds[2] = {&ai, &pl};
        for (int i = 0; i < 2; ++i) {
            Character& c = *worlds[i]->character(X);
            c.inventory = {{"berries", 4}, {"wood", 2}, {"meal", 1}};
            if (d.targetsObject()) {
                tgt[i] = findTarget(*worlds[i], c, d, c.pos, nullptr);
                if (tgt[i] != kNone) c.pos = worlds[i]->object(tgt[i])->pos;
            }
        }
        CHECK_EQ(tgt[0], tgt[1]);
        Character& ca = *ai.character(X);
        Character& cp = *pl.character(X);
        std::string wa, wp;
        const bool okA = canPerform(ai, ca, d, ai.object(tgt[0]), &wa);
        const bool okP = canPerform(pl, cp, d, pl.object(tgt[1]), &wp);
        CHECK_EQ(okA, okP);
        CHECK_EQ(wa, wp);
        if (!okA) continue;
        applyEffects(ai, ca, d, ai.object(tgt[0]));
        applyEffects(pl, cp, d, pl.object(tgt[1]));
        Json ja = ca, jp = cp;
        for (Json* j : {&ja, &jp}) {
            j->erase("controller");
            j->erase("ai");
        }
        CHECK_EQ(ja.dump(), jp.dump());
        ++checked;
    }
    CHECK(checked >= 8);
    // The player's direct order goes through the same planner and executor.
    Character& p = *pl.character(X);
    p.inventory = {{"meal", 1}};
    p.needs["hunger"] = 0.3;
    CHECK_EQ(pl.applyNow(cmdOf("perform", {{"id", X}, {"interaction", "eat_meal"}})).result, kOk);
    pl.run(4);
    CHECK(pl.character(X)->need("hunger") > 0.5);
    CHECK_EQ(pl.character(X)->has("meal"), 0);
}

TEST(altera_sim_switching_replays_from_the_log) {
    World a = aiWorld();
    const auto [A, B] = twoAdults(a);
    a.run(50);
    a.submit(cmdOf("take_control", {{"id", A}}));
    a.run(10);
    a.applyNow(cmdOf("set_goal", {{"id", A}, {"goal", "stock_food"}}));
    a.run(20);
    a.submit(cmdOf("take_control", {{"id", B}}));
    a.run(30);
    a.applyNow(cmdOf("hint", {{"id", A}, {"goal", "sleep"}, {"delta", 0.3}, {"ttl", 50}}));
    a.submit(cmdOf("take_control", {{"id", A}}));
    a.run(40);
    World b = aiWorld();
    b.replay(a.commandLog());
    b.run(150);
    CHECK_EQ(a.stateHash(), b.stateHash());
}

// AL-80: the quick bars' queue: a task the player queued is taken back by its
// place in the queue; a place that is not there is refused.
TEST(altera_sim_control_unqueue) {
    World w = aiWorld();
    const auto [A, B] = twoAdults(w);
    (void)B;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", A}}, 1)).result, kOk);
    std::vector<std::string> ids;
    for (const auto& [id, d] : content().interactions)
        if (d.target == "self" && ids.size() < 2) ids.push_back(id);
    CHECK_EQ(ids.size(), std::size_t(2));
    const std::size_t before = w.character(A)->ai.queue.size();
    for (const auto& id : ids) w.applyNow(cmdOf("queue_task", {{"id", A}, {"task", id}}));
    CHECK_EQ(w.character(A)->ai.queue.size(), before + 2);
    CHECK_EQ(w.applyNow(cmdOf("unqueue", {{"id", A}, {"index", int(before)}})).result, kOk);
    CHECK_EQ(w.character(A)->ai.queue.size(), before + 1);
    CHECK_EQ(w.character(A)->ai.queue.back().task, ids[1]);
    CHECK(w.applyNow(cmdOf("unqueue", {{"id", A}, {"index", 99}})).result != kOk);
}
