#pragma once
// The 2D client's pictures as pixels, for the native window (AL-94): the 16x16
// sprite set (content/sprites_2d/sprites.json, grids of palette letters), the
// layered 20x20 pawns and the beasts. A port of tools/game_2d/web/sprites.js,
// pawns.js and game2d.js's beastSprite, so the window and the page draw the
// same people. No SDL here: RGBA pixels only (0 is transparent).
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace game2d {

using Rgba = std::uint32_t;   // 0xAARRGGBB
Rgba parseHex(const std::string& hex);   // "#rrggbb" -> opaque colour; bad: magenta
Rgba mulRgb(Rgba c, double k);

// A square picture, `size` x `size`, row by row.
struct Pixels {
    int size = 0;
    std::vector<Rgba> px;
};

class SpriteSet {
public:
    void load(const nlohmann::json& spritesJson);   // {pal: {letter: "#hex"}, sprites: {name: [rows]}}
    void merge(const nlohmann::json& more);          // more grids in the same palette ({sprites: {...}})
    bool has(const std::string& name) const { return grids_.count(name) != 0; }
    Rgba colour(char letter) const;
    Rgba colour(const std::string& letter) const { return letter.empty() ? colour('?') : colour(letter[0]); }
    // The sprite with letters swapped by `recolour` ({from: to}); empty Pixels when there is none.
    Pixels get(const std::string& name, const std::map<char, char>& recolour = {}) const;

private:
    std::map<char, Rgba> pal_;
    std::map<std::string, std::vector<std::string>> grids_;
};

// What a pawn wears and holds (pawns.js's spec).
struct PawnSpec {
    std::string body = "norm", skin = "light";
    bool child = false;
    std::pair<std::string, std::string> hair{"short", "#5a3a20"};
    bool beard = false;
    std::vector<std::pair<std::string, std::string>> wear;   // [layer, "#hex"]
    std::string cloak;                                      // "#hex" or empty
    std::pair<std::string, std::string> hat;                // [kind, "#hex"] or empty kind
    std::string tool;
    std::string key() const;
};

constexpr int kPawn = 20;
// The pawn seen from side 'S' | 'E' | 'N' | 'W' (W mirrors E), bob 0 | 1.
Pixels pawnPixels(const PawnSpec& spec, char side, int bob);
// A beast of species `sp` as a 16x16 blob facing 'E' or 'W'.
Pixels beastPixels(const std::string& sp, char face);

// game2d.js's hash of an id, so the window picks the same hair and build as the page.
std::uint32_t idHash(double n, int salt);

} // namespace game2d
