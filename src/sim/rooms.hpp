#pragma once
// AL-81: rooms as RimWorld has them. A room is the cells of one storey that
// built floors (footings on the ground, planks above) join without a wall,
// a gable or a door between them; closed when walls and doors bound it on
// every side, open when it leaks but stands under a roof (a canopy on posts:
// a workshop outdoors). What it is comes from what stands in it, by the rules
// of rooms.json (Content::rooms); a building is the rooms on touching
// footings, for what its highest room is for. The same reading serves every
// building - planned by the village generator, raised by the player or by
// people - and is redone when the pieces or the things in rooms change.
#include <string>
#include <vector>

#include "sim/building.hpp"
#include "sim/world.hpp"

namespace altera::sim {

// Something a room's rules read: an object of the world (or a test's).
struct RoomThing {
    EntityId id = kNone;
    std::string type;
    Vec2 pos;
    int storey = 0;
};

struct RoomsResult {
    std::vector<Room> rooms;
    std::vector<RoomBuilding> buildings;
};

// Rooms of the built pieces (planned ones do not count) with the things in them.
RoomsResult detectRooms(const BuildCatalog& build, const RoomCatalog& rules, const PieceMap& pieces,
                        const std::vector<RoomThing>& things);
// The type of a room holding these things (closed or open).
std::string roomType(const RoomCatalog& rules, const std::vector<std::string>& things, bool enclosed);

// The world's rooms afresh (World::rooms, World::roomBuildings).
void refreshRooms(World& w);
// Per tick: redo the rooms when the pieces or the things the rules read changed.
void roomsTick(World& w);
// The room holding a point of a storey (nullptr: none).
const Room* roomAt(const World& w, Vec2 p, int storey = 0);

}  // namespace altera::sim
