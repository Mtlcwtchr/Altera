#include "sim/relief.hpp"

#include <algorithm>
#include <cmath>

#include "sim/data_fs.hpp"
#include "sim/world.hpp"
#include "terrain_core/terrain_gen.hpp"

namespace lr::sim {

namespace gen = terrain_core::gen;

namespace {

double smoothstep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double num(const Json& j, const char* k, double d) {
    return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d;
}

} // namespace

double Relief::coarseAt(Vec2 p) const {
    if (n <= 1 || heights.empty()) return 0.0;
    const double fi = std::clamp(p.x / spacing, 0.0, double(n - 1)), fj = std::clamp(p.y / spacing, 0.0, double(n - 1));
    const int i = std::min(int(fi), n - 2), j = std::min(int(fj), n - 2);
    const double tx = fi - i, ty = fj - j;
    auto h = [&](int a, int b) { return double(heights[std::size_t(b) * std::size_t(n) + std::size_t(a)]); };
    const double a = h(i, j) + (h(i + 1, j) - h(i, j)) * tx;
    const double b = h(i, j + 1) + (h(i + 1, j + 1) - h(i, j + 1)) * tx;
    return a + (b - a) * ty;
}

double Relief::heightAt(Vec2 p) const {
    double h = coarseAt(p);
    if (detailM > 0.0 && detailScaleM > 0.0) {
        // fine relief fades out on the low ground by the water (no steps along the banks)
        const double keep = std::clamp(h / (levelM * 2.0), 0.0, 1.0);
        h += detailM * keep * double(gen::fbm(p.x / detailScaleM, p.y / detailScaleM, 4, false, seed ^ 0x5eedu));
    }
    for (const auto& f : flats) {
        const double d = dist(p, f.c);
        if (d >= f.r + f.fall) continue;
        const double t = f.fall > 0.0 ? smoothstep01((d - f.r) / f.fall) : (d <= f.r ? 0.0 : 1.0);
        h = f.h + (h - f.h) * t;
    }
    return std::max(0.0, h);
}

int Relief::levelAt(Vec2 p) const {
    return levelM > 0.0 ? int(std::floor(heightAt(p) / levelM)) : 0;
}

const Relief* relief(const World& w) { return w.map().reliefData.get(); }

double reliefHeightAt(const World& w, Vec2 p) {
    const Relief* r = relief(w);
    return r ? r->heightAt(p) : 0.0;
}

int reliefLevelAt(const World& w, Vec2 p) {
    const Relief* r = relief(w);
    return r ? r->levelAt(p) : 0;
}

void makeRelief(World& w, const Json& cfg) {
    MapGrid& m = w.map();
    m.relief = Json::object();
    m.reliefData.reset();
    if (cfg.contains("relief") && cfg["relief"].is_boolean() && !cfg["relief"].get<bool>()) return;
    Json spec = Json::object();
    if (!w.content().dir.empty()) spec = data::readJsonOr(w.content().dir + "/../world/relief.json", Json::object());
    if (!spec.is_object()) spec = Json::object();
    if (cfg.contains("relief") && cfg["relief"].is_object()) data::mergeJson(spec, cfg["relief"]);
    if (spec.empty() || !spec.value("enabled", true)) return;
    spec.erase("_doc");
    spec["seed"] = std::uint32_t(hashCombine(w.seed(), hashStr("relief")) & 0xffffffffu);
    // the settlements' level ground: their roads, zones and houses, padded
    const double pad = num(spec, "flat_pad_m", 40.0), fall = num(spec, "flat_fall_m", 90.0),
                 maxR = num(spec, "flat_max_r_m", 450.0);
    Json flats = Json::array();
    for (const auto& [sid, s] : w.settlements()) {
        double r = std::max(s.squareR, 30.0);
        for (const auto& line : s.roads)
            for (const Vec2& p : line)
                if (dist(p, s.center) <= maxR) r = std::max(r, dist(p, s.center));
        for (const auto& z : s.zones)
            if (dist(z.c, s.center) <= maxR) r = std::max(r, dist(z.c, s.center) + std::max(z.hw, z.hd));
        for (const auto& [oid, o] : w.objects())
            if (o.ownerFamily != kNone && dist(o.pos, s.center) <= maxR) r = std::max(r, dist(o.pos, s.center));
        flats.push_back({s.center.x, s.center.y, std::min(r, maxR) + pad, fall});
    }
    spec["flats"] = flats;
    m.relief = spec;
    buildRelief(m);
}

void buildRelief(MapGrid& m) {
    m.reliefData.reset();
    const Json& s = m.relief;
    if (!s.is_object() || s.empty() || m.sizeM <= 0.0) return;
    auto r = std::make_shared<Relief>();
    r->n = std::clamp(int(num(s, "samples", 256)), 16, 1024);
    r->spacing = m.sizeM / double(r->n - 1);
    r->levelM = std::max(0.5, num(s, "level_m", 4.0));
    r->detailM = num(s, "detail_m", 1.6);
    r->detailScaleM = num(s, "detail_scale_m", 36.0);
    r->climbM = num(s, "climb_m", 6.0);
    r->seed = s.value("seed", std::uint32_t{1});
    const int n = r->n;
    const std::size_t nn = std::size_t(n) * std::size_t(n);

    // uplift by the coarse map's biome, softened, with a noise over it; water is base level
    const Json up = s.value("uplift", Json::object());
    const double upDefault = num(up, "_default", 0.35);
    std::vector<double> biomeUp(m.biomeNames.size(), upDefault);
    for (std::size_t b = 0; b < m.biomeNames.size(); ++b) biomeUp[b] = num(up, m.biomeNames[b].c_str(), upDefault);
    std::vector<float> uplift(nn, float(upDefault));
    std::vector<std::uint8_t> wet(nn, 0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const Vec2 p{i * r->spacing, j * r->spacing};
            const int c = m.cellIndex(p);
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            if (c >= 0 && !m.biome.empty() && m.biome[std::size_t(c)] < biomeUp.size()) uplift[k] = float(biomeUp[m.biome[std::size_t(c)]]);
            if (m.waterAt(p)) wet[k] = 1;
        }
    for (int pass = 0; pass < int(num(s, "blur_cells", 6)); ++pass) {
        std::vector<float> next = uplift;
        for (int j = 1; j < n - 1; ++j)
            for (int i = 1; i < n - 1; ++i) {
                float sum = 0;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) sum += uplift[std::size_t(j + dj) * std::size_t(n) + std::size_t(i + di)];
                next[std::size_t(j) * std::size_t(n) + std::size_t(i)] = sum / 9.0f;
            }
        uplift.swap(next);
    }
    const double noiseShare = num(s, "uplift_noise", 0.45);
    const double featureM = 1800.0;   // the noise's hills and saddles, about this wide
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(j) * std::size_t(n) + std::size_t(i);
            const double nz = 0.5 + 0.5 * gen::fbm(i * r->spacing / featureM, j * r->spacing / featureM, 4, false, r->seed);
            uplift[k] = wet[k] ? 0.0f : float(std::max(0.0, uplift[k] * ((1.0 - noiseShare) + noiseShare * 2.0 * nz)));
        }

    gen::Grid g = gen::Grid::make(n, n, float(r->spacing), 0.0f);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::size_t k = g.index(i, j);
            g.heights[k] = wet[k] ? 0.0f : 0.5f + 4.0f * uplift[k];
        }
    const double maxH = num(s, "max_height_m", 90.0);
    const Json geoJ = s.value("geology", Json::object());
    gen::GeologySettings geo;
    geo.iterations = int(num(geoJ, "iterations", 30));
    geo.diffusion = float(num(geoJ, "diffusion", 0.12));
    geo.maxHeight = float(maxH);
    gen::geology(g, uplift, geo, 0.0f);
    gen::smooth(g, 1);
    // the border is base level to the geology: take the heights from just inside instead of a wall
    {
        const int band = 3;
        std::vector<float> h = g.heights;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int ci = std::clamp(i, band, n - 1 - band), cj = std::clamp(j, band, n - 1 - band);
                if (ci != i || cj != j) h[g.index(i, j)] = g.at(ci, cj);
            }
        g.heights.swap(h);
    }
    const Json waterJ = s.value("water", Json::object());
    gen::WaterSettings ws;
    ws.drops = float(num(waterJ, "drops", 0.6));
    if (ws.drops > 0) gen::water(g, ws, r->seed ^ 0xd209u, {});
    if (const double deg = num(s, "thermal_deg", 34.0); deg > 0) gen::thermal(g, float(deg), 6);
    // the water stays the low ground, the land stays within the asked height
    float top = 0.0f;
    for (std::size_t k = 0; k < nn; ++k) top = std::max(top, g.heights[k]);
    const float scale = top > float(maxH) && top > 0 ? float(maxH) / top : 1.0f;
    for (std::size_t k = 0; k < nn; ++k) g.heights[k] = wet[k] ? 0.0f : std::max(0.0f, g.heights[k] * scale);
    r->heights = std::move(g.heights);

    for (const auto& f : s.value("flats", Json::array()))
        if (f.is_array() && f.size() >= 4) {
            ReliefFlat fl;
            fl.c = {f[0].get<double>(), f[1].get<double>()};
            fl.r = f[2].get<double>();
            fl.fall = f[3].get<double>();
            fl.h = r->coarseAt(fl.c);
            // a flat sits on a level's middle, not on its edge
            fl.h = (std::floor(fl.h / r->levelM) + 0.5) * r->levelM;
            r->flats.push_back(fl);
        }
    m.reliefData = std::move(r);
}

double reliefMoveFactor(const World& w, Vec2 from, Vec2 to, double step) {
    const Relief* r = relief(w);
    if (!r || r->climbM <= 0.0) return 1.0;
    const double d = std::min(step, dist(from, to));
    if (d < 1e-9) return 1.0;
    const double t = d / dist(from, to);
    const Vec2 end{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
    const int levels = std::abs(r->levelAt(end) - r->levelAt(from));
    return levels == 0 ? 1.0 : d / (d + r->climbM * levels);
}

} // namespace lr::sim
