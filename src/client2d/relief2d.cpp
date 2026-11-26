#include "client2d/relief2d.hpp"

#include <algorithm>
#include <cmath>

#include "sim/relief.hpp"

namespace lr::client2d {

using namespace lr::sim;

int ReliefRules::styleOf(const std::string& name) const {
    for (std::size_t i = 0; i < styles.size(); ++i)
        if (styles[i] == name) return int(i);
    return -1;
}

ReliefRules ReliefRules::fromJson(const Json& j) {
    ReliefRules r;
    if (!j.is_object()) return r;
    if (j.contains("styles") && j["styles"].is_array() && !j["styles"].empty()) {
        r.styles.clear();
        for (const auto& s : j["styles"])
            if (s.is_string()) r.styles.push_back(s.get<std::string>());
        if (r.styles.empty()) r.styles.push_back("stone");
    }
    if (j.contains("by_biome") && j["by_biome"].is_object())
        for (const auto& [b, s] : j["by_biome"].items())
            if (s.is_string())
                if (const int k = r.styleOf(s.get<std::string>()); k >= 0) r.byBiome[b] = k;
    if (j.contains("high") && j["high"].is_array())
        for (const auto& h : j["high"]) {
            const int k = r.styleOf(h.value("style", std::string()));
            if (k >= 0) r.high.push_back({h.value("from_level", 0), k});
        }
    std::sort(r.high.begin(), r.high.end(), [](const High& a, const High& b) { return a.fromLevel < b.fromLevel; });
    r.rampOnRoads = j.value("ramp_on_roads", true);
    r.gentleStyle = r.styleOf(j.value("gentle_style", std::string()));
    r.cliffSlope = j.value("cliff_slope", r.cliffSlope);
    r.slopeSpanM = std::max(1.0, j.value("slope_span_m", r.slopeSpanM));
    return r;
}

void paintRelief(const World& w, const ReliefRules& rules, Grid2d& g) {
    g.level.clear();
    g.cliff.clear();
    g.cliffStyle.clear();
    g.cliffStyles.clear();
    const Relief* rel = relief(w);
    if (!rel || g.w <= 0 || g.h <= 0) return;
    // the window's levels with a ring of neighbours round it
    const int W = g.w + 2, H = g.h + 2;
    std::vector<int> lv(std::size_t(W) * std::size_t(H), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            lv[std::size_t(y) * std::size_t(W) + std::size_t(x)] = rel->levelAt({g.x0 + x - 1 + 0.5, g.y0 + y - 1 + 0.5});
    auto L = [&](int x, int y) { return lv[std::size_t(y + 1) * std::size_t(W) + std::size_t(x + 1)]; };
    const std::size_t n = std::size_t(g.w) * std::size_t(g.h);
    g.level.assign(n, 0);
    g.cliff.assign(n, 0);
    g.cliffStyle.assign(n, 0);
    g.cliffStyles = rules.styles;
    static const int dx[8] = {0, 1, 0, -1, 1, 1, -1, -1}, dy[8] = {-1, 0, 1, 0, -1, 1, 1, -1};
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(g.w) + std::size_t(x);
            const int own = L(x, y);
            g.level[i] = std::uint8_t(std::clamp(own, 0, 255));
            if (Ground(g.ground[i]) == Ground::water) continue;   // the bank is the land's, not a cliff in the river
            std::uint8_t mask = 0;
            int top = own, tx = x, ty = y;
            for (int d = 0; d < 8; ++d) {
                const int nl = L(x + dx[d], y + dy[d]);
                if (nl > own) {
                    mask |= std::uint8_t(1u << d);
                    if (nl > top) top = nl, tx = x + dx[d], ty = y + dy[d];
                }
            }
            if (!mask) continue;
            g.cliff[i] = mask;
            // the rock is the higher ground's
            const Vec2 hp{g.x0 + tx + 0.5, g.y0 + ty + 0.5};
            int style = 0;
            if (auto it = rules.byBiome.find(w.map().biomeAt(hp)); it != rules.byBiome.end()) style = it->second;
            for (const auto& h : rules.high)
                if (top >= h.fromLevel) style = h.style;
            if (rules.gentleStyle >= 0) {
                // the lie of the land round the step: a gentle rise is a bank
                const Vec2 c{g.x0 + x + 0.5, g.y0 + y + 0.5};
                const double s = rules.slopeSpanM;
                const double gx = rel->coarseAt({c.x + s, c.y}) - rel->coarseAt({c.x - s, c.y});
                const double gy = rel->coarseAt({c.x, c.y + s}) - rel->coarseAt({c.x, c.y - s});
                if (std::hypot(gx, gy) / (2.0 * s) < rules.cliffSlope) {
                    g.cliffStyle[i] = std::uint8_t(rules.gentleStyle);
                    continue;
                }
            }
            const Ground gi = Ground(g.ground[i]);
            const bool ramp = rules.rampOnRoads && (gi == Ground::road || roadAt(w, g.x0 + x + 0.5, g.y0 + y + 0.5) ||
                                                    roadAt(w, hp.x, hp.y));
            g.cliffStyle[i] = std::uint8_t(std::min(style, 0x7f) | (ramp ? 0x80 : 0));
        }
}

} // namespace lr::client2d
