// AL-69: the procedural world - the Campfire terrain steps and the generator's checks.

#include "framework.hpp"

#include "terrain_core/terrain_gen.hpp"
#include "terrain_core/world_gen.hpp"

#include <cmath>

using namespace terrain_core;

TEST(world_gen_geology_raises_uplifted_ground_and_drains_it) {
    gen::Grid g = gen::Grid::make(64, 64, 50.f, 10.f);
    std::vector<float> uplift(g.heights.size(), 0.f);
    for (int j = 16; j < 48; ++j)
        for (int i = 16; i < 48; ++i) uplift[g.index(i, j)] = 1.f;
    gen::GeologySettings s;
    s.iterations = 20;
    s.maxHeight = 800;
    gen::geology(g, uplift, s, 0.f);
    float top = 0;
    for (const float h : g.heights) top = std::max(top, h);
    CHECK(std::fabs(top - 800.f) < 1.f);
    CHECK(g.at(32, 32) > g.at(4, 4) + 200.f);
}

TEST(world_gen_droplets_keep_the_ground_finite_and_cut_into_slopes) {
    gen::Grid g = gen::Grid::make(96, 96, 10.f);
    for (int j = 0; j < 96; ++j)
        for (int i = 0; i < 96; ++i) g.at(i, j) = float(i) * 5.f + std::sin(float(j) * 0.3f) * 3.f;
    const auto before = g.heights;
    gen::WaterSettings w;
    gen::water(g, w, 7, {});
    double changed = 0;
    bool finite = true;
    for (std::size_t k = 0; k < g.heights.size(); ++k) {
        finite = finite && std::isfinite(g.heights[k]);
        changed += std::fabs(g.heights[k] - before[k]);
    }
    CHECK(finite);
    CHECK(changed > 1.0);
}

TEST(world_gen_spec_round_trips_and_keys_differ) {
    WorldSpec a;
    a.seed = 42;
    a.sizeKm = 16;
    a.water = 2;
    WorldSpec b;
    CHECK(parseWorldSpec(worldSpecJson(a), b));
    CHECK(a == b);
    CHECK(b.key() == a.key());
    b.mountains = 2;
    CHECK(!(a == b));
    CHECK(a.key() != b.key());
    WorldSpec lore;
    lore.kind = "lore";
    CHECK(lore.key() == "lore");
}

TEST(world_gen_worlds_are_deterministic_and_pass_their_checks) {
    for (int water = 0; water < 3; ++water) {
        WorldSpec s;
        s.seed = 11u + std::uint32_t(water);
        s.sizeKm = 8;
        s.water = water;
        const GeneratedWorld a = generateWorld(s, 512);
        CHECK(a.passed());
        CHECK(a.landShare > 0.25);
        CHECK(a.biomeSize == 512);
        if (water == 1) {
            const GeneratedWorld b = generateWorld(s, 512);
            CHECK(a.heights == b.heights);
            CHECK(a.biomes == b.biomes);
        }
    }
}
