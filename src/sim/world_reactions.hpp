#pragma once
// AL-115 (LIVELYREALMS_SETTING_v0.2 §5.6, §6.3, M4): how the elements meet.
// Not a damage table but reactions in the world, all data
// (content/magic/reactions.json):
//  - pairs: what two aspects of one spell make together (Fire+Water = steam,
//    Life+Death cancel...), as extra world_channels on top of each aspect's own;
//    every pair of aspects has an entry or an explicit "none" (validator);
//  - cells: rules over a cell's fields that run every tick or day whoever
//    made the state - a fireball on a wet meadow and a hot spring alike make
//    steam; cold water is ice; soaked ground is mud and walls on it lose their
//    footing; heat with nothing to burn melts the ground; taint in water runs
//    along it and sickens who drinks.
// The ecology reads the same fields: beasts run from fire, heat and poison,
// flames take a standing crop, a growth spell ripens it and drains the soil.

#include <array>
#include <map>
#include <string>
#include <vector>

#include "sim/world_fields.hpp"

namespace lr::sim {

struct Character;

struct PairReaction {
    std::string result, fx;
    Json channels = Json::object();
    bool cancel = false;
    double minPower = 0.0;
};
struct CellRule {
    std::string id, state, fx;
    // when (absent = any): degrees C, bytes, flags (-1 any, 0 no, 1 yes)
    double heatCMin = -1e9, heatCMax = 1e9;
    int heatMin = -1, moistureMin = -1, moistureMax = 256, taintMin = -1, fuelMax = 256;
    int water = -1, burnt = -1;
    std::array<int, kFieldCount> doTick{}, doDay{};
    int pieceDamagePerDay = 0;
    double spreadWithWater = 0.0;
};
struct ReactionsConfig {
    bool loaded = false;
    std::vector<std::string> aspects;           // ids of aspects.json, in its order
    std::map<std::string, Json> channels;      // aspect -> world_channels
    std::map<std::string, PairReaction> pairs; // "a+b" (sorted); "none" pairs are not stored
    std::vector<std::string> nonePairs;
    std::vector<CellRule> cells;
    std::map<std::string, double> moveFactor;  // cell state -> walking speed factor
    static ReactionsConfig fromJson(const Json& reactions, const Json& aspects);
    static std::string key(const std::string& a, const std::string& b);
    const PairReaction* pair(const std::string& a, const std::string& b) const;
    // pairs of aspects with neither a reaction nor an explicit "none"; unknown aspects in the matrix
    std::vector<std::string> problems() const;
};
const ReactionsConfig& reactionsConfig(const World& w);

// The state of a cell by the first cell rule that fits ("" = nothing special).
std::string cellStateAt(const World& w, Vec2 at);
// Walking speed factor of a cell: its state (mud, ice, molten: reactions.json move_factor)
// and the slope of the ground (Earth~); 1 = as fast as anywhere.
double moveFactorAt(const World& w, Vec2 at);
// The same for a step of `step` metres from `from` toward `to`: time-weighted along
// the way (only cells something wrote count; untouched ground is the norm).
double stepMoveFactor(const World& w, Vec2 from, Vec2 to, double step);
// Is it dangerous to stand here (flames, heat near burning, strong taint)?
// If so, `away` is the nearest safer place within lookM (upwind first).
bool fieldDanger(const World& w, Vec2 at, double lookM, Vec2* away);
// Does a person's household draw water where the water is tainted?
bool drinksTaintedWater(const World& w, const Character& c);

// A spell's aspects landing on the world: each aspect's world_channels for its
// vector, then the reactions of every pair among them (a cancelling pair drops
// both). Returns what reacted and the channel keys left for other systems.
struct WorldSpellPart {
    std::string aspect;
    std::string vec = "+";   // + | - | ~
};
struct SpellWorldResult {
    std::vector<std::string> reactions, unhandled;
    bool cancelled = false;
};
SpellWorldResult applySpellToWorld(World& w, Vec2 at, double radiusM, const std::vector<WorldSpellPart>& parts, double power);
// The ecology's channels applyChannels leaves: growth (crops ripen, the soil pays), lure (beasts).
// Returns the keys it did not take either.
std::vector<std::string> applyLifeChannels(World& w, Vec2 at, double radiusM, const Json& channels, double power,
                                           const std::vector<std::string>& keys);

// Every tick from fieldsTick (daily: true once a day): the cell rules, crops in flames,
// people stepping out of fire.
void reactionsTick(World& w, bool daily);

} // namespace lr::sim
