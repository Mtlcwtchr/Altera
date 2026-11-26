#pragma once
// What remains of the Campfire local map generator (removed with the legacy
// simulation): the country kind a world cell is refined as, which the world
// map still stores per cell (world_map_gen.hpp).

#include <cstdint>

namespace generation {

// A byte, because a world map holds a million cells and a four-byte enum in
// WorldCell is four megabytes of nothing.
enum class Biome : std::uint8_t { Temperate, RiverValley };

} // namespace generation
