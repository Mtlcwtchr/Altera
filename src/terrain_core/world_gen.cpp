// AL-69: the procedural world; see world_gen.hpp.

#include "terrain_core/world_gen.hpp"

#include "engine/world_source/png_io.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/terrain_gen.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace terrain_core {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::uint32_t kVersion = 6;   // bump when the generator changes what a spec makes

float smoothstep(float a, float b, float x) {
    if (a == b) return x < a ? 0.f : 1.f;
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Metres from every cell to the nearest cell where `from` is set (0 on it):
// an 8-neighbour chamfer, two passes.
std::vector<float> distanceTo(const std::vector<std::uint8_t>& from, int n, float cell) {
    const float far = 1e9f, side = cell, diag = cell * 1.41421356f;
    std::vector<float> d(from.size());
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = from[i] ? 0.f : far;
    auto at = [&](int x, int y) -> float& { return d[std::size_t(y) * std::size_t(n) + std::size_t(x)]; };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float& v = at(x, y);
            if (x > 0) v = std::min(v, at(x - 1, y) + side);
            if (y > 0) {
                v = std::min(v, at(x, y - 1) + side);
                if (x > 0) v = std::min(v, at(x - 1, y - 1) + diag);
                if (x < n - 1) v = std::min(v, at(x + 1, y - 1) + diag);
            }
        }
    for (int y = n - 1; y >= 0; --y)
        for (int x = n - 1; x >= 0; --x) {
            float& v = at(x, y);
            if (x < n - 1) v = std::min(v, at(x + 1, y) + side);
            if (y < n - 1) {
                v = std::min(v, at(x, y + 1) + side);
                if (x < n - 1) v = std::min(v, at(x + 1, y + 1) + diag);
                if (x > 0) v = std::min(v, at(x - 1, y + 1) + diag);
            }
        }
    return d;
}

// The value below which `share` of the samples lie.
float percentile(std::vector<float> v, double share) {
    if (v.empty()) return 0;
    const std::size_t k = std::min(v.size() - 1, std::size_t(std::clamp(share, 0.0, 1.0) * double(v.size())));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

// Biome ids of content/config/terrain_core/biomes.json used here.
enum : std::uint8_t {
    kSea = 0, kMarsh = 1, kDarkForest = 2, kGoldenDesert = 4, kDragonCliff = 5, kPlateau = 6, kFlowerLake = 7,
    kWhiteMountains = 8, kRiverValley = 9, kNorthernIsles = 13, kStoneDesert = 16, kForestEdge = 17, kScree = 22
};

// The fields of the layout, on the coarse grid the geology runs on.
struct Layout {
    int n = 0;
    float cell = 0;
    std::vector<float> h0, uplift, mountain, coast;   // metres, 0..1, 0..1, metres to the sea
    std::vector<std::uint8_t> sea;
};

struct Tuning {
    double landShare, mountainShare, peak, hills;
};

Tuning tuning(const WorldSpec& s) {
    static const double kLand[3] = {1.0, 0.70, 0.42};
    static const double kMountain[3] = {0.07, 0.17, 0.30};
    static const double kPeak[3] = {0.42, 0.62, 0.86};
    const double above = s.heightRange() * (1.0 - WorldSpec::kSeaLevel) * 0.95;
    return {kLand[std::clamp(s.water, 0, 2)], kMountain[std::clamp(s.mountains, 0, 2)],
            above * kPeak[std::clamp(s.mountains, 0, 2)], 70.0 + 30.0 * s.mountains};
}

Layout layout(const WorldSpec& spec, std::uint32_t salt, int n) {
    Layout L;
    L.n = n;
    const float W = float(spec.worldMetres());
    L.cell = W / float(n - 1);
    const Tuning t = tuning(spec);
    const std::uint32_t seed = spec.seed * 2654435761u + salt * 40503u + 7u;
    const std::size_t count = std::size_t(n) * std::size_t(n);

    // Land: a signed field cut at the share of land asked for.
    std::vector<float> landField(count);
    const float angle = float(gen::valueNoise(3.7, 1.1, seed) * 6.2831853);
    const float ca = std::cos(angle), sa = std::sin(angle);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float u = float(i) / float(n - 1), v = float(j) / float(n - 1);
            const double x = double(u) * W, y = double(v) * W;
            const float wobble = gen::fbm(x / 3500.0, y / 3500.0, 5, false, seed + 11);
            float f;
            if (spec.water == 0) f = 1.0f + 0.1f * wobble;   // all land; lakes come from the hollows
            else if (spec.water == 1) {
                // a coast across one side, bent by noise and with a bay or a cape
                const float s = (u - 0.5f) * ca + (v - 0.5f) * sa, along = -(u - 0.5f) * sa + (v - 0.5f) * ca;
                f = -s + 0.13f * gen::fbm(along * 3.0 + 7.0, 0.5, 4, false, seed + 13) + 0.07f * wobble;
            } else {
                const float du = u - 0.5f, dv = v - 0.5f;
                const float edge = std::min({u, v, 1.0f - u, 1.0f - v});
                f = 0.55f * gen::fbm(x / 4200.0, y / 4200.0, 5, false, seed + 17) + 0.35f - 2.3f * (du * du + dv * dv) -
                    1.5f * smoothstep(0.14f, 0.02f, edge);   // the shores bend away before the world's edge
            }
            landField[std::size_t(j) * std::size_t(n) + std::size_t(i)] = f;
        }
    const float cut = t.landShare >= 0.999 ? -1e9f : percentile(landField, 1.0 - t.landShare);
    L.sea.assign(count, 0);
    for (std::size_t k = 0; k < count; ++k) L.sea[k] = landField[k] <= cut ? 1 : 0;
    // islands: the world's edge is sea whatever the noise did
    if (spec.water == 2)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                if (std::min({i, j, n - 1 - i, n - 1 - j}) < n / 40) L.sea[std::size_t(j) * std::size_t(n) + std::size_t(i)] = 1;
    bool anySea = false;
    for (const auto s : L.sea) anySea = anySea || s;
    L.coast = anySea ? distanceTo(L.sea, n, L.cell) : std::vector<float>(count, 1e9f);
    std::vector<std::uint8_t> land(count);
    for (std::size_t k = 0; k < count; ++k) land[k] = L.sea[k] ? 0 : 1;
    const std::vector<float> offshore = distanceTo(land, n, L.cell);

    // Mountains: belts where a slow warped noise crosses zero, away from the
    // coast, and a frame along the land's edges (the valley is closed in);
    // cut at the share asked for.
    std::vector<float> belt(count), landBelt;
    landBelt.reserve(count);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            const double x = double(i) * L.cell, y = double(j) * L.cell;
            const double wx = x + gen::fbm(x / 6000.0, y / 6000.0, 3, false, seed + 21) * 2500.0;
            const double wy = y + gen::fbm(x / 6000.0 + 4.1, y / 6000.0 - 2.3, 3, false, seed + 22) * 2500.0;
            float b = 1.0f - std::fabs(gen::fbm(wx / 9000.0, wy / 9000.0, 4, false, seed + 23)) * 2.2f;
            b += 0.25f * gen::fbm(x / 2500.0, y / 2500.0, 3, false, seed + 24);
            const float edge = float(std::min({i, j, n - 1 - i, n - 1 - j})) * L.cell;
            // the frame: a broken, wandering rise along the edge, not a wall
            if (spec.water != 2) {
                const float reach = 0.16f * W * (0.6f + 0.8f * gen::valueNoise(x / 5000.0 + 3.3, y / 5000.0 - 8.1, seed + 25));
                const float gap = smoothstep(-0.15f, 0.25f, gen::fbm(x / 4000.0, y / 4000.0, 3, false, seed + 26));
                b = std::max(b, (1.05f - edge / reach) * gap);
            }
            b *= smoothstep(400.f, 2500.f, L.coast[k]);
            belt[k] = b;
            if (!L.sea[k]) landBelt.push_back(b);
        }
    const float mcut = percentile(landBelt, 1.0 - t.mountainShare);
    L.mountain.assign(count, 0);
    L.uplift.assign(count, 0);
    L.h0.assign(count, 0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            const double x = double(i) * L.cell, y = double(j) * L.cell;
            if (L.sea[k]) {
                L.h0[k] = -(4.0f + std::min(offshore[k], 2500.f) * 0.03f);
                continue;
            }
            const float m = smoothstep(mcut - 0.18f, mcut + 0.22f, belt[k]);
            L.mountain[k] = m;
            const float hills = gen::fbm(x / 2600.0, y / 2600.0, 5, false, seed + 31) * 0.5f + 0.5f;
            const float massif = gen::fbm(x / 3200.0, y / 3200.0, 5, true, seed + 32);
            const float inland = smoothstep(200.f, 4000.f, L.coast[k]);
            float h = 3.0f + 70.0f * inland + float(t.hills) * hills * (0.35f + 0.65f * inland);
            h += m * float(t.peak) * 0.45f * (0.45f + 0.55f * massif);
            L.h0[k] = h;
            L.uplift[k] = 0.03f + 0.10f * hills * inland + m * (0.55f + 0.45f * massif);
        }
    return L;
}

GeneratedWorld build(const WorldSpec& spec, std::uint32_t size, std::uint32_t salt, const WorldProgress& progress) {
    auto report = [&](const char* stage, float done) {
        if (progress) progress(stage, done);
    };
    const Tuning t = tuning(spec);
    const float W = float(spec.worldMetres());
    const int coarse = 384;
    report("layout", 0);
    Layout L = layout(spec, salt, coarse);

    // Geology on the coarse grid: ridges and the valleys between them.
    gen::Grid g = gen::Grid::make(coarse, coarse, L.cell);
    g.heights = L.h0;
    gen::GeologySettings geo;
    geo.iterations = 40;
    geo.maxHeight = float(t.peak);
    geo.diffusion = 0.12f;
    if (const char* e = std::getenv("LR_WORLDGEN_SKIP"); !(e && std::string(e).find("geology") != std::string::npos))
        gen::geology(g, L.uplift, geo, 0.0f, [&](float d) { report("geology", d); });
    gen::smooth(g, 1);   // the coarse cells' steps, before they are blown up
    // The border is base level to the geology and stays where it was while
    // the ground inside rises: a wall round the world. Its band takes the
    // heights from just inside instead.
    {
        const int band = 3;
        for (int j = 0; j < coarse; ++j)
            for (int i = 0; i < coarse; ++i) {
                const int ci = std::clamp(i, band, coarse - 1 - band), cj = std::clamp(j, band, coarse - 1 - band);
                if (ci != i || cj != j) g.at(i, j) = g.at(ci, cj);
            }
    }
    for (std::size_t k = 0; k < g.heights.size(); ++k) {
        if (L.sea[k]) {
            g.heights[k] = L.h0[k];
            continue;
        }
        const float keep = 0.25f + 0.75f * L.mountain[k];   // the lowland's hills stay; the ranges are the geology's
        g.heights[k] = lerp(L.h0[k], g.heights[k], keep);
    }

    // The full grid: the coarse relief, finer noise, then the Auto brush's chain.
    const int n = int(size);
    gen::Grid f = g.resampled(n, n);
    std::vector<float> mountain(std::size_t(n) * std::size_t(n)), coast(mountain.size());
    std::vector<std::uint8_t> sea(mountain.size());
    const std::uint32_t seed = spec.seed * 2654435761u + salt * 40503u + 7u;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double ci = double(i) * (coarse - 1) / (n - 1), cj = double(j) * (coarse - 1) / (n - 1);
            const int ii = std::clamp(int(ci + 0.5), 0, coarse - 1), jj = std::clamp(int(cj + 0.5), 0, coarse - 1);
            const std::size_t ck = std::size_t(jj) * coarse + std::size_t(ii), k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            const auto bil = [&](const std::vector<float>& v) {
                const int i0 = std::min(int(ci), coarse - 2), j0 = std::min(int(cj), coarse - 2);
                const float u = float(ci - i0), w = float(cj - j0);
                const auto at = [&](int a, int b) { return v[std::size_t(b) * coarse + std::size_t(a)]; };
                return lerp(lerp(at(i0, j0), at(i0 + 1, j0), u), lerp(at(i0, j0 + 1), at(i0 + 1, j0 + 1), u), w);
            };
            mountain[k] = bil(L.mountain);
            coast[k] = bil(L.coast);
            sea[k] = L.sea[ck];
            const double x = double(i) * f.spacing, y = double(j) * f.spacing;
            const float m = mountain[k];
            // crests a few hundred metres apart on the ranges, swells elsewhere
            f.heights[k] += (gen::fbm(x / 700.0, y / 700.0, 4, true, seed + 41) - 0.35f) * (6.0f + 70.0f * m) +
                            gen::fbm(x / 260.0, y / 260.0, 3, false, seed + 42) * (1.5f + 10.0f * m);
        }
    // LR_WORLDGEN_SKIP=geology,water,thermal: a step left out, to look at the others alone
    const char* skipEnv = std::getenv("LR_WORLDGEN_SKIP");
    const std::string skip = skipEnv ? skipEnv : "";
    report("water", 0);
    gen::WaterSettings ws;
    ws.drops = 0.5f;
    ws.erosion = 0.35f;
    std::vector<float> mask(mountain.size());
    for (std::size_t k = 0; k < mask.size(); ++k) mask[k] = sea[k] ? 0.f : 0.35f + 0.65f * mountain[k];
    if (skip.find("water") != std::string::npos) {
    } else if (n > 1024) {
        // the droplets on a grid of 1024 (Campfire's constants keep their
        // meaning at any spacing); what they changed is added to the full one
        gen::Grid half = f.resampled(1024, 1024);
        const std::vector<float> before = half.heights;
        std::vector<float> halfMask(before.size());
        for (int j = 0; j < 1024; ++j)
            for (int i = 0; i < 1024; ++i) {
                const int fi = std::min(n - 1, i * (n - 1) / 1023), fj = std::min(n - 1, j * (n - 1) / 1023);
                halfMask[std::size_t(j) * 1024 + std::size_t(i)] = mask[std::size_t(fj) * std::size_t(n) + std::size_t(fi)];
            }
        gen::water(half, ws, seed + 51, halfMask, [&](float d) { report("water", d); });
        for (std::size_t k = 0; k < before.size(); ++k) half.heights[k] -= before[k];
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                f.at(i, j) += half.sample(double(i) * 1023 / (n - 1), double(j) * 1023 / (n - 1));
    } else
        gen::water(f, ws, seed + 51, mask, [&](float d) { report("water", d); });
    report("thermal", 0);
    if (skip.find("thermal") == std::string::npos) gen::thermal(f, 38.0f, 4);
    gen::noise(f, 1.2f, 1.0f / 45.0f, 3, false, 0, seed + 61);
    // the coast: the sea stays sea, the land just above it
    for (std::size_t k = 0; k < f.heights.size(); ++k) {
        if (sea[k]) f.heights[k] = std::min(f.heights[k], -1.5f);
        else if (coast[k] < 400.f) f.heights[k] = std::max(f.heights[k], 0.4f + coast[k] * 0.004f);
    }

    GeneratedWorld out;
    out.attempt = int(salt);
    out.size = size;
    out.heights.resize(f.heights.size());
    const double range = spec.heightRange();
    double peak = -1e9, land = 0, mount = 0;
    for (std::size_t k = 0; k < f.heights.size(); ++k) {
        const double hm = f.heights[k];
        peak = std::max(peak, hm);
        if (hm > 0) land += 1;
        if (hm > 0 && mountain[k] > 0.5f) mount += 1;
        out.heights[k] = std::uint16_t(std::clamp((WorldSpec::kSeaLevel + hm / range) * 65535.0, 0.0, 65535.0));
    }
    out.peakMetres = peak;
    out.landShare = land / double(f.heights.size());
    out.mountainShare = land > 0 ? mount / land : 0;

    // ---- biomes -------------------------------------------------------------------------------------------
    report("biomes", 0);
    const int bn = std::min(n, 1024), step = n / bn;
    out.biomeSize = std::uint32_t(bn);
    out.biomes.assign(std::size_t(bn) * std::size_t(bn), kSea);
    gen::Grid c = f.resampled(coarse, coarse);
    const std::vector<float> area = gen::catchment(c, 0.0f);   // cells of the coarse grid
    // where the big water runs, blurred into "near a river"
    std::vector<std::uint8_t> riverCell(area.size(), 0);
    const float riverCells = 2.5e6f / (c.spacing * c.spacing);   // 2.5 km^2 of catchment
    for (std::size_t k = 0; k < area.size(); ++k) riverCell[k] = area[k] > riverCells && c.heights[k] > 0 ? 1 : 0;
    const std::vector<float> toRiver = distanceTo(riverCell, coarse, c.spacing);
    static const float kWarm[3] = {0.18f, 0.5f, 0.8f};
    const float warm0 = kWarm[std::clamp(spec.climate, 0, 2)];
    const float top = float(std::max(1.0, t.peak));
    std::vector<std::uint8_t> ids(out.biomes.size());
    for (int y = 0; y < bn; ++y)
        for (int x = 0; x < bn; ++x) {
            const int i = std::min(n - 1, x * step + step / 2), j = std::min(n - 1, y * step + step / 2);
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            const float h = f.heights[k];
            std::uint8_t id = kSea;
            if (h > 0) {
                const int i0 = std::max(0, i - step), i1 = std::min(n - 1, i + step);
                const int j0 = std::max(0, j - step), j1 = std::min(n - 1, j + step);
                const float sx = (f.at(i1, j) - f.at(i0, j)) / (float(i1 - i0) * f.spacing);
                const float sy = (f.at(i, j1) - f.at(i, j0)) / (float(j1 - j0) * f.spacing);
                const float slope = std::sqrt(sx * sx + sy * sy);
                const float e = h / top;
                const double wx = double(i) * f.spacing, wy = double(j) * f.spacing;
                const int ci = std::clamp(int(double(i) * (coarse - 1) / (n - 1) + 0.5), 0, coarse - 1);
                const int cj = std::clamp(int(double(j) * (coarse - 1) / (n - 1) + 0.5), 0, coarse - 1);
                const float nearRiver = 1.0f - smoothstep(150.f, 900.f, toRiver[std::size_t(cj) * coarse + std::size_t(ci)]);
                const float wet = 0.5f + 0.32f * gen::fbm(wx / 3500.0, wy / 3500.0, 4, false, seed + 71) + 0.22f * nearRiver -
                                  0.18f * e + (spec.water == 0 ? 0.04f : 0.0f) + 0.08f * float(spec.rivers - 1);
                const float warm = warm0 + 0.12f * gen::fbm(wx / 6000.0, wy / 6000.0, 3, false, seed + 72) - 0.55f * e;
                const float odd = gen::fbm(wx / 1500.0, wy / 1500.0, 3, false, seed + 73);
                const float m = mountain[k];
                if ((warm < 0.12f && e > 0.4f) || e > 0.72f) id = kWhiteMountains;
                else if (slope > 0.85f && e > 0.2f) id = kScree;
                else if (m > 0.45f && e > 0.32f) id = warm < 0.32f ? kWhiteMountains : warm > 0.62f ? kDragonCliff : kDarkForest;
                else if (wet > 0.74f && e < 0.06f && slope < 0.06f && warm < 0.7f) id = kMarsh;
                else if (nearRiver > 0.55f && e < 0.3f && slope < 0.2f) id = kRiverValley;
                else if (warm > 0.66f) id = wet < 0.34f ? kGoldenDesert : wet < 0.5f ? kStoneDesert : wet < 0.64f ? kPlateau : kRiverValley;
                else if (warm < 0.3f) id = wet > 0.52f ? kDarkForest : wet > 0.42f ? kForestEdge : kNorthernIsles;
                else if (wet > 0.6f) id = kDarkForest;
                else if (wet > 0.5f) id = kForestEdge;
                else if (odd > 0.55f && e < 0.25f) id = kFlowerLake;
                else id = kPlateau;
            }
            ids[std::size_t(y) * std::size_t(bn) + std::size_t(x)] = id;
        }
    // two majority passes: patches, not speckle (the sea stays where the heights put it)
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::uint8_t> next = ids;
        for (int y = 1; y < bn - 1; ++y)
            for (int x = 1; x < bn - 1; ++x) {
                const std::size_t k = std::size_t(y) * std::size_t(bn) + std::size_t(x);
                if (ids[k] == kSea) continue;
                std::array<int, 256> votes{};
                int best = ids[k], bestVotes = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const std::uint8_t v = ids[k + std::size_t(dy * bn + dx)];
                        if (v == kSea) continue;
                        if (++votes[v] > bestVotes) bestVotes = votes[v], best = v;
                    }
                if (bestVotes >= 5) next[k] = std::uint8_t(best);
            }
        ids.swap(next);
    }
    out.biomes = std::move(ids);

    // ---- checks -------------------------------------------------------------------------------------------
    report("checks", 0);
    {
        // The village's place: dry, level, open ground at least 1.5 km from the
        // world's edge, as the game's own search wants it (sim_game.cpp mapFromWorld).
        const int sn = coarse;
        const float cs = c.spacing;
        auto biomeAt = [&](int ci, int cj) {
            const int bx = std::clamp(int(double(ci) / (sn - 1) * (bn - 1) + 0.5), 0, bn - 1);
            const int by = std::clamp(int(double(cj) / (sn - 1) * (bn - 1) + 0.5), 0, bn - 1);
            return out.biomes[std::size_t(by) * std::size_t(bn) + std::size_t(bx)];
        };
        auto slopeAt = [&](int ci, int cj) {
            const float sx = (c.at(std::min(ci + 1, sn - 1), cj) - c.at(std::max(ci - 1, 0), cj)) / (2 * cs);
            const float sy = (c.at(ci, std::min(cj + 1, sn - 1)) - c.at(ci, std::max(cj - 1, 0))) / (2 * cs);
            return std::sqrt(sx * sx + sy * sy);
        };
        const int margin = int(std::ceil(2000.0 / cs));
        double bestScore = -1e9;
        int bx = sn / 2, by = sn / 2, candidates = 0;
        for (int cj = margin; cj < sn - margin; ++cj)
            for (int ci = margin; ci < sn - margin; ++ci) {
                const float h = c.at(ci, cj);
                const std::uint8_t id = biomeAt(ci, cj);
                if (h < 1.0f || id == kSea || id == kMarsh || id == kWhiteMountains || id == kScree || id == kGoldenDesert)
                    continue;
                const float sl = slopeAt(ci, cj);
                if (sl > 0.12f) continue;
                ++candidates;
                const double open = id == kRiverValley ? 1.0 : id == kPlateau ? 0.8 : id == kForestEdge || id == kFlowerLake ? 0.6
                                  : id == kDarkForest ? -0.2 : 0.3;
                const double water = 1.0 - smoothstep(200.f, 1200.f, std::min(toRiver[std::size_t(cj) * sn + std::size_t(ci)],
                                                                             L.coast[std::size_t(cj) * sn + std::size_t(ci)]));
                const double centre = std::hypot(ci - sn * 0.5, cj - sn * 0.5) * cs / W;
                const double score = open + 1.2 * water - sl * 6.0 - centre;
                if (score > bestScore) bestScore = score, bx = ci, by = cj;
            }
        out.siteX = double(bx) / (sn - 1);
        out.siteY = double(by) / (sn - 1);
        // the ground a walker reaches from it (no water, slopes under ~30 degrees)
        std::vector<std::uint8_t> seen(std::size_t(sn) * std::size_t(sn), 0);
        std::vector<int> stack{by * sn + bx};
        seen[std::size_t(by * sn + bx)] = 1;
        double reach = 0, landCells = 0;
        for (std::size_t k = 0; k < c.heights.size(); ++k) landCells += c.heights[k] > 0 ? 1 : 0;
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            reach += 1;
            const int ci = k % sn, cj = k / sn;
            static const int Dx[4] = {1, -1, 0, 0}, Dy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int ni = ci + Dx[d], nj = cj + Dy[d];
                if (ni < 0 || nj < 0 || ni >= sn || nj >= sn) continue;
                const int nk = nj * sn + ni;
                if (seen[std::size_t(nk)] || c.heights[std::size_t(nk)] <= 0.3f) continue;
                if (std::fabs(c.heights[std::size_t(nk)] - c.heights[std::size_t(k)]) > cs * 0.58f) continue;
                seen[std::size_t(nk)] = 1;
                stack.push_back(nk);
            }
        }
        const double reachShare = landCells > 0 ? reach / landCells : 0;
        int kinds = 0;
        std::map<int, int> counts;
        for (const auto id : out.biomes)
            if (id != kSea) ++counts[id];
        for (const auto& [id, cnt] : counts)
            if (cnt > int(out.biomes.size() / 200)) ++kinds;
        const double landWant = t.landShare * 0.8;
        out.checks = {
                {"land_share", out.landShare >= landWant, out.landShare, landWant},
                {"village_places", candidates >= 40, double(candidates), 40},
                {"walkable_from_village", reachShare >= 0.45, reachShare, 0.45},
                {"peak_height", out.peakMetres >= t.peak * 0.6, out.peakMetres, t.peak * 0.6},
                {"mountain_share", out.mountainShare >= t.mountainShare * 0.4, out.mountainShare, t.mountainShare * 0.4},
                {"biome_kinds", kinds >= 3, double(kinds), 3},
        };
    }
    report("done", 1);
    return out;
}

} // namespace

std::string WorldSpec::key() const {
    char b[96];
    std::snprintf(b, sizeof b, "w%dkm_s%u_%d%d%d%d_v%u", sizeKm, seed, water, mountains, climate, rivers, kVersion);
    return lore() ? std::string("lore") : std::string(b);
}

bool WorldSpec::operator==(const WorldSpec& o) const {
    if (lore() || o.lore()) return lore() == o.lore();
    return seed == o.seed && sizeKm == o.sizeKm && water == o.water && mountains == o.mountains && climate == o.climate &&
           rivers == o.rivers;
}

std::string worldSpecJson(const WorldSpec& s) {
    return Json{{"kind", s.kind}, {"seed", s.seed}, {"size_km", s.sizeKm}, {"water", s.water}, {"mountains", s.mountains},
                {"climate", s.climate}, {"rivers", s.rivers}}
            .dump();
}

bool parseWorldSpec(const std::string& text, WorldSpec& s, std::string* why) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (why) *why = "world spec: not a JSON object";
        return false;
    }
    s.kind = j.value("kind", std::string("procedural"));
    s.seed = j.value("seed", 1u);
    s.sizeKm = j.value("size_km", 12);
    static const int kSizes[] = {8, 12, 16, 24};
    if (std::find(std::begin(kSizes), std::end(kSizes), s.sizeKm) == std::end(kSizes)) s.sizeKm = 12;
    s.water = std::clamp(j.value("water", 1), 0, 2);
    s.mountains = std::clamp(j.value("mountains", 1), 0, 2);
    s.climate = std::clamp(j.value("climate", 1), 0, 2);
    s.rivers = std::clamp(j.value("rivers", 1), 0, 2);
    return true;
}

bool readWorldSpec(const fs::path& file, WorldSpec& spec, std::string* why) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (why) *why = "cannot read " + file.string();
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseWorldSpec(ss.str(), spec, why);
}

bool writeWorldSpec(const fs::path& file, const WorldSpec& spec, std::string* why) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out << worldSpecJson(spec);
    if (!out && why) *why = "cannot write " + file.string();
    return bool(out);
}

void tuneHydrology(const WorldSpec& spec, HydrologySettings& h) {
    const double km2 = double(spec.sizeKm) * spec.sizeKm;
    static const double kRain[3] = {0.55, 1.0, 1.8};
    const double rain = kRain[std::clamp(spec.rivers, 0, 2)];
    h.resolution = std::min<std::uint32_t>(h.resolution, spec.mapSize());
    h.streamArea = 0.35 / rain;
    h.fillDepth = 6.0;
    h.fillArea = 0.25;
    h.lakeDepth = 0.8;
    h.minLakeArea = 0.04;
    h.widthScale = 2.2;
    h.deltaArea = km2 * 0.3;
    h.deltaHeight = 25.0;
    h.mainRiverArea = std::max(1.5, km2 * 0.05 / rain);
    h.mainLakeArea = 0.12;
    h.mainLakes = std::uint32_t(4 + 3 * spec.rivers + (spec.water == 0 ? 4 : 0));
}

bool GeneratedWorld::passed() const {
    for (const auto& c : checks)
        if (!c.ok) return false;
    return !heights.empty();
}

std::string GeneratedWorld::report() const {
    Json checksJson = Json::array();
    for (const auto& c : checks) checksJson.push_back({{"name", c.name}, {"ok", c.ok}, {"value", c.value}, {"limit", c.limit}});
    std::map<std::string, double> shares;
    for (const auto id : biomes) shares[std::to_string(int(id))] += 1.0 / double(std::max<std::size_t>(1, biomes.size()));
    return Json{{"attempt", attempt}, {"passed", passed()}, {"land_share", landShare}, {"mountain_share", mountainShare},
                {"peak_m", peakMetres}, {"site", {siteX, siteY}}, {"biomes", shares}, {"checks", checksJson}}
            .dump(2);
}

GeneratedWorld generateWorld(const WorldSpec& spec, std::uint32_t size, const WorldProgress& progress) {
    if (size == 0) size = spec.mapSize();
    GeneratedWorld best;
    for (std::uint32_t salt = 0; salt < 4; ++salt) {
        GeneratedWorld w = build(spec, size, salt, progress);
        if (w.passed()) return w;
        if (best.heights.empty()) best = std::move(w);   // none passes: the seed's own, its checks say why
    }
    return best;
}

std::vector<std::uint8_t> worldPicture(const GeneratedWorld& w, std::uint32_t size) {
    struct C { std::uint8_t r, g, b; };
    static const std::map<int, C> kColour = {
            {kMarsh, {74, 140, 110}},   {kDarkForest, {38, 82, 52}},  {kGoldenDesert, {226, 196, 112}},
            {kDragonCliff, {176, 102, 60}}, {kPlateau, {128, 168, 82}}, {kFlowerLake, {176, 150, 170}},
            {kWhiteMountains, {214, 218, 224}}, {kRiverValley, {96, 150, 72}}, {kNorthernIsles, {136, 150, 140}},
            {kStoneDesert, {178, 150, 112}}, {kForestEdge, {72, 116, 64}}, {kScree, {130, 124, 118}}};
    std::vector<std::uint8_t> px(std::size_t(size) * size * 4, 255);
    if (w.size == 0) return px;
    auto height = [&](double u, double v) {
        const int x = std::clamp(int(u * (w.size - 1)), 0, int(w.size) - 1), y = std::clamp(int(v * (w.size - 1)), 0, int(w.size) - 1);
        return double(w.heights[std::size_t(y) * w.size + std::size_t(x)]) / 65535.0 - WorldSpec::kSeaLevel;
    };
    const double d = 1.0 / size;
    for (std::uint32_t py = 0; py < size; ++py)
        for (std::uint32_t px0 = 0; px0 < size; ++px0) {
            const double u = (px0 + 0.5) * d, v = 1.0 - (py + 0.5) * d;   // north up
            const double h = height(u, v);
            const int bx = std::clamp(int(u * w.biomeSize), 0, int(w.biomeSize) - 1);
            const int by = std::clamp(int(v * w.biomeSize), 0, int(w.biomeSize) - 1);
            const int id = w.biomes.empty() ? 0 : w.biomes[std::size_t(by) * w.biomeSize + std::size_t(bx)];
            float r, g, b;
            if (h <= 0) {
                const float deep = float(std::clamp(-h * 40.0, 0.0, 1.0));
                r = 40 - 20 * deep; g = 96 - 40 * deep; b = 150 - 30 * deep;
            } else {
                const auto it = kColour.find(id);
                const C c = it == kColour.end() ? C{140, 140, 120} : it->second;
                // light from the north-west
                const double dx = height(u + d, v) - height(u - d, v), dy = height(u, v + d) - height(u, v - d);
                const float shade = float(std::clamp(1.0 + (-dx + dy) * 900.0, 0.55, 1.35));
                r = c.r * shade; g = c.g * shade; b = c.b * shade;
            }
            std::uint8_t* o = &px[(std::size_t(py) * size + px0) * 4];
            o[0] = std::uint8_t(std::clamp(r, 0.f, 255.f));
            o[1] = std::uint8_t(std::clamp(g, 0.f, 255.f));
            o[2] = std::uint8_t(std::clamp(b, 0.f, 255.f));
            o[3] = 255;
        }
    return px;
}

bool cachedWorld(const WorldSpec& spec, const fs::path& folder, fs::path& heightFile, fs::path& biomeFile,
                 const WorldProgress& progress, std::string* why) {
    const fs::path dir = folder / spec.key();
    heightFile = dir / "height.r16";
    biomeFile = dir / "biomes.png";
    if (fs::exists(heightFile) && fs::exists(biomeFile)) return true;
    const GeneratedWorld w = generateWorld(spec, 0, progress);
    std::error_code ec;
    fs::create_directories(dir, ec);
    MacroMap macro;
    macro.assign(w.size, w.heights);
    if (!macro.save(heightFile, why)) return false;
    engine::world_source::Image image;
    image.width = image.height = w.biomeSize;
    image.channels = 1;
    image.bits = 8;
    image.allocate();
    for (std::uint32_t y = 0; y < w.biomeSize; ++y)   // north (+y) at the top of the picture
        for (std::uint32_t x = 0; x < w.biomeSize; ++x)
            image.set(x, w.biomeSize - 1 - y, 0, w.biomes[std::size_t(y) * w.biomeSize + x]);
    if (!engine::world_source::writePng(biomeFile, image, why)) return false;
    {
        std::ofstream out(dir / "report.json", std::ios::binary);
        out << w.report();
    }
    {
        engine::world_source::Image pic;
        pic.width = pic.height = 512;
        pic.channels = 4;
        pic.bits = 8;
        pic.bytes = worldPicture(w, 512);
        std::string ignored;
        engine::world_source::writePng(dir / "preview.png", pic, &ignored);
    }
    writeWorldSpec(dir / "spec.json", spec);
    return true;
}

} // namespace terrain_core
