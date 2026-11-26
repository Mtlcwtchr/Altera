// LR-152: stamina for every act (content/ai/stamina.json, formulas/stamina.json): the cap
// falls with fatigue, lack of sleep, hunger; work spends it and a spent body stalls; the
// player's run spends it by real seconds and a winded body walks; a fight starts from what
// is left and leaves what the fight left; a client watching gets the fight's rounds.

#include <cmath>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/combat_show.hpp"
#include "sim/stamina.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World testWorld() {
    return makeTestWorld(real(), loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
}

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) out.push_back(&c);
    return out;
}

void rested(Character& c) {
    c.body.fatigue = 0.0;
    c.body.health = 1.0;
    c.body.blood = 100.0;
    for (const char* n : {"energy", "hunger", "thirst"}) c.needs[n] = 1.0;
}

} // namespace

TEST(lr_stamina_rules_and_cap_formula) {
    const StaminaRules* r = staminaRules(real());
    CHECK(r != nullptr);
    CHECK(real().formulas.has("stamina_max") && real().formulas.has("stamina_regen"));
    World w = testWorld();
    Character& c = *people(w)[0];
    rested(c);
    const double full = staminaCap(real(), c);
    CHECK(full > 40.0 && full < 200.0);
    c.body.fatigue = 0.8;
    const double tired = staminaCap(real(), c);
    CHECK(tired < full * 0.8);
    rested(c);
    c.needs["energy"] = 0.0;   // no sleep
    CHECK(staminaCap(real(), c) < full * 0.8);
    rested(c);
    c.needs["hunger"] = 0.0;   // starving
    CHECK(staminaCap(real(), c) < full * 0.8);
    rested(c);
    c.body.health = 0.3;       // wounded
    CHECK(staminaCap(real(), c) < full * 0.7);
    // costs: by act id first, then category, then the FSM state
    CHECK(staminaCost(*r, real().interaction("chop_tree"), "Work") > staminaCost(*r, real().interaction("sew_tunic"), "Work"));
    CHECK_EQ(staminaCost(*r, nullptr, "Idle"), 0.0);
    CHECK(staminaCost(*r, nullptr, "Sleep") < 0.0);
}

TEST(lr_stamina_hard_work_spends_rest_restores) {
    World w = testWorld();
    Character& c = *people(w)[0];
    rested(c);
    staminaTick(w);
    const double cap = c.body.staminaMax;
    CHECK(cap > 0 && std::abs(c.body.stamina - cap) < 1e-9);
    // a hard act every tick: the stamina goes down, then the body is spent and stalls part of the ticks
    c.ai.fsm = FsmState{"Work", "chop_tree", kNone, c.pos, 1000, w.now()};
    int stalled = 0;
    double low = cap;
    for (int i = 0; i < 40; ++i) {
        // the cap formula reads the needs: keep them up so only the act counts
        rested(c);
        staminaTick(w);
        low = std::min(low, c.body.stamina);
        if (c.body.stalledAt == w.now()) ++stalled;
        w.step();
    }
    const double regen = staminaRegen(real(), c);
    const StaminaRules& r = *staminaRules(real());
    if (staminaCost(r, real().interaction("chop_tree"), "Work") > regen) {
        CHECK(low < cap * 0.5);
        CHECK(stalled > 0);
    }
    // idle: back within a tick
    c.ai.fsm = FsmState{};
    c.body.stamina = 0.0;
    staminaTick(w);
    CHECK(c.body.stamina > cap * 0.9);
}

TEST(lr_stamina_run_spends_winded_walks) {
    World w = testWorld();
    Character& c = *people(w)[0];
    rested(c);
    staminaTick(w);
    const double cap = c.body.staminaMax;
    int ran = 0;
    for (int i = 0; i < 400 && !c.body.winded; ++i) ran += staminaSteer(w, c, 0.75, 0.1, true) ? 1 : 0;
    CHECK(c.body.winded);
    CHECK(c.body.stamina <= cap * staminaRules(real())->spentBelow + 1e-9);
    CHECK(ran > 20);                                   // a sprint of some seconds
    CHECK(!staminaCanRun(real(), c));
    CHECK(!staminaSteer(w, c, 0.4, 0.1, true));        // asked to run, walked
    // standing breathes it back in real seconds
    for (int i = 0; i < 300 && c.body.winded; ++i) staminaSteer(w, c, 0.0, 0.1, false);
    CHECK(!c.body.winded);
    CHECK(staminaCanRun(real(), c));
}

TEST(lr_stamina_fight_starts_from_what_is_left) {
    World w = testWorld();
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    rested(a);
    rested(b);
    staminaTick(w);
    const CombatBridge& br = *combatBridge(real());
    a.body.stamina = 5.0;
    const auto spec = fighterOf(br, a, "fighting");
    CHECK(std::abs(spec.stamina - 5.0) < 1e-9);
    CHECK(std::abs(spec.staminaCap - a.body.staminaMax) < 1e-9);
    const InteractionDef& duel = *real().interaction("duel");
    const CombatResolution r = resolveCombat(w, a, duel, b, nullptr, "full");
    CHECK(r.ran);
    CHECK(r.actorStamina >= 0.0 && r.actorStamina <= a.body.staminaMax + 1e-9);
    CHECK(r.targetStamina >= 0.0 && r.targetStamina <= b.body.staminaMax + 1e-9);
    CHECK(r.eventLines.empty());   // nobody records
}

TEST(lr_stamina_fights_recorded_for_the_client) {
    World w = testWorld();
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    a.inventory["spear"] = 1;
    a.controller.kind = Controller::Kind::Player;   // a fight the player is in plays in full (not the LOD table)
    w.client.recordFights = true;
    staminaTick(w);
    const InteractionDef& brawl = *real().interaction("brawl");
    applyEffects(w, a, brawl, nullptr, &b);
    CHECK(!w.client.fights.empty());
    const Json& f = w.client.fights.back();
    CHECK_EQ(f["a"].get<EntityId>(), a.id);
    CHECK_EQ(f["b"].get<EntityId>(), b.id);
    CHECK(f["ev"].is_array() && !f["ev"].empty());
    bool attack = false;
    for (const Json& e : f["ev"]) attack |= e.value("k", std::string()) == "attack";
    CHECK(attack);
    CHECK(f["round_ms"].get<double>() > 0);
    const std::uint64_t id = f["id"].get<std::uint64_t>();
    CHECK_EQ(fightsSince(w, id - 1).size(), size_t(1));
    CHECK(fightsSince(w, id).empty());
    CHECK(fightsSince(w, 0, a.pos.x + 5000, a.pos.y, 10).empty());   // far from the view
}
