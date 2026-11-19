// altera_sim <-> combat core: an act with a combat block (FSM Fight) is played
// out by src/sim/combat; outcome effects, health, memory and chronicle follow.

#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/aftermath.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace combat = altera::combat;

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

TEST(altera_sim_combat_wounds_stay_in_profile) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    a.inventory["axe"] = 1;
    for (const auto& item : combatBridge(real())->weaponItems) b.inventory.erase(item);
    a.skills["fighting"] = 0.8;
    b.skills["fighting"] = 0.2;
    const InteractionDef& brawl = *real().interaction("brawl");
    for (int i = 0; i < 10 && b.wounds.empty(); ++i) {
        applyEffects(w, a, brawl, nullptr, &b);
        w.step();
    }
    CHECK(!b.wounds.empty());
    const combat::CombatData& cd = *combatBridge(real())->data;
    double debt = 0.0;
    for (const Wound& wd : b.wounds) {
        CHECK(cd.zoneIx.count(wd.zone) == 1);
        CHECK(cd.sevIx.count(wd.severity) == 1);
        CHECK(wd.dmg > 0.0);
        CHECK(wd.daysLeft > 0.0 && wd.daysLeft == wd.daysTotal);
        debt += wd.healthDebt;
    }
    CHECK(debt > 0.0 && debt <= 1.0);
    CHECK(b.body.blood <= cd.bloodMax);
    // The profile survives a save.
    Json j = b;
    Character back = j.get<Character>();
    CHECK_EQ(back.wounds.size(), b.wounds.size());
    CHECK_EQ(back.wounds.front().zone, b.wounds.front().zone);
    // Old wounds weaken the zone in the next fight.
    combat::Engine e(cd, 1);
    combat::Fighter fresh = e.make(fighterOf(*combatBridge(real()), b, "fighting"));
    combat::Fighter hurt = fresh;
    carryWounds(cd, combatBridge(real())->recovery, b, hurt);
    const int z = cd.zone(b.wounds.front().zone);
    CHECK(hurt.hp[z] < fresh.hp[z]);
    CHECK(hurt.pain > fresh.pain);
    // A permanent injury disables its zone.
    Character maimed = b;
    maimed.wounds.clear();
    maimed.injuries.push_back({"leg_l", "limp", 0});
    combat::Fighter lame = fresh;
    carryWounds(cd, combatBridge(real())->recovery, maimed, lame);
    CHECK((lame.disabled & (1u << cd.zone("leg_l"))) != 0);
}

namespace {

Wound freshWound(const World& w, const std::string& zone, const std::string& sev, double dmg) {
    const combat::CombatData& cd = *combatBridge(real())->data;
    Wound wd;
    wd.zone = zone;
    wd.severity = sev;
    wd.dmg = dmg;
    wd.healthDebt = 0.2;
    wd.daysTotal = wd.daysLeft = cd.treat.recoverDays[cd.sev(sev)];
    wd.bleeding = cd.bleedRate[cd.sev(sev)] > 0.0;
    wd.aided = !wd.bleeding;
    wd.since = w.now();
    return wd;
}

void liveDays(World& w, int days) {
    // No minds here (makeTestWorld): keep the bodies fed so only the wounds matter.
    for (int i = 0; i < days * kTicksPerDay; ++i) {
        for (auto& [id, c] : w.characters())
            for (auto& [n, v] : c.needs) v = 1.0;
        w.step();
        healWounds(w);
    }
}

} // namespace

TEST(altera_sim_combat_wounds_heal_in_ticks) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    for (Character* c : {&a, &b}) {
        c->needs["hunger"] = 1.0;
        c->skills["herbalism"] = 0.0;
        c->inventory.erase("bandage");
        c->inventory.erase("herbs");
        c->inventory.erase("dried_herbs");
    }
    if (WorldObject* home = w.object(a.home)) {
        home->stock.erase("bandage");
        home->stock.erase("herbs");
        home->stock.erase("dried_herbs");
    }
    // A cut with a bandage at hand: bandaged within the hour, healed in days, health back.
    a.inventory["bandage"] = 1;
    a.body.health = 0.7;
    a.wounds.push_back(freshWound(w, "arm_l", "cut", 6.0));
    liveDays(w, 1);
    CHECK(a.wounds.empty() || a.wounds.front().treatment == "bandage");
    CHECK_EQ(a.has("bandage"), 0);
    liveDays(w, 25);
    CHECK(a.body.alive);
    CHECK(a.wounds.empty());
    CHECK(a.body.health > 0.85);
    // A healer of the same settlement with herbs treats a deep wound.
    b.status.settlement = a.status.settlement;
    b.skills["herbalism"] = 0.9;
    b.inventory["dried_herbs"] = 2;
    a.body.blood = 100.0;
    a.wounds.push_back(freshWound(w, "leg_r", "deep", 15.0));
    liveDays(w, 1);
    CHECK(!a.wounds.empty());
    CHECK_EQ(a.wounds.front().treatment, std::string("healer"));
    CHECK_EQ(b.has("dried_herbs"), 1);
    CHECK(!a.wounds.front().bleeding);
}

TEST(altera_sim_combat_wounds_kill_npcs_not_the_player) {
    // heal_sim: an untreated deep wound with little blood left bleeds out in
    // about (1 - p_stop) * bleed_death * 3 = 27% of cases; the player never does.
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    int npcDead = 0, playerDead = 0, trials = 0;
    for (int round = 0; round < 40; ++round) {
        for (std::size_t k = 0; k < ps.size(); ++k) {
            Character& c = *ps[k];
            if (!c.body.alive) continue;
            c.inventory.clear();
            c.skills["herbalism"] = 0.0;
            c.controller.kind = k == 0 ? Controller::Kind::Player : Controller::Kind::Ai;
            c.body.blood = 40.0;
            c.wounds.clear();
            c.wounds.push_back(freshWound(w, "torso", "deep", 20.0));
        }
        for (Character* c : ps)
            if (WorldObject* home = w.object(c->home)) home->stock.clear();
        for (int i = 0; i < 2 * (kTicksPerDay / 24); ++i) {
            w.step();
            healWounds(w);
        }
        for (std::size_t k = 0; k < ps.size(); ++k) {
            if (k == 0) playerDead += !ps[k]->body.alive;
            else {
                ++trials;
                npcDead += !ps[k]->body.alive;
            }
        }
        for (Character* c : ps) {
            c->body.alive = true; // reuse the bodies for the next round
            c->body.health = 1.0;
            c->body.causeOfDeath.clear();
        }
    }
    CHECK_EQ(playerDead, 0);
    const double share = static_cast<double>(npcDead) / std::max(1, trials);
    CHECK(share > 0.12 && share < 0.42);
}

TEST(altera_sim_combat_winner_decides_by_motivation) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    const AftermathRules& R = combatBridge(real())->aftermath;
    const InteractionDef& brawl = *real().interaction("brawl");
    const InteractionDef& attack = *real().interaction("attack");
    auto best = [&](const InteractionDef& d, const std::string& st, std::size_t wit) {
        const auto sc = finishScores(w, R, a, b, d, st, wit);
        std::size_t k = 0;
        for (std::size_t i = 1; i < sc.size(); ++i)
            if (sc[i] > sc[k]) k = i;
        return R.options[k].id;
    };
    a.status.family = b.status.family = kNone;
    a.status.dynasty = b.status.dynasty = kNone;
    a.relations[b.id] = Relation{};
    a.traits.clear();
    CHECK_EQ(best(brawl, "ko", 0), std::string("spare")); // a brawl among neighbours
    a.relations[b.id].enmity = 1.0;
    a.traits = {"unscrupulous"};
    CHECK_EQ(best(attack, "ko", 0), std::string("kill")); // a lethal attack on a hated foe
    CHECK(finishScores(w, R, a, b, attack, "ko", 4)[1] < finishScores(w, R, a, b, attack, "ko", 0)[1]); // witnesses deter
    a.relations[b.id].enmity = 0.2;
    a.traits.clear();
    a.status.rights.insert("arrest");
    b.status.verdicts.insert("exile");
    CHECK_EQ(best(attack, "yield", 0), std::string("bind")); // a guard with a sentence to carry out
    a.status.rights.clear();
    b.status.verdicts.clear();
    // Applied: a kill kills, a bind sets the captor pair, a spare leaves memories.
    a.relations[b.id].enmity = 1.0;
    a.traits = {"unscrupulous"};
    CHECK_EQ(finishFight(w, a, b, attack, "ko", {}), std::string("kill"));
    CHECK(!b.body.alive);
    Character& c = *ps[2];
    a.relations[c.id] = Relation{};
    a.traits.clear();
    c.status.family = kNone;
    c.status.dynasty = kNone;
    CHECK_EQ(finishFight(w, a, c, brawl, "ko", {}), std::string("spare"));
    CHECK(c.body.alive && c.body.downUntil > w.now());
    bool spared = false;
    for (const auto& m : c.memory) spared |= m.type == "was_spared";
    CHECK(spared);
    CHECK(finishFight(w, a, c, brawl, "flee", {}).empty()); // nobody to decide over
    // The player as winner chooses by focus.
    a.controller.kind = Controller::Kind::Player;
    a.controller.combatFinish = "bind";
    c.body.downUntil = 0;
    CHECK_EQ(finishFight(w, a, c, brawl, "yield", {}), std::string("bind"));
    CHECK(a.status.hasPair(c.id, "captor_captive"));
}

TEST(altera_sim_combat_player_is_rescued_not_killed) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& p = *ps[0];
    Character& foe = *ps[2]; // another family: ps[1] is the player's kin
    for (Character* c : ps) {
        c->skills["hunting"] = 0.0;
        c->skills["herbalism"] = 0.0;
    }
    p.controller.kind = Controller::Kind::Player;
    foe.relations[p.id].enmity = 1.0;
    foe.traits = {"unscrupulous"};
    foe.status.family = foe.status.dynasty = kNone;
    p.inventory["log"] = 10;
    p.wounds.clear();
    p.wounds.push_back(Wound{"torso", "deep", 20.0, 0.2, 18.0, 18.0, "none", true, false, false, false, w.now(), 0});
    p.pos = Vec2{p.pos.x + 3000.0, p.pos.y};
    const InteractionDef& attack = *real().interaction("attack");
    // A lethal attack by a hated foe would end in "kill": not for the player.
    const std::string decision = finishFight(w, foe, p, attack, "ko", {});
    CHECK(decision == "spare" || decision == "bind");
    CHECK(p.body.alive);
    if (decision == "spare") {
        CHECK(p.body.downUntil > w.now());
        CHECK_EQ(p.ai.fsm.state, std::string("Incapacitated"));
        CHECK(p.has("log") < 10); // the winner took a share
        CHECK(foe.has("log") > 0);
        CHECK(!p.wounds.front().bleeding && p.wounds.front().aided);
        bool rescued = false;
        for (const auto& m : p.memory) rescued |= m.type == "rescued";
        CHECK(rescued);
        bool chron = false;
        for (const auto& e : w.chronicleLog()) chron |= e.code == "chronicle.rescue";
        CHECK(chron);
    }
    // Rescuers are drawn by weight: with kin around, the family is the most common.
    std::map<std::string, int> by;
    for (int i = 0; i < 200; ++i) {
        p.body.downUntil = 0;
        w.step();
        by[rescuePlayer(w, p, nullptr)]++;
    }
    int top = 0;
    std::string topId;
    for (const auto& [id, n] : by)
        if (n > top) {
            top = n;
            topId = id;
        }
    CHECK(by.size() >= 2);
    CHECK_EQ(topId, std::string("family"));
    // Through the act: the player who loses a lethal fight is alive afterwards.
    Character& q = *ps[3];
    q.controller.kind = Controller::Kind::Player;
    q.skills["fighting"] = 0.0;
    foe.skills["fighting"] = 1.0;
    foe.inventory["axe"] = 1;
    for (int i = 0; i < 10; ++i) {
        q.body.health = 1.0;
        q.wounds.clear();
        q.body.downUntil = 0;
        applyEffects(w, foe, attack, nullptr, &q);
        CHECK(q.body.alive);
        w.step();
    }
}

TEST(altera_sim_combat_lod_mode_by_distance_to_player) {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    auto ps = people(w);
    Character& a = *ps[0];
    Character& b = *ps[1];
    const CombatBridge& br = *combatBridge(real());
    for (Character* c : ps) {
        c->wounds.clear();
        c->body.blood = br.data->bloodMax;
        c->controller.kind = Controller::Kind::Ai;
    }
    const InteractionDef& duel = *real().interaction("duel");
    CHECK(!combatNeedsFullMode(w, br, a, b));
    CHECK_EQ(resolveCombat(w, a, duel, b).mode, std::string("stat"));
    Character& p = *ps[2];
    p.controller.kind = Controller::Kind::Player;
    p.pos = a.pos;
    CHECK(combatNeedsFullMode(w, br, a, b)); // the player is watching
    CHECK_EQ(resolveCombat(w, a, duel, b).mode, std::string("full"));
    p.pos = Vec2{a.pos.x + 10.0 * br.lodFullRadiusM, a.pos.y};
    b.pos = a.pos;
    CHECK(!combatNeedsFullMode(w, br, a, b));
    b.wounds.push_back(Wound{"arm_l", "cut", 5.0, 0.05, 6.0, 6.0, "none", false, true, false, false, w.now(), 0});
    CHECK(combatNeedsFullMode(w, br, a, b)); // old wounds need the full loop
}
