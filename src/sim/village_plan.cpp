#include "sim/village_plan.hpp"

#include <algorithm>
#include <cmath>

#include "sim/world.hpp"

namespace altera::sim {
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
VillagePlan planVillage(const VillageSite& site, const MapGrid& map, Vec2 centre, const VillageWanted& want,
                        std::uint64_t seed) {
    VillagePlan plan;
    Rng rng(hashCombine(seed, hashStr("village_plan")));
    auto inMap = [&](Vec2 p) { return p.x > 2 && p.y > 2 && p.x < map.sizeM - 2 && p.y < map.sizeM - 2; };
    auto wetAt = [&](Vec2 p) { return site.water(p) || (map.w > 0 && map.waterAt(p)); };
    auto usable = [&](Vec2 p) { return inMap(p) && site.inside(p) && !wetAt(p); };
    auto slopeAlong = [&](Vec2 a, Vec2 b) {
        const double l = dist(a, b);
        return l > 0 ? std::fabs(site.height(b) - site.height(a)) / l : 0.0;
    };

    // the radius grows with the households (lots on both sides of the roads)
    const double lotStep = want.houseW + 4.0;
    const double radius = std::clamp(35.0 + lotStep * 0.55 * want.houses, 50.0, 170.0);

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

    // A road from the square out to `len` metres along direction a, bending a
    // little and stopping at water or the edge of the site.
    auto layRoad = [&](Vec2 from, double a, double len, double width) {
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
            if (bestS > 1e17 || bestS > 0.35) break;
            heading = bestA;
            p = {p.x + std::cos(heading) * 8, p.y + std::sin(heading) * 8};
            line.push_back(p);
        }
        if (line.size() >= 2) {
            plan.roads.push_back(line);
            plan.roadWidth.push_back(width);
        }
    };
    {
        // both halves of the main road start at the square's edge
        const Vec2 dir{std::cos(plan.yaw), std::sin(plan.yaw)};
        std::vector<Vec2> both;
        const std::size_t before = plan.roads.size();
        layRoad(plan.square, plan.yaw + kPi, radius, 4.0);
        layRoad(plan.square, plan.yaw, radius, 4.0);
        if (plan.roads.size() == before + 2) {
            // one polyline end to end through the square
            std::vector<Vec2> a = plan.roads[before], b = plan.roads[before + 1];
            std::reverse(a.begin(), a.end());
            a.insert(a.end(), b.begin() + 1, b.end());
            plan.roads.resize(before);
            plan.roadWidth.resize(before);
            plan.roads.push_back(a);
            plan.roadWidth.push_back(4.0);
        } else if (plan.roads.empty()) {
            plan.roads.push_back({add(plan.square, mul(dir, -8)), add(plan.square, mul(dir, 8))});
            plan.roadWidth.push_back(4.0);
            plan.notes.push_back("main road blocked");
        }
        (void)both;
        // lanes off the main road, alternating sides, as many as the households need
        const int lanes = std::clamp(want.houses / 4, 0, 4);
        const std::vector<Vec2> main = plan.roads[0];
        for (int k = 0; k < lanes && main.size() > 2; ++k) {
            const std::size_t i = std::clamp<std::size_t>(
                std::size_t((0.25 + 0.5 * (k + 0.5) / std::max(1, lanes)) * double(main.size() - 1)), 1, main.size() - 2);
            const Vec2 a = main[i - 1], b = main[i + 1];
            const double along = std::atan2(b.y - a.y, b.x - a.x);
            if (dist(main[i], plan.square) < plan.squareR + 10) continue;
            layRoad(main[i], along + (k % 2 ? -kPi / 2 : kPi / 2) + rng.range(-0.2, 0.2), radius * rng.range(0.45, 0.7),
                    2.5);
        }
    }

    // Shared places round the square, off the road.
    for (int k = 0; k < want.squareSpots; ++k) {
        const double a = plan.yaw + kPi / 2 + (k - (want.squareSpots - 1) * 0.5) * 0.8 + (k % 2 ? kPi : 0.0);
        Vec2 p{plan.square.x + std::cos(a) * plan.squareR * 0.75, plan.square.y + std::sin(a) * plan.squareR * 0.75};
        if (distToPolyline(p, plan.roads[0]) < 3.5) p = add(p, mul({std::cos(a), std::sin(a)}, 4.0));
        plan.squareSpots.push_back(p);
    }

    // Lots: along every road on both sides, a house facing the road with its
    // yard behind; the nearest to the square are taken first.
    std::vector<PlanRect> taken;
    auto clearOfRoads = [&](const PlanRect& r) {
        for (std::size_t ri = 0; ri < plan.roads.size(); ++ri)
            for (double u : {-1.0, -0.5, 0.0, 0.5, 1.0})
                for (double v : {-1.0, 0.0, 1.0})
                    if (distToPolyline(r.at(u * r.hw, v * r.hd), plan.roads[ri]) < plan.roadWidth[ri] * 0.5 + 0.8)
                        return false;
        return true;
    };
    auto groundOk = [&](const PlanRect& r, double& mean) {
        double lo = 1e9, hi = -1e9, sum = 0;
        int cnt = 0;
        for (double u : {-1.0, 0.0, 1.0})
            for (double v : {-1.0, 0.0, 1.0}) {
                const Vec2 q = r.at(u * r.hw, v * r.hd);
                if (!usable(q)) return false;
                const double h = site.height(q);
                lo = std::min(lo, h), hi = std::max(hi, h), sum += h, ++cnt;
            }
        mean = sum / cnt;
        return hi - lo <= want.maxRelief;
    };
    struct Cand {
        VillageLot lot;
        double d;
    };
    std::vector<Cand> cands;
    for (std::size_t ri = 0; ri < plan.roads.size(); ++ri) {
        const auto& line = plan.roads[ri];
        double carried = 0;
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            const Vec2 a = line[i], b = line[i + 1];
            const double len = dist(a, b);
            const Vec2 u{(b.x - a.x) / len, (b.y - a.y) / len};
            for (double t = carried; t < len; t += lotStep) {
                const Vec2 p = add(a, mul(u, t));
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    const Vec2 out{-u.y * sgn, u.x * sgn};   // from the road to the lot
                    VillageLot L;
                    L.road = int(ri);
                    L.house.hw = want.houseW * 0.5;
                    L.house.hd = want.houseD * 0.5;
                    L.house.c = add(p, mul(out, plan.roadWidth[ri] * 0.5 + 2.0 + L.house.hd));
                    // the front faces the road: front = -out
                    L.house.yaw = std::atan2(-out.x, out.y);
                    L.yard = L.house;
                    L.yard.hd = want.yardD * 0.5;
                    L.yard.c = add(L.house.c, mul(out, L.house.hd + L.yard.hd));
                    if (dist(L.house.c, plan.square) < plan.squareR + L.house.hw + 2) continue;
                    cands.push_back({L, dist(L.house.c, plan.square) + rng.range(0, 3)});
                }
            }
            carried = std::fmod(carried + len, lotStep);
            carried = carried > 0 ? lotStep - carried : 0;
            if (carried >= lotStep) carried = 0;
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    const PlanRect squareRect{plan.square, 0, plan.squareR, plan.squareR};
    for (auto& c : cands) {
        if (int(plan.lots.size()) >= want.houses) break;
        double mean = 0;
        if (!groundOk(c.lot.house, mean) || !clearOfRoads(c.lot.house) || !clearOfRoads(c.lot.yard)) continue;
        if (c.lot.house.overlaps(squareRect, 2.0)) continue;
        bool hit = false;
        for (const auto& t : taken)
            if (c.lot.house.overlaps(t, 3.0) || c.lot.yard.overlaps(t, 1.0)) hit = true;
        if (hit) continue;
        c.lot.ground = mean;
        taken.push_back(c.lot.house);
        taken.push_back(c.lot.yard);
        plan.lots.push_back(c.lot);
    }
    if (int(plan.lots.size()) < want.houses)
        plan.notes.push_back("only " + std::to_string(plan.lots.size()) + " of " + std::to_string(want.houses) +
                             " lots fit");

    // Fields: beyond each household's yard, else further out the same way,
    // else anywhere round the village edge.
    for (std::size_t i = 0; i < plan.lots.size() && int(plan.fields.size()) < want.fields; ++i) {
        const VillageLot& L = plan.lots[i];
        const Vec2 out = mul(L.house.front(), -1.0);
        bool placed = false;
        for (double extra = 4; extra < 140 && !placed; extra += 12)
            for (double shift : {0.0, 20.0, -20.0, 40.0, -40.0}) {
                PlanRect f;
                f.yaw = L.house.yaw;
                f.hw = want.fieldW * 0.5;
                f.hd = want.fieldD * 0.5;
                f.c = add(add(L.yard.c, mul(out, L.yard.hd + extra + f.hd)), mul(L.house.side(), shift));
                double mean = 0;
                if (!groundOk(f, mean) || !clearOfRoads(f)) continue;
                bool hit = false;
                for (const auto& t : taken)
                    if (f.overlaps(t, 2.0)) hit = true;
                if (hit) continue;
                taken.push_back(f);
                plan.fields.push_back(f);
                placed = true;
                break;
            }
        if (!placed) plan.notes.push_back("no field for lot " + std::to_string(i));
    }
    return plan;
}

}  // namespace altera::sim
