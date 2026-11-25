#include "sim/village_plan.hpp"

#include <algorithm>
#include <cmath>

#include "sim/world.hpp"

namespace lr::sim {
namespace {
constexpr double kPi = 3.14159265358979323846;

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
}  // namespace

// ---- the site ---------------------------------------------------------------
bool VillageSite::inside(Vec2 p) const {
    if (!loaded()) return true;
    const double gx = (p.x - x0) / cell, gy = (p.y - y0) / cell;
    return gx >= 0 && gy >= 0 && gx <= n - 1 && gy <= n - 1;
}

double VillageSite::height(Vec2 p) const {
    if (!loaded()) return 0.0;
    const double gx = std::clamp((p.x - x0) / cell, 0.0, double(n - 1) - 1e-6);
    const double gy = std::clamp((p.y - y0) / cell, 0.0, double(n - 1) - 1e-6);
    const int ix = int(gx), iy = int(gy);
    const double fx = gx - ix, fy = gy - iy;
    auto at = [&](int x, int y) { return double(h[std::size_t(y) * std::size_t(n) + std::size_t(x)]); };
    return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
}

bool VillageSite::water(Vec2 p) const {
    if (!loaded() || wet.size() != h.size()) return false;
    const int ix = int(std::lround((p.x - x0) / cell)), iy = int(std::lround((p.y - y0) / cell));
    if (ix < 0 || iy < 0 || ix >= n || iy >= n) return false;
    return wet[std::size_t(iy) * std::size_t(n) + std::size_t(ix)] != 0;
}

bool VillageSite::woodland(Vec2 p) const {
    if (!loaded() || wood.size() != h.size()) return false;
    const int ix = int(std::lround((p.x - x0) / cell)), iy = int(std::lround((p.y - y0) / cell));
    if (ix < 0 || iy < 0 || ix >= n || iy >= n) return false;
    return wood[std::size_t(iy) * std::size_t(n) + std::size_t(ix)] != 0;
}

VillageSite VillageSite::fromJson(const Json& j) {
    VillageSite s;
    if (!j.is_object()) return s;
    s.x0 = j.value("x0", 0.0);
    s.y0 = j.value("y0", 0.0);
    s.cell = j.value("cell", 4.0);
    s.n = j.value("n", 0);
    // heights in decimetres keep the world file small
    if (j.contains("h_dm"))
        for (const auto& v : j["h_dm"]) s.h.push_back(float(v.get<double>() * 0.1));
    if (j.contains("wet"))
        for (const auto& v : j["wet"]) s.wet.push_back(std::uint8_t(v.get<int>() ? 1 : 0));
    if (j.contains("wood"))   // AL-84
        for (const auto& v : j["wood"]) s.wood.push_back(std::uint8_t(v.get<int>() ? 1 : 0));
    if (!s.loaded()) s = VillageSite{};
    return s;
}

// ---- rectangles -----------------------------------------------------------------
Vec2 PlanRect::front() const { return {std::sin(yaw), -std::cos(yaw)}; }
Vec2 PlanRect::side() const { return {std::cos(yaw), std::sin(yaw)}; }
Vec2 PlanRect::at(double across, double along) const { return add(c, add(mul(side(), across), mul(front(), along))); }

bool PlanRect::overlaps(const PlanRect& o, double margin) const {
    // separating axes of two oriented rectangles
    const Vec2 axes[4] = {side(), front(), o.side(), o.front()};
    const Vec2 d{o.c.x - c.x, o.c.y - c.y};
    for (const Vec2& a : axes) {
        auto reach = [&](const PlanRect& r) {
            return (r.hw + margin * 0.5) * std::fabs(r.side().x * a.x + r.side().y * a.y) +
                   (r.hd + margin * 0.5) * std::fabs(r.front().x * a.x + r.front().y * a.y);
        };
        if (std::fabs(d.x * a.x + d.y * a.y) > reach(*this) + reach(o)) return false;
    }
    return true;
}

Vec2 closestOnPolyline(Vec2 p, const std::vector<Vec2>& line) {
    if (line.empty()) return p;
    Vec2 best = line[0];
    double bd = 1e18;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const Vec2 a = line[i], b = line[i + 1];
        const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
        const double t = l2 > 0 ? std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
        const Vec2 q{a.x + vx * t, a.y + vy * t};
        const double d = dist(p, q);
        if (d < bd) bd = d, best = q;
    }
    return best;
}

VillageLayout VillageLayout::fromJson(const Json& j) {
    VillageLayout L;
    if (!j.is_object()) return L;
    L.windFromDeg = j.value("wind_from_deg", L.windFromDeg);
    L.roadOut = j.value("road_out", L.roadOut);
    L.rowBonus = j.value("row_bonus", L.rowBonus);
    L.rowGap = j.value("row_gap", L.rowGap);
    L.nuisanceR = j.value("nuisance_r", L.nuisanceR);
    L.pathW = j.value("path_w", L.pathW);
    L.fieldArea = j.value("field_area", L.fieldArea);
    if (j.contains("strip_w") && j["strip_w"].size() == 2) L.stripLo = j["strip_w"][0].get<double>(), L.stripHi = j["strip_w"][1].get<double>();
    L.spareLots = j.value("spare_lots", L.spareLots);
    for (const auto& c : j.value("commons", Json::array())) {
        Common k;
        k.kind = c.value("kind", std::string());
        k.by = c.value("by", std::string());
        k.w = c.value("w", k.w);
        k.d = c.value("d", k.d);
        k.awayFromHomes = c.value("away_from_homes", 0.0);
        if (!k.kind.empty()) L.commons.push_back(k);
    }
    return L;
}

void addPath(VillagePlan& plan, Vec2 from, const Vec2* to, double width, bool anyRoad) {
    Vec2 end;
    if (to) end = *to;
    else {
        double bd = 1e18;
        for (std::size_t r = 0; r < plan.roads.size(); ++r) {
            const std::string& k = r < plan.roadKind.size() ? plan.roadKind[r] : std::string("main");
            if (k != "main" && k != "lane" && !(anyRoad && k == "out")) continue;
            const Vec2 q = closestOnPolyline(from, plan.roads[r]);
            const double d = dist(from, q) - plan.roadWidth[r] * 0.5;
            if (d < bd) bd = d, end = q;
        }
        if (bd > 1e17) return;
    }
    if (dist(from, end) < 1.5) return;
    plan.roads.push_back({from, end});
    plan.roadWidth.push_back(width);
    plan.roadKind.push_back("path");
}

double distToPolyline(Vec2 p, const std::vector<Vec2>& line) {
    double best = 1e18;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const Vec2 a = line[i], b = line[i + 1];
        const double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
        double t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(p.x - (a.x + vx * t), p.y - (a.y + vy * t)));
    }
    if (line.size() == 1) best = dist(p, line[0]);
    return best;
}

// ---- the plan -------------------------------------------------------------------
namespace {

// AL-84: how far the water and the wood are from a point: a chamfer distance
// over the site's grid, else the map's coarse cells (their centres).
struct NearField {
    const VillageSite* site = nullptr;
    std::vector<float> dWater, dWood;   // metres, site grid
    std::vector<Vec2> mapWater, mapWood;
    double mapHalf = 0;

    static std::vector<float> chamfer(const VillageSite& s, const std::vector<std::uint8_t>& src) {
        const int n = s.n;
        const float inf = 1e9f;
        std::vector<float> d(std::size_t(n) * std::size_t(n), inf);
        bool any = false;
        for (std::size_t i = 0; i < d.size() && i < src.size(); ++i)
            if (src[i]) d[i] = 0, any = true;
        if (!any) return {};
        auto at = [&](int x, int y) -> float& { return d[std::size_t(y) * std::size_t(n) + std::size_t(x)]; };
        const float a = float(s.cell), b = float(s.cell * 1.41421356);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                float& v = at(x, y);
                if (x > 0) v = std::min(v, at(x - 1, y) + a);
                if (y > 0) v = std::min(v, at(x, y - 1) + a);
                if (x > 0 && y > 0) v = std::min(v, at(x - 1, y - 1) + b);
                if (x + 1 < n && y > 0) v = std::min(v, at(x + 1, y - 1) + b);
            }
        for (int y = n - 1; y >= 0; --y)
            for (int x = n - 1; x >= 0; --x) {
                float& v = at(x, y);
                if (x + 1 < n) v = std::min(v, at(x + 1, y) + a);
                if (y + 1 < n) v = std::min(v, at(x, y + 1) + a);
                if (x + 1 < n && y + 1 < n) v = std::min(v, at(x + 1, y + 1) + b);
                if (x > 0 && y + 1 < n) v = std::min(v, at(x - 1, y + 1) + b);
            }
        return d;
    }

    NearField(const VillageSite& s, const MapGrid& map, Vec2 centre, double reach) : site(&s) {
        if (s.loaded()) {
            dWater = chamfer(s, s.wet);
            dWood = chamfer(s, s.wood);
        }
        mapHalf = map.cellM * 0.5;
        if (map.w > 0)
            for (int y = 0; y < map.h; ++y)
                for (int x = 0; x < map.w; ++x) {
                    const Vec2 c{(x + 0.5) * map.cellM, (y + 0.5) * map.cellM};
                    if (dist(c, centre) > reach + map.cellM) continue;
                    const std::size_t i = std::size_t(y) * std::size_t(map.w) + std::size_t(x);
                    if (i < map.water.size() && map.water[i]) mapWater.push_back(c);
                    if (i < map.biome.size() && map.biome[i] < map.biomeNames.size() && map.biomeNames[map.biome[i]] == "forest")
                        mapWood.push_back(c);
                }
    }
    double fromGrid(const std::vector<float>& d, Vec2 p) const {
        if (d.empty() || !site->inside(p)) return 1e9;
        const int ix = std::clamp(int(std::lround((p.x - site->x0) / site->cell)), 0, site->n - 1);
        const int iy = std::clamp(int(std::lround((p.y - site->y0) / site->cell)), 0, site->n - 1);
        return d[std::size_t(iy) * std::size_t(site->n) + std::size_t(ix)];
    }
    double fromMap(const std::vector<Vec2>& cells, Vec2 p) const {
        double best = 1e9;
        for (const Vec2& c : cells) best = std::min(best, std::max(0.0, dist(c, p) - mapHalf));
        return best;
    }
    // the site's own reading wins where it has one (the map's cells are 250 m)
    double water(Vec2 p) const {
        const double g = fromGrid(dWater, p);
        return g < 1e8 ? g : fromMap(mapWater, p);
    }
    double wood(Vec2 p) const {
        if (site->loaded() && site->wood.size() == site->h.size()) {
            const double g = fromGrid(dWood, p);
            if (g < 1e8 || site->inside(p)) return std::min(g, fromMap(mapWood, p) + 200.0);
        }
        return fromMap(mapWood, p);
    }
};

}  // namespace

VillagePlan planVillage(const VillageSite& site, const MapGrid& map, Vec2 centre, const VillageWanted& wanted,
                        std::uint64_t seed) {
    VillagePlan plan;
    // AL-76: the lots asked for; the old way (`houses` homes) as specs
    VillageWanted want = wanted;
    if (want.lots.empty())
        for (int k = 0; k < want.houses; ++k) {
            LotSpec L;
            L.w = std::max(1, int(std::lround(want.houseW / want.cellM)));
            L.d = std::max(1, int(std::lround(want.houseD / want.cellM)));
            L.yardD = want.yardD;
            want.lots.push_back(L);
        }
    const VillageLayout& lay = want.layout;
    const int nLots = int(want.lots.size());
    double meanW = 0;
    for (const auto& L : want.lots) meanW += std::max(L.w, L.d) * want.cellM / std::max(1, nLots);
    Rng rng(hashCombine(seed, hashStr("village_plan")));
    auto inMap = [&](Vec2 p) { return p.x > 2 && p.y > 2 && p.x < map.sizeM - 2 && p.y < map.sizeM - 2; };
    auto wetAt = [&](Vec2 p) { return site.water(p) || (map.w > 0 && map.waterAt(p)); };
    auto usable = [&](Vec2 p) { return inMap(p) && site.inside(p) && !wetAt(p); };
    auto slopeAlong = [&](Vec2 a, Vec2 b) {
        const double l = dist(a, b);
        return l > 0 ? std::fabs(site.height(b) - site.height(a)) / l : 0.0;
    };

    // the radius grows with the households (lots on both sides of the roads)
    const double lotStep = meanW + 4.0;
    const double radius = std::clamp(35.0 + lotStep * 0.55 * nLots, 50.0, 170.0);
    // AL-84: a bigger village, a bigger square
    plan.squareR = std::clamp(8.0 + nLots * 0.6, 10.0, 18.0);
    const NearField near(site, map, centre, radius * 3.0);
    const double windA = lay.windFromDeg * kPi / 180.0;
    const Vec2 downwind{-std::cos(windA), -std::sin(windA)};   // the way the wind carries smoke

    // The square: the flattest dry spot near the centre.
    {
        Vec2 best = centre;
        double bestScore = 1e18;
        for (int k = 0; k < 40; ++k) {
            const double a = rng.range(0, 2 * kPi), r = k == 0 ? 0.0 : rng.range(0, radius * 0.4);
            const Vec2 p{centre.x + std::cos(a) * r, centre.y + std::sin(a) * r};
            if (!usable(p)) continue;
            double relief = 0;
            for (int q = 0; q < 8; ++q) {
                const double b = q * kPi / 4;
                const Vec2 e{p.x + std::cos(b) * 12, p.y + std::sin(b) * 12};
                relief = std::max(relief, std::fabs(site.height(e) - site.height(p)));
                if (!usable(e)) relief += 5;
            }
            const double score = relief + r * 0.01;
            if (score < bestScore) bestScore = score, best = p;
        }
        plan.square = best;
    }

    // The main road: of twelve directions the one along which the ground climbs
    // least and stays dry longest (a road follows the valley, not the slope).
    {
        double bestScore = 1e18;
        const double phase = rng.range(0, kPi / 12);
        for (int k = 0; k < 12; ++k) {
            const double a = phase + k * kPi / 12;
            double s = 0;
            for (int sgn = -1; sgn <= 1; sgn += 2)
                for (double t = 6; t <= radius; t += 6) {
                    const Vec2 p0{plan.square.x + std::cos(a) * sgn * (t - 6), plan.square.y + std::sin(a) * sgn * (t - 6)};
                    const Vec2 p1{plan.square.x + std::cos(a) * sgn * t, plan.square.y + std::sin(a) * sgn * t};
                    s += slopeAlong(p0, p1) * 6 + (usable(p1) ? 0.0 : 3.0);
                }
            if (s < bestScore) bestScore = s, plan.yaw = a;
        }
    }

    // A road from `from` out to `len` metres along direction a, bending a
    // little and stopping at water or the edge of the site.
    auto layRoad = [&](Vec2 from, double a, double len, double width, const std::string& kind, double maxSlope = 0.35) {
        std::vector<Vec2> line{from};
        double heading = a;
        Vec2 p = from;
        for (double t = 8; t <= len; t += 8) {
            // try straight ahead, then a slight bend either way: the lower climb wins
            double bestA = heading, bestS = 1e18;
            for (double turn : {0.0, 0.12, -0.12, 0.25, -0.25}) {
                const double h = heading + turn;
                if (std::fabs(std::remainder(h - a, 2 * kPi)) > 0.45) continue;
                const Vec2 q{p.x + std::cos(h) * 8, p.y + std::sin(h) * 8};
                if (!usable(q)) continue;
                const double s = slopeAlong(p, q) + std::fabs(turn) * 0.05 + rng.range(0, 0.004);
                if (s < bestS) bestS = s, bestA = h;
            }
            if (bestS > 1e17 || bestS > maxSlope) break;
            heading = bestA;
            p = {p.x + std::cos(heading) * 8, p.y + std::sin(heading) * 8};
            line.push_back(p);
        }
        if (line.size() >= 2) {
            plan.roads.push_back(line);
            plan.roadWidth.push_back(width);
            plan.roadKind.push_back(kind);
            return true;
        }
        return false;
    };
    int waterLane = 0;   // AL-84: the lane down to the water (0: none)
    {
        // both halves of the main road start at the square's edge
        const Vec2 dir{std::cos(plan.yaw), std::sin(plan.yaw)};
        const std::size_t before = plan.roads.size();
        layRoad(plan.square, plan.yaw + kPi, radius, 4.0, "main");
        layRoad(plan.square, plan.yaw, radius, 4.0, "main");
        if (plan.roads.size() == before + 2) {
            // one polyline end to end through the square
            std::vector<Vec2> a = plan.roads[before], b = plan.roads[before + 1];
            std::reverse(a.begin(), a.end());
            a.insert(a.end(), b.begin() + 1, b.end());
            plan.roads.resize(before);
            plan.roadWidth.resize(before);
            plan.roadKind.resize(before);
            plan.roads.push_back(a);
            plan.roadWidth.push_back(4.0);
            plan.roadKind.push_back("main");
        } else if (plan.roads.empty()) {
            plan.roads.push_back({add(plan.square, mul(dir, -8)), add(plan.square, mul(dir, 8))});
            plan.roadWidth.push_back(4.0);
            plan.roadKind.push_back("main");
            plan.notes.push_back("main road blocked");
        }
        const std::vector<Vec2> main = plan.roads[0];
        // AL-84: a lane down to the water when it is near (the watering place,
        // the wash, the trades that need it), from the main road's point
        // nearest it, the shortest dry way
        {
            double bestD = 1e18;
            Vec2 from;
            for (const Vec2& p : main) {
                const double d = near.water(p);
                if (d < bestD && dist(p, plan.square) > plan.squareR + 6) bestD = d, from = p;
            }
            if (bestD > 6.0 && bestD < radius * 1.2) {
                double bestA = 0, bestL = 1e18;
                for (int k = 0; k < 32; ++k) {
                    const double a = k * kPi / 16;
                    for (double t = 4; t < bestD * 1.6 + 8; t += 4)
                        if (wetAt({from.x + std::cos(a) * t, from.y + std::sin(a) * t})) {
                            if (t < bestL) bestL = t, bestA = a;
                            break;
                        }
                }
                if (bestL < 1e17 && layRoad(from, bestA, bestL, 2.5, "lane", 0.3)) waterLane = int(plan.roads.size()) - 1;
            }
        }
        // lanes off the main road, alternating sides, as many as the households need
        const int lanes = std::clamp(nLots / 4 - (waterLane ? 1 : 0), 0, 4);
        for (int k = 0; k < lanes && main.size() > 2; ++k) {
            const std::size_t i = std::clamp<std::size_t>(
                std::size_t((0.25 + 0.5 * (k + 0.5) / std::max(1, lanes)) * double(main.size() - 1)), 1, main.size() - 2);
            const Vec2 a = main[i - 1], b = main[i + 1];
            const double along = std::atan2(b.y - a.y, b.x - a.x);
            if (dist(main[i], plan.square) < plan.squareR + 10) continue;
            layRoad(main[i], along + (k % 2 ? -kPi / 2 : kPi / 2) + rng.range(-0.2, 0.2), radius * rng.range(0.45, 0.7),
                    2.5, "lane");
        }
        // AL-84: the main road goes on out of the village both ways (the way
        // travellers come in); the way in is the end facing the map's middle,
        // else the longer one.
        plan.entrance = main.back();
        if (lay.roadOut && main.size() >= 2) {
            const Vec2 mapMid{map.sizeM * 0.5, map.sizeM * 0.5};
            double bestE = -1e18;
            for (int end = 0; end < 2; ++end) {
                const Vec2 tip = end ? main.back() : main.front();
                const Vec2 prev = end ? main[main.size() - 2] : main[1];
                const double a = std::atan2(tip.y - prev.y, tip.x - prev.x);
                const std::size_t k = plan.roads.size();
                const double reach = site.loaded() ? (site.n - 1) * site.cell : radius * 3.0;
                layRoad(tip, a, reach, 3.0, "out", 0.45);
                const double len = plan.roads.size() > k ? double(plan.roads[k].size()) * 8.0 : 0.0;
                const Vec2 toMid{mapMid.x - tip.x, mapMid.y - tip.y};
                const double faces = (std::cos(a) * toMid.x + std::sin(a) * toMid.y) / std::max(1.0, std::hypot(toMid.x, toMid.y));
                const double e = faces * 100.0 + len * 0.1;
                if (e > bestE) bestE = e, plan.entrance = tip;
            }
        }
    }

    // Shared places round the square, off the road.
    for (int k = 0; k < want.squareSpots; ++k) {
        const double a = plan.yaw + kPi / 2 + (k - (want.squareSpots - 1) * 0.5) * 0.8 + (k % 2 ? kPi : 0.0);
        Vec2 p{plan.square.x + std::cos(a) * plan.squareR * 0.75, plan.square.y + std::sin(a) * plan.squareR * 0.75};
        if (distToPolyline(p, plan.roads[0]) < 3.5) p = add(p, mul({std::cos(a), std::sin(a)}, 4.0));
        plan.squareSpots.push_back(p);
    }

    // Lots: along every street on both sides, a house facing the street with
    // its yard behind. AL-76: each lot as asked (its size, its zone): the
    // square's first (nearest the square), then the main road's, the lanes',
    // the edge's (the farthest out); the house turned by quarters to face the
    // road as near as it can and set on the build grid. AL-84: in rows (a lot
    // likes a neighbour), by what it needs (the water, the wood, the way in,
    // downwind) and a nuisance away from the quiet houses and the square.
    std::vector<PlanRect> taken;
    auto clearOfRoads = [&](const PlanRect& r) {
        for (std::size_t ri = 0; ri < plan.roads.size(); ++ri)
            for (double u : {-1.0, -0.5, 0.0, 0.5, 1.0})
                for (double v : {-1.0, -0.5, 0.0, 0.5, 1.0})
                    if (distToPolyline(r.at(u * r.hw, v * r.hd), plan.roads[ri]) < plan.roadWidth[ri] * 0.5 + 0.8)
                        return false;
        return true;
    };
    auto groundOk = [&](const PlanRect& r, double& mean, double* top = nullptr, double relief = -1) {
        double lo = 1e9, hi = -1e9, sum = 0;
        int cnt = 0;
        // every few metres (a long strip must not straddle a stream)
        const int nu = std::max(2, int(std::ceil(r.hw / 4.0))), nv = std::max(2, int(std::ceil(r.hd / 4.0)));
        for (int iu = 0; iu <= nu; ++iu)
            for (int iv = 0; iv <= nv; ++iv) {
                const double u = -1.0 + 2.0 * iu / nu, v = -1.0 + 2.0 * iv / nv;
                const Vec2 q = r.at(u * r.hw, v * r.hd);
                if (!usable(q)) return false;
                const double h = site.height(q);
                lo = std::min(lo, h), hi = std::max(hi, h), sum += h, ++cnt;
            }
        mean = sum / cnt;
        if (top) *top = hi;
        return hi - lo <= (relief > 0 ? relief : want.maxRelief);
    };
    auto hitsTaken = [&](const PlanRect& r, double margin) {
        for (const auto& t : taken)
            if (r.overlaps(t, margin)) return true;
        return false;
    };
    // where a lot may front a street: a point on it, the way out to the lot
    struct Anchor {
        Vec2 p, out;
        int road;
    };
    std::vector<Anchor> anchors;
    for (std::size_t ri = 0; ri < plan.roads.size(); ++ri) {
        if (plan.roadKind[ri] != "main" && plan.roadKind[ri] != "lane") continue;
        const auto& line = plan.roads[ri];
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            const Vec2 a = line[i], b = line[i + 1];
            const double len = dist(a, b);
            if (len < 1e-6) continue;
            const Vec2 u{(b.x - a.x) / len, (b.y - a.y) / len};
            for (double t = 0; t < len; t += 2.0)
                for (int sgn = -1; sgn <= 1; sgn += 2) anchors.push_back({add(a, mul(u, t)), {-u.y * sgn, u.x * sgn}, int(ri)});
        }
    }
    const double c = want.cellM;
    auto stripHalf = [&](double houseHw) { return std::clamp(houseHw + lay.rowGap, lay.stripLo * 0.5, lay.stripHi * 0.5); };
    auto snap = [&](double v) { return std::round(v / c) * c; };
    // the lot of spec L at anchor A, `extra` metres further from the road
    auto lotAt = [&](const LotSpec& L, const Anchor& A, double extra) {
        VillageLot lot;
        lot.road = A.road;
        // front = -out, turned to the nearest quarter
        const double yaw = std::atan2(-A.out.x, A.out.y);
        const int q = int(std::lround(yaw / (kPi / 2))) & 3;
        lot.house.yaw = q * kPi / 2;
        lot.house.hw = L.w * c * 0.5;
        lot.house.hd = L.d * c * 0.5;
        const Vec2 back = mul(lot.house.front(), -1.0);
        Vec2 ctr = add(A.p, mul(A.out, plan.roadWidth[std::size_t(A.road)] * 0.5 + 2.0 + extra));
        ctr = add(ctr, mul(back, lot.house.hd));
        // the corner on the grid: the house's cells are the world's
        const double ex = (q & 1) ? lot.house.hd : lot.house.hw, ey = (q & 1) ? lot.house.hw : lot.house.hd;
        lot.house.c = {snap(ctr.x - ex) + ex, snap(ctr.y - ey) + ey};
        lot.yard = lot.house;
        lot.yard.hd = std::max(1.0, L.yardD * 0.5);
        // AL-84: a home's plot is as wide as its field strip, so the strips lie
        // side by side straight behind the houses of a row
        if (L.home && lay.fieldArea > 0) lot.yard.hw = stripHalf(lot.house.hw);
        lot.yard.c = add(lot.house.c, mul(back, lot.house.hd + lot.yard.hd));
        return lot;
    };
    const PlanRect squareRect{plan.square, 0, plan.squareR, plan.squareR};
    std::vector<int> placedIdx;   // lots placed so far (their quiet / nuisance)
    plan.lots.assign(std::size_t(nLots), VillageLot{});
    for (auto& L : plan.lots) L.placed = false;
    auto placeLot = [&](const LotSpec& L, VillageLot& out) {
        double bestScore = 1e18;
        bool found = false;
        for (const Anchor& A : anchors) {
            const double d0 = dist(A.p, plan.square);
            if (L.zone == "edge" && d0 > radius) continue;
            for (double extra : {0.0, 2.0, 4.0}) {
                VillageLot lot = lotAt(L, A, extra);
                const Vec2 hc = lot.house.c;
                const double d = dist(hc, plan.square);
                double score = d + rng.range(0, 3) + extra;
                if (L.zone == "road") score += plan.roadKind[std::size_t(A.road)] != "main" ? 80.0 : 0.0;
                else if (L.zone == "lane") score += A.road == 0 ? 20.0 : 0.0;
                else if (L.zone == "edge") score = -std::min(d, radius * 0.6) + rng.range(0, 3) + extra;   // out of the middle, then by its needs
                // AL-84: what the household needs from the site
                if (L.water > 0) score += L.water * std::min(near.water(hc), 400.0);
                if (L.wood > 0) score += L.wood * std::min(near.wood(hc), 400.0);
                if (L.entrance > 0) score += L.entrance * dist(hc, plan.entrance);
                if (L.downwind > 0) score -= L.downwind * ((hc.x - plan.square.x) * downwind.x + (hc.y - plan.square.y) * downwind.y);
                if (L.nuisance && d < lay.nuisanceR) score += 200.0;
                const double reachRow = std::max(lot.house.hw, lot.house.hd) + lay.rowGap + 3.0;
                bool neighbour = false;
                for (int k : placedIdx) {
                    const VillageLot& o = plan.lots[std::size_t(k)];
                    const LotSpec& os = want.lots[std::size_t(k)];
                    const double od = dist(hc, o.house.c);
                    if ((L.nuisance && os.quiet) || (L.quiet && os.nuisance))
                        if (od < lay.nuisanceR) score += 200.0;
                    if (o.road == lot.road && std::fabs(o.house.yaw - lot.house.yaw) < 1e-6 &&
                        od < reachRow + std::max(o.house.hw, o.house.hd))
                        neighbour = true;
                }
                if (neighbour) score -= lay.rowBonus;
                if (score >= bestScore) continue;
                if (d < plan.squareR + std::min(lot.house.hw, lot.house.hd) + 2) continue;
                double mean = 0, top = 0;
                if (!groundOk(lot.house, mean, &top) || !clearOfRoads(lot.house) || !clearOfRoads(lot.yard)) continue;
                if (lot.house.overlaps(squareRect, 2.0) || lot.yard.overlaps(squareRect, 1.0)) continue;
                bool hit = false;
                for (const auto& t : taken)
                    if (lot.house.overlaps(t, lay.rowGap) || lot.yard.overlaps(t, 1.0)) hit = true;
                if (hit) continue;
                lot.ground = mean;
                lot.groundMax = top;
                bestScore = score;
                out = lot;
                found = true;
                break;   // further from the road only when this spot fails
            }
        }
        return found;
    };
    static const char* kZones[] = {"square", "road", "lane", "edge"};
    std::vector<int> order;
    for (const char* z : kZones)
        for (int k = 0; k < nLots; ++k)
            if (want.lots[std::size_t(k)].zone == z) order.push_back(k);
    for (int k = 0; k < nLots; ++k)
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    for (int k : order) {
        VillageLot best;
        if (!placeLot(want.lots[std::size_t(k)], best)) continue;
        best.placed = true;
        taken.push_back(best.house);
        taken.push_back(best.yard);
        plan.lots[std::size_t(k)] = best;
        placedIdx.push_back(k);
    }
    int placedN = 0;
    for (const auto& L : plan.lots) placedN += L.placed;
    if (placedN < nLots)
        plan.notes.push_back("only " + std::to_string(placedN) + " of " + std::to_string(nLots) + " lots fit");

    // Fields: beyond each home's yard, else further out the same way, else
    // shifted along. AL-84: a strip as wide as the lot's frontage and as long
    // as its area needs (the long fields behind a street village).
    // AL-84: the households farthest out first (they have the room behind
    // them); a home in the crowded middle gets its strip beside another's, so
    // the strips lie in blocks (furlongs) round the village, not one by one.
    std::vector<std::size_t> homes;
    for (std::size_t i = 0; i < plan.lots.size(); ++i)
        if (plan.lots[i].placed && want.lots[i].home) homes.push_back(i);
    std::stable_sort(homes.begin(), homes.end(), [&](std::size_t x, std::size_t y) {
        return dist(plan.lots[x].house.c, plan.square) > dist(plan.lots[y].house.c, plan.square);
    });
    auto dryWay = [&](Vec2 a, Vec2 b) {   // no water between (a field is not across the river)
        const double l = dist(a, b);
        for (double t = 0; t < l; t += 4.0)
            if (wetAt(add(a, mul({(b.x - a.x) / l, (b.y - a.y) / l}, t)))) return false;
        return true;
    };
    const VillageLot* fieldFor = nullptr;
    auto fieldOk = [&](const PlanRect& f) {
        if (fieldFor && !dryWay(fieldFor->house.c, f.c)) return false;
        double mean = 0;
        return groundOk(f, mean, nullptr, want.maxRelief * 3) && clearOfRoads(f) && !hitsTaken(f, 1.5) &&
               !f.overlaps(squareRect, 4.0);
    };
    for (std::size_t i : homes) {
        if (int(plan.fields.size()) >= want.fields) break;
        VillageLot& L = plan.lots[i];
        const Vec2 out = mul(L.house.front(), -1.0);
        fieldFor = &L;
        PlanRect proto;
        proto.yaw = L.house.yaw;
        if (lay.fieldArea > 0) {
            proto.hw = stripHalf(L.house.hw);
            proto.hd = lay.fieldArea / (4.0 * proto.hw);
        } else {
            proto.hw = want.fieldW * 0.5;
            proto.hd = want.fieldD * 0.5;
        }
        bool placed = false;
        PlanRect got;
        // straight behind the yard
        for (double extra = 1; extra < 20 && !placed; extra += 6)
            for (double shift : {0.0, 6.0, -6.0}) {
                PlanRect f = proto;
                f.c = add(add(L.yard.c, mul(out, L.yard.hd + extra + f.hd)), mul(L.house.side(), shift));
                if (fieldOk(f)) {
                    got = f, placed = true;
                    break;
                }
            }
        // beside a strip already laid (the nearest such spot to the house)
        if (!placed) {
            double bestD = 1e18;
            for (const PlanRect& F : plan.fields)
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    PlanRect f = proto;
                    f.yaw = F.yaw;
                    f.c = add(F.c, mul(F.side(), sgn * (F.hw + f.hw + 1.0)));
                    f.c = add(f.c, mul(F.front(), f.hd - F.hd));   // the inner ends in a line
                    const double d = dist(f.c, L.house.c);
                    if (d < bestD && fieldOk(f)) bestD = d, got = f, placed = true;
                }
        }
        // else further out the same way, shifted along
        for (double extra = 20; extra < 160 && !placed; extra += 8)
            for (double shift : {0.0, 12.0, -12.0, 24.0, -24.0, 40.0, -40.0}) {
                PlanRect f = proto;
                f.c = add(add(L.yard.c, mul(out, L.yard.hd + extra + f.hd)), mul(L.house.side(), shift));
                if (fieldOk(f)) {
                    got = f, placed = true;
                    break;
                }
            }
        // else anywhere round the village, its length pointing away from the
        // middle, the nearest such spot to the house
        if (!placed) {
            double bestD = 1e18;
            for (int k = 0; k < 48 * 8; ++k) {
                const double a = (k % 48) * kPi / 24, r = radius * 0.4 + (k / 48) * 14.0;
                PlanRect f = proto;
                f.yaw = std::fmod(std::round((a + kPi / 2) / (kPi / 2)) * (kPi / 2) + 4 * kPi, 2 * kPi);
                f.c = {plan.square.x + std::cos(a) * (r + f.hd), plan.square.y + std::sin(a) * (r + f.hd)};
                const double d = dist(f.c, L.house.c);
                if (d < bestD && fieldOk(f)) bestD = d, got = f, placed = true;
            }
        }
        if (!placed) {
            plan.notes.push_back("no field for lot " + std::to_string(i));
            continue;
        }
        taken.push_back(got);
        L.field = int(plan.fields.size());
        plan.fields.push_back(got);
        plan.zones.push_back({"field", got});
    }

    // AL-84: the commons: the churchyard behind the chapel, the pasture by the
    // water, the village's wood at the forest's edge; the nearest good spot.
    for (const auto& cm : lay.commons) {
        PlanRect proto;
        proto.hw = cm.w * 0.5;
        proto.hd = cm.d * 0.5;
        bool placed = false;
        if (cm.by.rfind("behind:", 0) == 0) {
            const std::string who = cm.by.substr(7);
            for (std::size_t i = 0; i < plan.lots.size() && !placed; ++i) {
                if (!plan.lots[i].placed || want.lots[i].recipe != who) continue;
                const VillageLot& L = plan.lots[i];
                const Vec2 out = mul(L.house.front(), -1.0);
                proto.yaw = L.house.yaw;
                for (double extra = 1; extra < 40 && !placed; extra += 3)
                    for (double shift : {0.0, 6.0, -6.0, 12.0, -12.0}) {
                        PlanRect f = proto;
                        f.c = add(add(L.house.c, mul(out, L.house.hd + extra + f.hd)), mul(L.house.side(), shift));
                        double mean = 0;
                        if (!groundOk(f, mean, nullptr, want.maxRelief * 2) || !clearOfRoads(f)) continue;
                        bool hit = false;
                        for (const auto& t : taken)   // its own yard is where the churchyard goes
                            if (dist(t.c, L.yard.c) > 1e-6 && f.overlaps(t, 1.0)) hit = true;
                        if (hit) continue;
                        taken.push_back(f);
                        plan.zones.push_back({cm.kind, f});
                        placed = true;
                        break;
                    }
                // no room behind it: the nearest free ground by it
                double bestD = 1e18;
                PlanRect best;
                for (int k = 0; k < 24 * 10 && !placed; ++k) {
                    const double a = (k % 24) * kPi / 12, r = std::max(L.house.hw, L.house.hd) + 4.0 + (k / 24) * 5.0;
                    PlanRect f = proto;
                    f.c = {L.house.c.x + std::cos(a) * (r + f.hd), L.house.c.y + std::sin(a) * (r + f.hd)};
                    const double d = dist(f.c, L.house.c);
                    double mean = 0;
                    if (d >= bestD || !groundOk(f, mean, nullptr, want.maxRelief * 2) || !clearOfRoads(f) ||
                        hitsTaken(f, 1.0) || f.overlaps(squareRect, 2.0))
                        continue;
                    bestD = d, best = f;
                }
                if (!placed && bestD < 1e17) {
                    taken.push_back(best);
                    plan.zones.push_back({cm.kind, best});
                    placed = true;
                }
            }
        } else {
            double bestS = 1e18;
            PlanRect best;
            proto.yaw = std::fmod(std::round(plan.yaw / (kPi / 2)) * (kPi / 2) + 4 * kPi, 2 * kPi);
            for (int k = 0; k < 360; ++k) {
                const double a = k * 2 * kPi / 36 + (k / 36) * 0.17, r = radius * (0.5 + 0.18 * (k / 36));
                PlanRect f = proto;
                f.c = {plan.square.x + std::cos(a) * r, plan.square.y + std::sin(a) * r};
                double s = dist(f.c, plan.square) * 0.15;
                const bool byWood = cm.by.find("wood") != std::string::npos;
                if (cm.by.find("water") != std::string::npos) s += near.water(f.c);
                if (byWood) s += near.wood(f.c);
                if (cm.by.find("downwind") != std::string::npos)
                    s -= (f.c.x - plan.square.x) * downwind.x + (f.c.y - plan.square.y) * downwind.y;
                if (s >= bestS) continue;
                if (cm.awayFromHomes > 0) {
                    bool close = false;
                    for (const auto& L : plan.lots)
                        if (L.placed && dist(L.house.c, f.c) < cm.awayFromHomes) close = true;
                    if (close) continue;
                }
                double mean = 0;
                if (byWood) {   // in the trees: only dry ground under it
                    bool ok = true;
                    for (double u : {-1.0, 0.0, 1.0})
                        for (double v : {-1.0, 0.0, 1.0})
                            if (!usable(f.at(u * f.hw, v * f.hd))) ok = false;
                    if (!ok) continue;
                } else if (!groundOk(f, mean, nullptr, want.maxRelief * 4)) continue;
                if (!clearOfRoads(f) || hitsTaken(f, 3.0) || f.overlaps(squareRect, 4.0)) continue;
                if (!dryWay(plan.square, f.c)) continue;   // reached from the square without crossing water
                bestS = s, best = f, placed = true;
            }
            if (placed) {
                taken.push_back(best);
                plan.zones.push_back({cm.kind, best});
            }
        }
        if (!placed) plan.notes.push_back("no room for the " + cm.kind);
    }

    // AL-84: lots kept free along the streets for the households to come.
    {
        LotSpec spare;
        double w = 0, d = 0;
        int homes = 0;
        for (const auto& L : want.lots)
            if (L.home) w += L.w, d += L.d, ++homes;
        spare.w = homes ? int(std::lround(w / homes)) : 7;
        spare.d = homes ? int(std::lround(d / homes)) : 6;
        spare.yardD = want.yardD;
        for (int k = 0; k < lay.spareLots; ++k) {
            VillageLot lot;
            if (!placeLot(spare, lot)) break;
            taken.push_back(lot.house);
            taken.push_back(lot.yard);
            PlanRect r = lot.house;
            r.hd = lot.house.hd + lot.yard.hd;
            r.c = add(lot.house.c, mul(lot.house.front(), -lot.yard.hd));
            plan.zones.push_back({"spare", r});
        }
    }

    // AL-84: a lane goes as far as its last house (or its fields' gates) and no
    // further; a lane nobody lives on goes.
    for (std::size_t r = 1; r < plan.roads.size(); ++r) {
        if (plan.roadKind[r] != "lane" || int(r) == waterLane) continue;   // the way to the water stays whole
        auto& line = plan.roads[r];
        double reach = 0;   // along the lane, metres
        for (const auto& L : plan.lots)
            if (L.placed && L.road == int(r)) {
                double acc = 0, best = 1e18, at = 0;
                for (std::size_t i = 0; i + 1 < line.size(); ++i) {
                    const double seg = dist(line[i], line[i + 1]);
                    const double dd = distToPolyline(L.house.c, {line[i], line[i + 1]});
                    if (dd < best) {
                        best = dd;
                        const Vec2 q = closestOnPolyline(L.house.c, {line[i], line[i + 1]});
                        at = acc + dist(line[i], q);
                    }
                    acc += seg;
                }
                reach = std::max(reach, at + std::max(L.house.hw, L.house.hd) + 4.0);
            }
        for (const auto& z : plan.zones)
            if (z.kind == "spare" && distToPolyline(z.rect.c, line) < z.rect.hd + 8.0)
                reach = std::max(reach, 1e9);   // a spare lot on it keeps it whole
        std::vector<Vec2> cut{line.front()};
        double acc = 0;
        for (std::size_t i = 0; i + 1 < line.size() && acc < reach; ++i) {
            const double seg = dist(line[i], line[i + 1]);
            if (acc + seg <= reach) cut.push_back(line[i + 1]);
            else cut.push_back(add(line[i], mul({(line[i + 1].x - line[i].x) / seg, (line[i + 1].y - line[i].y) / seg}, reach - acc)));
            acc += seg;
        }
        if (reach <= 0) cut.clear();
        line = cut;
    }
    for (std::size_t r = plan.roads.size(); r-- > 1;)
        if (plan.roads[r].size() < 2) {
            for (auto& L : plan.lots)
                if (L.road > int(r)) --L.road;
            plan.roads.erase(plan.roads.begin() + std::ptrdiff_t(r));
            plan.roadWidth.erase(plan.roadWidth.begin() + std::ptrdiff_t(r));
            plan.roadKind.erase(plan.roadKind.begin() + std::ptrdiff_t(r));
        }

    // AL-84: footpaths: from each door to its street, from each yard's gate to
    // its field.
    for (std::size_t i = 0; i < plan.lots.size(); ++i) {
        const VillageLot& L = plan.lots[i];
        if (!L.placed) continue;
        addPath(plan, L.house.at(0, L.house.hd + 0.3), nullptr, lay.pathW);
        if (L.field >= 0) {   // the gate to the field when it is behind; else the field to the nearest road
            const PlanRect& f = plan.fields[std::size_t(L.field)];
            const Vec2 gate = L.yard.at(0, -L.yard.hd);
            const Vec2 edge = closestOnPolyline(gate, {f.at(-f.hw, f.hd), f.at(f.hw, f.hd)});
            if (dist(gate, edge) < 25.0) addPath(plan, gate, &edge, lay.pathW);
            else addPath(plan, f.at(0, f.hd), nullptr, lay.pathW, true);
        }
    }
    return plan;
}

std::vector<std::string> checkVillage(const VillagePlan& plan, const VillageSite& site, const MapGrid& map,
                                      const VillageWanted& want) {
    std::vector<std::string> bad;
    auto wetAt = [&](Vec2 p) { return site.water(p) || (map.w > 0 && map.waterAt(p)); };
    auto street = [&](std::size_t r) {
        const std::string k = r < plan.roadKind.size() ? plan.roadKind[r] : std::string("main");
        return k == "main" || k == "lane";
    };
    // the streets meet: every lane starts on the main road
    for (std::size_t r = 1; r < plan.roads.size(); ++r)
        if (street(r) && !plan.roads[r].empty() && distToPolyline(plan.roads[r].front(), plan.roads[0]) > 1.0)
            bad.push_back("lane " + std::to_string(r) + " does not meet the main road");
    for (std::size_t i = 0; i < plan.lots.size(); ++i) {
        const VillageLot& L = plan.lots[i];
        if (!L.placed) continue;
        const std::string id = "lot " + std::to_string(i) + (i < want.lots.size() && !want.lots[i].recipe.empty() ? " (" + want.lots[i].recipe + ")" : "");
        double lo = 1e9, hi = -1e9;
        for (double u : {-1.0, 0.0, 1.0})
            for (double v : {-1.0, 0.0, 1.0}) {
                const Vec2 q = L.house.at(u * L.house.hw, v * L.house.hd);
                if (wetAt(q)) bad.push_back(id + " stands in water");
                lo = std::min(lo, site.height(q)), hi = std::max(hi, site.height(q));
            }
        if (hi - lo > want.maxRelief + 1e-6) bad.push_back(id + " on too steep ground");
        // the door reaches a street, straight or by its path
        const Vec2 door = L.house.at(0, L.house.hd + 0.3);
        double best = 1e18;
        for (std::size_t r = 0; r < plan.roads.size(); ++r)
            best = std::min(best, distToPolyline(door, plan.roads[r]) - plan.roadWidth[r] * 0.5);
        if (best > 1.6) bad.push_back(id + " has no way to the street");
        for (std::size_t j = i + 1; j < plan.lots.size(); ++j)
            if (plan.lots[j].placed && L.house.overlaps(plan.lots[j].house, 0.5)) bad.push_back(id + " overlaps lot " + std::to_string(j));
        if (i < want.lots.size() && want.lots[i].nuisance) {
            if (dist(L.house.c, plan.square) < want.layout.nuisanceR) bad.push_back(id + " (a nuisance) by the square");
            for (std::size_t j = 0; j < plan.lots.size(); ++j)
                if (j < want.lots.size() && plan.lots[j].placed && want.lots[j].quiet &&
                    dist(L.house.c, plan.lots[j].house.c) < want.layout.nuisanceR)
                    bad.push_back(id + " (a nuisance) by quiet lot " + std::to_string(j));
        }
    }
    for (const auto& z : plan.zones) {
        if (z.kind == "woodlot") continue;
        for (std::size_t r = 0; r < plan.roads.size(); ++r) {
            if (!street(r) && plan.roadKind[r] != "out") continue;
            if (distToPolyline(z.rect.c, plan.roads[r]) < plan.roadWidth[r] * 0.5 + std::min(z.rect.hw, z.rect.hd) * 0.5)
                bad.push_back(z.kind + " on a road");
        }
    }
    return bad;
}

}  // namespace lr::sim
