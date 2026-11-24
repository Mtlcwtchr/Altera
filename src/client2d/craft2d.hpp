#pragma once
// AL-97: the workshop of the 2D client (gdd/ui/CRAFT_UI_VARIANTS.md, chosen
// C + B with drag from A and D as the helper; gdd/ui/client_2d/ui_h3 screen 04).
// What the page draws of a person's crafting: the stations round them (C: the
// workshop stands in the world), the processes as a ring round the work piece
// (B), the bag of materials and made things to drag onto the slots (A), the
// draft's forecast (blurred for a novice), the recipe book with its fragments,
// the last outcome, things to repair, and the novice helper by wish (D).
// Display strings are looked up here; the page picks sprites and glyphs by
// ids (content/sprites_2d/craft2d.json, resources.json). Acting goes through
// the sim's own commands craft_attempt / craft_fast / craft_inspect / craft_repair.

#include <string>

#include "client2d/view2d.hpp"
#include "sim/world.hpp"

namespace altera::client2d {

struct WorkshopQuery {
    sim::Json draft = sim::Json::object();   // {process, slots{slot:[refs]}, shape, mode, heat, len}
    std::string wish;                        // the helper's wish id (content/ui/craft.json wishes), "" none
};

// The whole workshop of `who` (null when there is no such living person or no
// crafting): who, name, stations, heat_max, materials, made, processes,
// recipes, forecast (with a draft), last, wishes, candidates (with a wish).
// `ui` is content/ui/craft.json (wishes, helper_max).
sim::Json workshopJson(const sim::World& w, const ViewText& t, const sim::Json& ui, sim::EntityId who,
                       const WorkshopQuery& q);
// The workshop's strings (hud.craft.*, craft.tier.*, craft.outcome.*), once per page.
sim::Json workshopTexts(const ViewText& t);
// A trial kit (the 3D game's craft_kit, for shots and trying the workshop out):
// gathered materials, a few made tools and the starter recipes of some crafts.
void craftKit(sim::World& w, sim::Character& c);

} // namespace altera::client2d
