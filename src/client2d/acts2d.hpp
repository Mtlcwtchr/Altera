#pragma once
// AL-96: what the player's person can do, for the 2D client (the act menu on
// a thing, a person or a beast; the quick bar's acts and goals): every act
// checked by the sim (sim/act_info.hpp) and put in the player's words with
// the same loc keys the 3D HUD uses (hud.req.*, hud.why.*, hud.gain.*).
// Read-only: the page orders with the commands perform / set_goal.

#include <string>
#include <vector>

#include "client2d/view2d.hpp"
#include "sim/world.hpp"

namespace lr::client2d {

struct ActCatalog {
    std::vector<std::string> personActs;                       // content/ui/wheel.json: the acts on a person
    std::map<std::string, std::vector<std::string>> beastActs; // species -> acts (creature:<species>)
    std::map<std::string, std::vector<std::string>> objectActs; // object type -> acts (filled on demand)
    // wheel.json (null: none); the beasts' acts from the content
    static ActCatalog make(const sim::World& w, const sim::Json& wheel);
};

// The acts of `me` on `target` (an object or a character; kNone: on oneself,
// the acts of the self target). Each: id, name, can, why, needs [[text, ok]],
// gains, to_target, minutes, legal.
sim::Json actsJson(const sim::World& w, const ViewText& t, ActCatalog& cat, sim::EntityId me, sim::EntityId target);
// Quick bar slots checked for `me`: [{kind: act|goal, id}] -> [{kind, id, name, can, why}].
// An act on a person is checked against `target` (kNone: none chosen).
sim::Json barJson(const sim::World& w, const ViewText& t, sim::EntityId me, sim::EntityId target, const sim::Json& slots);
// A command's result ("", "rejected:<code>[:<why>]") in the player's words ("" when it went through).
std::string resultText(const ViewText& t, const std::string& result);

} // namespace lr::client2d
