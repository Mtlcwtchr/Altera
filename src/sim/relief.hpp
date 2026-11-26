#pragma once
// LR-156: the lie of the land of a world - heights in metres over the whole map,
// made once at worldgen by the project's terrain generator (src/terrain_core/
// terrain_gen: geology, droplets, talus) on a coarse grid, read back bilinear
// with a fine noise on top. Heights cut into levels of `level_m` metres give the
// 2D client its cliffs (src/client2d/grid2d) and the walk its climbs.
//
// The settings are content/world/relief.json; the ones a world was made with are
// copied into its map (MapGrid::relief, saved), and the heights are made again
// from them on load, so a save does not carry them and an old world keeps its land.

#include <cstdint>
#include <memory>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct MapGrid;

// Level ground around a settlement: within `r` the height is `h`, back to the
// land over `fall` metres beyond it.
struct ReliefFlat {
    Vec2 c;
    double r = 0.0, fall = 0.0;
    double h = 0.0;
};

struct Relief {
    int n = 0;                     // coarse samples a side
    double spacing = 1.0;          // metres between them
    std::vector<float> heights;    // n*n metres, row 0 first (y)
    double levelM = 4.0;
    double detailM = 0.0, detailScaleM = 36.0;
    double climbM = 0.0;
    std::uint32_t seed = 1;
    std::vector<ReliefFlat> flats;

    double coarseAt(Vec2 p) const;   // bilinear over the coarse grid
    double heightAt(Vec2 p) const;   // + detail, flats
    int levelAt(Vec2 p) const;
};

// The relief the world's map carries (null: a flat world).
const Relief* relief(const World& w);
double reliefHeightAt(const World& w, Vec2 p);   // 0 on a flat world
int reliefLevelAt(const World& w, Vec2 p);       // 0 on a flat world

// Worldgen: the settings (content/world/relief.json, then cfg "relief" over it;
// cfg "relief": false leaves the world flat) and the settlements' flats into
// MapGrid::relief, then the heights.
void makeRelief(World& w, const Json& cfg);
// The heights from MapGrid::relief (on load); an empty spec clears them.
void buildRelief(MapGrid& m);

// Walking: the share of the plain pace kept over the way from -> to (a step of
// `step` metres at most) for the levels it climbs or goes down (1: no relief).
double reliefMoveFactor(const World& w, Vec2 from, Vec2 to, double step);

} // namespace lr::sim
