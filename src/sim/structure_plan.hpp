#pragma once
// AL-76 / AL-81: a house of the village put together from rooms
// (content/ai/rooms.json). There are no building types: a household's recipe
// (its trade) names the rooms it needs - a home, a smithy on posts beside
// it, a storeroom leaning on it, bedrooms upstairs - and where each goes on
// the house (the main volume, a room behind a partition, the upper storey,
// an open canopy, a lean-to, a porch). The house is raised from modules of the
// build kit (build.json) and each room furnished with what makes it that
// room (sim/rooms.hpp tells them apart by that alone). Planned in the house's
// own frame (cells of cell_m, the front to local -y, as rot 0 of the kit),
// then turned by quarters onto the world grid, where the modules become
// built pieces of World::pieces like the ones a household raises itself.
// Pure and deterministic: the same recipe, size and seed give the same house.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/content.hpp"
#include "sim/core.hpp"

namespace lr::sim {

struct StructureSlotDef {
    std::string what;                  // object type
    std::string at = "middle";         // back | side | middle | front (in its room)
};
struct RoomFurnish {
    std::vector<StructureSlotDef> stations, decor;
    bool beds = false;                 // the household sleeps here
};
// One annex of a house: the room in it and where it goes.
struct StructureAnnexDef {
    std::string kind;                  // canopy | shed (a lean-to) | porch
    std::string room;                  // what it is furnished as ("" = nothing: a porch)
    std::vector<std::string> sides;    // front | back | left | right, by preference
    double chance = 1.0;
    bool required = false;
    int lenLo = 1, lenHi = 1;          // cells along the wall it leans on
    int depthLo = 1, depthHi = 1;      // cells out from it
    bool door = false;                 // a door from the main volume into it
};
// A household's (or a shared) house as its recipe lays it out.
struct StructureTypeDef {
    std::string id;                    // the recipe (farmer, smith, chapel...)
    bool home = false, shared = false;
    std::string trade;                 // the household's skill (knowledge id)
    std::vector<std::string> hosts;    // per_settlement objects it takes in
    std::vector<std::string> yard;     // objects behind it
    std::string zone = "lane";         // square | road | lane | edge
    // AL-84: where on the site it would rather stand (VillageWanted LotSpec)
    double siteWater = 0, siteWood = 0, siteEntrance = 0, siteDownwind = 0;
    bool nuisance = false, quiet = false;
    int wLo = 3, wHi = 3, dLo = 3, dHi = 3;
    int bigFamily = 0;                 // this many people or more: one cell wider
    int storeys = 1;
    std::vector<std::string> walls, roofs;
    double gableFront = 0.0;
    bool aisle = false;                // keep the middle column free (a chapel's aisle)
    std::string mainRoom;              // the main volume's room
    std::string splitRoom;             // a second ground-floor room behind a partition
    int splitW = 2;
    std::string upperRoom;             // the upper storey's rooms
    int upperN = 1;
    std::vector<StructureAnnexDef> annexes;
};
struct StructureMaterial {
    std::string wall, door, window, gable;
};
struct StructureCatalog {
    std::vector<StructureTypeDef> types;   // households' recipes and the shared ones
    std::map<std::string, RoomFurnish> furnish;
    std::map<std::string, StructureMaterial> materials;
    std::string foundation = "foundation_stone", floor = "floor_plank", pillar = "pillar_log";
    std::vector<std::string> trades, shared;
    double tradeShare = 0.67;
    double yardD = 10.0;
    std::string fence;
    Json layout;                       // AL-84: village.layout (VillageLayout::fromJson)
    const StructureTypeDef* find(const std::string& id) const {
        for (const auto& t : types)
            if (t.id == id) return &t;
        return nullptr;
    }
    bool loaded() const { return !types.empty(); }
};
StructureCatalog parseStructures(const Json& j);

// The house in its own frame: cell (i, j), i across the front, j from the
// front row (0) to the back; rot as the kit's (0 faces -y = the front).
struct LocalPiece {
    std::string kind;
    int i = 0, j = 0, z = 0, rot = 0;
};
struct LocalThing {
    std::string what;     // object type
    double u = 0, v = 0;  // metres in the house's frame (cell (i, j) spans [i c, (i+1) c] x [j c, (j+1) c])
    double yaw = 0;       // the way it faces, in the house's frame (0 = to the front)
    int i = 0, j = 0, z = 0;   // the cell it takes, the storey
    int part = -1;        // the room it furnishes (StructurePlan::parts)
    bool yard = false;    // behind the house, outside its footprint
    bool stacked = false; // one more bed by another (no cell of its own)
};
struct StructureVolume {
    std::string role;     // main | canopy | shed | porch
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;   // cells [i0, i1) x [j0, j1)
    int storeys = 1;
};
// A room the plan makes: what it is furnished as, its storey and cells.
struct StructurePart {
    std::string room;     // the room type it is furnished as ("" = none: a porch)
    std::string at;       // main | split | upper | canopy | shed | porch
    int z = 0;
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;
};
struct StructurePlan {
    std::string recipe;
    std::vector<StructureVolume> volumes;  // the main one first
    std::vector<StructurePart> parts;
    std::vector<LocalPiece> pieces;
    std::vector<LocalThing> stations;      // the sim's objects a room needs (reachable)
    std::vector<LocalThing> decor;         // furniture (sim objects too), where it fits
    std::vector<LocalThing> beds;          // where the household sleeps, in order
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;    // the footprint (every volume), cells
    int doorI = 0;                         // the main door: front edge of cell (doorI, 0)
    std::vector<std::string> notes;        // what the planner had to give up
};

struct StructureParams {
    int people = 4;             // who will live in it (beds)
    std::uint64_t seed = 1;
};

StructurePlan planStructure(const StructureCatalog& cat, const BuildCatalog& build, const StructureTypeDef& type,
                            const StructureParams& p);

// What is wrong with a plan ("" = nothing): two pieces in one slot, a piece of
// an unknown kind, a thing outside the footprint or on another's cell, the
// main door shut or a station nobody can reach from outside.
std::vector<std::string> checkStructure(const BuildCatalog& build, const StructurePlan& s);

// Turning the house's frame onto the world grid: q quarter turns (the front
// faces rot q), origin cell (gx, gy).
struct StructureFrame {
    int q = 0, gx = 0, gy = 0;
    double cellM = 2.0;
    void cell(int i, int j, int& x, int& y) const;      // a local cell -> the world cell
    int rot(int localRot) const;                          // a local rot -> the world one (gables keep the mirror bit)
    Vec2 point(double u, double v) const;                 // local metres -> sim metres
    double yaw(double localYaw) const { return localYaw + q * 1.5707963267948966; }
};
// The footprint centre in the house's frame (metres).
Vec2 footprintCentre(const StructurePlan& s, double cellM);

}  // namespace lr::sim
