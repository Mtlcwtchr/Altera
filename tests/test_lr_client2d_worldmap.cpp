// LR-153: the world map (src/sim/worldmap, src/client2d/worldmap2d): the land around
// the realm's settlements, rivers, places of interest, fealty and kingdoms, battlefields,
// and the window's data named in the language.
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include "client2d/worldmap2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/caravan.hpp"
#include "sim/trade.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/worldgen.hpp"
#include "sim/worldmap.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& mapContent() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

const Loc& mapLoc() {
    static const Loc l = Loc::load(kRoot + "/content/loc");
    return l;
}

// the village without the per-tick minds; the realm's first day by hand
World mapWorld(std::uint64_t seed) {
    static const Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    World w = makeTestWorld(mapContent(), cfg, seed);
    installRealm(w);
    installWorldMap(w);
    w.run(kTicksPerDay);
    realmDaily(w);
    return w;
}

} // namespace

TEST(lr_worldmap_content) {
    CHECK(worldMapLoaded(mapContent()));
    for (const auto& e : checkWorldMap(mapContent())) {
        std::printf("  %s\n", e.c_str());
        CHECK(false);
    }
}

TEST(lr_worldmap_land) {
    for (std::uint64_t seed : {7ull, 42ull, 1234ull}) {
        World w = mapWorld(seed);
        const WorldMapState s = loadWorldMap(w);
        CHECK(s.inited);
        CHECK(s.grid >= 16);
        CHECK(s.ground.size() == static_cast<size_t>(s.grid * s.grid));
        std::set<std::string> seen;
        for (char ch : s.ground) seen.insert(s.kinds[static_cast<size_t>(ch - '0')]);
        CHECK(seen.size() >= 4);   // not one flat colour
        for (const auto& [id, p] : w.settlements()) {
            const std::string k = s.kindAt(p.km);
            CHECK(!k.empty());
            CHECK(k != "sea" && k != "lake");   // nobody lives in the water
            if (p.culture == "dwarven") CHECK(k == "mountains" || k == "hills");
            if (p.culture == "sylvan") CHECK(k == "forest" || k == "marsh");
        }
        // places of interest: on their land, away from the settlements and each other
        CHECK(s.pois.size() >= 8);
        std::set<EntityId> ids;
        for (const auto& p : s.pois) {
            CHECK(ids.insert(p.id).second);
            CHECK(!s.kindAt(p.km).empty());
            for (const auto& [id, st] : w.settlements()) CHECK(dist(p.km, st.km) >= 4.4);
        }
    }
}

TEST(lr_worldmap_deterministic) {
    World a = mapWorld(42), b = mapWorld(42), c = mapWorld(43);
    const WorldMapState sa = loadWorldMap(a), sb = loadWorldMap(b), sc = loadWorldMap(c);
    CHECK(sa.ground == sb.ground);
    CHECK(sa.river == sb.river);
    CHECK(sa.pois.size() == sb.pois.size());
    CHECK(sa.ground != sc.ground);
    // the save keeps it
    WorldMapState back;
    from_json(Json(sa), back);
    CHECK(back.inited && back.ground == sa.ground && back.pois.size() == sa.pois.size());
}

TEST(lr_worldmap_fealty_kingdoms) {
    int vassals = 0, bigDomains = 0, kingdoms = 0;
    for (std::uint64_t seed : {7ull, 42ull, 99ull, 1234ull}) {
        World w = mapWorld(seed);
        for (const auto& [id, p] : w.settlements()) {
            if (p.liege == kNone) continue;
            ++vassals;
            CHECK(p.tagged("vassal"));
            const Settlement& l = w.settlements().at(p.liege);
            CHECK(l.institution == "lord");
            CHECK(l.pop > p.pop);
            CHECK(!p.tagged("outlaw") && p.institution != "abbot" && p.institution != "lair");
        }
        std::set<EntityId> heads;
        for (const auto& [id, p] : w.settlements()) heads.insert(worldMapDomainHead(w, id));
        for (EntityId h : heads) {
            int n = 0;
            for (const auto& [id, p] : w.settlements()) n += worldMapDomainHead(w, id) == h;
            if (n >= 2) ++bigDomains;
            if (worldMapDomainTitle(w, h) == "kingdom") ++kingdoms;
        }
    }
    CHECK(vassals >= 4);
    CHECK(bigDomains >= 2);
    CHECK(kingdoms >= 1);
}

TEST(lr_worldmap_battlefields) {
    World w = mapWorld(7);
    RealmState r = loadRealm(w);
    std::vector<EntityId> ps;
    for (const auto& [id, p] : w.settlements())
        if (!p.full()) ps.push_back(id);
    CHECK(ps.size() >= 2);
    RealmWar war;
    war.id = 9001;
    war.attacker = ps[0];
    war.defender = ps[1];
    war.battles = 2;
    r.wars.push_back(war);
    saveRealm(w, r);
    const size_t before = loadWorldMap(w).pois.size();
    worldMapDaily(w);
    WorldMapState s = loadWorldMap(w);
    CHECK(s.pois.size() == before + 2);
    CHECK(s.pois.back().kind == "battlefield" && s.pois.back().a == ps[0]);
    worldMapDaily(w);   // the same battles are not marked twice
    CHECK(loadWorldMap(w).pois.size() == before + 2);
}

TEST(lr_worldmap_window_json) {
    const char* seedEnv = std::getenv("LR_WORLDMAP_SEED");
    World w = mapWorld(seedEnv ? std::strtoull(seedEnv, nullptr, 10) : 42);
    c2::ViewText t;
    t.loc = &mapLoc();
    EntityId me = kNone;
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) {
            me = id;
            break;
        }
    const Json d = c2::worldMapJson(w, t, me);
    CHECK(d.is_object());
    if (const char* out = std::getenv("LR_WORLDMAP_DUMP")) {   // a picture of it: tools/game_2d/worldmap_preview.py
        std::ofstream f(out);
        f << d.dump();
    }
    CHECK(d["ground"].get<std::string>().size() == static_cast<size_t>(d["grid"].get<int>() * d["grid"].get<int>()));
    CHECK(d["settlements"].size() == w.settlements().size());
    int here = 0;
    for (const auto& s : d["settlements"]) {
        const std::string n = s["name"].get<std::string>();
        CHECK(!n.empty() && n[0] != '[');
        here += s["here"].get<bool>();
    }
    CHECK(here == 1);   // the led person is in the map's village
    CHECK(!d["domains"].empty());
    for (const auto& dm : d["domains"]) {
        const std::string n = dm["name"].get<std::string>();
        CHECK(!n.empty() && n.find('[') == std::string::npos && n.find('{') == std::string::npos);
    }
    CHECK(!d["roads"].empty());
    for (const auto& p : d["pois"]) {
        const std::string n = p["name"].get<std::string>(), k = p["kind_name"].get<std::string>(), near = p["near"].get<std::string>();
        CHECK(!n.empty() && n.find('[') == std::string::npos && n.find("worldmap.") == std::string::npos);
        CHECK(k.find("worldmap.") == std::string::npos);
        CHECK(near.find("worldmap.") == std::string::npos && near.find('{') == std::string::npos);
    }
    for (const auto& k : d["kinds"]) CHECK(k["name"].get<std::string>().find("worldmap.") == std::string::npos);
}

// LR-163: a caravan on the road shows on the map, named, between the settlements it goes by
TEST(lr_worldmap_caravans) {
    static const Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    World w = makeTestWorld(mapContent(), cfg, 7);
    installRealm(w);
    installWorldMap(w);
    installCaravans(w);
    installTrade(w);
    w.run(kTicksPerDay);
    realmDaily(w);
    Settlement* local = realmLocal(w);
    CHECK(local != nullptr);
    if (!local) return;
    Character* t = nullptr;
    for (auto& [id, c] : w.offstage())
        if (c.body.alive && !c.isAnimal() && c.standing.role.empty() && c.standing.life == "settled" && c.body.lifeYears() >= 20 &&
            c.body.lifeYears() <= 45 && c.occupation.empty() && c.status.settlement != kNone && c.status.settlement == c.standing.at) {
            t = &c;
            break;
        }
    CHECK(t != nullptr);
    if (!t) return;
    t->standing.purse = 200;
    t->inventory["cloth"] += 20;
    const int id = caravanSetOut(w, t->id, local->id);
    CHECK(id > 0);
    c2::ViewText vt;
    vt.loc = &mapLoc();
    const Json d = c2::worldMapJson(w, vt, kNone);
    CHECK(d["caravans"].is_array());
    bool found = false;
    for (const auto& v : d["caravans"]) {
        if (v["id"].get<int>() != id) continue;
        found = true;
        CHECK(v["to"].get<EntityId>() == local->id);
        const std::string k = v["kind_name"].get<std::string>();
        CHECK(!k.empty() && k.find("caravan.") == std::string::npos);
        CHECK(!v["leader"].get<std::string>().empty());
        CHECK(v["km"].is_array() && v["km"].size() == 2);
        for (const auto& g : v["cargo"]) CHECK(g.get<std::string>().find("item.") == std::string::npos);
    }
    CHECK(found);
}
