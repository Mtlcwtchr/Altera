#pragma once
// The HUD's palette, read from the interface theme (ui_theme.hpp, AL-62).
// One instance for the whole game; Hud::init fills it from
// content/ui/themes/<id>.json. The names are the roles the HUD's drawing
// had in style G (gold rings, text on a dark panel, ink on a card), so the
// drawing code reads a role and the theme decides its colour.

#include "engine/ui/ui.hpp"
#include "game_view/ui_theme.hpp"

namespace altera::game {

struct HudPalette {
    ui::Colour panel, card, tip, gold, goldPale, goldDark, rim, text, mute, hot;
    ui::Colour cardInk, cardInkSoft, cardLine, cardRed;   // writing and rules on a card
    ui::Colour good, mid, bad, ringGood, ringMid, ringBad;
    ui::Colour dark, seal, red, grn, blu, unk;
    ui::Colour line, frame, sel, key, keyEdge, dim;
    UiTheme theme;
};

HudPalette& hudPalette();            // hud.cpp
void applyHudTheme(const UiTheme& t);

inline ui::Colour toColour(const ThemeColour& c) { return {c.r, c.g, c.b, c.a}; }

// The roles under the names the drawing code uses (style G's constants).
namespace hudroles {
inline const ui::Colour& kNavy = hudPalette().panel;
inline const ui::Colour& kGold = hudPalette().gold;
inline const ui::Colour& kGoldPale = hudPalette().goldPale;
inline const ui::Colour& kGoldDark = hudPalette().goldDark;
inline const ui::Colour& kRim = hudPalette().rim;
inline const ui::Colour& kText = hudPalette().text;
inline const ui::Colour& kMute = hudPalette().mute;
inline const ui::Colour& kHot = hudPalette().hot;
inline const ui::Colour& kInk = hudPalette().cardInk;
inline const ui::Colour& kInkSoft = hudPalette().cardInkSoft;
inline const ui::Colour& kLine = hudPalette().cardLine;
inline const ui::Colour& kInkRed = hudPalette().cardRed;
inline const ui::Colour& kGood = hudPalette().good;
inline const ui::Colour& kMid = hudPalette().mid;
inline const ui::Colour& kBad = hudPalette().bad;
inline const ui::Colour& kRingGood = hudPalette().ringGood;
inline const ui::Colour& kRingMid = hudPalette().ringMid;
inline const ui::Colour& kRingBad = hudPalette().ringBad;
inline const ui::Colour& kDark = hudPalette().dark;
inline const ui::Colour& kSeal = hudPalette().seal;
} // namespace hudroles

} // namespace altera::game
