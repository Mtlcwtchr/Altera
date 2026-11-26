#pragma once
// LR-152: stamina, the strength for actions right now (Kingdom Come's stamina,
// kept for every act, not only for blows). gdd: /mnt/project-files/gdd/combat/COMBAT_STATE_LR152.md.
//
// Every act spends it by the second (content/ai/stamina.json: costs by act id,
// then category, then FSM state), running spends it fast, the combat core spends
// it per blow; it comes back within seconds while one does nothing hard. The cap
// is a formula of the content (formulas/stamina.json, stamina_max): endurance up,
// fatigue, lack of sleep, hunger, thirst, wounds and blood loss down. Spent to the
// bottom, a person's work stalls (half the ticks do not count) and the player
// cannot run. Units are the combat core's (combat_config stamina.max = 100), so
// a fight starts from what is left and leaves what the fight left.
//
// Time: the world's ticks for everyone (tick_seconds of play per tick); a body the
// player walks by hand runs in the real seconds the client sends with steer.

#include <map>
#include <string>

#include "sim/core.hpp"

namespace lr::sim {

struct Character;
struct Content;
struct InteractionDef;
class World;

struct StaminaRules {
    std::string maxFormula = "stamina_max", regenFormula = "stamina_regen";
    double fallbackMax = 100.0, fallbackRegen = 1.0; // without the formulas
    double tickSeconds = 600.0;
    double spentBelow = 0.08;   // share of the cap: below it the body is spent
    double stallShare = 0.5;    // share of ticks a spent body's act does not advance
    double defaultCost = 0.0;   // per second, an act none of the maps names
    std::map<std::string, double> byAct, byCategory, byFsm; // per second (negative = rest helps)
    struct Move {
        double walkPerS = 0.3, runPerS = 7.0;   // per (real, for the player) second
        double walkRegenMult = 0.6;             // regen while walking
        double runMinShare = 0.15;              // to start a run again after being spent
    } move;
    bool enabled = true;
};

// The rules of the content (cached per data generation); null without stamina.json.
const StaminaRules* staminaRules(const Content& c);

// The cap and the regen per second of a person (the formulas, with fallbacks).
double staminaCap(const Content& c, const Character& ch);
double staminaRegen(const Content& c, const Character& ch);
// Seconds-cost of an act (per second; negative = a rest that helps the regen).
double staminaCost(const StaminaRules& r, const InteractionDef* d, const std::string& fsmState);

// The world's step: caps, regen and the cost of what each person does.
void staminaTick(World& w);
// The player's hand walk (steer): `metres` walked in `dt` real seconds, running or not.
// Returns false when a run was asked of a spent body (it walked instead).
bool staminaSteer(World& w, Character& c, double metres, double dt, bool run);
// A spent body: this tick its act may not advance (deterministic, stallShare of ticks);
// staminaTick marks such a tick in Body::stalledAt, which the FSM reads.
bool staminaStalls(const World& w, const Character& c);
bool staminaSpent(const Content& cont, const Character& c);
// Running is allowed (not spent; after being spent, back above runMinShare).
bool staminaCanRun(const Content& cont, const Character& c);

} // namespace lr::sim
