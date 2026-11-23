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
    bool loaded() const { return n > 1 && h.size() == std::size_t(n) * std::size_t(n); }
    bool inside(Vec2 p) const;
    double height(Vec2 p) const;     // bilinear; 0 outside
    bool water(Vec2 p) const;        // nearest cell; false outside
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
};

struct VillagePlan {
    double yaw = 0;                              // the main road's direction (radians from +x)
    std::vector<std::vector<Vec2>> roads;        // polylines, the main road first
    std::vector<double> roadWidth;
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
};

VillagePlan planVillage(const VillageSite& site, const MapGrid& map, Vec2 centre, const VillageWanted& want,
                        std::uint64_t seed);

// Distance from p to a polyline.
double distToPolyline(Vec2 p, const std::vector<Vec2>& line);

}  // namespace altera::sim
