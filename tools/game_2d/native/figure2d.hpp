#pragma once
// LR-165: the 48x64 people of the mythic world (centaurs on a horse's body), drawn from
// data: content/sprites_2d/figures2d.json. A line-by-line port of tools/game_2d/web/figures.js,
// pixel-equal with it (node tools/game_2d/figure_sheet.js --dump + figure2d_check compare).
// No SDL here: RGBA pixels only (0 is transparent).
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "native/pixels2d.hpp"

namespace game2d {

// Who is drawn: the people (body), the outfit (dress), the variant, the hair and beard style
// ("" = the people's by the variant), the tool in hand, colours chosen at creation.
struct FigureSpec {
    std::string people, culture, outfit;   // outfit "": bare
    bool fem = false, child = false, hide = false, kit = false;
    int v = 0;
    std::string hair, beard, tool;
    std::string skin, hairColour, dye;     // "#hex" or ""
    std::string key() const;
};

struct Figure {
    int w = 0, h = 0;
    std::vector<Rgba> px;    // row by row
    int ax = 0, ay = 0;      // the feet in the picture
    double scale = 1;        // the people's size against a man's
};

void setFigureData(const nlohmann::json& data);   // figures2d.json (an empty object: none)
const nlohmann::json& figureData();
bool figuresOn();                                  // the data is there and enabled
// The figure seen from 'S' | 'E' | 'N' | 'W' (W mirrors E) in the pose.
Figure drawFigure(const FigureSpec& spec, char side, const PawnPose& pose);
// As a square picture for the atlas (the figure as it is: the canvas is square).
Pixels figurePixels(const FigureSpec& spec, char side, const PawnPose& pose);

} // namespace game2d
