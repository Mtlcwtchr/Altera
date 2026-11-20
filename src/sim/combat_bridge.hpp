// altera_sim <-> combat core (src/sim/combat). An interaction with a `combat`
// block (interactions.json) runs as the FSM state Fight; when the act ends the
// core plays the fight out round by round from a seed of (world seed, the two
// ids, the tick) and the result comes back as an outcome of the actor
// (win/lose/draw/flee), health of both sides, deaths. Data: content/combat next
// to the content/ai directory; without it the bridge is off and the quick form
// (CombatDef.damage) is used. Spec: COMBAT_CORE_SPEC_v0.1.md §6.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sim/combat/combat.hpp"
#include "sim/combat/combat_lod.hpp"
#include "sim/content.hpp"
#include "sim/aftermath.hpp"
#include "sim/captivity.hpp"
#include "sim/group_combat.hpp"
#include "sim/wounds.hpp"

namespace altera::sim {

class World;
struct Character;

struct CombatBridge {
    std::shared_ptr<const combat::CombatData> data;
    double skillScale = 100.0, defaultSkill = 0.2, healthMinAfter = 0.05;
    std::vector<std::string> weaponItems;                 // preference order
    std::map<std::string, std::string> weaponOfItem, armorOfItem;
    std::string animalWeapon = "fangs", animalSkillKey = "fighting";
    double animalDefaultSkill = 0.4;
    std::map<std::string, std::string> memory;            // outcome -> memory type
    std::map<std::string, double> memoryValence;
    std::string chronicle = "chronicle.combat";
    RecoveryRules recovery; // recovery.json: wounds in the profile, healing
    AftermathRules aftermath; // aftermath.json: the winner's decision
    RescueRules rescue;       // rescue.json: a defeated player is rescued, not killed
    GroupRules group;         // group.json: more than two fighters fight at once
    CaptivityRules captivity; // captivity.json: the captive's fate, trials, executions
    // LOD (CORE_SPEC §7): fights far from every player are drawn from a table
    // of outcomes of the same core instead of the round loop.
    bool lodEnabled = false;
    double lodFullRadiusM = 300.0;
    std::shared_ptr<combat::DuelTable> lod;
};

// Loaded once per content directory; nullptr when there is no content/combat.
// Throws combat::DataError on broken combat data.
const CombatBridge* combatBridge(const Content& c);

struct CombatResolution {
    bool ran = false;
    std::string outcome;             // actor's: win | lose | draw | flee
    std::string actorState, targetState;
    double actorHealth = 1.0, targetHealth = 1.0; // multipliers of current health
    bool actorDead = false, targetDead = false;
    int rounds = 0;
    std::vector<std::string> actorWounds, targetWounds; // severity ids (new wounds of this fight)
    std::vector<WoundHit> actorHits, targetHits;        // the same with zone and damage
    double actorBlood = 100.0, targetBlood = 100.0;     // after the fight
    std::string actorWeapon, targetWeapon;
    // weapon condition lost (0..1) and whether it broke: wear for a made weapon
    double actorWeaponWear = 0.0, targetWeaponWear = 0.0;
    bool actorWeaponBroke = false, targetWeaponBroke = false;
    std::string mode = "full"; // full (round loop) | stat (LOD)
};

combat::FighterSpec fighterOf(const CombatBridge& b, const Character& c, const std::string& skillKey);
// Made weapons and armour the character carries (crafting): numbers into the spec.
void applyMadeGear(const CombatBridge& b, const Character& c, combat::FighterSpec& s);
// The made weapon applyMadeGear chose wears by `wear` (1 = broke).
void wearMadeWeapon(Character& c, double wear);

// Full round loop or the statistical table (LOD): full when events are asked
// for, a player fights or stands within lod.full_radius_m, or a side carries
// old wounds; the table otherwise.
bool combatNeedsFullMode(const World& w, const CombatBridge& b, const Character& actor, const Character& target);

// Plays the fight; events go to `events` when given (client, tests).
// mode: "" = by combatNeedsFullMode, "full" or "stat" to force one.
CombatResolution resolveCombat(const World& w, const Character& actor, const InteractionDef& d, const Character& target,
                               combat::EventStream* events = nullptr, const std::string& mode = "");

} // namespace altera::sim
