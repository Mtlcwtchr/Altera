#include "client2d/farm2d.hpp"

#include <algorithm>
#include <cmath>

#include "sim/village_plan.hpp"

namespace lr::client2d {

using namespace lr::sim;

namespace {

std::uint32_t mix(std::uint32_t a, std::uint32_t b, std::uint32_t c = 0) {
    std::uint32_t h = a * 0x9e3779b1u ^ b * 0x85ebca77u ^ c * 0xc2b2ae3du;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

int st(const WorldObject& o, const char* key) {
    auto it = o.stock.find(key);
    return it == o.stock.end() ? 0 : it->second;
}

const CropDef* cropOf(const World& w, const std::string& type) {
    auto d = w.content().objects.find(type);
    return d != w.content().objects.end() && d->second.hasCrop ? &d->second.crop : nullptr;
}

std::vector<std::string> strings(const Json& j) {
    std::vector<std::string> v;
    if (j.is_array())
        for (const auto& s : j)
            if (s.is_string()) v.push_back(s.get<std::string>());
    return v;
}

// In the circular day-of-year window [from, to].
bool inWindow(int doy, int from, int to) {
    from = (from % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
    to = (to % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
    return from <= to ? (doy >= from && doy <= to) : (doy >= from || doy <= to);
}

// The ground a plot lies on, in its own frame: side() runs along the rows
// (bands, beds), front() across them.
struct Frame {
    PlanRect r;
    bool ok = false;
};

bool insideRect(const PlanRect& r, Vec2 p) {
    const Vec2 d{p.x - r.c.x, p.y - r.c.y};
    const Vec2 s = r.side(), f = r.front();
    return std::abs(d.x * s.x + d.y * s.y) <= r.hw && std::abs(d.x * f.x + d.y * f.y) <= r.hd;
}

// A field: its zone of the village plan (the one under the field's spot), else
// a rect of half_w by strips * strip_m round it.
PlanRect fieldRect(const World& w, const WorldObject& o, const PlotRule& rule, int strips) {
    for (const auto& [sid, s] : w.settlements()) {
        if (s.zones.empty() || dist(o.pos, s.center) > 2000.0) continue;
        for (const auto& z : s.zones) {
            if (z.kind != "field" && z.kind.rfind("field:", 0) != 0) continue;
            PlanRect r;
            r.c = z.c;
            r.yaw = z.yaw;
            r.hw = z.hw;
            r.hd = z.hd;
            if (insideRect(r, o.pos)) return r;
        }
    }
    PlanRect r;
    r.c = o.pos;
    r.yaw = o.faced ? o.yaw : 0.0;
    r.hw = rule.halfW;
    r.hd = std::max(1.0, strips * rule.stripM * 0.5);
    return r;
}

struct Beds {
    int len = 3, rows = 1, cols = 1;
    double width() const { return cols * len + (cols - 1); }
};

Beds bedsOf(const PlotRule& rule, int strips) {
    Beds b;
    b.len = std::max(1, rule.bedLen);
    b.rows = std::max(1, std::min(std::max(1, rule.maxRows), strips));
    b.cols = (strips + b.rows - 1) / b.rows;
    return b;
}

const std::vector<std::string>* tilesFor(const PlotRule& rule, const PlotPart& part) {
    if (!part.crop.empty()) {
        auto c = rule.crops.find(part.crop);
        if (c != rule.crops.end()) {
            auto t = c->second.find(part.stage);
            if (t != c->second.end() && !t->second.empty()) return &t->second;
        }
    }
    auto t = rule.tiles.find(part.stage);
    if (t != rule.tiles.end() && !t->second.empty()) return &t->second;
    return nullptr;
}

} // namespace

CropRules CropRules::fromJson(const Json& plots) {
    CropRules r;
    if (!plots.is_object()) return r;
    for (const auto& [type, j] : plots.items()) {
        if (!j.is_object()) continue;
        PlotRule p;
        p.layout = j.value("layout", p.layout);
        p.stripM = j.value("strip_m", p.stripM);
        p.halfW = j.value("half_w", p.halfW);
        p.bedLen = j.value("bed_len", p.bedLen);
        p.maxRows = j.value("max_rows", p.maxRows);
        p.pickPairs = std::max(1, j.value("pick_pairs", p.pickPairs));
        const Json stages = j.value("stages", Json::array());
        for (const auto& s : stages)
            if (s.is_array() && s.size() >= 2 && s[0].is_string() && s[1].is_number())
                p.stages.push_back({s[0].get<std::string>(), s[1].get<double>()});
        // kept: items() of a temporary dangles
        const Json tiles = j.value("tiles", Json::object()), crops = j.value("crops", Json::object());
        for (const auto& [k, v] : tiles.items()) p.tiles[k] = strings(v);
        p.path = strings(j.value("path", Json::array()));
        for (const auto& [crop, st] : crops.items())
            if (st.is_object())
                for (const auto& [k, v] : st.items()) p.crops[crop][k] = strings(v);
        r.byType[type] = std::move(p);
    }
    return r;
}

std::vector<PlotPart> plotParts(const World& w, const WorldObject& o, const PlotRule& rule) {
    const CropDef* cd = cropOf(w, o.type);
    const int strips = std::max(1, st(o, "strips"));
    std::vector<PlotPart> parts(static_cast<std::size_t>(strips));
    if (!cd) return parts;
    const int doy = (w.startDay() + w.day()) % kDaysPerYear;
    const int year = (w.startDay() + w.day()) / kDaysPerYear;
    const int ripe = st(o, "ripe"), sown = std::min(strips, st(o, "sown")), tilled = st(o, "tilled");
    // From the first ripening till the next sowing the reaped ground is stubble.
    const bool afterHarvest = !inWindow(doy, cd->sowFrom, cd->sowFrom + cd->growDays - 1);
    std::string grow = "sown";
    if (sown > 0) {
        const double f = double(st(o, "grown")) / std::max(1, cd->growDays);
        grow = rule.stages.empty() ? "grown" : rule.stages.back().first;
        for (const auto& [name, upTo] : rule.stages)
            if (f < upTo) {
                grow = name;
                break;
            }
    }
    if (ripe > 0) {
        // the standing crop, in strips: what a strip of an average year gives
        const double perStrip =
            std::max(1e-6, cd->seedPerStrip * 0.5 * (cd->yieldLo + cd->yieldHi) / std::max(1, cd->grainPerSheaf));
        const int standing = std::clamp(int(std::ceil(ripe / perStrip)), 1, strips);
        const std::string stage = st(o, "ripe_age") > cd->spoilAfter ? "over" : "ripe";
        for (int i = 0; i < strips; ++i) parts[std::size_t(i)].stage = i < strips - standing ? "stubble" : stage;
    } else {
        for (int i = 0; i < strips; ++i)
            parts[std::size_t(i)].stage = i < sown              ? grow
                                          : i < sown + tilled   ? "tilled"
                                          : afterHarvest        ? "stubble"
                                                                : "fallow";
    }
    if (!rule.crops.empty()) {
        std::vector<std::string> names;
        for (const auto& [name, st] : rule.crops) names.push_back(name);
        for (int i = 0; i < strips; ++i)
            parts[std::size_t(i)].crop =
                names[mix(std::uint32_t(o.id), std::uint32_t(i / rule.pickPairs), std::uint32_t(year)) % names.size()];
    }
    return parts;
}

void paintCrops(const World& w, const CropRules& rules, Grid2d& g) {
    if (rules.empty() || g.w <= 0 || g.h <= 0) return;
    std::map<std::pair<std::string, int>, std::uint16_t> index;
    auto tileId = [&](const std::string& sprite, int rot) -> std::uint16_t {
        auto it = index.find({sprite, rot});
        if (it != index.end()) return it->second;
        if (g.cropTiles.size() >= 79) return 0;   // the JSON's one char per cell
        g.cropTiles.push_back({sprite, rot});
        const auto id = std::uint16_t(g.cropTiles.size());
        index[{sprite, rot}] = id;
        return id;
    };
    for (const auto& [type, rule] : rules.byType) {
        if (!cropOf(w, type)) continue;
        for (EntityId id : w.objectsOfType(type)) {
            const WorldObject* o = w.object(id);
            if (!o) continue;
            const int strips = std::max(1, st(*o, "strips"));
            const bool beds = rule.layout == "beds";
            PlanRect r;
            Beds b;
            if (beds) {
                b = bedsOf(rule, strips);
                r.c = o->pos;
                r.yaw = o->faced ? o->yaw : 0.0;
                r.hw = b.width() * 0.5;
                r.hd = b.rows * 0.5;
            } else {
                r = fieldRect(w, *o, rule, strips);
            }
            const double rad = std::hypot(r.hw, r.hd) + 1.0;
            if (r.c.x + rad < g.x0 || r.c.x - rad > g.x0 + g.w || r.c.y + rad < g.y0 || r.c.y - rad > g.y0 + g.h) continue;
            const std::vector<PlotPart> parts = plotParts(w, *o, rule);
            const Vec2 s = r.side(), f = r.front();
            const int rot = std::abs(s.x) >= std::abs(s.y) ? 0 : 1;   // rows along x, else turned
            if (g.crop.empty()) g.crop.assign(std::size_t(g.w) * std::size_t(g.h), 0);
            // the cells of the rect's bounds that fall in the window
            const int cx0 = std::max(g.x0, int(std::floor(r.c.x - rad))), cx1 = std::min(g.x0 + g.w, int(std::ceil(r.c.x + rad)));
            const int cy0 = std::max(g.y0, int(std::floor(r.c.y - rad))), cy1 = std::min(g.y0 + g.h, int(std::ceil(r.c.y + rad)));
            bool any = false;
            for (int y = cy0; y < cy1; ++y)
                for (int x = cx0; x < cx1; ++x) {
                    const Vec2 d{x + 0.5 - r.c.x, y + 0.5 - r.c.y};
                    const double u = d.x * s.x + d.y * s.y, v = d.x * f.x + d.y * f.y;   // across .. along
                    if (std::abs(u) >= r.hw || std::abs(v) >= r.hd) continue;
                    const std::size_t i = std::size_t(y - g.y0) * std::size_t(g.w) + std::size_t(x - g.x0);
                    const Ground gr = Ground(g.ground[i]);
                    if (gr == Ground::water || gr == Ground::road || gr == Ground::floor || gr == Ground::dirt) continue;
                    const std::vector<std::string>* list = nullptr;
                    if (beds) {
                        const int bu = int(std::floor(u + r.hw)), bv = int(std::floor(v + r.hd));
                        const int col = bu / (b.len + 1), within = bu % (b.len + 1);
                        if (col >= b.cols || bv >= b.rows) continue;
                        if (within == b.len) {
                            list = rule.path.empty() ? nullptr : &rule.path;
                        } else {
                            const int bed = col * b.rows + bv;
                            if (bed >= int(parts.size())) continue;
                            list = tilesFor(rule, parts[std::size_t(bed)]);
                        }
                    } else {
                        const int band = std::clamp(int(std::floor((v + r.hd) / (2.0 * r.hd) * strips)), 0, strips - 1);
                        list = tilesFor(rule, parts[std::size_t(band)]);
                    }
                    if (!list || list->empty()) continue;
                    const auto& sprite = (*list)[std::size_t(g.var[i]) % list->size()];
                    if (const std::uint16_t t = tileId(sprite, rot)) {
                        g.crop[i] = t;
                        any = true;
                    }
                }
            if (!any) continue;
            Plot2d p;
            p.id = id;
            p.type = type;
            p.x = r.c.x;
            p.y = r.c.y;
            p.strips = strips;
            p.tilled = st(*o, "tilled");
            p.sown = st(*o, "sown");
            p.ripe = st(*o, "ripe");
            p.grown = st(*o, "grown");
            p.growDays = cropOf(w, type)->growDays;
            std::map<std::string, int> n;
            for (const auto& part : parts) ++n[part.stage];
            int best = -1;
            for (const auto& [k, c] : n)
                if (c > best) best = c, p.stage = k;
            g.plots.push_back(p);
        }
    }
}

} // namespace lr::client2d
