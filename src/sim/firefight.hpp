#pragma once
// AL-115: people put out a fire that comes near their village. A fire within
// FirefightRule::alarmM of a settlement raises the alarm (the chronicle); up to
// `crew` able grown-ups nearby drop what they do, run to the nearest flames and
// douse them (water from a river, a pond or a well near the fire works best,
// else branches and earth). When nothing burns near any more they go back to
// their plans. Rules: content/world/fields.json "firefight".

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Character;

// Every tick (fieldsTick): the alarm, who comes to fight, who is let go.
void firefightTick(World& w);
// One person's tick at the fire; true while the fire holds them (the mind waits).
bool firefighting(World& w, Character& c);

} // namespace altera::sim
