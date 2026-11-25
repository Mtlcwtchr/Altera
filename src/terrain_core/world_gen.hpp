#pragma once
// A procedural world from a few choices (AL-69): the new game's world, made
// in the game instead of imported - its size, its seed, how much water, how
// many mountains, the climate and the rivers.
//
// The relief: a land/sea layout and mountain belts from noise, then
// Campfire's terrain steps (terrain_gen.hpp) - geology (uplift against stream
// power: ridges and branching valleys), droplets (gullies, fans), talus
// (scree) and a little noise, the chain of Campfire's Auto brush. Biomes are
// the table's own (content/config/terrain_core/biomes.json ids) by height,
// slope, wetness and warmth. The checks every world must pass (land enough,
// a dry level place for the village, walkable ground joined to it, mountains
// as asked) run on the result; a world that fails is made again from the
// next variant of its seed, so whatever the seed the game gets a playable one.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "terrain_core/hydrology.hpp"

namespace terrain_core {

struct WorldSpec {
    std::string kind = "procedural";   // procedural | lore (the imported world of LivelyRealms)
    std::uint32_t seed = 1;
    int sizeKm = 12;     // 8 | 12 | 16 | 24
    int water = 1;       // 0 inland (lakes, rivers), 1 a coast, 2 islands
    int mountains = 1;   // 0 few, 1 some, 2 many
    int climate = 1;     // 0 cold, 1 temperate, 2 warm
    int rivers = 1;      // 0 few, 1 some, 2 many

    bool lore() const { return kind == "lore"; }
    double worldMetres() const { return double(sizeKm) * 1000.0; }
    double heightRange() const { return 1000.0 + 60.0 * double(sizeKm); }
    static constexpr double kSeaLevel = 0.1;   // macro value of height 0
    std::uint32_t mapSize() const { return sizeKm > 16 ? 4096u : 2048u; }
    // Folder name of the world's files: everything that shapes it.
    std::string key() const;
    bool operator==(const WorldSpec& o) const;
};

bool readWorldSpec(const std::filesystem::path& file, WorldSpec& spec, std::string* why = nullptr);
bool writeWorldSpec(const std::filesystem::path& file, const WorldSpec& spec, std::string* why = nullptr);
std::string worldSpecJson(const WorldSpec& spec);   // one line
bool parseWorldSpec(const std::string& json, WorldSpec& spec, std::string* why = nullptr);

// The rivers and lakes of a world this size (HydrologySettings is tuned for a 131 km one).
void tuneHydrology(const WorldSpec& spec, HydrologySettings& h);

struct WorldCheck {
    std::string name;
    bool ok = false;
    double value = 0, limit = 0;
};

struct GeneratedWorld {
    std::uint32_t size = 0;                // heights a side
    std::vector<std::uint16_t> heights;    // macro texels: kSeaLevel + metres / heightRange
    std::uint32_t biomeSize = 0;
    std::vector<std::uint8_t> biomes;      // ids of biomes.json, 0 the sea
    int attempt = 0;                       // the seed's variant that passed (0 the first)
    double landShare = 0, mountainShare = 0, peakMetres = 0;
    double siteX = 0.5, siteY = 0.5;       // best village place found, fractions of the world
    std::vector<WorldCheck> checks;
    bool passed() const;
    std::string report() const;            // JSON: the numbers and the checks
};

using WorldProgress = std::function<void(const char* stage, float done)>;

// The world at `size` heights a side (0: spec.mapSize()). Deterministic in the spec.
GeneratedWorld generateWorld(const WorldSpec& spec, std::uint32_t size = 0, const WorldProgress& progress = {});

// A picture of it, `size` pixels a side, north up: biome colours shaded by
// the relief, the sea blue (RGBA8).
std::vector<std::uint8_t> worldPicture(const GeneratedWorld& world, std::uint32_t size);

// The world's files in `folder` (height.png 16-bit, biomes.png, report.json,
// preview.png): made when missing, then read. False and `why` when neither works.
bool cachedWorld(const WorldSpec& spec, const std::filesystem::path& folder, std::filesystem::path& heightFile,
                 std::filesystem::path& biomeFile, const WorldProgress& progress = {}, std::string* why = nullptr);

} // namespace terrain_core
