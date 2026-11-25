// The tone of an act (content/ai/tones.json): the player picks it on the
// interaction wheel; it moves the act's chance and adds effects to its
// outcome. actChance() is the chance the sim itself rolls with, so the wheel
// shows the real number: the act's chance roll, the target's consent, the
// target's reception of a social act, or the chance to win a fight.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sim/rules.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;

struct ToneDef {
    std::map<std::string, double> chance; // by social meaning, "_" = any other
    std::map<std::string, double> rel;    // * the target's relation axis toward the actor
    std::vector<Effect> eff, onFail;
};

struct ToneRules {
    std::vector<std::string> order;       // the wheel's chips, by index
    std::map<std::string, ToneDef> tones;
    std::map<std::string, double> receptionBase; // by category
    std::map<std::string, double> receptionRel;
    double kinAdd = 0.1, moodMult = 0.15, recMin = 0.03, recMax = 0.97;
    std::vector<std::string> skipMeanings;
    std::vector<Effect> receptionOnFail;
};

// Loaded once per content directory (content/ai/tones.json); nullptr without the file.
const ToneRules* toneRules(const Content& c);

// The tone the actor's act on `target` carries now: the player's pick for a
// player character acting on another person; "" otherwise (plain rules).
std::string toneOf(const World& w, const Character& actor, const Character* target);

struct ActChance {
    double p = 1.0;
    std::string kind = "certain"; // certain | chance | consent | reception | win
};

// The chance of `d` done by `actor` on `target` with `tone` ("" = no tone),
// as the sim rolls it. Fights: the actor's chance to win (sides as they
// would gather now).
ActChance actChance(World& w, Character& actor, const InteractionDef& d, Character* target, const std::string& tone);

// The tone's shift of a chance for this act.
double toneShift(const ToneRules& R, const std::string& tone, const InteractionDef& d, const Character& actor,
                 const Character& target);

// Reception of a social act by its target (acts without chance and consent).
bool receptionRolls(const ToneRules& R, const InteractionDef& d);
double receptionChance(const World& w, const ToneRules& R, const Character& actor, const InteractionDef& d,
                       const Character& target, const std::string& tone);

} // namespace lr::sim
