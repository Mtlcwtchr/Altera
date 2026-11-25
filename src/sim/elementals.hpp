#pragma once
// AL-115 (LIVELYREALMS_SETTING_v0.2 §5.7): elementals - a clot of an aspect
// become a creature (content/creatures/elemental_*.json, moved by the animal
// drivers like any beast) that lives while the cell under it feeds it
// (content/world/elementals.json): a fire elemental in a dry wood grows and
// sets it alight, in the rain it goes out; a water one dries up in the heat.
// Wild ones are born where a field runs strong (a great fire, a flood, a
// storm, a place of power); magic summons them with summonElemental.

#include <string>

#include "sim/core.hpp"

namespace altera::sim {

class World;

// A new elemental of an aspect ("fire", "water", "air", "earth") at a place, its
// strength 0..1 from the spell's power; kNone if the aspect has none or the place starves it at once.
EntityId summonElemental(World& w, const std::string& aspect, Vec2 at, double power);
bool isElemental(const World& w, const std::string& species);
// Every tick (fieldsTick): feeding, writing into the cells, dying; once a day: wild births.
void elementalsTick(World& w, bool daily);

} // namespace altera::sim
