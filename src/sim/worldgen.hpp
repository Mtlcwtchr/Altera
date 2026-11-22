#pragma once
// The small test world of AI_ARCHITECTURE §11.1: a stub map (8-16 km), 2-3
// biomes, a river, natural resources and one settlement of a few families.
// Everything follows from (config, seed).

#include "sim/world.hpp"

namespace altera::sim {

// cfg: content/sim/test_world.json (AI_DATA_SCHEMA.md §11). `seed` overrides cfg.seed when non-zero.
World makeTestWorld(const Content& content, const Json& cfg, std::uint64_t seed = 0);

Json loadJsonFile(const std::string& path);

// Names as world generation makes them (AL-39: newborns and strangers): from the content's
// names.json (AL-61, the loc translates them by name.<id>), else made of syllables.
std::string randomPersonName(Rng& r, bool female, const Content* c = nullptr);
std::string randomFamilyName(Rng& r, const Content* c = nullptr);

} // namespace altera::sim
