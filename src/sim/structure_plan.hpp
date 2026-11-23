#pragma once
// AL-76: a building of the village by what it is for (content/ai/structures.json).
// A type is a graph of modules of the build kit (build.json): the main volume
// and annexes on its sides (an open canopy on posts, a closed lean-to shed, a
// byre, a porch before the door), storeys and a roof; inside and beside it the
// sim's own objects stand at slots (the forge, the anvil, the hearth...) and
// props only the view draws (tables, a counter, benches). Planned in the
// building's own frame (cells of cell_m, the front to local -y, as rot 0 of
// the kit), then turned by quarters and set on the world grid, where the
// modules become built pieces of World::pieces like the ones a household
// raises itself. Pure and deterministic: the same type, size and seed give
// the same building.
#include <cstdint>
#include <string>
#include <vector>

#include "sim/content.hpp"
#include "sim/core.hpp"

namespace altera::sim {

struct StructureAnnexDef {
    std::string kind;                  // canopy | shed | byre | porch
    std::vector<std::string> sides;    // front | back | left | right, by preference
    double chance = 1.0;
    bool required = false;
    int lenLo = 1, lenHi = 1;          // cells along the wall it leans on
    int depthLo = 1, depthHi = 1;      // cells out from it
    bool door = false;                 // a door from the main volume into it
    std::vector<std::string> stations; // objects standing in it
};
struct StructureSlotDef {
    std::string what;                  // object type (stations) or prop piece (decor)
    std::string at = "middle";         // back | side | middle | front | annex | yard
};
struct StructureTypeDef {
    std::string id;
    bool home = false, shared = false;
    std::string trade;                 // the household's skill (knowledge id)
    std::vector<std::string> hosts;    // per_settlement objects it takes in
    std::string zone = "lane";         // square | road | lane | edge
    int wLo = 3, wHi = 3, dLo = 3, dHi = 3;
    int bigFamily = 0;                 // this many people or more: one cell wider
    int storeys = 1;
    std::vector<std::string> walls, roofs;
    double gableFront = 0.0;
    bool rooms = false;                // the upper storey cut into rooms
    bool aisle = false;                // keep the middle column free (a chapel's aisle)
    std::vector<StructureAnnexDef> annexes;
    std::vector<StructureSlotDef> stations, decor;
};
struct StructureMaterial {
    std::string wall, door, window, gable;
};
struct StructureCatalog {
    std::vector<StructureTypeDef> types;
    std::map<std::string, StructureMaterial> materials;
    std::string foundation = "foundation_stone", floor = "floor_plank", pillar = "pillar_log";
    std::vector<std::string> trades, shared;
    double tradeShare = 0.67;
    double yardD = 10.0;
    std::string fence;
    const StructureTypeDef* find(const std::string& id) const {
        for (const auto& t : types)
            if (t.id == id) return &t;
        return nullptr;
    }
    bool loaded() const { return !types.empty(); }
};
StructureCatalog parseStructures(const Json& j);

// The building in its own frame: cell (i, j), i across the front, j from the
// front row (0) to the back; rot as the kit's (0 faces -y = the front).
struct LocalPiece {
    std::string kind;
    int i = 0, j = 0, z = 0, rot = 0;
};
struct LocalThing {
    std::string what;     // object type or prop piece
    double u = 0, v = 0;  // metres in the building's frame (cell (i, j) spans [i c, (i+1) c] x [j c, (j+1) c])
    double yaw = 0;       // the way it faces, in the building's frame (0 = to the front)
    int i = 0, j = 0;     // the cell it takes
    bool yard = false;    // behind the building, outside its footprint
};
struct StructureVolume {
    std::string role;     // main | canopy | shed | byre | porch
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;   // cells [i0, i1) x [j0, j1)
    int storeys = 1;
};
struct StructurePlan {
    std::string type;
    std::vector<StructureVolume> volumes;  // the main one first
    std::vector<LocalPiece> pieces;
    std::vector<LocalThing> stations;      // the sim's objects
    std::vector<LocalThing> decor;         // the view's props
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;    // the footprint (every volume), cells
    int doorI = 0;                         // the main door: front edge of cell (doorI, 0)
    std::vector<std::string> notes;        // what the planner had to give up
};

struct StructureParams {
    int people = 4;             // who will live in it (home types)
    std::uint64_t seed = 1;
};

StructurePlan planStructure(const StructureCatalog& cat, const BuildCatalog& build, const StructureTypeDef& type,
                            const StructureParams& p);

// What is wrong with a plan ("" = nothing): two pieces in one slot, a piece of
// an unknown kind, a station outside the footprint or on another's cell, the
// main door shut or a station nobody can reach from outside.
std::vector<std::string> checkStructure(const BuildCatalog& build, const StructurePlan& s);

// Turning the building's frame onto the world grid: q quarter turns (the
// front faces rot q), origin cell (gx, gy).
struct StructureFrame {
    int q = 0, gx = 0, gy = 0;
    double cellM = 2.0;
    void cell(int i, int j, int& x, int& y) const;      // a local cell -> the world cell
    int rot(int localRot) const;                          // a local rot -> the world one (gables keep the mirror bit)
    Vec2 point(double u, double v) const;                 // local metres -> sim metres
    double yaw(double localYaw) const { return localYaw + q * 1.5707963267948966; }
};
// The footprint centre in the building's frame (metres) and its half sizes.
Vec2 footprintCentre(const StructurePlan& s, double cellM);

}  // namespace altera::sim
