// After a fight (COMBAT_DESIGN §7-8, §17; CORE_SPEC §6, §11):
// - the winner decides over a knocked-out or yielding loser: spare, kill or
//   bind, by its own motivation (content/combat/aftermath.json);
// - a defeated player does not die: someone rescues them off-screen, chosen
//   by weights from the surroundings (content/combat/rescue.json); the player
//   loses time, things and wakes up in a safe place with the wounds tended.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/rules.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;
struct CombatBridge;

struct FinishTerm {
    std::string kind;   // trait | rel | act_lethal | loser | winner | winner_predator | kin | loser_state | witnesses_each | right | loser_verdict
    std::string key;    // trait id, rel axis, person|animal, ko|yield, right id
    bool flag = true;   // act_lethal / winner_predator / kin / loser_verdict
    double add = 0.0, mul = 0.0, cap = -1e9;
};

struct FinishOption {
    std::string id;
    double base = 0.0;
    std::vector<FinishTerm> terms;
    std::vector<Effect> eff;
    bool kills = false, playerLoser = true, personWinner = false;
    std::string deathCause = "combat", deathCauseAnimalWinner = "beast";
    std::string memWinner, memLoser;
    double valWinner = 0.0, valLoser = 0.0;
    std::array<double, 2> koDownHours{0.0, 0.0};
};

struct AftermathRules {
    std::vector<std::string> loserStates;
    double noise = 0.0;
    std::vector<FinishOption> options;
    std::string chronicle = "chronicle.combat_finish";
};

struct RescuerDef {
    std::string id, loc, who, wake, treatment;
    double base = 0.0, perKin = 0.0, perHolder = 0.0, nearHomeM = 0.0, nearAdd = 0.0;
    int kinMax = 0, holderMax = 0;
    std::string skill;
    double skillMin = 0.0;
    std::array<double, 2> skipHours{0.0, 0.0};
    double lossShare = 0.0;
};

struct RescueRules {
    std::vector<RescuerDef> rescuers;
    double takesPerson = 0.0, takesAnimal = 0.0, healthFloor = 0.1, bloodFloor = 30.0;
    std::string memRescued, memRescuer;
    double valRescued = 0.0, valRescuer = 0.0, rescuerAffinity = 0.0;
    std::string chronicle = "chronicle.rescue";
};

AftermathRules loadAftermath(const Json& j, const Content& c, std::vector<std::string>& err);
RescueRules loadRescue(const Json& j, const Content& c, std::vector<std::string>& err);

// Scores of the options for this winner and loser (tests, trace); the
// deterministic noise is not included.
std::vector<double> finishScores(const World& w, const AftermathRules& r, const Character& winner, const Character& loser,
                                 const InteractionDef& d, const std::string& loserState, std::size_t witnesses);

// The winner's decision, applied. Returns the option id ("" = no decision:
// the loser is dead, fled or still fighting). A player loser who is not bound
// is rescued (rescuePlayer).
std::string finishFight(World& w, Character& winner, Character& loser, const InteractionDef& d,
                        const std::string& loserState, const std::vector<EntityId>& witnesses);

// Off-screen rescue of a defeated player. Returns the rescuer id.
std::string rescuePlayer(World& w, Character& player, Character* winner);

} // namespace lr::sim
