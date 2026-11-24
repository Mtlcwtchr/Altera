#pragma once
// AL-95: the 2D client's map (gdd/ui/client_2d/CLIENT_2D.md §1.3): the sim's
// world cut into 1x1 m cells, one 16x16 tile each. Read-only over the sim:
// the ground of a cell comes from the biome, the water, the village's roads,
// square and zones (fields, churchyard...) and the floors of the built pieces;
// walls, doors and windows are the edge pieces on the cell lines; rooms and
// roofs come from World::rooms / the roof pieces. Things (trees, beds,
// forges) are listed with their sim positions; the view picks their sprites.
//
// What the ground of a biome, a zone or a road is, is data:
// content/sprites_2d/map2d.json "ground" (GroundRules::fromJson).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sim/world.hpp"

namespace altera::client2d {

using sim::EntityId;
using sim::Json;
using sim::Vec2;

// Kinds of ground, in this order (the JSON sends the index, `groundNames` the ids).
enum class Ground : std::uint8_t { grass, meadow, forest, marsh, water, sand, dirt, road, field, floor, count };
const std::vector<std::string>& groundNames();
Ground groundFromName(const std::string& id, Ground fallback);

struct GroundRules {
    std::map<std::string, Ground> biome;   // biome name -> ground (default: grass)
    std::map<std::string, Ground> zone;    // zone kind (or its prefix before ':') -> ground; missing: left as is
    std::map<std::string, Ground> road;    // road kind -> ground (default: road)
    Ground square = Ground::road;
    double shoreJitterM = 5.0;             // how far the coarse water grid's edge wanders
    double bankM = 1.5;                    // sand between water and land
    static GroundRules defaults();
    static GroundRules fromJson(const Json& j);   // "ground" of map2d.json; missing keys keep the defaults
};

// A wall, door or window on a cell line: 'h' runs along x on the line y,
// from x to x + len; 'v' along y on the line x.
struct Edge2d {
    int x = 0, y = 0, len = 1, z = 0;
    char axis = 'h';
    std::string kind;       // wall | door | window | gable
    std::string piece;      // the build.json piece id (its material)
    bool built = true;
};

// A room: its cells (1 m), type and the building it belongs to.
struct Room2d {
    int id = 0, building = -1, z = 0;
    std::string type;
    bool enclosed = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // cell bounds [x0, x1) x [y0, y1)
};

// A roofed piece cell (cellM x cellM metres from x, y) and the building under it (-1: none found).
struct Roof2d {
    int x = 0, y = 0, len = 1, z = 0, building = -1;
    std::string piece;
};

struct Thing2d {
    EntityId id = sim::kNone;
    std::string type;
    double x = 0, y = 0;
    int storey = 0;
    EntityId owner = sim::kNone;
};

struct Grid2d {
    int x0 = 0, y0 = 0, w = 0, h = 0;      // cells (= metres) of the window
    std::vector<std::uint8_t> ground;       // w*h, Ground
    std::vector<std::uint8_t> var;          // w*h, 0..255: a stable hash for tile variants
    std::vector<Edge2d> edges;
    std::vector<Room2d> rooms;
    std::vector<Roof2d> roofs;
    std::vector<Thing2d> things;
    Ground at(int x, int y) const { return Ground(ground[std::size_t(y - y0) * std::size_t(w) + std::size_t(x - x0)]); }
};

// The cells [x0, x0+w) x [y0, y0+h) of the world (clamped to the map).
Grid2d buildGrid(const sim::World& w, const GroundRules& rules, int x0, int y0, int wCells, int hCells);
// The ground of one cell, as buildGrid gives it (without pieces' floors).
Ground groundAt(const sim::World& w, const GroundRules& rules, int x, int y);

Json toJson(const Grid2d& g);

} // namespace altera::client2d
