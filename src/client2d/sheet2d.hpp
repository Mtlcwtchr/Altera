#pragma once
// AL-100: the character sheet of the 2D client (gdd/ui/client_2d/ui_h3/UI_H3.md,
// screen 02): everything the five pages show about one person, read from the
// sim and named in the language of the view. The page (tools/game_2d/web/
// character.js) draws it; /api/sheet?id=<id> serves it.
//
// {id, name, alive, cause, sex, age, stage, fam, house, role, standing, life,
//  occ, backstory, pl,
//  overview: {hp, fatigue, blood, mood, moodlets:[{id, name, v, h}], needs:[{id, name, v, crit}],
//             goal, act, plan:[{name, now}], wounds:[{zone, sev, days, bleeding, infected}],
//             injuries:[{zone, kind}], captive},
//  relations: [{id, name, fam, alive, age, sex, kin:[...], pairs:[...], group, axes:{familiarity..},
//               traits:[...], mem:[{t, text, v, src, conf}], act, goal}],
//  things: {worn:[{slot, slotName, layer, layerName, id, name, kg}], carried:[{id, name, n, kg, worth}],
//           slots:[{id, name}], kg, maxKg},
//  skills: {skills:[{id, name, cat, catName, v}], traits:[{id, name, desc}], attrs:[{id, name, desc, v}],
//           knows:[{id, name}], rep:[{id, name, v}], values:[{id, name, v}], renown, purse},
//  chronicle: {mem:[{day, t, text, v, src, conf, who}], log:[{day, t, text}]}}

#include "client2d/view2d.hpp"

namespace lr::client2d {

// null when there is no such character
sim::Json sheetJson(const sim::World& w, const ViewText& t, sim::EntityId id);

// The text of one memory as the rememberer would tell it (memory.<type>.text
// with who/target/object/item filled in); the type itself when there is no key.
std::string memoryText(const sim::World& w, const ViewText& t, const sim::Character& self, const sim::MemoryEntry& m);

} // namespace lr::client2d
