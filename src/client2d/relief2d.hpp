#pragma once
// LR-156: the relief on the 2D map. The sim's heights (sim/relief.hpp) cut into
// levels; a cell lower than a neighbour shows that neighbour's cliff face on its
// side (a face to the north, the long one seen from above-south; thin edges to
// the east and west; a lip to the south; corners). The style of the rock is the
// higher ground's (by its biome, by height: snow on the tops); where a road climbs
// the step is a ramp. Read-only over the sim.
//
// Styles and rules are data: content/sprites_2d/cliffs2d.json "rules" (tiles
// cf_<style>_<piece> in its "sprites", made by tools/content_gen/gen_cliffs_2d.py).

#include <map>
#include <string>
#include <vector>

#include "client2d/grid2d.hpp"

namespace lr::client2d {

struct ReliefRules {
    std::vector<std::string> styles{"stone"};    // index 0 = the default
    std::map<std::string, int> byBiome;          // biome -> style
    struct High {
        int fromLevel = 0;
        int style = 0;
    };
    std::vector<High> high;                      // by the higher ground's level, ascending
    bool rampOnRoads = true;
    // gentle ground: where the land around rises less than cliffSlope (metres a metre, over
    // slopeSpanM) a step between levels is a soft bank (gentleStyle), not a rock face
    int gentleStyle = -1;
    double cliffSlope = 0.1, slopeSpanM = 12.0;
    int styleOf(const std::string& name) const;  // -1: not a style
    static ReliefRules fromJson(const Json& rules);   // "rules" of cliffs2d.json
};

// Fills g.level, g.cliff, g.cliffStyle, g.cliffStyles (left empty on a flat world).
void paintRelief(const sim::World& w, const ReliefRules& rules, Grid2d& g);

} // namespace lr::client2d
