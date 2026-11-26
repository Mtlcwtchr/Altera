#pragma once
// LR-153: the world map - the land the realm (sim/realm.hpp) lies on, as one
// picture: the ground around the settlements (a coarse grid of terrain kinds
// grown from the world's seed, pulled by who lives where: a dwarven hold in the
// mountains, an elven grove in the forest), rivers, the generated places of
// interest (ruins, caves, barrows, shrines, lairs...), and the battlefields the
// realm's wars leave. Rules: content/ai/worldmap.json.
//
// One rule for every settlement: nothing here knows which one is the player's.
// What a settlement belongs to is its liege chain (Settlement::liege); the land
// at the realm's start swears fealty once by worldmap.json "fealty", so the
// region has its kingdoms and lordships, and wars move them on from there.
//
// The state lives in World::moduleState("worldmap") and is laid out on the first
// daily pass after the realm's genesis (or by worldMapEnsure). Deterministic.

#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Content;
struct Settlement;

struct WorldPoi {
    EntityId id = kNone;
    std::string kind;   // worldmap.json poi.kinds id, or "battlefield"
    Vec2 km;
    std::string name;   // loc key suffix: worldmap.poi.<kind>.names.<name>; battlefield: ""
    int day = 0;        // when it came to be (battlefields)
    EntityId a = kNone, b = kNone;   // battlefield: the sides
    std::string creature;            // LR-170: a lair's dweller (content/creatures id), "" otherwise
    int lastRaid = 0;                // LR-170: the day of its last raid
};

struct WorldMapState {
    bool inited = false;
    int grid = 0;                // cells per side
    Vec2 origin;                 // km of cell (0, 0)'s corner
    double cellKm = 1.0;
    std::string ground;          // grid*grid chars, an index into `kinds` ('0' + i); row-major, y down
    std::string river;           // grid*grid chars: '1' a river cell
    std::vector<std::string> kinds;
    std::vector<WorldPoi> pois;
    std::vector<std::pair<EntityId, int>> battlesSeen;   // war id -> battles already marked
    bool fealty = false;         // the start's fealty was sworn
    int nextId = 1;
    // the kind at a km point ("" outside the grid)
    std::string kindAt(Vec2 km) const;
};

void to_json(Json& j, const WorldMapState& s);
void from_json(const Json& j, WorldMapState& s);
WorldMapState loadWorldMap(const World& w);
void saveWorldMap(World& w, const WorldMapState& s);

// Whether the content has worldmap.json (enabled).
bool worldMapLoaded(const Content& c);
// Problems of worldmap.json (unknown terrain kinds in poi/pulls); empty = fine.
std::vector<std::string> checkWorldMap(const Content& c);

// Lays the map out if it is not yet (needs the realm laid out); true when there is one.
bool worldMapEnsure(World& w);
// The daily part (a realm hook): the first lay-out, the start's fealty, new battlefields.
void worldMapDaily(World& w);
struct RealmState;
void worldMapPass(World& w, const RealmState& realm);   // the same with the realm's state in hand
// LR-170: the lairs' raids of one day (the daily pass runs them); returns how many struck.
int worldMapLairRaids(World& w, int day);
// Registers the daily part with the realm (once).
void installWorldMap(World& w);

// The top of a settlement's liege chain (itself when free).
EntityId worldMapDomainHead(const World& w, EntityId settlement);
// The domain's title id by worldmap.json domains.titles (kingdom | lordship | hold | ... | free).
std::string worldMapDomainTitle(const World& w, EntityId head);
// The roads: settlement pairs (a < b) by worldmap.json roads.
std::vector<std::pair<EntityId, EntityId>> worldMapRoads(const World& w);

} // namespace lr::sim
