#pragma once
// LR-152: the fights a game client watches. When World::client.recordFights is on,
// a fight the core plays in full keeps its round events (combat::eventLine), and
// applyCombatResult files it here with who, where, the weapons and the outcome.
// The client plays the rounds back as the combat state (2D: lunges, swings by
// direction, blocks, parries, ripostes, clinches, blood, falls; numbers and timings
// in content/sprites_2d/combat2d.json). Presentation only: the world is not touched
// beyond this list, which is neither saved nor hashed.

#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct CombatResolution;

// Files the fight (no-op unless recording or when there are no events). Keeps the last `keep`.
void recordFight(World& w, const Character& actor, const Character& target, const std::string& act,
                 const CombatResolution& cr, std::size_t keep = 24);
// The fights filed after `sinceId` whose fighters stand within `radius` metres of (x, y)
// (radius <= 0: all): [{id, tick, act, a, b, pos_a, pos_b, weapon_a, weapon_b, outcome,
// a_state, b_state, rounds, round_ms, ev: [event objects]}].
Json fightsSince(const World& w, std::uint64_t sinceId, double x = 0, double y = 0, double radius = 0);

} // namespace lr::sim
