#pragma once
// LR-159: riding and draught beasts, carts, wagons, coaches, sleds (content/ai/transport.json).
//
// A mount is an ordinary domestic beast (species.json, a Character with species and
// group 1); a vehicle is a WorldObject of a vehicles type whose stock is its cargo.
// Who rides what and which beasts pull which vehicle is kept in the world's module
// state "transport" (saved with the world):
//   {ride: {person: beast}, veh: {vehicle: {team: [beasts], driver: person, pass: [people], fx, fy}}}
// transportTick moves the beasts and the vehicle with the rider / the driver after every
// step; the speed of a person on a mount or at the reins is transportSpeedMult.
//
// The player does it through the act menu: perform with an interaction "transport.<act>"
// (mount, dismount, hitch, unhitch, drive, board, alight, load, unload) and a target;
// transportActs lists what can be done on a target. NPCs mount their own beast for a long
// walk on their own (autoRide*).

#include <string>
#include <vector>

#include "sim/world.hpp"

namespace lr::sim {

struct MountDef {
    std::string id;
    double ride = 0.0;        // speed multiplier ridden (0: not ridden)
    double draft = 0.0;       // pull of one head
    double carryKg = 0.0;     // packs on it
    double tirePerKm = 0.03;  // the beast's fatigue per km
    double restPerHour = 0.1; // rest while free
    double tiredSlow = 0.5;   // share of speed lost at fatigue 1
    int maxRiderPx = 0;       // riders no larger than this (0: any)
    int riderDyPx = 6;        // the client lifts the rider this much
    std::vector<std::string> races; // only these kinds ride it (empty: all)
};

struct VehicleDef {
    std::string id;
    int teamMin = 0, teamMax = 0;   // heads in harness (0: pulled by the person)
    double draftNeed = 0.0;
    double capacityKg = 0.0;
    int seats = 0;
    double speed = 1.0;
    double cargoSlow = 0.3;
    std::vector<std::string> draft;   // beasts that may pull it (empty: any with draft)
    std::vector<std::string> seasons; // when it goes (empty: always)
    int seatDyPx = -1;                // the client lifts the driver this much (-1: the rules' seat_dy_px)
};

struct TransportStart {
    std::string mount, vehicle, needs;
    double chance = 0.0;
    int headsLo = 1, headsHi = 1;
    std::vector<std::string> races;
};

struct TransportRules {
    double rideMinM = 140.0, fetchM = 20.0;
    double riderTire = 0.3, driverTire = 0.15, minPull = 0.35;
    int seatDyPx = 4;
    double teamAheadM = 1.5, teamRowM = 1.4, teamSideM = 0.5; // where the team walks before the vehicle
    double pushAheadM = 0.9;                                  // a handcart before the one who pushes it
    std::map<std::string, MountDef> mounts;
    std::map<std::string, VehicleDef> vehicles;
    std::vector<TransportStart> start;
    const MountDef* mount(const std::string& species) const;
    const VehicleDef* vehicle(const std::string& type) const;
};

// transport.json of the content (cached per data generation); null when there is none.
const TransportRules* transportRules(const Content& c);

// --- state -----------------------------------------------------------------------------
EntityId rideOf(const World& w, EntityId person);       // the beast ridden (kNone: none)
EntityId vehicleOf(const World& w, EntityId person);    // the vehicle driven or sat in
EntityId driverOf(const World& w, EntityId vehicle);
std::vector<EntityId> teamOf(const World& w, EntityId vehicle);
std::vector<EntityId> passengersOf(const World& w, EntityId vehicle);
EntityId riderOf(const World& w, EntityId beast);       // who rides it
EntityId hitchedTo(const World& w, EntityId beast);     // the vehicle it pulls
bool harnessed(const World& w, EntityId beast);         // ridden or in a team: it does not wander
bool isVehicle(const World& w, const WorldObject& o);

// --- numbers ---------------------------------------------------------------------------
// The person's walking speed multiplier: 1 on foot, the mount's on a beast, the vehicle's
// at the reins (its team, its load, the beasts' fatigue); a passenger goes with the vehicle.
double transportSpeedMult(const World& w, const Character& c);
// Fatigue multiplier for the person's own walk (rider / driver tire less).
double transportTireMult(const World& w, const Character& c);
double vehicleSpeedMult(const World& w, const WorldObject& v);
double vehicleCapacityKg(const World& w, const WorldObject& v);
double vehicleCargoKg(const World& w, const WorldObject& v);
double teamDraft(const World& w, const WorldObject& v);

// --- acts ------------------------------------------------------------------------------
// Each returns "" when done, else "rejected:<code>".
std::string mountBeast(World& w, Character& rider, Character& beast);
std::string dismount(World& w, Character& rider);
std::string hitch(World& w, Character& who, WorldObject& v, Character& beast);
std::string unhitch(World& w, Character& who, WorldObject& v);
std::string drive(World& w, Character& who, WorldObject& v);   // take the reins (or push a handcart)
std::string board(World& w, Character& who, WorldObject& v);   // a seat
std::string alight(World& w, Character& who);
std::string loadCargo(World& w, Character& who, WorldObject& v);   // all one carries that is not worn
std::string unloadCargo(World& w, Character& who, WorldObject& v); // what fits in one's hands and bag

// What `me` can do on `target` with transport (a beast, a vehicle, kNone = oneself):
// [{id: "transport.<act>", can, why}] (why: a rejected code).
Json transportActs(const World& w, const Character& me, EntityId target);
// Applies a perform of a "transport.<act>" interaction (the acts above).
std::string performTransport(World& w, Character& me, const std::string& act, EntityId target);

// A vehicle of `type` with `heads` of `beast` in harness at `at`, owned by `family`; the
// driver (kNone: none) takes the reins. For caravans arriving at the edge of the map.
// Returns the vehicle id (kNone: unknown type).
EntityId spawnRig(World& w, EntityId family, Vec2 at, const std::string& type, const std::string& beast, int heads,
                  EntityId driver = kNone);
// Worldgen: the households' beasts and vehicles (transport.json start).
void spawnTransport(World& w, std::uint64_t seed);

// --- ticks -----------------------------------------------------------------------------
// After every step: mounts, teams, vehicles and passengers follow their rider / driver;
// beasts tire on the way and rest when free; the dead or the gone leave the harness.
void transportTick(World& w);
// NPC free will: before a walk to `dest`, mount one's own beast standing near when the
// walk is long. transportTick gets an NPC down a tick after the walk ends (so the arrival
// is seen on horseback); autoRideEnd gets down at once.
void autoRideStart(World& w, Character& c, Vec2 dest);
void autoRideEnd(World& w, Character& c);

void installTransport(World& w);

} // namespace lr::sim
