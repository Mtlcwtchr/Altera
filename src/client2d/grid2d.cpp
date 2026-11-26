#include "client2d/grid2d.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "sim/building.hpp"
#include "sim/village_plan.hpp"

namespace lr::client2d {

using namespace lr::sim;

const std::vector<std::string>& groundNames() {
    static const std::vector<std::string> n = {"grass", "meadow", "forest", "marsh", "water",
                                               "sand",  "dirt",   "road",   "field", "floor"};
    return n;
}

Ground groundFromName(const std::string& id, Ground fallback) {
    const auto& n = groundNames();
    for (std::size_t i = 0; i < n.size(); ++i)
        if (n[i] == id) return Ground(i);
    return fallback;
}

GroundRules GroundRules::defaults() {
    GroundRules r;
    r.biome = {{"meadow", Ground::meadow}, {"forest", Ground::forest}, {"marsh", Ground::marsh},
               {"steppe", Ground::grass},  {"desert", Ground::sand},   {"mountains", Ground::dirt}};
    r.zone = {{"field", Ground::field}, {"work", Ground::dirt}};
    r.road = {{"main", Ground::road}, {"lane", Ground::road}, {"out", Ground::dirt}, {"path", Ground::dirt}};
    return r;
}

GroundRules GroundRules::fromJson(const Json& j) {
    GroundRules r = defaults();
    auto readMap = [&](const char* key, std::map<std::string, Ground>& into) {
        if (!j.contains(key) || !j[key].is_object()) return;
        for (const auto& [k, v] : j[key].items())
            if (v.is_string()) into[k] = groundFromName(v.get<std::string>(), Ground::grass);
    };
    readMap("biome", r.biome);
    readMap("zone", r.zone);
    readMap("road", r.road);
    if (j.contains("square")) r.square = groundFromName(j.value("square", std::string("road")), Ground::road);
    r.shoreJitterM = j.value("shore_jitter_m", r.shoreJitterM);
    r.bankM = j.value("bank_m", r.bankM);
    return r;
}

namespace {

std::uint32_t hash2(int x, int y, std::uint32_t salt = 0) {
    std::uint32_t h = std::uint32_t(x) * 0x8da6b343u ^ std::uint32_t(y) * 0xd8163841u ^ salt * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

// A smooth-ish value noise in -1..1 over a lattice of `cell` metres.
double vnoise(double x, double y, double cell, std::uint32_t salt) {
    const double fx = x / cell, fy = y / cell;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const double tx = fx - ix, ty = fy - iy;
    auto v = [&](int a, int b) { return (hash2(a, b, salt) & 0xffff) / 32767.5 - 1.0; };
    const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const double a = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * sx;
    const double b = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * sx;
    return a + (b - a) * sy;
}

bool wetAt(const World& w, const GroundRules& r, double x, double y) {
    const double j = r.shoreJitterM;
    const double dx = j * vnoise(x, y, 9.0, 11), dy = j * vnoise(x, y, 9.0, 12);
    return w.map().waterAt({x + dx, y + dy});
}

double segDist(Vec2 p, Vec2 a, Vec2 b) {
    const double vx = b.x - a.x, vy = b.y - a.y;
    const double l2 = vx * vx + vy * vy;
    double t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + vx * t), p.y - (a.y + vy * t));
}

bool insideZone(const Settlement::Zone& z, Vec2 p) {
    PlanRect r;
    r.c = z.c;
    r.yaw = z.yaw;
    r.hw = z.hw;
    r.hd = z.hd;
    const Vec2 d{p.x - r.c.x, p.y - r.c.y};
    const Vec2 s = r.side(), f = r.front();
    return std::abs(d.x * s.x + d.y * s.y) <= r.hw && std::abs(d.x * f.x + d.y * f.y) <= r.hd;
}

Ground zoneGround(const GroundRules& r, const std::string& kind, bool& found) {
    auto it = r.zone.find(kind);
    if (it == r.zone.end()) {
        const auto colon = kind.find(':');
        if (colon != std::string::npos) it = r.zone.find(kind.substr(0, colon));
    }
    found = it != r.zone.end();
    return found ? it->second : Ground::grass;
}

} // namespace

Ground groundAt(const World& w, const GroundRules& r, int x, int y) {
    const Vec2 p{x + 0.5, y + 0.5};
    if (wetAt(w, r, p.x, p.y)) return Ground::water;
    // the village's own ground: roads, square, zones (later wins: roads over fields)
    Ground g = Ground::count;
    for (const auto& [sid, s] : w.settlements()) {
        if (s.roads.empty() && s.zones.empty()) continue;
        if (dist(p, s.center) > 2000.0) continue;
        for (const auto& z : s.zones) {
            bool found = false;
            const Ground zg = zoneGround(r, z.kind, found);
            if (found && insideZone(z, p)) g = zg;
        }
        if (s.squareR > 0 && dist(p, s.square) <= s.squareR) g = r.square;
        for (std::size_t i = 0; i < s.roads.size(); ++i) {
            const auto& line = s.roads[i];
            const double half = (i < s.roadWidth.size() ? s.roadWidth[i] : 3.0) * 0.5;
            const std::string kind = i < s.roadKind.size() ? s.roadKind[i] : "main";
            for (std::size_t k = 1; k < line.size(); ++k)
                if (segDist(p, line[k - 1], line[k]) <= half) {
                    auto it = r.road.find(kind);
                    g = it == r.road.end() ? Ground::road : it->second;
                    break;
                }
        }
    }
    if (g != Ground::count) return g;
    // a bank of sand along the water
    if (r.bankM > 0) {
        const double b = r.bankM;
        if (wetAt(w, r, p.x + b, p.y) || wetAt(w, r, p.x - b, p.y) || wetAt(w, r, p.x, p.y + b) ||
            wetAt(w, r, p.x, p.y - b))
            return Ground::sand;
    }
    const std::string& biome = w.map().biomeAt(p);
    auto it = r.biome.find(biome);
    return it == r.biome.end() ? Ground::grass : it->second;
}

bool roadAt(const World& w, double x, double y) {
    const Vec2 p{x, y};
    for (const auto& [sid, s] : w.settlements()) {
        if (dist(p, s.center) > 2000.0) continue;
        if (s.squareR > 0 && dist(p, s.square) <= s.squareR) return true;
        for (std::size_t i = 0; i < s.roads.size(); ++i) {
            const auto& line = s.roads[i];
            const double half = (i < s.roadWidth.size() ? s.roadWidth[i] : 3.0) * 0.5;
            for (std::size_t k = 1; k < line.size(); ++k)
                if (segDist(p, line[k - 1], line[k]) <= half) return true;
        }
    }
    return false;
}

Grid2d buildGrid(const World& w, const GroundRules& rules, int x0, int y0, int wCells, int hCells) {
    Grid2d g;
    const int size = int(std::floor(w.map().sizeM));
    const int x1 = std::clamp(x0 + std::max(0, wCells), 0, size), y1 = std::clamp(y0 + std::max(0, hCells), 0, size);
    g.x0 = std::clamp(x0, 0, size);
    g.y0 = std::clamp(y0, 0, size);
    g.w = std::max(0, x1 - g.x0);
    g.h = std::max(0, y1 - g.y0);
    g.ground.assign(std::size_t(g.w) * std::size_t(g.h), std::uint8_t(Ground::grass));
    g.var.assign(g.ground.size(), 0);
    for (int y = g.y0; y < y1; ++y)
        for (int x = g.x0; x < x1; ++x) {
            const std::size_t i = std::size_t(y - g.y0) * std::size_t(g.w) + std::size_t(x - g.x0);
            g.ground[i] = std::uint8_t(groundAt(w, rules, x, y));
            g.var[i] = std::uint8_t(hash2(x, y, 7) & 0xff);
        }
    const BuildCatalog& cat = w.content().build;
    const double c = cat.cellM;
    const int cell = std::max(1, int(std::lround(c)));
    auto inWindow = [&](double ax, double ay, double bx, double by) {
        return bx >= g.x0 && ax <= x1 && by >= g.y0 && ay <= y1;
    };
    // rooms first: the roofs ask which building a cell is in
    std::map<std::tuple<int, int, int>, int> cellBuilding;   // piece cell, storey -> building
    for (const auto& rm : w.rooms()) {
        for (const auto& [cx, cy] : rm.cells) cellBuilding[{cx, cy, rm.z}] = rm.building;
        const int rx0 = int(std::floor(rm.x0 * c)), ry0 = int(std::floor(rm.y0 * c));
        const int rx1 = int(std::floor(rm.x1 * c)), ry1 = int(std::floor(rm.y1 * c));
        if (!inWindow(rx0, ry0, rx1, ry1)) continue;
        g.rooms.push_back({rm.id, rm.building, rm.z, rm.type, rm.enclosed, rx0, ry0, rx1, ry1});
    }
    for (const auto& [id, p] : w.pieces()) {
        const BuildPieceDef* d = cat.find(p.kind);
        if (!d) continue;
        const int px = int(std::floor(p.x * c)), py = int(std::floor(p.y * c));
        if (!inWindow(px - cell, py - cell, px + 2 * cell, py + 2 * cell)) continue;
        const SlotKey k = slotOf(*d, p.x, p.y, p.z, p.rot);
        if (k.layer == 'f') {
            if (p.z != 0 || !p.built) continue;
            for (int yy = py; yy < py + cell; ++yy)
                for (int xx = px; xx < px + cell; ++xx)
                    if (xx >= g.x0 && xx < x1 && yy >= g.y0 && yy < y1)
                        g.ground[std::size_t(yy - g.y0) * std::size_t(g.w) + std::size_t(xx - g.x0)] =
                            std::uint8_t(Ground::floor);
        } else if (k.layer == 'h' || k.layer == 'v') {
            Edge2d e;
            e.axis = k.layer;
            e.x = int(std::floor(k.a * c));
            e.y = int(std::floor(k.b * c));
            e.len = cell;
            e.z = p.z;
            e.piece = p.kind;
            e.built = p.built;
            e.kind = d->door ? "door" : (d->category == "gable" ? "gable"
                                         : p.kind.find("window") != std::string::npos ? "window" : "wall");
            g.edges.push_back(e);
        } else if (k.layer == 'r') {
            if (!p.built) continue;   // AL-94: a planned roof is drawn by the build overlay (web/build.js)
            Roof2d r;
            r.x = px;
            r.y = py;
            r.len = cell;
            r.z = p.z;
            r.piece = p.kind;
            // a roof of storey z covers the rooms of the storeys under it (a steep roof spans several)
            for (int zz = p.z - 1; zz >= -1 && r.building < 0; --zz) {
                auto it = cellBuilding.find({p.x, p.y, zz < 0 ? p.z : zz});
                if (it != cellBuilding.end()) r.building = it->second;
            }
            g.roofs.push_back(r);
        }
    }
    for (const auto& [id, o] : w.objects())
        if (o.pos.x >= g.x0 - 1 && o.pos.x < x1 + 1 && o.pos.y >= g.y0 - 1 && o.pos.y < y1 + 1)
            g.things.push_back({id, o.type, o.pos.x, o.pos.y, o.storey, o.ownerFamily});
    return g;
}

Json toJson(const Grid2d& g) {
    Json j;
    j["x0"] = g.x0;
    j["y0"] = g.y0;
    j["w"] = g.w;
    j["h"] = g.h;
    // ground as a string of letters 'a' + index, var as hex pairs: compact and easy to read back
    std::string gs(g.ground.size(), 'a');
    for (std::size_t i = 0; i < g.ground.size(); ++i) gs[i] = char('a' + g.ground[i]);
    j["ground"] = gs;
    static const char* hex = "0123456789abcdef";
    std::string vs(g.var.size() * 2, '0');
    for (std::size_t i = 0; i < g.var.size(); ++i) {
        vs[2 * i] = hex[g.var[i] >> 4];
        vs[2 * i + 1] = hex[g.var[i] & 15];
    }
    j["var"] = vs;
    Json e = Json::array();
    for (const auto& x : g.edges)
        e.push_back({{"x", x.x}, {"y", x.y}, {"len", x.len}, {"z", x.z}, {"axis", std::string(1, x.axis)},
                     {"kind", x.kind}, {"piece", x.piece}, {"built", x.built}});
    j["edges"] = e;
    Json r = Json::array();
    for (const auto& x : g.rooms)
        r.push_back({{"id", x.id}, {"building", x.building}, {"z", x.z}, {"type", x.type}, {"enclosed", x.enclosed},
                     {"x0", x.x0}, {"y0", x.y0}, {"x1", x.x1}, {"y1", x.y1}});
    j["rooms"] = r;
    Json rf = Json::array();
    for (const auto& x : g.roofs)
        rf.push_back({{"x", x.x}, {"y", x.y}, {"len", x.len}, {"z", x.z}, {"building", x.building}, {"piece", x.piece}});
    j["roofs"] = rf;
    Json t = Json::array();
    for (const auto& x : g.things)
        t.push_back({x.id, x.type, std::round(x.x * 100) / 100, std::round(x.y * 100) / 100, x.storey, x.owner});
    j["things"] = t;
    // AL-112: fields and gardens: one char per cell, '.' none, '0' + k the tile k of crop_tiles
    if (!g.crop.empty()) {
        std::string cs(g.crop.size(), '.');
        for (std::size_t i = 0; i < g.crop.size(); ++i)
            if (g.crop[i] > 0 && g.crop[i] <= 79) cs[i] = char('0' + g.crop[i] - 1);
        j["crop"] = cs;
        Json ct = Json::array();
        for (const auto& x : g.cropTiles) ct.push_back({x.sprite, x.rot});
        j["crop_tiles"] = ct;
    }
    Json pl = Json::array();
    for (const auto& x : g.plots)
        pl.push_back({{"id", x.id}, {"type", x.type}, {"x", std::round(x.x * 10) / 10}, {"y", std::round(x.y * 10) / 10},
                      {"strips", x.strips}, {"tilled", x.tilled}, {"sown", x.sown}, {"ripe", x.ripe},
                      {"grown", x.grown}, {"grow_days", x.growDays}, {"stage", x.stage}});
    j["plots"] = pl;
    // LR-156: the relief: level '0' + n a cell, the cliff mask as hex pairs, the style
    // 'a' + k (a ramp 'A' + k) of cliff_styles
    if (!g.level.empty()) {
        std::string ls(g.level.size(), '0'), cl(g.cliff.size() * 2, '0'), st(g.cliffStyle.size(), 'a');
        for (std::size_t i = 0; i < g.level.size(); ++i) ls[i] = char('0' + std::min<int>(g.level[i], 78));
        for (std::size_t i = 0; i < g.cliff.size(); ++i) {
            cl[2 * i] = hex[g.cliff[i] >> 4];
            cl[2 * i + 1] = hex[g.cliff[i] & 15];
        }
        for (std::size_t i = 0; i < g.cliffStyle.size(); ++i)
            st[i] = char(((g.cliffStyle[i] & 0x80) ? 'A' : 'a') + std::min(g.cliffStyle[i] & 0x7f, 25));
        j["level"] = ls;
        j["cliff"] = cl;
        j["cliff_style"] = st;
        j["cliff_styles"] = g.cliffStyles;
    }
    return j;
}

} // namespace lr::client2d
