#pragma once
// AL-94 (settlement and building from above): the 2D client's build mode.
// The player plans with tools, as RimWorld has them - a room dragged out as a
// rectangle (footings, walls round it, a door, a thatched roof), a row of
// walls along a cell line, a door or a window in a wall, floors, a roof,
// a pillar, a piece of furniture on the 1 m grid, striking what is planned.
// A tool's stroke expands into the sim's modules (content/ai/build.json);
// planning is free and has no limit, for one's household or for the
// settlement in common; only raising a piece takes materials, which the
// builders bring (sim/building.hpp). What a room is comes from what stands
// in it (sim/rooms.hpp), so furniture is planned like any other piece.
//
// Everything is checked as the sim checks it (checkPlacement over a copy of
// the plan with the stroke's earlier pieces in it), so the preview shows only
// what the sim will take.

#include <string>
#include <vector>

#include "client2d/view2d.hpp"
#include "sim/building.hpp"
#include "sim/world.hpp"

namespace lr::client2d {

// One module of a stroke: build.json kind, grid cell (cell_m; furniture: thing_m), storey, rot.
struct StrokePiece {
    std::string kind;
    int x = 0, y = 0, z = 0, rot = 0;
};

// A tool's stroke: from point a to point b (sim metres), on storey z.
//   tool: room | wall | door | window | floor | roof | pillar | thing | remove
//   material: log | daub | stone (walls, doors, windows, gables); roof: roof_thatch | roof_shingle
//   side: the door's side of a room (rot: 0 front -y, 1 +x, 2 +y, 3 -x); thing: the build.json id (thing_bed)
struct Stroke {
    std::string tool = "room";
    double ax = 0, ay = 0, bx = 0, by = 0;
    int z = 0, rot = 0, side = 0;
    std::string material = "log", roof = "roof_thatch", thing;
    bool forSettlement = false;
    bool roofed = true;     // room: put a roof on it
    static Stroke fromJson(const sim::Json& j);
};

// The pieces of a stroke, supports first (footings, walls, gables and roof).
std::vector<StrokePiece> expandStroke(const sim::BuildCatalog& cat, const Stroke& s);

// A room as the room tool lays it: cells [x0, x1) x [y0, y1) of storey z.
std::vector<StrokePiece> roomPieces(const sim::BuildCatalog& cat, int x0, int y0, int x1, int y1, int z,
                                    const std::string& material, int doorSide, bool roofed, const std::string& roof);
// A roof over cells [x0, x1) x [y0, y1) whose eaves rest on storey z - 1: slopes to the
// ridge along the longer side, each row a storey higher, gables and walls at the ends.
std::vector<StrokePiece> roofPieces(const sim::BuildCatalog& cat, int x0, int y0, int x1, int y1, int z,
                                    const std::string& material, const std::string& roof);

// Who plans: the player's person and whom the plan is for.
struct Planner {
    sim::EntityId actor = sim::kNone;
    bool forSettlement = false;
    sim::EntityId owner(const sim::World& w) const;   // the household or the settlement (kNone: neither)
};

// The preview: each piece of the stroke with ok / why (a ui2d.build.why.* key's
// tail), what it would replace, and the materials of the ones that can go
// (what the builders will bring; nothing is taken now).
sim::Json previewJson(const sim::World& w, const ViewText& t, const Planner& who, const Stroke& s);
// Lay the stroke's pieces into the plan (place_piece / remove_piece commands
// as the player's person); the same answer as the preview, with `placed`.
sim::Json placeJson(sim::World& w, const ViewText& t, const Planner& who, const Stroke& s);

// The palette: tools, materials, roofs, furniture (by the rooms they make), with names and costs.
sim::Json catalogJson(const sim::World& w, const ViewText& t);
// The plan in a window of the map (metres): every piece planned or standing
// there - kind, cell, storey, rot, centre, built, whose (mine / settlement /
// other), what its site still lacks and how far the work is.
sim::Json planJson(const sim::World& w, const ViewText& t, sim::EntityId viewer, int x0, int y0, int wM, int hM);

// AL-94: the colony window of the viewer's settlement: overview (name, chief,
// people, households, rooms and buildings by type), households, people and
// what they do, the plan's sites and what they lack, the offices (rights).
sim::Json colonyJson(const sim::World& w, const ViewText& t, sim::EntityId viewer);

}  // namespace lr::client2d
