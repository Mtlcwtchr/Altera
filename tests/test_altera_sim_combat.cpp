// altera_sim <-> combat core: an act with a combat block (FSM Fight) is played
// out by src/sim/combat; outcome effects, health, memory and chronicle follow.

#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) out.push_back(&c);
    return out;
}

} // namespace

TEST(altera_sim_combat_bridge_loads) {
    CHECK(combatBridge(real()) != nullptr);
    Content noDir = real();
    noDir.dir.clear();
    CHECK(combatBridge(noDir) == nullptr); // fromJson content: quick form only
    const Content& c = real();
    CHECK(c.fsmInterruptible("Fight") == false);
    for (const char* id : {"brawl", "attack", "defend_other", "duel"}) {
        const InteractionDef* d = c.interaction(id);
        CHECK(d && d->combat.set && d->fsm == "Fight");
    }
}

TEST(altera_sim_combat_resolution_is_deterministic) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    CHECK(ps.size() >= 2);
    Character& a = *ps[0];
    Character& b = *ps[1];
    const InteractionDef& duel = *real().interaction("duel");
    const CombatBridge& br = *combatBridge(real());
    a.inventory["spear"] = 1;
    for (const auto& item : br.weaponItems) b.inventory.erase(item);
    CHECK_EQ(fighterOf(br, a, "fighting").weapon, std::string("spear"));
    CHECK_EQ(fighterOf(br, b, "fighting").weapon, std::string("fists"));
    const CombatResolution r1 = resolveCombat(w, a, duel, b), r2 = resolveCombat(w, a, duel, b);
    CHECK(r1.ran);
    CHECK_EQ(r1.outcome, r2.outcome);
    CHECK_EQ(r1.rounds, r2.rounds);
    CHECK(r1.actorWounds == r2.actorWounds && r1.targetWounds == r2.targetWounds);
    CHECK(r1.outcome == "win" || r1.outcome == "lose" || r1.outcome == "draw" || r1.outcome == "flee");
    CHECK(r1.targetHealth >= 0.0 && r1.targetHealth <= 1.0);
}

TEST(altera_sim_combat_act_applies_outcome) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    a.inventory["spear"] = 1;
    for (const auto& item : combatBridge(real())->weaponItems) b.inventory.erase(item);
    a.skills["fighting"] = 0.9;
    b.skills["fighting"] = 0.1;
    const InteractionDef& brawl = *real().interaction("brawl");
    int wins = 0, fights = 0;
    for (int i = 0; i < 20 && a.body.alive && b.body.alive; ++i) {
        a.body.health = b.body.health = 1.0;
        const std::size_t memA = a.memory.size();
        applyEffects(w, a, brawl, nullptr, &b);
        ++fights;
        bool won = false, remembered = false;
        for (std::size_t k = memA; k < a.memory.size(); ++k) {
            remembered |= a.memory[k].type == "won_fight" || a.memory[k].type == "lost_fight" ||
                          a.memory[k].type == "fought_draw" || a.memory[k].type == "fled_fight";
            won |= a.memory[k].type == "won_fight";
        }
        CHECK(remembered);
        wins += won;
        CHECK(a.body.alive && b.body.alive); // brawl is not lethal
        w.step();
    }
    CHECK(fights == 20);
    CHECK(wins >= 15); // a master with a spear against an unarmed novice
}
