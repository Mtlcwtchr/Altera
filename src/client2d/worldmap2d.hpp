#pragma once
// LR-153: /api/worldmap - the world map window of the 2D client (M): the land
// (sim/worldmap.hpp), the settlements and what holds them (domains by the liege
// chain), the roads, the wars, the places of interest, open bounties and where
// the viewer is. Everything named in the view's language.
//
// {grid, origin:[x,y], cell_km, kinds:[{id, name}], ground, river,
//  settlements:[{id, name, km, size, size_name, pop, ruler, house, culture, inst, tags:[names],
//                domain, liege, tribute_to, ruined, war, full, here}],
//  domains:[{id, name, title, members:[ids], n}],
//  roads:[[a, b]], wars:[{a, b, score, battles, siege, cause}],
//  pois:[{id, kind, kind_name, name, km, near, day}],
//  quests:[{at, name}], me:{at, km, name}}
// null when the world has no map (no realm or no worldmap.json).

#include "client2d/view2d.hpp"

namespace lr::client2d {

sim::Json worldMapJson(sim::World& w, const ViewText& t, sim::EntityId viewer);

} // namespace lr::client2d
