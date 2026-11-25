// Wounds in the character profile (Kenshi-like, COMBAT_DESIGN §6, CORE_SPEC §6):
// the combat core's wounds stay on the Character after a fight, weaken the
// zone in the next one and heal in the ordinary 10-minute ticks: first aid,
// infection, recovery, permanent injuries. Rules: content/combat/recovery.json,
// chances: wounds.json treatment.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sim/combat/combat.hpp"
#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;

struct TreatmentSource {
    std::string treatment;
    std::string helperSkill, helperScope; // empty skill = no helper needed
    double helperMin = 0.0;
    std::vector<std::string> consumesAny;
};

struct RecoveryRules {
    // carry
    double painShare = 0.5, disableWhileLeft = 0.5, minZoneHpShare = 0.05;
    // heal
    double firstAidHours = 1, infectionCheckHours = 24, feverDays = 3;
    double bleedDeathBloodBelow = 60, bleedDeathMult = 3, infectionDeathRef = 0.35, bloodRegenPerDay = 15;
    int treatMinSeverity = 0;
    double paceBase = 1.0, hungryBelow = 0.3, hungryMult = 0.5, restMult = 1.0;
    std::string hungryNeed = "hunger";
    std::vector<std::string> restFsm;
    bool playerWoundDeath = false;
    std::string deathCause = "combat";
    std::vector<std::string> rank;            // treatment ids, worst first
    std::vector<TreatmentSource> sources;     // best first
    std::vector<std::string> consumeFrom;     // self | home | helper
    std::map<std::string, std::string> injuries; // zone -> injury kind
    std::string memTreated, memTreater, memInjury;
    std::string chronInjury, chronDeath;

    int rankOf(const std::string& treatment) const;
};

// Parses recovery.json against the combat data; errors go to `err`.
RecoveryRules loadRecovery(const Json& j, const combat::CombatData& d, const Content& c, std::vector<std::string>& err);

// One wound a fight left (zone and severity are combat data indices).
struct WoundHit {
    int zone = 0, severity = 0;
    double dmg = 0.0;
};

std::vector<WoundHit> woundHits(const combat::Fighter& f, std::size_t from = 0);

// Old wounds and injuries of `c` applied to a fresh fighter: zone hp, pain,
// disabled zones, blood.
void carryWounds(const combat::CombatData& d, const RecoveryRules& r, const Character& c, combat::Fighter& f);

// Keeps a fight's wounds in the profile. healthDebt = body.health the fight took.
void addCombatWounds(World& w, Character& c, const std::vector<WoundHit>& hits, double bloodAfter, double healthDebt);

// 10-minute tick: first aid, treatment, infection, recovery, injuries, deaths.
void healWounds(World& w);

} // namespace lr::sim
