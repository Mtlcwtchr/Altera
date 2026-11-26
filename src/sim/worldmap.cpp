// LR-153: the world map (sim/worldmap.hpp, content/ai/worldmap.json).

#include "sim/worldmap.hpp"
#include "sim/bestiary.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/realm.hpp"
#include "sim/world.hpp"

namespace fs = std::filesystem;

namespace lr::sim {

namespace {

struct Pull {
    std::string culture, institution, tag;
    double h = -1, m = -1, radius = 5, weight = 0.7;
};
struct TerrainKind {
    std::string id;
    double hBelow = 2, hFrom = -1, mBelow = 2, mFrom = -1;
};
struct PoiKind {
    std::string id;
    int n0 = 1, n1 = 2, names = 4;
    std::set<std::string> terrain;
    std::string nearCulture;
    double nearKm = 0;
    std::vector<std::string> tags;
};
struct TitleRule {
    std::string title, institution, tag;
    int minMembers = 0;
    std::set<std::string> headSizes;
};

struct Rules {
    bool loaded = false, enabled = true;
    int grid = 96;
    double margin = 14, minHalf = 30;
    double scale = 26, moistScale = 18, edgeSea = 0.22;
    int octaves = 4;
    std::vector<TerrainKind> kinds;
    double landMin = 0.42, landMax = 0.6, landR = 3.5;
    std::vector<Pull> pulls;
    int rivers0 = 2, rivers1 = 4, riverLen = 160;
    double riverFrom = 0.7, riverBend = 6.0, lakeChance = 0.25, meander = 8.0, meanderCells = 6.0;
    double poiMinSettle = 4.5, poiMinBetween = 4;
    std::vector<PoiKind> poi;
    int battlefieldsMax = 8;
    double battleJitter = 3;
    std::set<std::string> disabledTags;
    int roadLinks = 2;
    double roadKm = 28;
    std::set<std::string> roadSkipTags, roadSkipInst;
    bool fealtyOn = true, sameCulture = true;
    std::set<std::string> lords, freeInst;
    double reach = 26, ratio = 1.4, fealtyChance = 0.8;
    std::vector<TitleRule> titles;
};

std::set<std::string> strSet(const Json& j, const char* k) {
    std::set<std::string> out;
    if (j.is_object() && j.contains(k) && j[k].is_array())
        for (const auto& v : j[k]) out.insert(v.get<std::string>());
    return out;
}

Rules parse(const Json& j) {
    Rules r;
    r.loaded = true;
    r.enabled = j.value("enabled", true);
    r.grid = std::clamp(j.value("grid", r.grid), 16, 256);
    r.margin = j.value("margin_km", r.margin);
    r.minHalf = j.value("min_half_km", r.minHalf);
    const Json t = j.value("terrain", Json::object());
    r.scale = t.value("scale_km", r.scale);
    r.moistScale = t.value("moist_scale_km", r.moistScale);
    r.octaves = std::clamp(t.value("octaves", r.octaves), 1, 8);
    r.edgeSea = t.value("edge_sea", r.edgeSea);
    for (const auto& k : t.value("kinds", Json::array())) {
        TerrainKind d;
        d.id = k.value("id", "");
        d.hBelow = k.value("h_below", d.hBelow);
        d.hFrom = k.value("h_from", d.hFrom);
        d.mBelow = k.value("m_below", d.mBelow);
        d.mFrom = k.value("m_from", d.mFrom);
        r.kinds.push_back(d);
    }
    if (r.kinds.empty()) r.kinds.push_back({"plains"});
    const Json sl = t.value("settle_land", Json::object());
    r.landMin = sl.value("h_min", r.landMin);
    r.landMax = sl.value("h_max", r.landMax);
    r.landR = sl.value("radius_km", r.landR);
    for (const auto& p : t.value("pulls", Json::array())) {
        Pull d;
        d.culture = p.value("culture", "");
        d.institution = p.value("institution", "");
        d.tag = p.value("tag", "");
        d.h = p.value("h", -1.0);
        d.m = p.value("m", -1.0);
        d.radius = p.value("radius_km", d.radius);
        d.weight = p.value("weight", d.weight);
        r.pulls.push_back(d);
    }
    const Json rv = j.value("rivers", Json::object());
    if (rv.contains("n") && rv["n"].is_array() && rv["n"].size() >= 2) r.rivers0 = rv["n"][0], r.rivers1 = rv["n"][1];
    r.riverFrom = rv.value("from_h", r.riverFrom);
    r.riverLen = rv.value("max_len", r.riverLen);
    r.riverBend = rv.value("bend", r.riverBend);
    r.lakeChance = rv.value("lake_chance", r.lakeChance);
    r.meander = rv.value("meander", r.meander);
    r.meanderCells = std::max(1.0, rv.value("meander_cells", r.meanderCells));
    const Json po = j.value("poi", Json::object());
    r.poiMinSettle = po.value("min_km_settlement", r.poiMinSettle);
    r.poiMinBetween = po.value("min_km_between", r.poiMinBetween);
    r.battlefieldsMax = po.value("battlefields_max", r.battlefieldsMax);
    r.battleJitter = po.value("battlefield_jitter_km", r.battleJitter);
    for (const auto& k : po.value("kinds", Json::array())) {
        PoiKind d;
        d.id = k.value("id", "");
        if (k.contains("n") && k["n"].is_array() && k["n"].size() >= 2) d.n0 = k["n"][0], d.n1 = k["n"][1];
        d.names = std::max(1, k.value("names", d.names));
        d.terrain = strSet(k, "terrain");
        d.nearCulture = k.value("near_culture", "");
        d.nearKm = k.value("near_km", 0.0);
        d.tags = k.value("tags", std::vector<std::string>{});
        r.poi.push_back(d);
    }
    r.disabledTags = strSet(j, "disabled_tags");
    const Json ro = j.value("roads", Json::object());
    r.roadLinks = ro.value("links", r.roadLinks);
    r.roadKm = ro.value("link_km", r.roadKm);
    r.roadSkipTags = strSet(ro, "skip_tags");
    r.roadSkipInst = strSet(ro, "skip_institutions");
    const Json fe = j.value("fealty", Json::object());
    r.fealtyOn = fe.value("enabled", r.fealtyOn);
    r.lords = strSet(fe, "lords");
    r.freeInst = strSet(fe, "free_institutions");
    r.reach = fe.value("reach_km", r.reach);
    r.ratio = std::max(1.01, fe.value("ratio", r.ratio));
    r.fealtyChance = fe.value("chance", r.fealtyChance);
    r.sameCulture = fe.value("same_culture", r.sameCulture);
    for (const auto& tr : j.value("domains", Json::object()).value("titles", Json::array())) {
        TitleRule d;
        d.title = tr.value("title", "free");
        d.institution = tr.value("institution", "");
        d.tag = tr.value("tag", "");
        d.minMembers = tr.value("min_members", 0);
        d.headSizes = strSet(tr, "head_sizes");
        r.titles.push_back(d);
    }
    return r;
}

const Rules& rules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Rules>> cache;
    static const Rules kOff;
    if (c.dir.empty()) return kOff;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    if (auto it = cache.find(key); it != cache.end()) return *it->second;
    auto r = std::make_unique<Rules>();
    const fs::path p = fs::path(c.dir) / "worldmap.json";
    if (data::exists(p)) {
        data::InFile in(p);
        *r = parse(Json::parse(in));
    }
    return *cache.emplace(key, std::move(r)).first->second;
}

// --- noise ------------------------------------------------------------------------
double lattice(std::uint64_t seed, int x, int y) {
    std::uint64_t h = hashCombine(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
    h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)));
    Rng r(h);
    return r.uniform();
}
double smooth(double t) { return t * t * (3 - 2 * t); }
double valueNoise(std::uint64_t seed, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = smooth(x - x0), fy = smooth(y - y0);
    const double a = lattice(seed, x0, y0), b = lattice(seed, x0 + 1, y0);
    const double c = lattice(seed, x0, y0 + 1), d = lattice(seed, x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}
double fbm(std::uint64_t seed, double x, double y, int oct) {
    double s = 0, amp = 1, norm = 0, f = 1;
    for (int i = 0; i < oct; ++i) {
        s += amp * valueNoise(hashCombine(seed, static_cast<std::uint64_t>(i)), x * f, y * f);
        norm += amp;
        amp *= 0.5;
        f *= 2.03;
    }
    return s / norm;
}

bool pullMatches(const Pull& p, const Settlement& s) {
    if (!p.culture.empty() && s.culture != p.culture) return false;
    if (!p.institution.empty() && s.institution != p.institution) return false;
    if (!p.tag.empty() && !s.tagged(p.tag)) return false;
    return !(p.culture.empty() && p.institution.empty() && p.tag.empty());
}

bool tagsOn(const Rules& r, const std::vector<std::string>& tags) {
    for (const auto& t : tags)
        if (r.disabledTags.count(t)) return false;
    return true;
}

int kindIndex(const WorldMapState& s, const std::string& id) {
    for (size_t i = 0; i < s.kinds.size(); ++i)
        if (s.kinds[i] == id) return static_cast<int>(i);
    return -1;
}

// LR-170: each creature with a mythic "lair" gets its places near settlements of its belts
// (no belts: anywhere its terrain is). lair: {poi, terrain, coast, n, near_km, names, raid}.
void layLairs(const World& w, const Rules& r, WorldMapState& s, Rng& pr) {
    const Content& ct = w.content();
    const int G = s.grid;
    const int sea = static_cast<int>(std::find(s.kinds.begin(), s.kinds.end(), "sea") - s.kinds.begin());
    auto kindOf = [&](int c) { return s.ground[static_cast<size_t>(c)] - '0'; };
    static const int kDi[4] = {1, -1, 0, 0}, kDj[4] = {0, 0, 1, -1};
    for (const auto& [sid, sp] : ct.species) {
        const Json& L = sp.mythic.lair;
        if (!L.is_object() || L.empty()) continue;
        const std::string poi = L.value("poi", std::string());
        if (poi.empty()) continue;
        if (!tagsOn(r, L.value("tags", std::vector<std::string>{}))) continue;
        std::set<std::string> terrain;
        for (const auto& t : L.value("terrain", std::vector<std::string>{})) terrain.insert(t);
        const bool coast = L.value("coast", false);
        const double nearKm = L.value("near_km", 30.0);
        const int names = std::max(1, L.value("names", 4));
        // the settlements of its belts the lair lies near
        std::vector<Vec2> homes;
        for (const auto& [id, p] : w.settlements())
            if (sp.mythic.belts.empty() || creatureInBelt(sp, beltOfCulture(ct, p.culture))) homes.push_back(p.km);
        if (homes.empty()) continue;   // nobody of its belts here: its lairs lie elsewhere in the world
        int n0 = 1, n1 = 1;
        if (L.contains("n") && L["n"].is_array() && L["n"].size() >= 2) n0 = L["n"][0], n1 = L["n"][1];
        const int want = pr.irange(n0, n1);
        std::set<int> usedNames;
        for (int n = 0; n < want; ++n) {
            for (int tries = 0; tries < 400; ++tries) {
                const int c = pr.irange(0, G * G - 1);
                const int ci = c % G, cj = c / G;
                if (ci < 2 || cj < 2 || ci >= G - 2 || cj >= G - 2) continue;
                const int g = kindOf(c);
                const std::string& kind = s.kinds[static_cast<size_t>(g)];
                if (!terrain.empty() && !terrain.count(kind)) continue;
                if (s.river[static_cast<size_t>(c)] == '1') continue;
                if (coast) {
                    if (g == sea) continue;
                    bool shore = false;
                    for (int d = 0; d < 4; ++d) shore |= kindOf((cj + kDj[d]) * G + ci + kDi[d]) == sea;
                    if (!shore) continue;
                }
                const Vec2 km{s.origin.x + (ci + pr.range(0.2, 0.8)) * s.cellKm, s.origin.y + (cj + pr.range(0.2, 0.8)) * s.cellKm};
                bool ok = true, near = false;
                for (const auto& [id, p] : w.settlements())
                    if (dist(km, p.km) < r.poiMinSettle) ok = false;
                for (const Vec2& h : homes) near |= dist(km, h) < nearKm;
                if (!ok || (!near && tries < 300)) continue;
                for (const WorldPoi& o : s.pois)
                    if (dist(o.km, km) < r.poiMinBetween) ok = false;
                if (!ok) continue;
                WorldPoi p;
                p.id = s.nextId++;
                p.kind = poi;
                p.creature = sid;
                p.km = km;
                int nm = pr.irange(1, names);
                for (int q = 0; q < names && usedNames.count(nm); ++q) nm = nm % names + 1;
                usedNames.insert(nm);
                p.name = "n" + std::to_string(nm);
                s.pois.push_back(p);
                break;
            }
        }
    }
}

// LR-170: a lair's dweller raids the nearest settlement now and then (lair.raid: per_year,
// km, wealth_loss [lo, hi]); the chronicle tells of it. Returns whether anything changed.
bool lairRaids(World& w, WorldMapState& s, int day) {
    bool dirty = false;
    const Content& ct = w.content();
    for (WorldPoi& p : s.pois) {
        if (p.creature.empty()) continue;
        const SpeciesDef* sp = ct.speciesOf(p.creature);
        if (!sp || !sp->mythic.lair.is_object()) continue;
        const Json raid = sp->mythic.lair.value("raid", Json::object());
        const double perYear = raid.value("per_year", 0.0);
        if (perYear <= 0.0 || (day + static_cast<int>(p.id)) % 30 != 0) continue;
        Rng rng = agentRng(w.seed(), p.id, day, "worldmap:lair_raid");
        if (!rng.chance(perYear * 30.0 / kDaysPerYear)) continue;
        const double reach = raid.value("km", 15.0);
        Settlement* best = nullptr;
        double bd = reach;
        for (auto& [id, st] : w.settlements()) {
            if (st.ruined || st.full()) continue;   // the map's own village meets its beasts on the ground
            const double d = dist(st.km, p.km);
            if (d < bd) bd = d, best = &st;
        }
        if (!best) continue;
        double lo = 0.03, hi = 0.1;
        if (raid.contains("wealth_loss") && raid["wealth_loss"].is_array() && raid["wealth_loss"].size() >= 2)
            lo = raid["wealth_loss"][0], hi = raid["wealth_loss"][1];
        const double loss = best->wealth * rng.range(lo, hi);
        best->wealth -= loss;
        p.lastRaid = day;
        dirty = true;
        w.chronicle("lair_raid", {}, "chronicle.lair_raid." + p.creature, {{"place", best->id}, {"loss", loss}});
    }
    return dirty;
}

void layOut(World& w, const Rules& r, WorldMapState& s) {
    // the bounds: every settlement and a margin, square, centred on their middle
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& [id, p] : w.settlements()) {
        x0 = std::min(x0, p.km.x), y0 = std::min(y0, p.km.y);
        x1 = std::max(x1, p.km.x), y1 = std::max(y1, p.km.y);
    }
    if (x0 > x1) x0 = y0 = x1 = y1 = 0;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const double half = std::max(r.minHalf, std::max(x1 - x0, y1 - y0) / 2 + r.margin);
    const int G = r.grid;
    s.grid = G;
    s.cellKm = 2 * half / G;
    s.origin = {cx - half, cy - half};
    s.kinds.clear();
    for (const auto& k : r.kinds) s.kinds.push_back(k.id);
    if (kindIndex(s, "lake") < 0) s.kinds.push_back("lake");
    const std::uint64_t seedH = hashCombine(w.seed(), hashStr("worldmap:h"));
    const std::uint64_t seedM = hashCombine(w.seed(), hashStr("worldmap:m"));

    std::vector<double> H(static_cast<size_t>(G * G)), M(static_cast<size_t>(G * G));
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            const Vec2 km{s.origin.x + (i + 0.5) * s.cellKm, s.origin.y + (j + 0.5) * s.cellKm};
            double h = fbm(seedH, km.x / r.scale, km.y / r.scale, r.octaves);
            double m = fbm(seedM, km.x / r.moistScale, km.y / r.moistScale, r.octaves);
            // stretch the middle of the noise (value noise bunches around 0.5)
            h = std::clamp(0.5 + (h - 0.5) * 1.9, 0.0, 1.0);
            m = std::clamp(0.5 + (m - 0.5) * 1.8, 0.0, 1.0);
            // the edge of the known land drops to the sea here and there
            const double e = std::max(std::fabs(i + 0.5 - G / 2.0), std::fabs(j + 0.5 - G / 2.0)) / (G / 2.0);
            h -= r.edgeSea * smooth(std::clamp((e - 0.72) / 0.28, 0.0, 1.0));
            // who lives where pulls the land
            for (const auto& [id, p] : w.settlements()) {
                const double d = dist(km, p.km);
                for (const Pull& pl : r.pulls) {
                    if (!pullMatches(pl, p) || d >= pl.radius) continue;
                    const double f = std::pow(1 - d / pl.radius, 2) * pl.weight;
                    if (pl.h >= 0) h += (pl.h - h) * f;
                    if (pl.m >= 0) m += (pl.m - m) * f;
                }
            }
            // and the others stand on dry, liveable land (after every pull: a hold's mountain
            // does not swallow the village next to it)
            for (const auto& [id, p] : w.settlements()) {
                const double d = dist(km, p.km);
                if (d >= r.landR) continue;
                bool pulledH = false;
                for (const Pull& pl : r.pulls) pulledH = pulledH || (pl.h >= 0 && pullMatches(pl, p));
                const double f = smooth(1 - d / r.landR);
                const double want = pulledH ? std::max(h, r.landMin) : std::clamp(h, r.landMin, r.landMax);
                h += (want - h) * f;
            }
            H[static_cast<size_t>(j * G + i)] = std::clamp(h, 0.0, 1.0);
            M[static_cast<size_t>(j * G + i)] = std::clamp(m, 0.0, 1.0);
        }
    s.ground.assign(static_cast<size_t>(G * G), '0');
    s.river.assign(static_cast<size_t>(G * G), '0');
    for (int c = 0; c < G * G; ++c) {
        const double h = H[static_cast<size_t>(c)], m = M[static_cast<size_t>(c)];
        int k = 0;
        for (size_t q = 0; q < r.kinds.size(); ++q) {
            const TerrainKind& t = r.kinds[q];
            if (h < t.hBelow && h >= t.hFrom && m < t.mBelow && m >= t.mFrom) {
                k = static_cast<int>(q);
                break;
            }
        }
        s.ground[static_cast<size_t>(c)] = static_cast<char>('0' + k);
    }
    // rivers: from high ground down to the sea or off the land's edge, the shortest way the
    // ground allows (a distance to the water, bent by the heights); a hollow on the way may hold a lake
    Rng rng = agentRng(w.seed(), 0, 0, "worldmap:rivers");
    const std::uint64_t seedR = hashCombine(w.seed(), hashStr("worldmap:wind"));
    const int sea = kindIndex(s, "sea"), lake = kindIndex(s, "lake");
    std::vector<int> D(static_cast<size_t>(G * G), -1);
    std::vector<int> q;
    for (int c = 0; c < G * G; ++c) {
        const int ci = c % G, cj = c / G;
        if (s.ground[static_cast<size_t>(c)] - '0' == sea || ci == 0 || cj == 0 || ci == G - 1 || cj == G - 1)
            D[static_cast<size_t>(c)] = 0, q.push_back(c);
    }
    static const int kDi[4] = {1, -1, 0, 0}, kDj[4] = {0, 0, 1, -1};
    for (size_t h = 0; h < q.size(); ++h) {
        const int c = q[h], ci = c % G, cj = c / G;
        for (int d = 0; d < 4; ++d) {
            const int ni = ci + kDi[d], nj = cj + kDj[d];
            if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
            const int nc = nj * G + ni;
            if (D[static_cast<size_t>(nc)] >= 0) continue;
            D[static_cast<size_t>(nc)] = D[static_cast<size_t>(c)] + 1;
            q.push_back(nc);
        }
    }
    std::vector<int> highs;
    for (int c = 0; c < G * G; ++c)
        if (H[static_cast<size_t>(c)] >= r.riverFrom && D[static_cast<size_t>(c)] > 6) highs.push_back(c);
    const int nr = rng.irange(r.rivers0, r.rivers1);
    for (int n = 0; n < nr && !highs.empty(); ++n) {
        int c = highs[static_cast<size_t>(rng.irange(0, static_cast<int>(highs.size()) - 1))];
        if (s.river[static_cast<size_t>(c)] == '1') continue;
        std::set<int> seen;
        bool lakeMade = false;
        for (int step = 0; step < r.riverLen; ++step) {
            seen.insert(c);
            const int g = s.ground[static_cast<size_t>(c)] - '0';
            if (g == sea || g == lake) break;
            if (step > 0 && s.river[static_cast<size_t>(c)] == '1') break;   // it joins another river
            if (step > 1) s.river[static_cast<size_t>(c)] = '1';
            const int ci = c % G, cj = c / G;
            if (ci == 0 || cj == 0 || ci == G - 1 || cj == G - 1) break;   // off the land's edge
            int best = -1;
            double bs = 1e18, lowest = 1e9;
            for (int d = 0; d < 4; ++d) {
                const int ni = ci + kDi[d], nj = cj + kDj[d];
                if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
                const int nc = nj * G + ni;
                lowest = std::min(lowest, H[static_cast<size_t>(nc)]);
                if (seen.count(nc)) continue;
                const double wind = r.meander * valueNoise(seedR, ni / r.meanderCells, nj / r.meanderCells);   // it winds
                const double score = D[static_cast<size_t>(nc)] + r.riverBend * H[static_cast<size_t>(nc)] + wind + rng.range(0, 0.4);
                if (score < bs) bs = score, best = nc;
            }
            if (best < 0) break;
            // a hollow the river passes: now and then it stands as a lake
            if (!lakeMade && step > 8 && lowest > H[static_cast<size_t>(c)] + 0.01 && rng.chance(r.lakeChance)) {
                for (int d = -1; d < 4; ++d) {
                    const int ni = ci + (d < 0 ? 0 : kDi[d]), nj = cj + (d < 0 ? 0 : kDj[d]);
                    if (ni <= 0 || nj <= 0 || ni >= G - 1 || nj >= G - 1) continue;
                    const size_t nc = static_cast<size_t>(nj * G + ni);
                    if (s.ground[nc] - '0' == sea) continue;
                    s.ground[nc] = static_cast<char>('0' + lake), s.river[nc] = '0';
                }
                lakeMade = true;
            }
            c = best;
        }
    }
    // the places of interest
    Rng pr = agentRng(w.seed(), 0, 0, "worldmap:poi");
    for (const PoiKind& k : r.poi) {
        if (!tagsOn(r, k.tags)) continue;
        const int want = pr.irange(k.n0, k.n1);
        std::set<int> usedNames;
        for (int n = 0; n < want; ++n) {
            for (int tries = 0; tries < 300; ++tries) {
                const int c = pr.irange(0, G * G - 1);
                const int ci = c % G, cj = c / G;
                if (ci < 2 || cj < 2 || ci >= G - 2 || cj >= G - 2) continue;
                const std::string& kind = s.kinds[static_cast<size_t>(s.ground[static_cast<size_t>(c)] - '0')];
                if (!k.terrain.empty() && !k.terrain.count(kind)) continue;
                if (s.river[static_cast<size_t>(c)] == '1') continue;
                const Vec2 km{s.origin.x + (ci + pr.range(0.2, 0.8)) * s.cellKm, s.origin.y + (cj + pr.range(0.2, 0.8)) * s.cellKm};
                bool ok = true, near = k.nearCulture.empty();
                for (const auto& [id, p] : w.settlements()) {
                    const double d = dist(km, p.km);
                    if (d < r.poiMinSettle) ok = false;
                    if (!near && p.culture == k.nearCulture && d < k.nearKm) near = true;
                }
                if (!ok || (!near && tries < 200)) continue;
                for (const WorldPoi& o : s.pois)
                    if (dist(o.km, km) < r.poiMinBetween) ok = false;
                if (!ok) continue;
                WorldPoi p;
                p.id = s.nextId++;
                p.kind = k.id;
                p.km = km;
                int nm = pr.irange(1, k.names);
                for (int q = 0; q < k.names && usedNames.count(nm); ++q) nm = nm % k.names + 1;
                usedNames.insert(nm);
                p.name = "n" + std::to_string(nm);
                s.pois.push_back(p);
                break;
            }
        }
    }
    layLairs(w, r, s, pr);   // LR-170: the myth creatures' lairs, by belt
    s.inited = true;
}

std::string cultureOf(const Settlement& p) { return p.culture; }

void swearFealty(World& w, const Rules& r) {
    std::vector<Settlement*> all;
    for (auto& [id, p] : w.settlements())
        if (!p.ruined) all.push_back(&p);
    std::stable_sort(all.begin(), all.end(), [](const Settlement* a, const Settlement* b) { return a->pop < b->pop; });
    for (Settlement* s : all) {
        if (s->liege != kNone || r.freeInst.count(s->institution) || s->tagged("outlaw")) continue;
        Settlement* best = nullptr;
        double bd = 1e18;
        for (Settlement* l : all) {
            if (l == s || !r.lords.count(l->institution) || l->tagged("outlaw")) continue;
            if (l->pop < s->pop * r.ratio) continue;
            if (r.sameCulture && !cultureOf(*s).empty() && !cultureOf(*l).empty() && cultureOf(*s) != cultureOf(*l)) continue;
            const double d = dist(s->km, l->km);
            if (d <= r.reach && d < bd) bd = d, best = l;
        }
        if (!best) continue;
        Rng rng = w.rng(s->id, "worldmap:fealty");
        if (!rng.chance(r.fealtyChance)) continue;
        s->liege = best->id;
        realmClassify(w, *s);
    }
}

} // namespace

std::string WorldMapState::kindAt(Vec2 km) const {
    if (!inited || grid <= 0) return "";
    const int i = static_cast<int>(std::floor((km.x - origin.x) / cellKm));
    const int j = static_cast<int>(std::floor((km.y - origin.y) / cellKm));
    if (i < 0 || j < 0 || i >= grid || j >= grid) return "";
    const int k = ground[static_cast<size_t>(j * grid + i)] - '0';
    return k >= 0 && k < static_cast<int>(kinds.size()) ? kinds[static_cast<size_t>(k)] : "";
}

void to_json(Json& j, const WorldMapState& s) {
    Json pois = Json::array();
    for (const auto& p : s.pois) {
        Json e = {{"id", p.id}, {"kind", p.kind}, {"km", p.km}, {"name", p.name}};
        if (p.day) e["day"] = p.day;
        if (p.a != kNone) e["a"] = p.a;
        if (p.b != kNone) e["b"] = p.b;
        if (!p.creature.empty()) e["creature"] = p.creature;
        if (p.lastRaid) e["raid"] = p.lastRaid;
        pois.push_back(e);
    }
    Json seen = Json::array();
    for (const auto& [war, n] : s.battlesSeen) seen.push_back({war, n});
    j = {{"inited", s.inited}, {"grid", s.grid}, {"origin", s.origin}, {"cell_km", s.cellKm}, {"ground", s.ground},
         {"river", s.river}, {"kinds", s.kinds}, {"pois", pois}, {"battles", seen}, {"fealty", s.fealty}, {"next", s.nextId}};
}

void from_json(const Json& j, WorldMapState& s) {
    s = WorldMapState{};
    if (!j.is_object()) return;
    s.inited = j.value("inited", false);
    s.grid = j.value("grid", 0);
    if (j.contains("origin")) s.origin = j["origin"].get<Vec2>();
    s.cellKm = j.value("cell_km", 1.0);
    s.ground = j.value("ground", std::string());
    s.river = j.value("river", std::string());
    s.kinds = j.value("kinds", std::vector<std::string>{});
    for (const auto& e : j.value("pois", Json::array())) {
        WorldPoi p;
        p.id = e.value("id", kNone);
        p.kind = e.value("kind", "");
        p.creature = e.value("creature", "");
        p.lastRaid = e.value("raid", 0);
        if (e.contains("km")) p.km = e["km"].get<Vec2>();
        p.name = e.value("name", "");
        p.day = e.value("day", 0);
        p.a = e.value("a", kNone);
        p.b = e.value("b", kNone);
        s.pois.push_back(p);
    }
    for (const auto& e : j.value("battles", Json::array()))
        if (e.is_array() && e.size() == 2) s.battlesSeen.push_back({e[0].get<EntityId>(), e[1].get<int>()});
    s.fealty = j.value("fealty", false);
    s.nextId = j.value("next", 1);
    if (s.grid <= 0 || s.ground.size() != static_cast<size_t>(s.grid * s.grid)) s.inited = false;
    if (s.river.size() != s.ground.size()) s.river.assign(s.ground.size(), '0');
}

WorldMapState loadWorldMap(const World& w) {
    WorldMapState s;
    if (const Json* j = w.moduleState("worldmap"); j && j->is_object()) from_json(*j, s);
    return s;
}

void saveWorldMap(World& w, const WorldMapState& s) { w.moduleState("worldmap") = Json(s); }

bool worldMapLoaded(const Content& c) {
    const Rules& r = rules(c);
    return r.loaded && r.enabled;
}

std::vector<std::string> checkWorldMap(const Content& c) {
    std::vector<std::string> out;
    const Rules& r = rules(c);
    if (!r.loaded) return out;
    std::set<std::string> kinds = {"lake"};
    for (const auto& k : r.kinds) {
        if (k.id.empty()) out.push_back("worldmap.json: a terrain kind without id");
        kinds.insert(k.id);
    }
    std::set<std::string> ids;
    for (const auto& p : r.poi) {
        if (p.id.empty() || p.id == "battlefield") out.push_back("worldmap.json: bad poi kind id '" + p.id + "'");
        if (!ids.insert(p.id).second) out.push_back("worldmap.json: poi kind '" + p.id + "' twice");
        for (const auto& t : p.terrain)
            if (!kinds.count(t)) out.push_back("worldmap.json: poi '" + p.id + "' wants unknown terrain '" + t + "'");
    }
    for (const auto& [sid, sp] : c.species) {   // LR-170: the creatures' lairs
        const Json& L = sp.mythic.lair;
        if (!L.is_object() || L.empty()) continue;
        const std::string poi = L.value("poi", std::string());
        if (poi.empty() || poi == "battlefield" || !ids.insert(poi).second)
            out.push_back("creature " + sid + ": lair poi '" + poi + "' is empty or taken");
        for (const auto& t : L.value("terrain", std::vector<std::string>{}))
            if (!kinds.count(t)) out.push_back("creature " + sid + ": lair wants unknown terrain '" + t + "'");
    }
    if (r.titles.empty()) out.push_back("worldmap.json: domains.titles is empty");
    return out;
}

bool worldMapEnsure(World& w) {
    const Rules& r = rules(w.content());
    if (!r.loaded || !r.enabled) return false;
    WorldMapState s = loadWorldMap(w);
    if (s.inited) return true;
    if (w.settlements().empty() || !loadRealm(w).inited) return false;   // the land is laid around the realm's settlements
    layOut(w, r, s);
    saveWorldMap(w, s);
    return true;
}

void worldMapDaily(World& w) { worldMapPass(w, loadRealm(w)); }

int worldMapLairRaids(World& w, int day) {
    WorldMapState s = loadWorldMap(w);
    if (!s.inited) return 0;
    if (!lairRaids(w, s, day)) return 0;
    saveWorldMap(w, s);
    int n = 0;
    for (const auto& p : s.pois) n += !p.creature.empty() && p.lastRaid == day;
    return n;
}

void worldMapPass(World& w, const RealmState& realm) {
    const Rules& r = rules(w.content());
    if (!r.loaded || !r.enabled) return;
    if (!realm.inited) return;
    WorldMapState s = loadWorldMap(w);
    bool dirty = false;
    if (!s.fealty) {
        if (r.fealtyOn) swearFealty(w, r);
        s.fealty = true;
        dirty = true;
    }
    if (!s.inited) {
        layOut(w, r, s);
        dirty = true;
    }
    dirty |= lairRaids(w, s, w.day());   // LR-170
    // a battle of the realm's wars leaves a field
    for (const RealmWar& war : realm.wars) {
        auto it = std::find_if(s.battlesSeen.begin(), s.battlesSeen.end(), [&](const auto& e) { return e.first == war.id; });
        const int seen = it == s.battlesSeen.end() ? 0 : it->second;
        if (war.battles <= seen) continue;
        const auto& ps = w.settlements();
        auto a = ps.find(war.attacker), d = ps.find(war.defender);
        if (a != ps.end() && d != ps.end()) {
            for (int b = seen; b < war.battles; ++b) {
                Rng rng = agentRng(w.seed(), war.id, b, "worldmap:battle");
                const double t = rng.range(0.35, 0.75);
                WorldPoi p;
                p.id = s.nextId++;
                p.kind = "battlefield";
                p.km = {a->second.km.x + (d->second.km.x - a->second.km.x) * t + rng.range(-r.battleJitter, r.battleJitter),
                        a->second.km.y + (d->second.km.y - a->second.km.y) * t + rng.range(-r.battleJitter, r.battleJitter)};
                p.day = w.day();
                p.a = war.attacker;
                p.b = war.defender;
                s.pois.push_back(p);
            }
            int fields = 0;
            for (const auto& p : s.pois) fields += p.kind == "battlefield";
            while (fields > r.battlefieldsMax) {   // the oldest are forgotten
                auto old = std::find_if(s.pois.begin(), s.pois.end(), [](const WorldPoi& p) { return p.kind == "battlefield"; });
                s.pois.erase(old);
                --fields;
            }
        }
        if (it == s.battlesSeen.end()) s.battlesSeen.push_back({war.id, war.battles});
        else it->second = war.battles;
        dirty = true;
    }
    if (dirty) saveWorldMap(w, s);
}

void installWorldMap(World&) {
    static const bool once = [] {
        addRealmHook([](World& w, RealmState& s) { worldMapPass(w, s); });
        return true;
    }();
    (void)once;
}

EntityId worldMapDomainHead(const World& w, EntityId id) {
    const auto& ps = w.settlements();
    EntityId at = id;
    for (int i = 0; i < 16; ++i) {
        auto it = ps.find(at);
        if (it == ps.end()) return at;
        const EntityId up = it->second.liege;
        if (up == kNone || up == id) return at;
        auto ut = ps.find(up);
        if (ut == ps.end() || ut->second.ruined) return at;
        at = up;
    }
    return at;
}

std::string worldMapDomainTitle(const World& w, EntityId head) {
    const Rules& r = rules(w.content());
    const auto& ps = w.settlements();
    auto it = ps.find(head);
    if (it == ps.end()) return "free";
    const Settlement& h = it->second;
    int members = 0;
    for (const auto& [id, p] : ps)
        if (!p.ruined && worldMapDomainHead(w, id) == head) ++members;
    for (const TitleRule& t : r.titles) {
        if (members < t.minMembers) continue;
        if (!t.headSizes.empty() && !t.headSizes.count(h.size)) continue;
        if (!t.institution.empty() && h.institution != t.institution) continue;
        if (!t.tag.empty() && !h.tagged(t.tag)) continue;
        return t.title;
    }
    return "free";
}

std::vector<std::pair<EntityId, EntityId>> worldMapRoads(const World& w) {
    const Rules& r = rules(w.content());
    std::vector<const Settlement*> on;
    for (const auto& [id, p] : w.settlements()) {
        bool skip = r.roadSkipInst.count(p.institution) > 0 || (p.ruined && r.roadSkipTags.count("ruined"));
        for (const auto& t : r.roadSkipTags)
            if (p.tagged(t)) skip = true;
        if (!skip) on.push_back(&p);
    }
    std::set<std::pair<EntityId, EntityId>> out;
    for (const Settlement* a : on) {
        std::vector<std::pair<double, EntityId>> near;
        for (const Settlement* b : on)
            if (a != b) {
                const double d = dist(a->km, b->km);
                if (d <= r.roadKm) near.push_back({d, b->id});
            }
        std::sort(near.begin(), near.end());
        for (size_t i = 0; i < near.size() && static_cast<int>(i) < r.roadLinks; ++i)
            out.insert({std::min(a->id, near[i].second), std::max(a->id, near[i].second)});
    }
    return {out.begin(), out.end()};
}

} // namespace lr::sim
