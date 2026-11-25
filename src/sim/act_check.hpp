#pragma once
// LR-141: an act's check in data (gdd/stats/STATS_SKILLS_v0.3.md §6).
//
//   "check": {
//     "factors": { "hold": {"attr.charisma": 0.5, "attr.voice": 0.25, ...},   // weighted mean
//                  "tale": "0.55*skill.eloquence + 0.25*attr.creativity" },    // or an expression
//     "quality": "sqrt(fx.hold * fx.tale)",       // default: the mean of the factors
//     "spread":  "0.2 - 0.12*skill.eloquence",     // sd of the noise per target (default 0.15)
//     "audience": {"radius_m": 8, "max": 12},      // absent: the act's target character
//     "need":    "f.listener_need",                // per target, t. = that target (default 0.5)
//     "outcomes": [ {"id": "spellbound", "over": 0.25, "eff": ["t.need:fun+0.35", ...]},
//                   ..., {"id": "bored", "else": true, "eff": [...]} ] }
//
// Names: the person's (sim/char_expr.hpp) with t. for the target, fx.<factor>,
// f.<formula>. The statistical LOD asks actCheckOdds (p_over, no roll).

#include <string>
#include <utility>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

struct ActCheck;
struct Character;
struct Content;
struct InteractionDef;
struct ValidationReport;
struct WorldObject;
class World;

ActCheck parseActCheck(const Json& j, const std::string& where, std::vector<std::string>& errors);

struct ActCheckRoll {
    double quality = 0.0;
    std::vector<std::pair<std::string, double>> factors;
    std::vector<std::pair<EntityId, std::string>> outcomes; // target -> outcome id
    int succeeded = 0;                                      // targets with margin >= 0
};

// The actor's factors and quality (`target` is read as t. when given).
ActCheckRoll actCheckQuality(const World& w, const Character& actor, const InteractionDef& d, const Character* target);
// Rolls the check for every target and applies the outcomes' effects. Returns true when
// at least one target got a margin >= 0 (or the act has no targets).
bool resolveActCheck(World& w, Character& actor, const InteractionDef& d, WorldObject* target, Character* targetChar,
                     const std::vector<EntityId>* witnesses, ActCheckRoll* out = nullptr);
// The probability of each outcome against `target`, without a roll (statistical LOD, AI estimates).
std::vector<std::pair<std::string, double>> actCheckOdds(const World& w, const Character& actor, const InteractionDef& d,
                                                         const Character& target);
// Who the act's audience is (awake people around the actor), by the check's radius.
std::vector<EntityId> actAudience(const World& w, const Character& actor, const InteractionDef& d);

void validateActChecks(const Content& c, ValidationReport& r);

} // namespace lr::sim
