// Group fights (COMBAT_DESIGN §9, CORE_SPEC §6): when an act with a combat
// block draws more than two fighters (a pack of wolves is several heads, kin
// and friends standing near join a lethal fight, the village turns out
// against beasts), the whole skirmish is resolved at once by the core's
// battle loop instead of a chain of duels, and the losses are laid out per
// person and per head. Rules: content/combat/group.json.
#pragma once

#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/rules.hpp"
#include "sim/wounds.hpp"

namespace altera::sim {

class World;
struct Character;
struct WorldObject;
struct Content;
struct InteractionDef;

struct GroupRules {
    bool enabled = false;
    double joinRadiusM = 30.0;
    int maxSide = 8;          // fighters on one side (heads of a pack count one each)
    int packHeadsMax = 6;
    bool kinJoin = true;
    double friendAffinity = 0.35;
    bool villageVsBeasts = true;
    std::vector<std::string> joinActs; // person-vs-person acts that draw helpers in
    bool beastAttack = true;           // a pack that attacks a person fights through the core
    std::string beastAct = "attack";
    int winSamples = 32;               // battles drawn for the wheel's chance of a group fight
    std::string chronicle = "chronicle.combat_group";
};

GroupRules loadGroupRules(const Json& j, const Content& c, std::vector<std::string>& err);

struct Sides {
    std::vector<Character*> a, b; // a[0] is the actor, b[0] the target
    int headsA = 0, headsB = 0;   // fighters (a pack counts its heads)
    bool group() const { return headsA + headsB > 2; }
};

// Who stands with whom when `actor` starts `d` on `target`.
Sides gatherSides(World& w, Character& actor, const InteractionDef& d, Character& target);

struct GroupFighterResult {
    Character* who = nullptr;
    int head = 0;            // a pack's head index
    int side = 0;            // 0 = actor's, 1 = target's
    std::string state;       // fight | ko | dead | yield | flee
    double health = 1.0;     // multiplier of current health
    double blood = 100.0;
    std::vector<WoundHit> hits;
};

struct GroupResolution {
    bool ran = false;
    std::string outcome; // the actor's side: win | lose | draw | flee
    int rounds = 0, aliveA = 0, aliveB = 0, fallenA = 0, fallenB = 0;
    std::vector<GroupFighterResult> fighters;
};

GroupResolution resolveGroupCombat(const World& w, const Sides& s, const InteractionDef& d, std::uint64_t salt = 0);

// Health, wounds, deaths and lost heads, memory, chronicle, the winner's
// decision over the fallen (aftermath.json) and the rescue of a fallen player.
void applyGroupResult(World& w, const Sides& s, const InteractionDef& d, const GroupResolution& r,
                      WorldObject* target, const std::vector<EntityId>& witnesses);

// The actor's chance to win `d` against `target` as it would be fought now:
// the duel table for one on one, a sample of battles for a group.
double winChance(World& w, Character& actor, const InteractionDef& d, Character& target);

} // namespace altera::sim
