// LR-153: the world map's data for the 2D client (client2d/worldmap2d.hpp).

#include "client2d/worldmap2d.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/world.hpp"
#include "sim/worldmap.hpp"

namespace lr::client2d {

using sim::EntityId;
using sim::Json;

namespace {

std::string say(const ViewText& t, const std::string& key, const Json& args = Json::object(), const std::string& dflt = "") {
    if (!t.loc || !t.loc->has(t.lang, key)) return dflt.empty() ? key : dflt;
    return t.loc->text(t.lang, key, args);
}

// a settlement's noun ({nom, gen, ...}) in the view's language
Json placeNoun(const sim::World& w, const ViewText& t, EntityId id) {
    if (!t.loc) {
        auto it = w.settlements().find(id);
        return Json{{"nom", it == w.settlements().end() ? std::string() : it->second.name}};
    }
    Json n = sim::realmNoun(*t.loc, t.lang, w, id);
    return n.is_object() ? n : Json{{"nom", ""}};
}
std::string nounCase(const Json& n, const char* c) {
    if (n.is_object() && n.contains(c) && n[c].is_string()) return n[c].get<std::string>();
    return n.is_object() ? n.value("nom", std::string()) : std::string();
}

// "6 km north of X"
std::string nearText(const sim::World& w, const ViewText& t, sim::Vec2 km) {
    const sim::Settlement* best = nullptr;
    double bd = 1e18;
    for (const auto& [id, p] : w.settlements()) {
        const double d = sim::dist(km, p.km);
        if (!p.ruined && d < bd) bd = d, best = &p;
    }
    if (!best) return "";
    static const char* dirs[] = {"e", "se", "s", "sw", "w", "nw", "n", "ne"};
    const double ang = std::atan2(km.y - best->km.y, km.x - best->km.x);   // y down: south is +y
    int o = static_cast<int>(std::lround(ang / (3.14159265358979 / 4)));
    o = ((o % 8) + 8) % 8;
    const Json n = placeNoun(w, t, best->id);
    return say(t, "worldmap.near", {{"km", std::to_string(static_cast<int>(std::lround(bd)))},
                                   {"dir", say(t, std::string("worldmap.dir.") + dirs[o])},
                                   {"place", nounCase(n, "gen")}});
}

} // namespace

Json worldMapJson(sim::World& w, const ViewText& t, EntityId viewer) {
    if (!sim::worldMapEnsure(w)) return nullptr;
    const sim::WorldMapState s = sim::loadWorldMap(w);
    const sim::RealmState realm = sim::loadRealm(w);
    Json out = {{"grid", s.grid}, {"origin", s.origin}, {"cell_km", s.cellKm}, {"ground", s.ground}, {"river", s.river}};
    Json kinds = Json::array();
    for (const auto& k : s.kinds) kinds.push_back({{"id", k}, {"name", say(t, "worldmap.terrain." + k, Json::object(), k)}});
    out["kinds"] = kinds;

    // where the viewer is: on the map - the map's settlement; away - where the person is
    EntityId here = sim::kNone;
    std::string meName;
    if (const sim::Character* c = w.character(viewer)) {
        if (const sim::Settlement* l = sim::realmLocal(w)) here = l->id;
        meName = t.loc ? nounCase(t.loc->personNoun(t.lang, *c), "nom") : c->name;
    } else if (auto it = w.offstage().find(viewer); it != w.offstage().end()) {
        here = it->second.standing.at;
        meName = t.loc ? nounCase(t.loc->personNoun(t.lang, it->second), "nom") : it->second.name;
    }

    std::map<EntityId, bool> atWar;
    Json wars = Json::array();
    for (const auto& war : realm.wars) {
        if (war.over) continue;
        atWar[war.attacker] = atWar[war.defender] = true;
        wars.push_back({{"a", war.attacker}, {"b", war.defender}, {"score", war.score}, {"battles", war.battles},
                        {"siege", war.siege}, {"cause", say(t, "realm.casus." + war.cause + ".name", Json::object(), war.cause)}});
    }
    out["wars"] = wars;

    Json sets = Json::array();
    std::map<EntityId, std::vector<EntityId>> domains;
    for (const auto& [id, p] : w.settlements()) {
        const EntityId head = sim::worldMapDomainHead(w, id);
        if (!p.ruined) domains[head].push_back(id);
        Json tags = Json::array();
        for (const auto& g : p.tags)
            if (t.loc && t.loc->has(t.lang, "worldmap.tag." + g)) tags.push_back(say(t, "worldmap.tag." + g));
        std::string ruler;
        if (const sim::Character* r = w.person(p.chief); r && r->body.alive)
            ruler = t.loc ? nounCase(t.loc->personNoun(t.lang, *r), "nom") : r->name;
        sets.push_back({{"id", id},
                        {"name", nounCase(placeNoun(w, t, id), "nom")},
                        {"km", p.km},
                        {"size", p.size},
                        {"size_name", p.size.empty() ? std::string() : say(t, "worldmap.size." + p.size, Json::object(), p.size)},
                        {"pop", static_cast<int>(std::lround(p.pop))},
                        {"ruler", ruler},
                        {"house", p.house},
                        {"culture", p.culture.empty() ? std::string() : t.name("culture", p.culture)},
                        {"inst", say(t, "worldmap.inst." + p.institution, Json::object(), p.institution)},
                        {"inst_id", p.institution},
                        {"culture_id", p.culture},
                        {"tag_ids", p.tags},
                        {"tags", tags},
                        {"domain", head},
                        {"liege", p.liege},
                        {"tribute_to", p.tributeTo},
                        {"ruined", p.ruined},
                        {"war", atWar.count(id) > 0},
                        {"full", p.full()},
                        {"here", id == here}});
    }
    out["settlements"] = sets;

    Json doms = Json::array();
    for (const auto& [head, members] : domains) {
        const std::string title = sim::worldMapDomainTitle(w, head);
        const Json n = placeNoun(w, t, head);
        auto hp = w.settlements().find(head);
        const std::string house = hp == w.settlements().end() ? std::string() : hp->second.house;
        doms.push_back({{"id", head},
                        {"title", title},
                        {"name", say(t, "worldmap.domain." + title, {{"place", nounCase(n, "nom")}, {"place_gen", nounCase(n, "gen")}, {"house", house}},
                                     nounCase(n, "nom"))},
                        {"title_name", say(t, "worldmap.title." + title, Json::object(), title)},
                        {"members", members},
                        {"n", static_cast<int>(members.size())}});
    }
    out["domains"] = doms;

    Json roads = Json::array();
    for (const auto& [a, b] : sim::worldMapRoads(w)) roads.push_back({a, b});
    out["roads"] = roads;

    Json pois = Json::array();
    for (const auto& p : s.pois) {
        std::string name;
        if (p.kind == "battlefield")
            name = say(t, "worldmap.poi.battlefield.of", {{"a", nounCase(placeNoun(w, t, p.a), "gen")}, {"b", nounCase(placeNoun(w, t, p.b), "gen")}});
        else
            name = say(t, "worldmap.poi." + p.kind + "." + p.name, Json::object(), say(t, "worldmap.poi." + p.kind + ".name"));
        Json e = {{"id", p.id}, {"kind", p.kind}, {"kind_name", say(t, "worldmap.poi." + p.kind + ".name", Json::object(), p.kind)},
                  {"name", name}, {"km", p.km}, {"near", nearText(w, t, p.km)}};
        if (p.day) e["day"] = p.day;
        pois.push_back(e);
    }
    out["pois"] = pois;

    Json quests = Json::array();
    for (const auto& q : realm.quests)
        if (q.status == "open" && q.polity != sim::kNone)
            quests.push_back({{"at", q.polity}, {"name", say(t, "realm.quest." + q.kind + ".name", Json::object(), q.kind)}, {"reward", q.reward}});
    out["quests"] = quests;

    Json me = {{"at", here}, {"name", meName}};
    if (auto it = w.settlements().find(here); it != w.settlements().end()) me["km"] = it->second.km;
    out["me"] = me;
    return out;
}

} // namespace lr::client2d
