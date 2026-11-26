#pragma once
// LR-170: the myth creatures of the belts (gdd/setting/LIVELYREALMS_SETTING_v0.3 §5.2, §7.1):
// sirens and gorgons of the Middle Sea, the leshy, rusalki and upyri of the woods,
// draugar of the north, albasty of the steppe, mummies of the Nile. They are animals
// in the sim's sense (content/creatures/*.json, the species.json format) with a
// "mythic" block that says where they live and how they behave; there is no
// `if (species == siren)` anywhere, every behaviour is a field any creature may have:
//
//   belts        which belts of the world it belongs to (the culture's `belt`);
//   active       night | day | any: out of its lair only then, the rest of the time at home;
//   sun_harm     heads lost per hour caught by daylight away from the lair;
//   guard_m      falls on anyone who comes this near its lair, hungry or not;
//   lure         a song or a call: a lone person within radius walks toward it unless
//                the will holds (stat), and it strikes once they are near;
//   regrow_days  a lost head grows back (the hydra), unless burned;
//   grows_on_kill the killed one joins the group (the upyr's curse);
//   rise_days    a destroyed undead rises again at its lair, unless laid to rest
//                (died of `laid_by`: fire, a rite);
//   weapon/skill/armor  its body in the combat core (natural weapons and hides);
//   lair         its place on the world map (sim/worldmap) and its raids on settlements.
//
// Shared rules for the bestiary (fallback belts of old cultures, the default belt):
// content/creatures/_bestiary.json.

#include <string>
#include <vector>

#include "sim/content.hpp"

namespace lr::sim {

class World;

void parseMythic(const Json& j, SpeciesDef::Mythic& m);

// The belt a culture lies in: the culture's own "belt" (content/species/cultures),
// else _bestiary.json "belt_of_culture", else "" (unknown).
std::string beltOfCulture(const Content& c, const std::string& culture);
// The belt the local map lies in: the culture of its full settlement(s), else the
// species catalog's map culture, else _bestiary.json "default_belt".
std::string localBelt(const World& w);
// Whether a creature belongs to the belt (no belts = everywhere; empty belt = only
// creatures with no belts).
bool creatureInBelt(const SpeciesDef& s, const std::string& belt);

// Problems in the mythic blocks (unknown weapons/armour are checked by the bridge).
std::vector<std::string> checkBestiary(const Content& c);

// The shown name of a creature kind: loc key species.<id>.
inline std::string creatureLocKey(const std::string& id) { return "species." + id; }

} // namespace lr::sim
