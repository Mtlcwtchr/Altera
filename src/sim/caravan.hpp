#pragma once
// LR-154: caravans, peddlers and supply trains (content/ai/caravans.json).
//
// No special entity. A venture is ordinary people of one settlement - a trader
// (occupation "trader", the same role the village's trader has) and, when the
// load is worth it, carriers and guards - who carry goods where they fetch more.
// What the party is (a lone peddler, a caravan, a supply train of grain) is
// classified from its numbers each day (caravans.json kinds), never set.
//
// One rule for every settlement. Each has a market: a stat settlement's stock is
// counted from its numbers (make/use per head and year - expr formulas over pop,
// land, crafts, fort, tags, peoples and the deposits rolled for it), the map's
// village's from the real stores of its households and what its people carry.
// Prices follow stock against need. A trader at home looks for the route with
// the best margin after the road's cost and risk, buys, hires guards, sets out;
// on the road outlaw camps near the way may fall on the party; at the other end
// it sells, buys for the way back and goes home, where the profit stays.
// Grain is a settlement's food: supply trains replace the realm's instant grain
// trade (realm grainTrade steps aside while this layer is on).
//
// A venture that reaches the map (its destination, or a stop on a road that
// passes the village) walks on as the same people: the trader trades by the
// ordinary trade rules (trade.hpp) - villagers come to buy and to sell, and the
// player can deal with him - and when the market days are over the party walks
// off the map with what it has and goes on.
//
// The cargo is the trader's own inventory, on the map and off it; the coin is his
// purse offstage and his coin on the map (as with every person of the realm).
// Deterministic (World::rng by id and salt). State: World::moduleState("caravans").
// Transport (LR-159) joins through caravans.json capacity / speed formulas.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Content;
struct Settlement;

struct Venture {
    EntityId id = kNone;
    EntityId leader = kNone;            // the trader
    std::vector<EntityId> members;      // leader first, then carriers and guards
    int guards = 0, beasts = 0;
    EntityId home = kNone, from = kNone, to = kNone;
    EntityId then = kNone;              // a stop on the way: where it goes after it
    int depart = 0, arrive = 0;
    std::string state = "road";        // road | market | done | lost
    int marketUntil = 0;
    std::string kind;                   // classified (caravans.json kinds)
    double bought = 0.0, sold = 0.0;    // coin over the whole venture
    int legs = 0;
    int stops = 0;                      // market days kept on the map this venture
};

struct CaravanState {
    bool on = false;
    int nextId = 1;
    std::vector<Venture> ventures;
    std::map<std::string, std::map<std::string, double>> markets;  // settlement id -> good -> stock (stat)
    std::map<std::string, std::vector<std::string>> deposits;       // settlement id -> goods it digs or grows wild
    std::map<std::string, int> rest;                                // trader id -> day he may set out again
    std::map<std::string, int> stats;
};

void to_json(Json& j, const Venture& v);
void from_json(const Json& j, Venture& v);
void to_json(Json& j, const CaravanState& s);
void from_json(const Json& j, CaravanState& s);

CaravanState loadCaravans(const World& w);
void saveCaravans(World& w, const CaravanState& s);

// Whether the layer is on for this world (caravans.json present and enabled, installCaravans called).
bool caravansOn(const World& w);
// Problems of caravans.json against the content (unknown items, formulas that do not compile).
std::vector<std::string> checkCaravans(const Content& c);
// Loc keys the module writes.
std::vector<std::string> caravanLocKeys();

// The price of one `good` in settlement `p` now (coin), and its stock there.
double caravanPrice(const World& w, const Settlement& p, const std::string& good);
double caravanStock(const World& w, const Settlement& p, const std::string& good);

// The daily pass (a realm hook): markets, plans, the road, arrivals.
void caravansDaily(World& w);
// Where a venture is on the region map now (km), for the world map.
Vec2 ventureKm(const World& w, const Venture& v);
// Every venture as JSON (id, kind, leader, members, from, to, km, cargo, state).
Json caravansJson(const World& w);

// Tests and tools: an outlaw band of `camp` falls on venture `id` now; true when it was robbed.
bool caravanAmbush(World& w, int id, EntityId camp);
// Tests and tools: the trader `leader` sets out now with the best route he has (or to `to`); the venture id or 0.
int caravanSetOut(World& w, EntityId leader, EntityId to = kNone);

// Commands: caravan.list -> result "ok:" + JSON
void installCaravans(World& w);

} // namespace lr::sim
