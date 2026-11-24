#pragma once
// AL-8: the start village laid out on the world's own ground, after the
// settlement generator of Campfire (cl/landscape/settlement, layout "Roads"):
// a main road along the easiest line through the site, lanes off it, a square
// in the middle with the well, the fire and the shrine, houses on lots along the
// roads facing them with a yard behind, the fields beyond the yards. Pure and
// deterministic: the same site, seed and counts give the same plan.
#include <cstdint>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

struct MapGrid;

// The ground around the village as the game reads it from the terrain (sim
// coordinates, metres): heights and wet cells on a square grid. Without one the
// ground is flat and dry, but for the map's own water cells.
struct VillageSite {
    double x0 = 0, y0 = 0, cell = 4.0;
    int n = 0;
    std::vector<float> h;            // n*n heights, metres
    std::vector<std::uint8_t> wet;   // n*n, 1 = water
    std::vector<std::uint8_t> wood;  // AL-84: n*n, 1 = woodland (optional; else the map's forest biome)
    bool loaded() const { return n > 1 && h.size() == std::size_t(n) * std::size_t(n); }
    bool inside(Vec2 p) const;
    double height(Vec2 p) const;     // bilinear; 0 outside
    bool water(Vec2 p) const;        // nearest cell; false outside
    bool woodland(Vec2 p) const;     // AL-84: nearest cell; false outside or without wood
    static VillageSite fromJson(const Json& j);
};

// An oriented rectangle on the ground: centre, the way its front faces (yaw:
// radians, 0 = facing -y as the game's buildings do, turning toward +x) and
// half extents across (hw) and along the facing (hd).
struct PlanRect {
    Vec2 c;
    double yaw = 0, hw = 1, hd = 1;
    Vec2 front() const;              // unit vector the front faces
    Vec2 side() const;               // unit vector to its right
    Vec2 at(double across, double along) const;   // a point in its own frame (along = toward the front)
    bool overlaps(const PlanRect& o, double margin) const;
};

struct VillageLot {
    PlanRect house;                  // the building, front to the road
    PlanRect yard;                   // behind it
    double ground = 0;               // mean ground height under the house
    double groundMax = 0;            // AL-76: the highest ground under it (the floor goes there)
    int road = 0;                    // which road it fronts
    bool placed = true;              // AL-76: false = no room for it (lots follow VillageWanted::lots)
    int field = -1;                  // AL-76: its field in VillagePlan::fields, -1 none
};

// AL-76: a lot the village is asked for: the building's footprint in cells of
// cell_m (w across its front, d deep), where it goes (zone: square = by the
// square, road = along the main road, lane = anywhere, edge = out at the
// edge), a yard behind it and a field beyond (home). The house is turned by
// quarters (its modules stand on the world's grid) and set on the grid.
struct LotSpec {
    int w = 7, d = 6;
    std::string zone = "lane";
    double yardD = 12.0;
    bool home = true;
    // AL-84: where on the site it would rather stand (StructureTypeDef::site):
    // metres of distance it trades for a metre nearer the water / the wood /
    // the village's way in, or further downwind; a nuisance (smoke, stench,
    // the hammer) keeps off the quiet ones (the chapel, the inn) and the square.
    std::string recipe;
    double water = 0, wood = 0, entrance = 0, downwind = 0;
    bool nuisance = false, quiet = false;
};

// AL-84: how the village is laid out (rooms.json village.layout).
struct VillageLayout {
    double windFromDeg = 270;          // the prevailing wind blows from (0 = +x, 90 = +y)
    bool roadOut = true;               // the main road goes on out of the village both ways
    double rowBonus = 18.0;            // metres a lot gives for standing next to a neighbour
    double rowGap = 3.0;               // metres between neighbours in a row
    double nuisanceR = 40.0;           // a nuisance keeps this far from the quiet ones and the square
    double pathW = 1.4;                // footpaths: door to street, yard gate to field
    double fieldArea = 1200.0;         // a household's field, m2, as a strip behind its yard
    double stripLo = 14.0, stripHi = 26.0;   // the strip's width
    int spareLots = 2;                 // empty lots left along the streets for new households
    struct Common {
        std::string kind;              // cemetery | pasture | woodlot | work:<object> (a smoky works out of the village)
        std::string by;                // behind:<recipe> | water | wood | downwind (several: "wood,downwind")
        double w = 20, d = 20;
        double awayFromHomes = 0;      // AL-84: metres it keeps off every house
    };
    std::vector<Common> commons;
    static VillageLayout fromJson(const Json& j);
};

// AL-84: a piece of village land that is not a building: a field, a common
// (the churchyard, the pasture, the village's wood) or a lot kept for later.
struct VillageZone {
    std::string kind;   // field | cemetery | pasture | woodlot | spare
    PlanRect rect;
};

struct VillagePlan {
    double yaw = 0;                              // the main road's direction (radians from +x)
    std::vector<std::vector<Vec2>> roads;        // polylines, the main road first
    std::vector<double> roadWidth;
    std::vector<std::string> roadKind;           // AL-84: main | lane | out (beyond the village) | path
    Vec2 entrance;                               // AL-84: where the main road leaves toward the world
    std::vector<VillageZone> zones;              // AL-84: fields, commons, spare lots
    Vec2 square;                                 // the village square
    double squareR = 12.0;
    std::vector<VillageLot> lots;                // one per VillageWanted lot, in its order (see placed)
    std::vector<PlanRect> fields;                // the homes' fields (VillageLot::field)
    std::vector<Vec2> squareSpots;               // places round the square for shared things
    std::vector<std::string> notes;              // what the planner had to give up
};

struct VillageWanted {
    int houses = 3;
    int fields = 3;
    int squareSpots = 3;
    double houseW = 13.0, houseD = 11.0;   // footprint of a house (the Skyrim farmhouses are ~13 m)
    double yardD = 12.0;
    double fieldW = 40.0, fieldD = 30.0;
    double maxRelief = 2.2;                // the most a lot's corners may differ in height
    // AL-76: the lots one by one (their order is the plan's lots' order);
    // empty = `houses` homes of houseW x houseD
    std::vector<LotSpec> lots;
    double cellM = 2.0;                    // the grid the houses stand on
    VillageLayout layout;                  // AL-84
};

VillagePlan planVillage(const VillageSite& site, const MapGrid& map, Vec2 centre, const VillageWanted& want,
                        std::uint64_t seed);

// Distance from p to a polyline.
double distToPolyline(Vec2 p, const std::vector<Vec2>& line);
// AL-84: the point of a polyline nearest p.
Vec2 closestOnPolyline(Vec2 p, const std::vector<Vec2>& line);

// AL-84: a footpath from `from` to the nearest street (main or lane) or to
// `to` when given (anyRoad: or the road out); appended as kind "path".
void addPath(VillagePlan& plan, Vec2 from, const Vec2* to, double width, bool anyRoad = false);

// AL-84: what is wrong with a village plan ("" = nothing): a placed house on
// water or too steep, a door far from every street, a field on a road, a
// nuisance by a quiet house or the square, roads that do not meet.
std::vector<std::string> checkVillage(const VillagePlan& plan, const VillageSite& site, const MapGrid& map,
                                      const VillageWanted& want);

}  // namespace altera::sim
