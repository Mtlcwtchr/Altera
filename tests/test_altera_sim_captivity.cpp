// Group fights (group.json), the captive's fate and executions
// (captivity.json), the dynasty's heir, and the tone of an act (tones.json).

#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/captivity.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/group_combat.hpp"
#include "sim/tones.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World testWorld() {
    World w = makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    installAi(w);
    return w;
}

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) out.push_back(&c);
    return out;
}

Character* anyPack(World& w) {
    for (auto& [id, c] : w.characters())
        if (c.isAnimal() && c.body.alive && c.species == "wolf") return &c;
    return nullptr;
}

bool chronicled(const World& w, const std::string& code) {
    for (const auto& e : w.chronicleLog())
        if (e.code == code) return true;
    return false;
}

} // namespace

TEST(altera_sim_group_rules_load) {
    const CombatBridge* b = nullptr;
    try {
        b = combatBridge(real());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bridge: %s\n", e.what());
        if (auto* de = dynamic_cast<const altera::combat::DataError*>(&e))
            for (const auto& m : de->errors) std::fprintf(stderr, "  %s\n", m.c_str());
        throw;
    }
    CHECK(b != nullptr);
    CHECK(b->group.enabled);
    CHECK(b->captivity.loaded);
    CHECK(toneRules(real()) != nullptr);
    CHECK_EQ(toneRules(real())->order.size(), std::size_t(7));
}

TEST(altera_sim_group_fight_pack_heads_and_helpers) {
    World w = testWorld();
    auto ps = people(w);
    CHECK(ps.size() >= 3);
    Character* wolf = anyPack(w);
    if (!wolf) {
        // No wolves in the fixture: make a pack of five.
        Character c;
        c.name = "pack";
        c.species = "wolf";
        c.group = 5;
        c.pos = ps[0]->pos;
        wolf = &w.addCharacter(c);
    }
    ps = people(w);
    Character& man = *ps[0];
    wolf->group = 5;
    wolf->pos = Vec2{man.pos.x + 3.0, man.pos.y};
    const InteractionDef& attack = *real().interaction("attack");
    // Five heads against one man: a group fight; his kin within reach stand with him.
    Character* kin = nullptr;
    for (Character* c : ps)
        if (c != &man && c->status.family == man.status.family && c->body.stage() != "child") kin = c;
    if (kin) kin->pos = Vec2{man.pos.x + 1.0, man.pos.y + 1.0};
    const Sides s = gatherSides(w, *wolf, attack, man);
    CHECK(s.group());
    CHECK_EQ(s.headsA, 5);
    if (kin) CHECK(std::find(s.b.begin(), s.b.end(), kin) != s.b.end());
    const GroupResolution r = resolveGroupCombat(w, s, attack);
    CHECK(r.ran);
    CHECK_EQ(int(r.fighters.size()), s.headsA + s.headsB);
    const GroupResolution r2 = resolveGroupCombat(w, s, attack);
    CHECK_EQ(r.outcome, r2.outcome); // deterministic
    CHECK_EQ(r.rounds, r2.rounds);
    const int before = wolf->group;
    applyGroupResult(w, s, attack, r, nullptr, {});
    CHECK(chronicled(w, "chronicle.combat_group"));
    CHECK(wolf->group <= before);
    CHECK_EQ(wolf->group, before - (r.fallenA - [&] {
                 int ko = 0;
                 for (const auto& f : r.fighters)
                     if (f.side == 0 && f.state == "ko") ++ko;
                 return ko;
             }()));
    // The wheel's chance of a group fight is a share of drawn battles.
    const double p = winChance(w, man, attack, *wolf);
    CHECK(p >= 0.0 && p <= 1.0);
}

TEST(altera_sim_win_share_follows_skill) {
    const CombatBridge& b = *combatBridge(real());
    altera::combat::FighterSpec strong, weak;
    strong.skill = 80;
    strong.weapon = "axe";
    strong.armor.clear();
    weak.skill = 15;
    weak.weapon = "fists";
    weak.armor.clear();
    const double hi = b.lod->winShare(strong, weak), lo = b.lod->winShare(weak, strong);
    CHECK(hi > 0.6);
    CHECK(lo < 0.4);
}

TEST(altera_sim_captive_trial_execution_and_heir) {
    World w = testWorld();
    auto ps = people(w);
    Character& captor = *ps[2];
    Character& p = *ps[0];
    p.controller.kind = Controller::Kind::Player;
    p.controller.playerId = 0;
    takeCaptive(w, captor, p);
    CHECK(p.captivity.held());
    CHECK(p.status.hasPair(captor.id, "captor_captive"));
    CHECK(chronicled(w, "chronicle.captivity.taken"));
    // Led along: the captive follows the captor.
    captor.pos = Vec2{captor.pos.x + 50.0, captor.pos.y};
    w.step();
    CHECK(dist(p.pos, captor.pos) < 30.0 || !p.captivity.held());
    if (!p.captivity.held()) takeCaptive(w, captor, p); // an escape on the first try: take again
    // A trial with a death sentence is a real death, also for a player.
    carryOutVerdict(w, captor, p, "execute");
    CHECK(!p.body.alive);
    CHECK_EQ(p.body.causeOfDeath, std::string("execution"));
    CHECK(chronicled(w, "chronicle.captivity.verdict_execute"));
    w.step(); // the heir takes over
    Character* heir = nullptr;
    for (auto& [id, c] : w.characters())
        if (c.controller.isPlayer()) heir = &c;
    CHECK(heir != nullptr);
    if (heir) {
        CHECK(heir->body.alive);
        CHECK(heir->status.family == p.status.family || heir->status.dynasty == p.status.dynasty);
    }
    CHECK(chronicled(w, "chronicle.succession"));
}

TEST(altera_sim_captive_fate_release_and_escape) {
    World w = testWorld();
    auto ps = people(w);
    Character& captor = *ps[2];
    Character& c = *ps[3];
    takeCaptive(w, captor, c);
    CHECK(decideFate(w, captor, c, "release"));
    CHECK(!c.captivity.held());
    CHECK(!c.status.hasPair(captor.id, "captor_captive"));
    // Ransom only when the family can pay.
    takeCaptive(w, captor, c);
    for (auto& [id, f] : w.families())
        if (id == c.status.family)
            for (EntityId m : f.members)
                if (Character* x = w.character(m)) x->inventory.clear();
    if (const Family* f = w.familyOf(c); f && f->storage != kNone) w.object(f->storage)->stock.clear();
    CHECK(!decideFate(w, captor, c, "ransom"));
    if (const Family* f = w.familyOf(c); f && f->storage != kNone) w.object(f->storage)->stock["axe"] = 3;
    CHECK(decideFate(w, captor, c, "ransom"));
    CHECK(captor.has("axe") >= 2);
    // Left alone long enough, a captive gets away or meets the captor's decision.
    takeCaptive(w, captor, c);
    for (int i = 0; i < 6 * 24 * 4 && c.captivity.held(); ++i) w.step();
    CHECK(!c.captivity.held() || !c.body.alive);
}

TEST(altera_sim_tone_moves_reception_and_effects) {
    World w = testWorld();
    auto ps = people(w);
    Character& a = *ps[0];
    Character& t = *ps[2];
    a.controller.kind = Controller::Kind::Player;
    const ToneRules& R = *toneRules(real());
    const InteractionDef& compliment = *real().interaction("compliment");
    CHECK(receptionRolls(R, compliment));
    t.relations.erase(a.id); // a stranger: the tone is what decides
    t.status.dynasty = kNone;
    const double warm = receptionChance(w, R, a, compliment, t, "warm");
    const double hostile = receptionChance(w, R, a, compliment, t, "hostile");
    CHECK(warm > hostile + 0.1);
    a.controller.tone = "warm";
    CHECK_EQ(actChance(w, a, compliment, &t, "warm").kind, std::string("reception"));
    CHECK(std::abs(actChance(w, a, compliment, &t, "warm").p - warm) < 1e-9);
    // No tone (an NPC): the plain rules, the act is certain.
    a.controller.tone.clear();
    CHECK_EQ(actChance(w, a, compliment, &t, "").kind, std::string("certain"));
    // Hostile tone leaves fear behind whatever the target makes of it.
    a.controller.tone = "hostile";
    const double fear0 = t.relations[a.id].fear;
    applyEffects(w, a, compliment, nullptr, &t);
    CHECK(t.relations[a.id].fear > fear0);
    // Fights show the chance to win.
    CHECK_EQ(actChance(w, a, *real().interaction("brawl"), &t, "warm").kind, std::string("win"));
}
