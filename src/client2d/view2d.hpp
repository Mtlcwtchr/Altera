#pragma once
// AL-95: what the 2D client reads of the sim besides the map (grid2d.hpp):
// the people as the map draws them (where, what they wear and hold, what
// they are doing) and the card of the one selected. Display strings are
// looked up in content/loc here; the page keeps only keys of its own (ui2d.*).

#include <string>

#include "sim/loc.hpp"
#include "sim/world.hpp"

namespace lr::client2d {

struct ViewText {
    const sim::Loc* loc = nullptr;
    std::string lang = "ru";
    // LR-160: the player the answer is for (multiplayer: one per client; 1 alone)
    int player = 1;
    // kind.id.name when there is one, else the id
    std::string name(const std::string& kind, const std::string& id) const;
};

// Every character (people and beasts): id, name, x/y (metres), alive/down/
// sleeping, sex, age, player, family, the act and goal (names), the FSM state,
// the interaction id (the page picks its icon), where they walk (dest), and
// for people what they wear (Character::worn items) and the things in hand
// that show on the pawn (inventory ids; the page's map2d.json picks them).
// Beasts farther than `beastsR` metres from `near` are left out (null: all).
sim::Json peopleJson(const sim::World& w, const ViewText& t, const sim::Vec2* near = nullptr, double beastsR = 0);
// The card of one character (null when there is none).
sim::Json personJson(const sim::World& w, const ViewText& t, sim::EntityId id);
// Once per page: the map's size, the settlements, the names of the room
// types and object types, the ui2d.* strings of the language.
sim::Json staticJson(const sim::World& w, const ViewText& t);
// AL-96: the village chronicle from entry `since` on (at most `limit`, the newest):
// {n: the log's size, list: [{i, day, t, type, who, text}]}.
sim::Json chronicleJson(const sim::World& w, const ViewText& t, std::size_t since, std::size_t limit = 300);
// LR-140: a person's needs grouped by tier. Fills `needs` (every need: base ones
// in file order, then the derived; {id, name, v, tier, derived, care, crit?})
// and `tiers` (pyramid order, spirit last: {id, name, order, sat, needs: [ids]}).
void needsJson(const sim::World& w, const ViewText& t, const sim::Character& c, sim::Json& needs, sim::Json& tiers,
               bool withCrit);
// The clock and the weather, for the bottom bar.
sim::Json clockJson(const sim::World& w);

} // namespace lr::client2d
