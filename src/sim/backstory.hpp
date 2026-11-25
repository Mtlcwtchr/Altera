#pragma once
// Backstories (content/ai/backstories.json): who the player was before the
// game. At a new game the player picks one; it reshapes the person the player
// starts as (skills, traits, attributes, belongings, purse, recipes, where they
// stand, rights, repute and how the village sees them) and hands them to the
// player. Deterministic: everything rolled comes from World::rng.

#include <string>

#include "sim/world.hpp"

namespace lr::sim {

// The person a new game starts as: the head of a household who is not the
// chief (lowest id), else any living adult. kNone when there is nobody.
EntityId backstoryCandidate(const World& w);

// Applies `b` to `c`; false + why when it cannot (dead, an animal, a child).
bool applyBackstory(World& w, Character& c, const BackstoryDef& b, std::string& why);

// The command "start_backstory" {backstory, id?, player}: applies the
// backstory to `id` (default: backstoryCandidate) and gives the player control.
// false when `cmd` is not that command; sets cmd.result to rejected:<why> on failure.
bool handleBackstoryCommand(World& w, Command& cmd, int player);

} // namespace lr::sim
