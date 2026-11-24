#include <set>
#include <functional>
#include "sim/worldgen.hpp"
#include "sim/village_life.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/village_plan.hpp"
#include "sim/structure_plan.hpp"
#include "sim/rooms.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace altera::sim {

Json loadJsonFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return Json::parse(f);
}

namespace {

constexpr double kPi = 3.14159265358979323846;

double lattice(std::uint64_t seed, int x, int y) {
    const std::uint64_t h = hashCombine(hashCombine(seed, static_cast<std::uint64_t>(x) + 0x10000u),
                                        static_cast<std::uint64_t>(y) + 0x10000u);
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

// Smooth value noise, one octave per call.
double valueNoise(std::uint64_t seed, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = lattice(seed, x0, y0), b = lattice(seed, x0 + 1, y0);
    const double c = lattice(seed, x0, y0 + 1), d = lattice(seed, x0 + 1, y0 + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

double fbm(std::uint64_t seed, double x, double y) {
    return 0.6 * valueNoise(seed, x, y) + 0.3 * valueNoise(seed + 1, x * 2.1, y * 2.1) +
           0.1 * valueNoise(seed + 2, x * 4.3, y * 4.3);
}

std::string makeName(Rng& r, bool female, const Content* c = nullptr) {
    // the content's list (AL-61): names the loc has keys for
    // (the same three draws as the syllables, so a world differs from before only by its names)
    if (c && !(female ? c->givenNamesF : c->givenNamesM).empty()) {
        const auto& pool = female ? c->givenNamesF : c->givenNamesM;
        const int a = r.irange(0, 19), b = r.irange(0, 9), e = r.irange(0, 4);
        return pool[static_cast<size_t>(a * 50 + b * 5 + e) % pool.size()];
    }
    static const char* a[] = {"Ar", "Bel", "Cor", "Dra", "El", "Fen", "Gor", "Hal", "Is", "Kar",
                              "Lor", "Mor", "Nar", "Or", "Ral", "Sar", "Tor", "Ul", "Var", "Yr"};
    static const char* b[] = {"an", "ek", "il", "or", "un", "as", "em", "ith", "ar", "ol"};
    static const char* fem[] = {"a", "ia", "elle", "is", "wen"};
    static const char* mas[] = {"", "ric", "mund", "ald", "os"};
    std::string n = a[r.irange(0, 19)];
    n += b[r.irange(0, 9)];
    n += female ? fem[r.irange(0, 4)] : mas[r.irange(0, 4)];
    return n;
}

std::string makeFamilyName(Rng& r, const Content* c = nullptr) {
    if (c && !c->familyNames.empty()) {
        const int a = r.irange(0, 9), b = r.irange(0, 7);
        return c->familyNames[static_cast<size_t>(a * 8 + b) % c->familyNames.size()];
    }
    static const char* a[] = {"Ash", "Black", "Crow", "Dusk", "Grim", "Iron", "Moss", "Red", "Thorn", "Wolf"};
    static const char* b[] = {"vale", "barrow", "mere", "hold", "wood", "stone", "ford", "fell"};
    return std::string(a[r.irange(0, 9)]) + b[r.irange(0, 7)];
}

std::vector<std::string> pickTraits(Rng& r, const Content& c, const std::vector<std::string>& pool, int count) {
    std::vector<std::string> out;
    double total = 0.0;
    for (const auto& t : pool) {
        auto d = c.traits.find(t);
        total += d == c.traits.end() ? 1.0 : std::max(0.0, d->second.weightInPool);
    }
    auto draw = [&]() -> const std::string& {
        double x = r.uniform() * total;
        for (const auto& t : pool) {
            auto d = c.traits.find(t);
            x -= d == c.traits.end() ? 1.0 : std::max(0.0, d->second.weightInPool);
            if (x <= 0.0) return t;
        }
        return pool.back();
    };
    for (int guard = 0; guard < 32 && static_cast<int>(out.size()) < count && !pool.empty() && total > 0.0; ++guard) {
        const std::string& t = draw();
        if (std::find(out.begin(), out.end(), t) != out.end()) continue;
        bool clash = false;
        for (const auto& [x, y] : c.exclusiveTraits)
            for (const auto& o : out)
                if ((x == t && y == o) || (y == t && x == o)) clash = true;
        if (!clash) out.push_back(t);
    }
    return out;
}

} // namespace

World makeTestWorld(const Content& content, const Json& cfg, std::uint64_t seedOverride) {
    const std::uint64_t seed = seedOverride ? seedOverride : cfg.value("seed", std::uint64_t{1});
    World w(content, seed);
    {
        // start_day (day of year) or season: spring | summer | autumn | winter.
        static const char* kSeasons[] = {"spring", "summer", "autumn", "winter"};
        int startDay = cfg.value("start_day", 100); // early summer by default
        const std::string season = cfg.value("season", std::string());
        for (int i = 0; i < 4; ++i)
            if (season == kSeasons[i]) startDay = i * (kDaysPerYear / 4) + 10;
        w.setStartDay(startDay);
    }
    Rng rng(hashCombine(seed, hashStr("worldgen")));

    // --- map --------------------------------------------------------------
    MapGrid& m = w.map();
    m.sizeM = std::clamp(cfg.value("size_m", 12000.0), 8000.0, 16000.0);
    m.cellM = cfg.value("cell_m", 250.0);
    m.w = m.h = static_cast<int>(m.sizeM / m.cellM);
    for (const auto& b : cfg.value("biomes", Json::array({"meadow", "forest", "marsh"})))
        m.biomeNames.push_back(b.is_string() ? b.get<std::string>() : b.at("id").get<std::string>());
    auto biomeIdx = [&](const std::string& n) -> int {
        for (size_t i = 0; i < m.biomeNames.size(); ++i)
            if (m.biomeNames[i] == n) return static_cast<int>(i);
        return -1;
    };
    const int iMeadow = std::max(0, biomeIdx("meadow"));
    const int iForest = biomeIdx("forest");
    const int iMarsh = biomeIdx("marsh");
    m.biome.assign(static_cast<size_t>(m.w * m.h), static_cast<std::uint8_t>(iMeadow));
    m.water.assign(static_cast<size_t>(m.w * m.h), 0);
    const double scale = 1.0 / 10.0; // noise lattice every ~10 cells (2.5 km)
    const double phase = rng.range(0.0, 2.0 * kPi);
    const bool river = cfg.value("river", true);
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
            const size_t i = static_cast<size_t>(y * m.w + x);
            const double moist = fbm(seed ^ 0xA11CEull, x * scale, y * scale);
            int b = iMeadow;
            if (iMarsh >= 0 && moist > 0.66) b = iMarsh;
            else if (iForest >= 0 && moist > 0.44) b = iForest;
            m.biome[i] = static_cast<std::uint8_t>(b);
            if (river) {
                const double yy = (y + 0.5) * m.cellM;
                const double rx = m.sizeM * 0.5 + m.sizeM * 0.15 * std::sin(yy / m.sizeM * 2.0 * kPi * 1.3 + phase);
                if (std::abs((x + 0.5) * m.cellM - rx) < m.cellM * 0.75) m.water[i] = 1;
            }
        }

    // A map from outside (the game reads it from the world it draws): biome
    // names and water a cell, row by row, w x h of this grid.
    if (cfg.contains("map_biome") && cfg.contains("map_water")) {
        const Json& mb = cfg.at("map_biome");
        const Json& mw = cfg.at("map_water");
        if (mb.size() == m.biome.size() && mw.size() == m.water.size())
            for (size_t i = 0; i < m.biome.size(); ++i) {
                const int b = biomeIdx(mb[i].get<std::string>());
                m.biome[i] = static_cast<std::uint8_t>(b >= 0 ? b : iMeadow);
                m.water[i] = static_cast<std::uint8_t>(mw[i].get<int>() ? 1 : 0);
            }
    }

    // --- settlement site: the meadow cell nearest the map centre beside the river
    Vec2 center{m.sizeM * 0.5, m.sizeM * 0.5};
    {
        double best = 1e18;
        for (int y = 2; y < m.h - 2; ++y)
            for (int x = 2; x < m.w - 2; ++x) {
                const size_t i = static_cast<size_t>(y * m.w + x);
                if (m.water[i] || m.biome[i] != iMeadow) continue;
                bool nearWater = !river;
                // The window (radius 3) is wider than the scan margin (2):
                // neighbours outside the map are skipped, not read.
                for (int dy = -3; dy <= 3 && !nearWater; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
                        if (m.water[static_cast<size_t>(ny * m.w + nx)]) nearWater = true;
                    }
                if (!nearWater) continue;
                const Vec2 p{(x + 0.5) * m.cellM, (y + 0.5) * m.cellM};
                const double d = dist(p, Vec2{m.sizeM * 0.5, m.sizeM * 0.5});
                if (d < best) {
                    best = d;
                    center = p;
                }
            }
    }
    // AL-8: the world may name the village's place (the game picks it on the terrain).
    if (cfg.contains("village_centre")) center = cfg["village_centre"].get<Vec2>();
    Settlement& st = w.settlements()[w.newId()];
    st.id = w.settlements().rbegin()->first;
    st.name = cfg.value("settlement_name", std::string("Emberford"));
    st.center = center;

    // AL-8: a planned village (roads, a square, lots facing the roads, fields
    // beyond the yards) when the world brings its ground (the game passes
    // village_site from the terrain) or asks for one; else the old ring.
    const bool planned = cfg.contains("village_site") || cfg.value("village_plan", false);
    VillagePlan vplan;
    // AL-76 / AL-81: houses put together from rooms (rooms.json): each family's
    // by its recipe - a share of them a trade's (the smith and the innkeeper
    // first), the rest a farmer's - and the shared ones whose objects the
    // world has (the chapel's shrine, the mill); each raised from modules and
    // furnished room by room, then given a lot of its size in its zone. What
    // a house is for is then read from its rooms (sim/rooms.hpp).
    const StructureCatalog scat = planned ? parseStructures(content.roomsJson) : StructureCatalog{};
    struct Built {
        const StructureTypeDef* def = nullptr;
        StructurePlan plan;
        int family = -1;        // index of the family it houses, -1 shared
        StructureFrame frame;
        double h = 0.0;
        std::vector<bool> used; // stations already standing
        int index = -1;         // in st.structures
    };
    std::vector<Built> builds;
    std::vector<int> peopleOf;   // planned: each family's size drawn before the plan
    if (planned) {
        const int fams = std::max(1, cfg.value("families", 3));
        int lo = 4, hi = 6;
        if (cfg.contains("people_per_family")) lo = cfg["people_per_family"].at(0).get<int>(), hi = cfg["people_per_family"].at(1).get<int>();
        for (int f = 0; f < fams; ++f) peopleOf.push_back(rng.irange(lo, hi));
        if (scat.loaded() && scat.find("farmer")) {
            Rng sr(hashCombine(seed, hashStr("structures")));
            std::vector<std::string> trades;
            for (const auto& t : scat.trades)
                if (scat.find(t)) trades.push_back(t);
            for (std::size_t i = trades.size(); i > 2; --i)   // the smith and the innkeeper lead, the rest by seed
                std::swap(trades[i - 1], trades[2 + std::size_t(sr.irange(0, int(i) - 3))]);
            const int nTrade = fams >= 2 ? std::clamp(int(std::lround(fams * scat.tradeShare)), 1, std::min<int>(fams, int(trades.size()))) : 0;
            std::vector<int> idx(static_cast<std::size_t>(fams));
            for (int f = 0; f < fams; ++f) idx[std::size_t(f)] = f;
            for (int i = fams - 1; i > 0; --i) std::swap(idx[std::size_t(i)], idx[std::size_t(sr.irange(0, i))]);
            std::vector<std::string> famType(std::size_t(fams), "farmer");
            for (int t = 0; t < nTrade; ++t) famType[std::size_t(idx[std::size_t(t)])] = trades[std::size_t(t)];
            for (int f = 0; f < fams; ++f) {
                Built b;
                b.def = scat.find(famType[std::size_t(f)]);
                b.family = f;
                b.plan = planStructure(scat, content.build, *b.def, {peopleOf[std::size_t(f)], hashCombine(seed, std::uint64_t(f) + 1)});
                builds.push_back(std::move(b));
            }
            for (const auto& id : scat.shared) {
                const StructureTypeDef* d = scat.find(id);
                if (!d) continue;
                bool hosted = d->hosts.empty();
                for (const auto& h : d->hosts) {
                    auto o = content.objects.find(h);
                    if (o != content.objects.end() && o->second.perSettlement > 0) hosted = true;
                }
                if (!hosted) continue;
                Built b;
                b.def = d;
                b.plan = planStructure(scat, content.build, *d, {4, hashCombine(seed, hashStr("shared:" + id))});
                builds.push_back(std::move(b));
            }
        }
        const VillageSite site = VillageSite::fromJson(cfg.value("village_site", Json::object()));
        VillageWanted want;
        want.houses = want.fields = fams;
        want.cellM = content.build.cellM;
        want.layout = VillageLayout::fromJson(scat.layout);
        for (const auto& b : builds) {
            LotSpec L;
            L.recipe = b.def->id;   // AL-84: what the site gives it
            L.water = b.def->siteWater, L.wood = b.def->siteWood;
            L.entrance = b.def->siteEntrance, L.downwind = b.def->siteDownwind;
            L.nuisance = b.def->nuisance, L.quiet = b.def->quiet;
            L.w = b.plan.i1 - b.plan.i0;
            L.d = b.plan.j1 - b.plan.j0;
            L.zone = b.def->zone;
            L.home = b.def->home;
            L.yardD = b.def->home ? scat.yardD : 4.0;
            want.lots.push_back(L);
        }
        vplan = planVillage(site, m, center, want, seed);
        center = vplan.square;
        st.center = center;
        st.roads = vplan.roads;
        st.roadWidth = vplan.roadWidth;
        st.roadKind = vplan.roadKind;     // AL-84
        st.entrance = vplan.entrance;
        for (const auto& z : vplan.zones) st.zones.push_back({z.kind, z.rect.c, z.rect.yaw, z.rect.hw, z.rect.hd});
        st.square = vplan.square;
        st.squareR = vplan.squareR;
        // the buildings on their lots: the frame, the floor, the modules as built pieces
        for (std::size_t k = 0; k < builds.size() && k < vplan.lots.size(); ++k) {
            Built& b = builds[k];
            const VillageLot& L = vplan.lots[k];
            if (!L.placed) continue;
            const double c = content.build.cellM;
            b.frame.cellM = c;
            b.frame.q = int(std::lround(L.house.yaw / (kPi / 2))) & 3;
            const Vec2 fc = footprintCentre(b.plan, c);
            const StructureFrame f0{b.frame.q, 0, 0, c};
            const Vec2 r0 = f0.point(fc.x, fc.y);
            b.frame.gx = int(std::lround((L.house.c.x - r0.x) / c));
            b.frame.gy = int(std::lround((L.house.c.y - r0.y) / c));
            b.h = site.loaded() ? L.groundMax + 0.1 : 0.0;
            b.used.assign(b.plan.stations.size(), false);
            Settlement::Structure rec;
            rec.recipe = b.def->id;   // AL-84: what it was planned as (its rooms tell what it is)
            rec.q = b.frame.q, rec.gx = b.frame.gx, rec.gy = b.frame.gy;
            rec.i0 = b.plan.i0, rec.j0 = b.plan.j0, rec.i1 = b.plan.i1, rec.j1 = b.plan.j1;
            rec.h = b.h;
            rec.door = b.frame.point((b.plan.doorI + 0.5) * c, -0.6);
            // a fence round a home's yard (its back and sides, a gap at the back for the gate)
            if (b.def->home && !scat.fence.empty()) {
                const PlanRect& Y = L.yard;
                const double seg = 4.0;
                auto run = [&](Vec2 a, Vec2 z, double yaw, bool gate) {
                    const double len = dist(a, z);
                    const int n = std::max(1, int(std::lround(len / seg)));
                    for (int i = 0; i < n; ++i) {
                        if (gate && i == n / 2) continue;
                        const double t = (i + 0.5) / n;
                        rec.props.push_back({scat.fence, {a.x + (z.x - a.x) * t, a.y + (z.y - a.y) * t}, yaw});
                    }
                };
                run(Y.at(-Y.hw, -Y.hd), Y.at(Y.hw, -Y.hd), Y.yaw, true);              // the back
                run(Y.at(-Y.hw, -Y.hd), Y.at(-Y.hw, Y.hd), Y.yaw + kPi / 2, false);    // the sides
                run(Y.at(Y.hw, -Y.hd), Y.at(Y.hw, Y.hd), Y.yaw + kPi / 2, false);
            }
            b.index = int(st.structures.size());
            st.structures.push_back(std::move(rec));
        }
    }
    // AL-81: a house's modules, built pieces of its household (kNone: the settlement's)
    auto raise = [&](Built& b, EntityId fam) {
        if (b.index < 0) return;
        for (const auto& lp : b.plan.pieces) {
            BuildPiece bp;
            bp.id = w.newId();
            bp.kind = lp.kind;
            b.frame.cell(lp.i, lp.j, bp.x, bp.y);
            bp.z = lp.z;
            bp.rot = b.frame.rot(lp.rot);
            bp.h = b.h;
            bp.built = true;
            bp.family = fam;
            w.pieces()[bp.id] = bp;
        }
        w.piecesDirty = true;
    };
    for (auto& b : builds)
        if (b.family < 0) raise(b, kNone);
    // AL-76: a building's station of `type` not yet standing (index into its plan), -1 none
    auto freeStation = [&](Built& b, const std::string& type) {
        for (std::size_t i = 0; i < b.plan.stations.size(); ++i)
            if (!b.used[i] && b.plan.stations[i].what == type) return int(i);
        return -1;
    };

    // --- natural resources --------------------------------------------------
    const double cellKm2 = (m.cellM / 1000.0) * (m.cellM / 1000.0);
    for (const auto& [id, def] : content.objects) {
        if (def.biomes.empty() || def.perKm2 <= 0.0) continue;
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x) {
                const size_t i = static_cast<size_t>(y * m.w + x);
                if (m.water[i]) continue;
                bool fits = std::find(def.biomes.begin(), def.biomes.end(), m.biomeNames[m.biome[i]]) != def.biomes.end();
                // "river" is not a biome of the grid: it is a land cell on the bank.
                if (!fits && std::find(def.biomes.begin(), def.biomes.end(), "river") != def.biomes.end())
                    for (int dy = -1; dy <= 1 && !fits; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
                            if (m.water[static_cast<size_t>(ny * m.w + nx)]) fits = true;
                        }
                if (!fits) continue;
                Rng cr(hashCombine(hashCombine(seed, hashStr(id)), i));
                const double expect = def.perKm2 * cellKm2;
                int n = static_cast<int>(expect);
                if (cr.chance(expect - n)) ++n;
                for (int k = 0; k < n; ++k) {
                    WorldObject o;
                    o.type = id;
                    o.pos = {(x + cr.uniform()) * m.cellM, (y + cr.uniform()) * m.cellM};
                    o.stock = def.stock;
                    WorldObject& placed = w.addObject(o);
                    if (def.regrowEvery > 0)
                        w.schedule({def.regrowEvery + cr.irange(0, static_cast<int>(def.regrowEvery)), 0, "regrow",
                                    kNone, kNone, placed.id, {}});
                }
            }
    }

    // --- families -----------------------------------------------------------
    const int families = cfg.value("families", 3);
    int ppfLo = 4, ppfHi = 6;
    if (cfg.contains("people_per_family")) {
        ppfLo = cfg["people_per_family"].at(0).get<int>();
        ppfHi = cfg["people_per_family"].at(1).get<int>();
    }
    std::vector<std::string> pool = cfg.value("traits_pool", std::vector<std::string>{});
    if (pool.empty())
        for (const auto& [id, t] : content.traits) pool.push_back(id);
    // Start knowledge: explicit lists in the world file win (fixtures); else
    // knowledge.json start rules + household_must / settlement_must coverage.
    const bool explicitKnowledge = cfg.contains("knowledge_start") || content.knowledge.empty();
    const auto knowledgeStart = cfg.value("knowledge_start", std::vector<std::string>{"foraging", "cooking"});
    const auto childKnowledge = cfg.value("child_knowledge", std::vector<std::string>{"foraging"});
    auto startKnowledge = [&](Rng& r, int ageYears) {
        std::set<std::string> out;
        if (explicitKnowledge) {
            for (const auto& k : ageYears < 14 ? childKnowledge : knowledgeStart) out.insert(k);
            return out;
        }
        for (const auto& [id, k] : content.knowledge) {
            if (k.start == "all_from_child" && ageYears >= 5) out.insert(id);
            else if (k.start == "all_from_youth" && ageYears >= 12) out.insert(id);
            else if (k.start == "all_adults" && ageYears >= 14) out.insert(id);
            else if (ageYears >= 14 && k.startChanceAdult > 0.0 && r.chance(k.startChanceAdult)) out.insert(id);
        }
        // Only keep what the prerequisites allow.
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = out.begin(); it != out.end();) {
                auto d = content.knowledge.find(*it);
                bool ok = true;
                if (d != content.knowledge.end())
                    for (const auto& req : d->second.requiresKnowledge)
                        if (!out.count(req)) ok = false;
                if (ok) ++it;
                else {
                    it = out.erase(it);
                    changed = true;
                }
            }
        }
        return out;
    };
    // Teach `id` (and its prerequisites) to a character.
    std::function<void(Character&, const std::string&, Rng&)> teach = [&](Character& ch, const std::string& id, Rng& r) {
        if (ch.knowledge.count(id)) return;
        auto d = content.knowledge.find(id);
        if (d != content.knowledge.end())
            for (const auto& req : d->second.requiresKnowledge) teach(ch, req, r);
        ch.knowledge.insert(id);
        ch.skills[id] = r.range(0.2, 0.7);
    };
    const int traitsPer = cfg.value("traits_per_person", 2);
    const double knowRadius = cfg.value("know_radius_m", 1500.0);

    auto spawnSettlementObject = [&](const std::string& type, Vec2 pos, EntityId fam, const double* yaw = nullptr) -> EntityId {
        WorldObject o;
        o.type = type;
        o.pos = pos;
        o.ownerFamily = fam;
        if (yaw) {
            o.yaw = *yaw;
            o.faced = true;
        }
        auto d = content.objects.find(type);
        if (d != content.objects.end()) o.stock = d->second.stock;
        return w.addObject(o).id;
    };

    // Shared settlement objects (per_settlement) around the centre; in a
    // planned village round the square.
    std::size_t squareSpot = 0;
    for (const auto& [type, def] : content.objects) {
        if (!def.settlement) continue;
        Rng own(hashCombine(seed, hashStr("settlement:" + type)));
        Rng& r = def.ownSeed ? own : rng;
        for (int k = 0; k < def.perSettlement; ++k) {
            Vec2 p{center.x + r.range(-15.0, 15.0), center.y + r.range(-15.0, 15.0)};
            bool housed = false;
            for (auto& b : builds) {   // AL-76: in the house whose room holds it (a chapel's altar, a bakery's oven)
                if (b.index < 0) continue;
                const int si = freeStation(b, type);
                if (si < 0) continue;
                const LocalThing& t = b.plan.stations[std::size_t(si)];
                const double yaw = b.frame.yaw(t.yaw);
                b.used[std::size_t(si)] = true;
                st.structures[std::size_t(b.index)].objects.push_back(
                    spawnSettlementObject(type, b.frame.point(t.u, t.v), kNone, &yaw));
                housed = true;
                break;
            }
            if (housed) continue;
            const VillageZone* works = nullptr;   // AL-84: a smoky works out of the village (rooms.json commons work:<object>)
            if (planned)
                for (const auto& z : vplan.zones)
                    if (z.kind == "work:" + type) works = &z;
            // AL-84: things kept in the yards (rooms.json layout.in_yards: the hives),
            // one to a home's yard, at its back corner
            bool inYard = false;
            if (planned)
                for (const auto& y : scat.layout.value("in_yards", std::vector<std::string>{}))
                    if (y == type) inYard = true;
            std::vector<const VillageLot*> homeLots;
            if (inYard)
                for (std::size_t i = 0; i < vplan.lots.size() && i < builds.size(); ++i)
                    if (vplan.lots[i].placed && builds[i].def && builds[i].def->home) homeLots.push_back(&vplan.lots[i]);
            if (inYard && !homeLots.empty()) {
                const std::size_t i = (std::size_t(k) * 3 + 1) % homeLots.size();   // spread over the village
                const PlanRect& Y = homeLots[i]->yard;
                p = Y.at((k % 2 ? 1.0 : -1.0) * std::max(0.0, Y.hw - 1.5), -std::max(0.0, Y.hd - 1.5));
            } else if (works && k == 0) p = works->rect.c;
            else if (planned && type == scat.layout.value("square_centre", std::string()) && k == 0) p = vplan.square;   // AL-84: the well in the middle
            else if (planned && squareSpot < vplan.squareSpots.size()) p = vplan.squareSpots[squareSpot++];
            spawnSettlementObject(type, p, kNone);
        }
    }

    // AL-76: whatever else a building holds (an oven, a quern, the forge and the anvil) stands in it
    auto fillStations = [&](Built& b, EntityId fam) {
        if (b.index < 0) return;
        for (std::size_t i = 0; i < b.plan.stations.size(); ++i) {
            if (b.used[i]) continue;
            const LocalThing& t = b.plan.stations[i];
            const auto d = content.objects.find(t.what);
            if (d == content.objects.end() || d->second.perSettlement > 0 || d->second.perFamily > 0 || d->second.perPerson > 0)
                continue;   // unknown here, or the settlement's / the household's own loop places it
            const double yaw = b.frame.yaw(t.yaw);
            b.used[i] = true;
            const EntityId oid = spawnSettlementObject(t.what, b.frame.point(t.u, t.v), fam, &yaw);
            if (WorldObject* o = w.object(oid)) o->storey = t.z;
            st.structures[std::size_t(b.index)].objects.push_back(oid);
        }
        // AL-81: the furniture (sim objects: the rooms are read from them)
        for (const auto& t : b.plan.decor) {
            if (!content.objects.count(t.what)) continue;
            const double yaw = b.frame.yaw(t.yaw);
            const EntityId oid = spawnSettlementObject(t.what, b.frame.point(t.u, t.v), fam, &yaw);
            if (WorldObject* o = w.object(oid)) o->storey = t.z;
            st.structures[std::size_t(b.index)].objects.push_back(oid);
        }
    };
    for (auto& b : builds)
        if (b.family < 0) fillStations(b, kNone);

    std::vector<EntityId> everyone;
    for (int f = 0; f < families; ++f) {
        Family fam;
        fam.id = w.newId();
        fam.name = makeFamilyName(rng, &content);
        Dynasty dyn;
        dyn.id = w.newId();
        dyn.name = "House " + fam.name;
        fam.dynasty = dyn.id;
        fam.settlement = st.id;
        const double ang = 2.0 * kPi * (f + 0.25 * rng.uniform()) / std::max(1, families);
        const double rad = rng.range(60.0, 120.0);
        Vec2 house{center.x + std::cos(ang) * rad, center.y + std::sin(ang) * rad};
        const VillageLot* lot =
            planned && f < int(vplan.lots.size()) && vplan.lots[std::size_t(f)].placed ? &vplan.lots[std::size_t(f)] : nullptr;
        const PlanRect* field = lot && lot->field >= 0 ? &vplan.fields[std::size_t(lot->field)] : nullptr;
        if (lot) house = lot->house.c;
        const int people = f < int(peopleOf.size()) ? peopleOf[std::size_t(f)] : rng.irange(ppfLo, ppfHi);
        // AL-76: the family's building (its home, maybe a trade's)
        Built* home = f < int(builds.size()) && builds[std::size_t(f)].family == f && builds[std::size_t(f)].index >= 0
                          ? &builds[std::size_t(f)]
                          : nullptr;
        if (home) {
            st.structures[std::size_t(home->index)].family = fam.id;
            raise(*home, fam.id);   // the modules: built pieces of the household
        }
        int bedN = 0;
        for (const auto& [type, def] : content.objects) {
            if (!def.settlement) continue;
            const int n = def.perFamily + def.perPerson * people;
            for (int k = 0; k < n; ++k) {
                Vec2 p{house.x + rng.range(-6.0, 6.0), house.y + rng.range(-6.0, 6.0)};
                if (def.hasCrop) { // fields lie outside the houses, behind each household's yard
                    const double far = rad + rng.range(90.0, 160.0) + 40.0 * k;
                    p = {center.x + std::cos(ang) * far, center.y + std::sin(ang) * far};
                }
                // AL-8: on the household's lot - the house facing the road, the
                // hearth and beds inside, the store beside, the garden and the
                // rest in the yard, the field beyond it.
                double syaw = 0.0;
                int bedStorey = 0;
                const double* yaw = nullptr;
                if (lot) {
                    const PlanRect& H = lot->house;
                    const double ox = std::clamp(p.x - house.x, -6.0, 6.0) / 6.0, oy = std::clamp(p.y - house.y, -6.0, 6.0) / 6.0;
                    const int si = home ? freeStation(*home, type) : -1;
                    if (home && type == "house") {   // AL-76: the house is the main volume
                        const StructureVolume& mv = home->plan.volumes[0];
                        const double c = content.build.cellM;
                        p = home->frame.point((mv.i0 + mv.i1) * 0.5 * c, (mv.j0 + mv.j1) * 0.5 * c);
                        yaw = &H.yaw;
                    } else if (si >= 0) {   // at its slot in the building
                        const LocalThing& t = home->plan.stations[std::size_t(si)];
                        home->used[std::size_t(si)] = true;
                        p = home->frame.point(t.u, t.v);
                        syaw = home->frame.yaw(t.yaw);
                        yaw = &syaw;
                    } else if (home && type == "bed" && bedN < int(home->plan.beds.size())) {   // AL-81: in its bedroom
                        const LocalThing& t = home->plan.beds[std::size_t(bedN++)];
                        p = home->frame.point(t.u, t.v);
                        syaw = home->frame.yaw(t.yaw);
                        yaw = &syaw;
                        bedStorey = t.z;
                    } else if (type == "house") p = H.c, yaw = &H.yaw;
                    else if (type == "hearth") p = H.at(0.0, -1.0);
                    else if (type == "bed") p = H.at(ox * H.hw * 0.6, oy * H.hd * 0.6);
                    else if (type == "storage") p = H.at(H.hw + 3.0, -H.hd * 0.4), yaw = &H.yaw;
                    else if (type == "garden") p = lot->yard.c, yaw = &H.yaw;
                    else if (def.hasCrop && field && k == 0) p = field->c, yaw = &field->yaw;
                    else if (!def.hasCrop) p = lot->yard.at(ox * lot->yard.hw * 0.8, oy * lot->yard.hd * 0.8);
                }
                const EntityId oid = spawnSettlementObject(type, p, fam.id, yaw);
                if (bedStorey)
                    if (WorldObject* o = w.object(oid)) o->storey = bedStorey;
                if (home && (type == "house" || yaw)) st.structures[std::size_t(home->index)].objects.push_back(oid);
                if (type == "storage" && fam.storage == kNone) fam.storage = oid;
                if (type == "hearth" && fam.hearth == kNone) fam.hearth = oid;
            }
        }

        if (home) fillStations(*home, fam.id);   // AL-76: the trade's stations (the forge, the anvil...)

        std::vector<EntityId> members;
        auto makePerson = [&](char sex, int ageYears, const std::string& role) -> Character& {
            Character c;
            c.id = w.newId();
            c.body.sex = sex;
            c.body.ageDays = ageYears * kDaysPerYear + rng.irange(0, kDaysPerYear - 1);
            c.name = makeName(rng, sex == 'f', &content) + " " + fam.name;
            c.traits = pickTraits(rng, content, pool, traitsPer);
            for (const auto& n : content.needs) c.needs[n.id] = rng.range(n.startLo, n.startHi);
            const bool child = c.body.stage() == "child";
            c.knowledge = startKnowledge(rng, ageYears);
            for (const auto& k : c.knowledge) c.skills[k] = child ? rng.range(0.05, 0.2) : rng.range(0.2, 0.7);
            c.status.family = fam.id;
            c.status.dynasty = dyn.id;
            c.status.settlement = st.id;
            c.status.role = role;
            c.pos = {house.x + rng.range(-4.0, 4.0), house.y + rng.range(-4.0, 4.0)};
            if (lot) c.pos = lot->house.at(c.pos.x - house.x, lot->house.hd + 2.0 + std::fabs(c.pos.y - house.y) * 0.5);
            c.home = fam.storage;
            Character& placed = w.addCharacter(std::move(c));
            members.push_back(placed.id);
            return placed;
        };
        Character& father = makePerson('m', rng.irange(26, 46), "head");
        const EntityId fatherId = father.id;
        // AL-76: a trade's household: its head knows the trade
        if (home && !home->def->trade.empty() && content.knowledge.count(home->def->trade)) {
            teach(father, home->def->trade, rng);
            father.skills[home->def->trade] = std::max(father.skills[home->def->trade], 0.55);
        }
        Character& mother = makePerson('f', rng.irange(24, 42), "spouse");
        const EntityId motherId = mother.id;
        w.character(fatherId)->status.spouse = motherId;
        w.character(motherId)->status.spouse = fatherId;
        int left = people - 2;
        if (left >= 3 && rng.chance(0.5)) {
            makePerson(rng.chance(0.5) ? 'f' : 'm', rng.irange(61, 72), "elder");
            --left;
        }
        for (int k = 0; k < left; ++k) {
            Character& kid = makePerson(rng.chance(0.5) ? 'f' : 'm', rng.irange(3, 19), "child");
            kid.status.father = fatherId;
            kid.status.mother = motherId;
            w.character(fatherId)->status.children.push_back(kid.id);
            w.character(motherId)->status.children.push_back(kid.id);
        }
        // Every household covers household_must (on its head at least).
        if (!explicitKnowledge)
            for (const auto& k : content.householdMust) {
                bool covered = false;
                for (EntityId m : members)
                    if (w.character(m)->knowledge.count(k) && w.character(m)->body.stage() != "child") covered = true;
                if (!covered) teach(*w.character(fatherId), k, rng);
            }
        dyn.founder = fatherId;
        fam.members = members;
        w.families()[fam.id] = fam;
        w.dynasties()[dyn.id] = dyn;
        st.families.push_back(fam.id);
        everyone.insert(everyone.end(), members.begin(), members.end());
    }
    w.settlements()[st.id] = st;
    if (planned && !w.pieces().empty()) {   // AL-81: the rooms as the world starts
        refreshPieces(w);
        refreshRooms(w);
    }

    // --- the colony's mandate (GDD §2.2): the start is a chief - the head of
    // the eldest household, who drew the others to himself - holding every
    // right; "none" leaves the colony without a ruler.
    if (cfg.value("mandate", std::string("eldest_head")) == "eldest_head") {
        const Character* chief = nullptr;
        for (const auto& [id, c] : w.characters())
            if (!c.isAnimal() && c.status.settlement == st.id && c.status.role == "head" &&
                (!chief || c.body.ageDays > chief->body.ageDays))
                chief = &c;
        if (chief) grantStartMandate(w, st.id, chief->id);
    }

    // --- fields ---------------------------------------------------------------
    seedFields(w, seed, cfg);

    // --- last year's stores ------------------------------------------------
    // Until the history generator runs N years before the start, each
    // household's store is a cheap model of last year: how many days of food
    // a family of this size, these hands and this luck put aside by this
    // season (autumn: the harvest is in; spring: the hungry gap). Spread
    // between households on purpose - a poorer neighbour makes situations.
    if (!content.knowledge.empty()) {
        // A household's staple has to last until the next harvest (the
        // stores stand in for last year's farming). It covers a share
        // of the days left - the rest is foraging, fishing, hunting and the
        // neighbours. carryover_days {season: [lo, hi]} overrides it (tests).
        const Json carry = cfg.value("carryover_days", Json::object());
        // The next harvest: when the fields ripen (mid sowing + growth + reaping).
        int cropHarvest = 180;
        for (const auto& [oid, od] : content.objects)
            if (od.hasCrop && od.crop.staple) {
                const int len = (od.crop.sowTo - od.crop.sowFrom + kDaysPerYear) % kDaysPerYear + 1;
                cropHarvest = (od.crop.sowFrom + len / 2 + od.crop.growDays + 10) % kDaysPerYear;
            }
        const int harvestDay = cfg.value("harvest_day", cropHarvest);
        const int doy = w.startDay() % kDaysPerYear;
        const int daysToHarvest = ((harvestDay - doy) % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
        const Json share = cfg.value("carryover_share", Json::array({0.55, 0.8}));
        const std::string& season = w.season();
        // The hungry gap: the closer the next harvest, the more of the old one is eaten up.
        const double gap = season == "summer" ? 0.35 : (season == "spring" ? 0.65 : (season == "winter" ? 0.95 : 1.0));
        const Json range = carry.contains(season)
                               ? carry[season]
                               : Json::array({daysToHarvest * share.at(0).get<double>() * gap,
                                              daysToHarvest * share.at(1).get<double>() * gap});
        Rng yr(hashCombine(seed, hashStr("last_year")));
        const double villageLuck = yr.range(0.75, 1.2); // last year's weather for everyone
        const NeedDef* hungerNeed = content.need("hunger");
        const double perDay = hungerNeed ? hungerNeed->decayPerDay : 0.5;
        for (auto& [fid, fam] : w.families()) {
            WorldObject* store = w.object(fam.storage);
            if (!store) continue;
            int adults = 0, people = 0, diligent = 0, lazy = 0;
            bool fisher = false, hunter = false;
            for (EntityId id : fam.members) {
                const Character* ch = w.character(id);
                if (!ch) continue;
                ++people;
                if (ch->body.stage() == "adult") ++adults;
                for (const auto& t : ch->traits) {
                    if (t == "diligent" || t == "handy") ++diligent;
                    if (t == "lazy" || t == "slob") ++lazy;
                }
                fisher = fisher || ch->knowledge.count("fishing") > 0;
                hunter = hunter || ch->knowledge.count("hunting") > 0;
            }
            if (people == 0) continue;
            Rng fr(hashCombine(seed, hashStr("last_year:" + fam.name)));
            const double hands = std::clamp(adults / (0.5 * people), 0.5, 1.4);
            const double work = std::clamp(1.0 + 0.12 * diligent - 0.15 * lazy, 0.6, 1.4);
            const double luck = fr.range(0.6, 1.3);
            const double days = fr.range(range.at(0).get<double>(), range.at(1).get<double>()) * hands * work *
                                luck * villageLuck;
            const double value = days * people * perDay; // hunger units to cover
            auto put = [&](const std::string& item, double share) {
                auto d = content.items.find(item);
                if (d == content.items.end() || d->second.nutrition <= 0.0 || share <= 0.0) return;
                const int n = static_cast<int>(std::round(value * share / d->second.nutrition));
                if (n > 0) store->stock[item] += n;
            };
            double grain = 0.6, fish = fisher ? 0.15 : 0.0, meat = hunter ? 0.1 : 0.0, berries = 0.1;
            const double rest = 1.0 - grain - fish - meat - berries;
            grain += rest;
            // The staple comes from the household's own fields: last harvest
            // replayed from that year's weather and soil, less the seed kept
            // back and what has been eaten since. Without fields, the share.
            double fieldGrain = -1.0;
            for (const auto& [oid, od] : content.objects) {
                if (!od.hasCrop) continue;
                if (!od.crop.staple) {
                    // A kitchen garden's last crop: what is left of it in the
                    // cellar (it rots at its spoil_days half-life) and the seed
                    // kept for the next sowing.
                    for (EntityId gid : w.objectsOfType(oid)) {
                        const WorldObject* g = w.object(gid);
                        if (!g || g->ownerFamily != fam.id) continue;
                        const CropDef& cd = od.crop;
                        const int ripeDay = (cd.sowFrom + cd.growDays + 10) % kDaysPerYear;
                        const int since = ((doy - ripeDay) % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
                        const int strips = g->stock.count("strips") ? g->stock.at("strips") : 0;
                        const double crop = strips * cd.seedPerStrip *
                                            pastSeasonYield(w, *g, doy >= ripeDay ? 0 : -1) / cd.grainPerSheaf;
                        auto pd = content.items.find(cd.produce);
                        const double half = pd == content.items.end() ? 0.0 : pd->second.spoilDays;
                        const double keep = half > 0.0 ? std::exp(-0.6931471805599453 * since / half) : 1.0;
                        // Eaten since the harvest as well: a third of it by the turn of the year.
                        const double eaten = std::clamp(1.0 - since / 270.0, 0.2, 1.0);
                        const int left = static_cast<int>(std::round(crop * keep * eaten * work));
                        if (left > 0) store->stock[cd.produce] += left;
                    }
                    continue;
                }
                for (EntityId fidObj : w.objectsOfType(oid)) {
                    const WorldObject* f = w.object(fidObj);
                    if (!f || f->ownerFamily != fam.id) continue;
                    const CropDef& cd = od.crop;
                    const int lastYear = doy >= harvestDay ? 0 : -1;
                    const int strips = f->stock.count("strips") ? f->stock.at("strips") : 0;
                    const double harvest = strips * cd.seedPerStrip * pastSeasonYield(w, *f, lastYear);
                    // The seed sown or stored for the next sowing came out of it.
                    const double seedOut = strips * cd.seedPerStrip;
                    fieldGrain = std::max(0.0, fieldGrain) + std::max(0.0, harvest - seedOut);
                }
            }
            if (fieldGrain >= 0.0) {
                const auto gd = content.items.find("grain");
                const double nut = gd == content.items.end() ? 0.45 : std::max(0.05, gd->second.nutrition);
                const int sinceHarvest = ((doy - harvestDay) % kDaysPerYear + kDaysPerYear) % kDaysPerYear;
                const double eaten = sinceHarvest * people * perDay * grain / nut;
                // A famine year in the stand-in history is softened to a lean
                // one: until history generation plays it out (deaths, flight,
                // help from kin), nobody starts with less than ~80% of the
                // staple to the next harvest.
                const double floorGrain = 0.8 * std::max(10, daysToHarvest) * people * perDay * grain / nut;
                const double left = std::max(fieldGrain - eaten, floorGrain) * work * fr.range(0.9, 1.1);
                store->stock["grain"] += static_cast<int>(std::round(left));
            } else {
                put("grain", grain);
            }
            put("smoked_fish", fish);
            put("dried_meat", meat);
            put("dried_berries", berries);
            // Tools kept from last year: every adult a knife, hunters a spear,
            // fishers a line, the household a digging stick.
            for (EntityId id : fam.members) {
                Character* ch = w.character(id);
                if (!ch || ch->body.stage() == "child") continue;
                auto give = [&](const char* item) {
                    if (content.items.count(item)) ch->inventory[item] += 1;
                };
                give("stone_blade");
                if (ch->knowledge.count("hunting")) give("spear");
                if (ch->knowledge.count("fishing")) give("fishing_line");
            }
            if (content.items.count("digging_stick")) store->stock["digging_stick"] += 1;
            // Firewood: a few days' worth, more in the cold half of the year.
            const bool cold = season == "autumn" || season == "winter";
            store->stock["firewood"] += static_cast<int>(std::round(fr.range(4.0, 10.0) * (cold ? 2.0 : 1.0)));
            w.chronicle("last_year", {fam.members.empty() ? kNone : fam.members.front()}, "chronicle.last_year",
                        {{"family", fam.name}, {"days", std::round(days)}});
        }
    }

    // The settlement as a whole covers settlement_must: each missing piece goes
    // to a different adult, round-robin (the start chains must close).
    if (!explicitKnowledge && !everyone.empty()) {
        std::vector<EntityId> adults;
        for (EntityId id : everyone)
            if (w.character(id)->body.stage() == "adult") adults.push_back(id);
        size_t next = 0;
        for (const auto& k : content.settlementMust) {
            bool covered = false;
            for (EntityId id : adults)
                if (w.character(id)->knowledge.count(k)) covered = true;
            if (covered || adults.empty()) continue;
            teach(*w.character(adults[next++ % adults.size()]), k, rng);
        }
    }

    // --- relations and knowledge -------------------------------------------
    for (EntityId a : everyone) {
        Character& ca = *w.character(a);
        for (EntityId b : everyone) {
            if (a == b) continue;
            const Character& cb = *w.character(b);
            Relation r;
            Rng rr = agentRng(seed, a, b, "relation");
            if (ca.status.family == cb.status.family) {
                r.familiarity = 1.0;
                r.affinity = rr.range(0.4, 0.8);
                r.trust = rr.range(0.5, 0.9);
            } else {
                r.familiarity = rr.range(0.2, 0.6);
                r.affinity = rr.range(-0.3, 0.35);
                r.trust = rr.range(-0.1, 0.4);
            }
            ca.relations[b] = r;
        }
        for (const auto& [oid, o] : w.objects()) {
            const bool own = o.ownerFamily != kNone;
            if (own || dist(o.pos, center) <= knowRadius) ca.knownObjects.insert(oid);
        }
    }
    // --- animals: groups of every species over their habitat ------------
    spawnAnimals(w, seed);
    spawnLivestock(w, seed);
    seedVillageLife(w, seed); // AL-39: some wives are already with child
    return w;
}

std::string randomPersonName(Rng& r, bool female, const Content* c) { return makeName(r, female, c); }
std::string randomFamilyName(Rng& r, const Content* c) { return makeFamilyName(r, c); }

} // namespace altera::sim
