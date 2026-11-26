// LR-159: riding and draught beasts, vehicles (see transport.hpp).
#include "sim/transport.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "sim/data_fs.hpp"
#include "sim/gear.hpp"
#include "sim/species.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

constexpr const char* kState = "transport";

std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (j.contains(k) && j[k].is_array())
        for (const auto& s : j[k])
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::string key(EntityId id) { return std::to_string(id); }

Json& state(World& w) {
    Json& s = w.moduleState(kState);
    if (!s.is_object()) s = Json::object();
    if (!s.contains("ride")) s["ride"] = Json::object();
    if (!s.contains("veh")) s["veh"] = Json::object();
    return s;
}
const Json* stateC(const World& w) { return w.moduleState(kState); }

const Json* vehState(const World& w, EntityId v) {
    const Json* s = stateC(w);
    if (!s || !s->contains("veh")) return nullptr;
    const auto it = (*s)["veh"].find(key(v));
    return it == (*s)["veh"].end() ? nullptr : &*it;
}
Json& vehStateM(World& w, EntityId v) {
    Json& s = state(w)["veh"][key(v)];
    if (!s.is_object()) s = {{"team", Json::array()}, {"driver", kNone}, {"pass", Json::array()}, {"fx", 1.0}, {"fy", 0.0}};
    return s;
}
std::vector<EntityId> ids(const Json* j, const char* k) {
    std::vector<EntityId> out;
    if (j && j->contains(k))
        for (const auto& x : (*j)[k]) out.push_back(x.get<EntityId>());
    return out;
}
void eraseId(Json& arr, EntityId id) {
    for (auto it = arr.begin(); it != arr.end();)
        if (it->get<EntityId>() == id) it = arr.erase(it);
        else ++it;
}
// A vehicle state with nobody and nothing in it is dropped (the save stays small).
void tidy(World& w, EntityId v) {
    Json& veh = state(w)["veh"];
    const auto it = veh.find(key(v));
    if (it == veh.end()) return;
    if ((*it)["team"].empty() && (*it)["pass"].empty() && (*it).value("driver", kNone) == kNone) veh.erase(it);
}

bool raceOk(const World& w, const Character& c, const std::vector<std::string>& races) {
    if (races.empty()) return true;
    return contains(races, raceId(w.content(), c)) || (c.race.empty() && contains(races, "human"));
}

const MountDef* mountDefOf(const World& w, const Character& beast) {
    const TransportRules* R = transportRules(w.content());
    return R && beast.isAnimal() ? R->mount(beast.species) : nullptr;
}

// The beast is one's own (one's household's) or nobody's.
bool mayUse(const World& w, const Character& c, EntityId family) {
    (void)w;
    return family == kNone || family == c.status.family;
}

double kgOf(const World& w, const std::string& item) {
    const GearRules* G = gearRules(w.content());
    return G ? G->item(item).kg : 0.5;
}

// One head out of a herd: the beast one rides or harnesses is an animal of its own.
Character& single(World& w, Character& herd) {
    if (herd.group <= 1) return herd;
    Character one = herd;
    one.id = kNone;
    one.group = 1;
    one.inventory.clear();
    one.moodlets.clear();
    herd.group -= 1;
    return w.addCharacter(std::move(one));
}

const char* kActs[] = {"mount", "dismount", "hitch", "unhitch", "drive", "board", "alight", "load", "unload"};

bool hasSeason(const World& w, const VehicleDef& d) { return d.seasons.empty() || contains(d.seasons, w.season()); }

} // namespace

const MountDef* TransportRules::mount(const std::string& species) const {
    const auto it = mounts.find(species);
    return it == mounts.end() ? nullptr : &it->second;
}
const VehicleDef* TransportRules::vehicle(const std::string& type) const {
    const auto it = vehicles.find(type);
    return it == vehicles.end() ? nullptr : &it->second;
}

const TransportRules* transportRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<TransportRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const std::string ck = data::cacheKey(c.dir);
    if (auto it = cache.find(ck); it != cache.end()) return it->second.get();
    std::unique_ptr<TransportRules> r;
    const fs::path p = fs::path(c.dir) / "transport.json";
    if (data::exists(p.string())) {
        const Json j = data::readJson(p.string());
        r = std::make_unique<TransportRules>();
        r->rideMinM = j.value("ride_min_m", r->rideMinM);
        r->fetchM = j.value("fetch_m", r->fetchM);
        r->riderTire = j.value("rider_tire_mult", r->riderTire);
        r->driverTire = j.value("driver_tire_mult", r->driverTire);
        r->minPull = j.value("min_pull", r->minPull);
        r->seatDyPx = j.value("seat_dy_px", r->seatDyPx);
        r->teamAheadM = j.value("team_ahead_m", r->teamAheadM);
        r->teamRowM = j.value("team_row_m", r->teamRowM);
        r->teamSideM = j.value("team_side_m", r->teamSideM);
        r->pushAheadM = j.value("push_ahead_m", r->pushAheadM);
        const Json mj = j.value("mounts", Json::object()), vj = j.value("vehicles", Json::object()), sj = j.value("start", Json::array());
        for (const auto& [id, m] : mj.items()) {
            MountDef d;
            d.id = id;
            d.ride = m.value("ride", 0.0);
            d.draft = m.value("draft", 0.0);
            d.carryKg = m.value("carry_kg", 0.0);
            d.tirePerKm = m.value("tire_per_km", d.tirePerKm);
            d.restPerHour = m.value("rest_per_hour", d.restPerHour);
            d.tiredSlow = m.value("tired_slow", d.tiredSlow);
            d.maxRiderPx = m.value("max_rider_px", 0);
            d.riderDyPx = m.value("rider_dy_px", d.riderDyPx);
            d.races = strs(m, "races");
            r->mounts[id] = d;
        }
        for (const auto& [id, v] : vj.items()) {
            VehicleDef d;
            d.id = id;
            if (v.contains("team")) {
                d.teamMin = v["team"].at(0).get<int>();
                d.teamMax = v["team"].at(1).get<int>();
            }
            d.draftNeed = v.value("draft_need", 0.0);
            d.capacityKg = v.value("capacity_kg", 0.0);
            d.seats = v.value("seats", 0);
            d.speed = v.value("speed", 1.0);
            d.cargoSlow = v.value("cargo_slow", d.cargoSlow);
            d.draft = strs(v, "draft");
            d.seasons = strs(v, "seasons");
            d.seatDyPx = v.value("seat_dy_px", -1);
            r->vehicles[id] = d;
        }
        for (const auto& s : sj) {
            TransportStart t;
            t.mount = s.value("mount", std::string());
            t.vehicle = s.value("vehicle", std::string());
            t.needs = s.value("needs", std::string());
            t.chance = s.value("chance", 0.0);
            if (s.contains("heads")) {
                t.headsLo = s["heads"].at(0).get<int>();
                t.headsHi = s["heads"].at(1).get<int>();
            }
            t.races = strs(s, "races");
            r->start.push_back(t);
        }
    }
    return (cache[ck] = std::move(r)).get();
}

// --- state -------------------------------------------------------------------------------

EntityId rideOf(const World& w, EntityId person) {
    const Json* s = stateC(w);
    if (!s || !s->contains("ride")) return kNone;
    const auto it = (*s)["ride"].find(key(person));
    return it == (*s)["ride"].end() ? kNone : it->get<EntityId>();
}

EntityId vehicleOf(const World& w, EntityId person) {
    const Json* s = stateC(w);
    if (!s || !s->contains("veh")) return kNone;
    for (auto it = (*s)["veh"].begin(); it != (*s)["veh"].end(); ++it) {
        if (it->value("driver", kNone) == person) return std::stoull(it.key());
        for (const auto& p : (*it)["pass"])
            if (p.get<EntityId>() == person) return std::stoull(it.key());
    }
    return kNone;
}

EntityId driverOf(const World& w, EntityId vehicle) {
    const Json* v = vehState(w, vehicle);
    return v ? v->value("driver", kNone) : kNone;
}
std::vector<EntityId> teamOf(const World& w, EntityId vehicle) { return ids(vehState(w, vehicle), "team"); }
std::vector<EntityId> passengersOf(const World& w, EntityId vehicle) { return ids(vehState(w, vehicle), "pass"); }

EntityId riderOf(const World& w, EntityId beast) {
    const Json* s = stateC(w);
    if (!s || !s->contains("ride")) return kNone;
    for (auto it = (*s)["ride"].begin(); it != (*s)["ride"].end(); ++it)
        if (it->get<EntityId>() == beast) return std::stoull(it.key());
    return kNone;
}

EntityId hitchedTo(const World& w, EntityId beast) {
    const Json* s = stateC(w);
    if (!s || !s->contains("veh")) return kNone;
    for (auto it = (*s)["veh"].begin(); it != (*s)["veh"].end(); ++it)
        for (const auto& b : (*it)["team"])
            if (b.get<EntityId>() == beast) return std::stoull(it.key());
    return kNone;
}

bool harnessed(const World& w, EntityId beast) {
    const Json* s = stateC(w);
    if (!s || (s->value("ride", Json::object()).empty() && s->value("veh", Json::object()).empty())) return false;
    return riderOf(w, beast) != kNone || hitchedTo(w, beast) != kNone;
}

bool isVehicle(const World& w, const WorldObject& o) {
    const TransportRules* R = transportRules(w.content());
    return R && R->vehicle(o.type);
}

// --- numbers -----------------------------------------------------------------------------

double teamDraft(const World& w, const WorldObject& v) {
    const TransportRules* R = transportRules(w.content());
    if (!R) return 0.0;
    double d = 0.0;
    for (EntityId b : teamOf(w, v.id))
        if (const Character* a = w.character(b); a && a->body.alive)
            if (const MountDef* m = R->mount(a->species))
                d += m->draft * (1.0 - m->tiredSlow * std::clamp(a->body.fatigue, 0.0, 1.0));
    return d;
}

double vehicleCapacityKg(const World& w, const WorldObject& v) {
    const TransportRules* R = transportRules(w.content());
    const VehicleDef* d = R ? R->vehicle(v.type) : nullptr;
    return d ? d->capacityKg : 0.0;
}

double vehicleCargoKg(const World& w, const WorldObject& v) {
    double kg = 0.0;
    for (const auto& [item, n] : v.stock)
        if (n > 0) kg += kgOf(w, item) * n;
    return kg;
}

double vehicleSpeedMult(const World& w, const WorldObject& v) {
    const TransportRules* R = transportRules(w.content());
    const VehicleDef* d = R ? R->vehicle(v.type) : nullptr;
    if (!d) return 1.0;
    double pull = 1.0;
    if (d->teamMax > 0) {
        if (static_cast<int>(teamOf(w, v.id).size()) < std::max(1, d->teamMin)) return 0.0;   // nothing pulls it
        pull = d->draftNeed > 0.0 ? std::clamp(teamDraft(w, v) / d->draftNeed, R->minPull, 1.0) : 1.0;
    }
    const double load = d->capacityKg > 0.0 ? std::clamp(vehicleCargoKg(w, v) / d->capacityKg, 0.0, 1.0) : 0.0;
    double m = d->speed * pull * (1.0 - d->cargoSlow * load);
    if (!hasSeason(w, *d)) m *= 0.3;   // a sled on bare ground
    return std::max(0.05, m);
}

double transportSpeedMult(const World& w, const Character& c) {
    if (c.isAnimal()) return 1.0;
    if (const EntityId b = rideOf(w, c.id); b != kNone)
        if (const Character* a = w.character(b); a && a->body.alive)
            if (const MountDef* m = mountDefOf(w, *a); m && m->ride > 0.0)
                return std::max(0.3, m->ride * (1.0 - m->tiredSlow * std::clamp(a->body.fatigue, 0.0, 1.0)));
    if (const EntityId v = vehicleOf(w, c.id); v != kNone)
        if (const WorldObject* o = w.object(v)) return vehicleSpeedMult(w, *o);
    return 1.0;
}

double transportTireMult(const World& w, const Character& c) {
    const TransportRules* R = transportRules(w.content());
    if (!R || c.isAnimal()) return 1.0;
    if (rideOf(w, c.id) != kNone) return R->riderTire;
    if (const EntityId v = vehicleOf(w, c.id); v != kNone) {
        const WorldObject* o = w.object(v);
        const VehicleDef* d = o ? R->vehicle(o->type) : nullptr;
        if (d && d->teamMax == 0) return 1.0 + 0.5 * std::clamp(vehicleCargoKg(w, *o) / std::max(1.0, d->capacityKg), 0.0, 1.0);
        return R->driverTire;
    }
    return 1.0;
}

// --- acts --------------------------------------------------------------------------------

namespace {

std::string whyMount(const World& w, const Character& rider, const Character& beast) {
    const MountDef* m = mountDefOf(w, beast);
    if (!m || m->ride <= 0.0) return "rejected:not_ridden";
    if (!beast.body.alive) return "rejected:dead";
    if (!mayUse(w, rider, beast.status.family)) return "rejected:not_yours";
    if (rideOf(w, rider.id) != kNone) return "rejected:already_riding";
    if (vehicleOf(w, rider.id) != kNone) return "rejected:in_vehicle";
    if (harnessed(w, beast.id)) return "rejected:beast_busy";
    if (beast.body.ageDays < kDaysPerYear && beast.body.ageDays > 0) return "rejected:beast_young";
    if (beast.body.fatigue > 0.92) return "rejected:beast_tired";
    if (!raceOk(w, rider, m->races)) return "rejected:wrong_kind";
    if (m->maxRiderPx > 0 && raceOf(w.content(), rider).spritePx > m->maxRiderPx) return "rejected:too_big";
    if (rider.body.downUntil > w.now()) return "rejected:down";
    return "";
}

std::string whyHitch(const World& w, const Character& who, const WorldObject& v, const Character& beast) {
    const TransportRules* R = transportRules(w.content());
    const VehicleDef* d = R ? R->vehicle(v.type) : nullptr;
    const MountDef* m = mountDefOf(w, beast);
    if (!d) return "rejected:not_vehicle";
    if (!m || m->draft <= 0.0) return "rejected:not_draft";
    if (d->teamMax <= 0) return "rejected:no_harness";
    if (!d->draft.empty() && !contains(d->draft, beast.species)) return "rejected:wrong_beast";
    if (!mayUse(w, who, beast.status.family) || !mayUse(w, who, v.ownerFamily)) return "rejected:not_yours";
    if (harnessed(w, beast.id)) return "rejected:beast_busy";
    if (static_cast<int>(teamOf(w, v.id).size()) >= d->teamMax) return "rejected:team_full";
    return "";
}

} // namespace

std::string mountBeast(World& w, Character& rider, Character& beast) {
    if (std::string why = whyMount(w, rider, beast); !why.empty()) return why;
    Character& one = single(w, beast);
    state(w)["ride"][key(rider.id)] = one.id;
    one.pos = rider.pos;
    return "";
}

std::string dismount(World& w, Character& rider) {
    Json& ride = state(w)["ride"];
    const auto it = ride.find(key(rider.id));
    if (it == ride.end()) return "rejected:not_riding";
    const EntityId b = it->get<EntityId>();
    ride.erase(it);
    if (Character* a = w.character(b)) a->pos = {rider.pos.x + 1.2 * rider.facing.y, rider.pos.y - 1.2 * rider.facing.x};
    return "";
}

std::string hitch(World& w, Character& who, WorldObject& v, Character& beast) {
    if (std::string why = whyHitch(w, who, v, beast); !why.empty()) return why;
    Character& one = single(w, beast);
    vehStateM(w, v.id)["team"].push_back(one.id);
    return "";
}

std::string unhitch(World& w, Character& who, WorldObject& v) {
    (void)who;
    const Json* s = vehState(w, v.id);
    if (!s || (*s)["team"].empty()) return "rejected:no_team";
    Json& vs = vehStateM(w, v.id);
    int i = 0;
    for (const auto& b : vs["team"])
        if (Character* a = w.character(b.get<EntityId>())) a->pos = {v.pos.x + 2.0 + 1.0 * i++, v.pos.y + 1.5};
    vs["team"] = Json::array();
    tidy(w, v.id);
    return "";
}

std::string drive(World& w, Character& who, WorldObject& v) {
    const TransportRules* R = transportRules(w.content());
    const VehicleDef* d = R ? R->vehicle(v.type) : nullptr;
    if (!d) return "rejected:not_vehicle";
    if (!mayUse(w, who, v.ownerFamily)) return "rejected:not_yours";
    if (rideOf(w, who.id) != kNone) return "rejected:already_riding";
    if (vehicleOf(w, who.id) != kNone) return "rejected:in_vehicle";
    if (driverOf(w, v.id) != kNone) return "rejected:seat_taken";
    if (d->teamMax > 0 && static_cast<int>(teamOf(w, v.id).size()) < std::max(1, d->teamMin)) return "rejected:no_team";
    Json& vs = vehStateM(w, v.id);
    vs["driver"] = who.id;
    if (d->teamMax > 0) who.pos = v.pos;
    else {   // behind the handcart, facing the way it faces
        const double fx = vs.value("fx", 1.0), fy = vs.value("fy", 0.0);
        who.pos = {v.pos.x - fx * R->pushAheadM, v.pos.y - fy * R->pushAheadM};
    }
    return "";
}

std::string board(World& w, Character& who, WorldObject& v) {
    const TransportRules* R = transportRules(w.content());
    const VehicleDef* d = R ? R->vehicle(v.type) : nullptr;
    if (!d) return "rejected:not_vehicle";
    if (d->seats <= 0) return "rejected:no_seats";
    if (rideOf(w, who.id) != kNone) return "rejected:already_riding";
    if (vehicleOf(w, who.id) != kNone) return "rejected:in_vehicle";
    if (static_cast<int>(passengersOf(w, v.id).size()) >= d->seats) return "rejected:seats_full";
    vehStateM(w, v.id)["pass"].push_back(who.id);
    who.pos = v.pos;
    return "";
}

std::string alight(World& w, Character& who) {
    const EntityId v = vehicleOf(w, who.id);
    if (v == kNone) return "rejected:not_in_vehicle";
    Json& vs = vehStateM(w, v);
    if (vs.value("driver", kNone) == who.id) vs["driver"] = kNone;
    eraseId(vs["pass"], who.id);
    if (const WorldObject* o = w.object(v)) who.pos = {o->pos.x - 1.0, o->pos.y + 1.2};
    tidy(w, v);
    return "";
}

std::string loadCargo(World& w, Character& who, WorldObject& v) {
    const double cap = vehicleCapacityKg(w, v);
    if (cap <= 0.0) return "rejected:not_vehicle";
    if (!mayUse(w, who, v.ownerFamily)) return "rejected:not_yours";
    double room = cap - vehicleCargoKg(w, v);
    std::map<std::string, int> wornN;
    for (const auto& [slot, item] : who.worn) wornN[item]++;
    int moved = 0;
    for (auto it = who.inventory.begin(); it != who.inventory.end();) {
        const int keep = wornN[it->first];
        const double kg = std::max(0.01, kgOf(w, it->first));
        const int can = std::min(it->second - keep, static_cast<int>(std::floor(room / kg)));
        if (can > 0) {
            v.stock[it->first] += can;
            it->second -= can;
            room -= can * kg;
            moved += can;
        }
        if (it->second <= 0) it = who.inventory.erase(it);
        else ++it;
    }
    if (moved == 0) return room < 0.5 ? "rejected:vehicle_full" : "rejected:nothing_to_load";
    pruneGear(who);
    return "";
}

std::string unloadCargo(World& w, Character& who, WorldObject& v) {
    if (vehicleCapacityKg(w, v) <= 0.0) return "rejected:not_vehicle";
    if (!mayUse(w, who, v.ownerFamily)) return "rejected:not_yours";
    const GearRules* G = gearRules(w.content());
    double room = G ? maxLoadKg(*G, who) - loadKg(*G, who) : 1e9;
    int moved = 0;
    for (auto it = v.stock.begin(); it != v.stock.end();) {
        const double kg = std::max(0.01, kgOf(w, it->first));
        const int can = std::min(it->second, static_cast<int>(std::floor(room / kg)));
        if (can > 0) {
            who.inventory[it->first] += can;
            it->second -= can;
            room -= can * kg;
            moved += can;
        }
        if (it->second <= 0) it = v.stock.erase(it);
        else ++it;
    }
    if (moved == 0) return v.stock.empty() ? "rejected:vehicle_empty" : "rejected:hands_full";
    return "";
}

Json transportActs(const World& w, const Character& me, EntityId target) {
    Json out = Json::array();
    const TransportRules* R = transportRules(w.content());
    if (!R || me.isAnimal()) return out;
    auto add = [&](const std::string& act, const std::string& why) {
        out.push_back({{"id", std::string("transport.") + act}, {"can", why.empty()}, {"why", why}});
    };
    constexpr double kReach = 4.0;
    if (target == kNone || target == me.id) {
        if (rideOf(w, me.id) != kNone) add("dismount", "");
        if (vehicleOf(w, me.id) != kNone) add("alight", "");
        return out;
    }
    if (const Character* b = w.character(target); b && b->isAnimal()) {
        const MountDef* m = mountDefOf(w, *b);
        if (!m) return out;
        if (riderOf(w, b->id) == me.id) add("dismount", "");
        else if (m->ride > 0.0) add("mount", whyMount(w, me, *b));
        return out;
    }
    const WorldObject* v = w.object(target);
    if (!v || !R->vehicle(v->type)) return out;
    const VehicleDef& d = *R->vehicle(v->type);
    const bool far = dist(me.pos, v->pos) > kReach && vehicleOf(w, me.id) != v->id;
    const EntityId in = vehicleOf(w, me.id);
    if (in == v->id) add("alight", "");
    else {
        std::string why = far ? "rejected:too_far" : "";
        if (why.empty() && rideOf(w, me.id) != kNone) why = "rejected:already_riding";
        if (why.empty() && in != kNone) why = "rejected:in_vehicle";
        if (why.empty() && driverOf(w, v->id) != kNone) why = "rejected:seat_taken";
        if (why.empty() && !mayUse(w, me, v->ownerFamily)) why = "rejected:not_yours";
        if (why.empty() && d.teamMax > 0 && static_cast<int>(teamOf(w, v->id).size()) < std::max(1, d.teamMin)) why = "rejected:no_team";
        add("drive", why);
        if (d.seats > 0) {
            std::string bw = far ? "rejected:too_far" : "";
            if (bw.empty() && (rideOf(w, me.id) != kNone || in != kNone)) bw = "rejected:in_vehicle";
            if (bw.empty() && static_cast<int>(passengersOf(w, v->id).size()) >= d.seats) bw = "rejected:seats_full";
            add("board", bw);
        }
    }
    if (d.teamMax > 0) {
        // the nearest free draught beast of one's own household, for hitching
        std::string hw = "rejected:no_beast_near";
        for (const auto& [id, a] : w.characters()) {
            if (!a.isAnimal() || !a.body.alive || dist(a.pos, v->pos) > R->fetchM) continue;
            const MountDef* m = R->mount(a.species);
            if (!m || m->draft <= 0.0) continue;
            hw = whyHitch(w, me, *v, a);
            if (hw.empty()) break;
        }
        if (far && in != v->id) hw = "rejected:too_far";
        add("hitch", hw);
        if (!teamOf(w, v->id).empty()) add("unhitch", far && in != v->id ? "rejected:too_far" : "");
    }
    const std::string lw = far ? "rejected:too_far" : (!mayUse(w, me, v->ownerFamily) ? "rejected:not_yours" : "");
    add("load", lw);
    if (!v->stock.empty()) add("unload", lw);
    return out;
}

std::string performTransport(World& w, Character& me, const std::string& act, EntityId target) {
    const TransportRules* R = transportRules(w.content());
    if (!R) return "rejected:no_transport";
    if (act == "dismount") return dismount(w, me);
    if (act == "alight") return alight(w, me);
    Character* b = w.character(target);
    WorldObject* v = w.object(target);
    if (act == "mount") return b ? mountBeast(w, me, *b) : "rejected:no_target";
    if (!v) return "rejected:no_target";
    if (dist(me.pos, v->pos) > 4.0 && vehicleOf(w, me.id) != v->id) return "rejected:too_far";
    if (act == "drive") return drive(w, me, *v);
    if (act == "board") return board(w, me, *v);
    if (act == "unhitch") return unhitch(w, me, *v);
    if (act == "load") return loadCargo(w, me, *v);
    if (act == "unload") return unloadCargo(w, me, *v);
    if (act == "hitch") {
        Character* best = nullptr;
        double bd = R->fetchM;
        for (auto& [id, a] : w.characters()) {
            if (!a.isAnimal() || !a.body.alive) continue;
            const double d = dist(a.pos, v->pos);
            if (d > bd || !whyHitch(w, me, *v, a).empty()) continue;
            bd = d, best = &a;
        }
        return best ? hitch(w, me, *v, *best) : "rejected:no_beast_near";
    }
    return "rejected:unknown_act";
}

// --- spawning ----------------------------------------------------------------------------

namespace {

Character& newBeast(World& w, const std::string& species, EntityId family, Vec2 at, Rng& r) {
    Character a;
    a.species = species;
    a.name = species;
    a.group = 1;
    a.den = at;
    a.pos = at;
    a.status.family = family;
    a.needs["hunger"] = r.range(0.6, 0.9);
    a.body.ageDays = r.irange(3, 9) * kDaysPerYear;
    a.body.sex = r.chance(0.5) ? 'f' : 'm';
    return w.addCharacter(std::move(a));
}

} // namespace

EntityId spawnRig(World& w, EntityId family, Vec2 at, const std::string& type, const std::string& beast, int heads,
                  EntityId driver) {
    const TransportRules* R = transportRules(w.content());
    if (!R || !R->vehicle(type)) return kNone;
    WorldObject o;
    o.type = type;
    o.pos = at;
    o.ownerFamily = family;
    const EntityId vid = w.addObject(std::move(o)).id;
    Rng r(hashCombine(w.seed(), hashCombine(vid, hashStr("rig"))));
    if (R->mount(beast))
        for (int i = 0; i < heads; ++i) vehStateM(w, vid)["team"].push_back(newBeast(w, beast, family, at, r).id);
    if (driver != kNone && w.character(driver)) vehStateM(w, vid)["driver"] = driver;
    return vid;
}

void spawnTransport(World& w, std::uint64_t seed) {
    const TransportRules* R = transportRules(w.content());
    if (!R) return;
    for (const auto& [fid, fam] : w.families()) {
        const WorldObject* store = w.object(fam.storage);
        if (!store) continue;
        std::string race = "human";
        for (EntityId m : fam.members)
            if (const Character* c = w.character(m)) {
                race = raceId(w.content(), *c);
                if (race.empty()) race = "human";
                break;
            }
        int draft = 0;   // draught heads the household has
        for (const auto& s : R->start) {
            Rng r(hashCombine(seed, hashCombine(static_cast<std::uint64_t>(fid), hashStr("transport:" + s.mount + s.vehicle))));
            if (!s.races.empty() && !contains(s.races, race)) continue;
            if (!r.chance(s.chance)) continue;
            const Vec2 at{store->pos.x + r.range(-7.0, 7.0), store->pos.y + r.range(3.0, 8.0)};
            if (!s.mount.empty() && R->mount(s.mount) && w.content().speciesOf(s.mount)) {
                const int n = r.irange(s.headsLo, s.headsHi);
                for (int i = 0; i < n; ++i) newBeast(w, s.mount, fid, {at.x + i * 1.6, at.y}, r);
                if (R->mount(s.mount)->draft > 0.0) draft += n;
            } else if (!s.vehicle.empty() && R->vehicle(s.vehicle)) {
                if (s.needs == "draft" && draft < 1) continue;
                if (s.needs == "draft2" && draft < 2) continue;
                WorldObject o;
                o.type = s.vehicle;
                o.pos = at;
                o.ownerFamily = fid;
                const EntityId vid = w.addObject(std::move(o)).id;
                for (EntityId m : fam.members)
                    if (Character* c = w.character(m)) c->knownObjects.insert(vid);
            }
        }
    }
}

// --- ticks -------------------------------------------------------------------------------

void transportTick(World& w) {
    const TransportRules* R = transportRules(w.content());
    Json* sp = const_cast<Json*>(stateC(w));
    if (!R) return;
    constexpr Tick kHour = kTicksPerDay / 24;
    // the beasts at rest get their breath back
    if (w.now() % kHour == 0)
        for (auto& [id, a] : w.characters())
            if (a.isAnimal() && a.body.fatigue > 0.0)
                if (const MountDef* m = R->mount(a.species); m && !harnessed(w, id))
                    a.body.fatigue = std::max(0.0, a.body.fatigue - m->restPerHour);
    if (!sp) return;
    auto gone = [&](EntityId id) {
        const Character* c = w.character(id);
        return !c || !c->body.alive || c->captivity.held();
    };
    auto tire = [&](Character& a, double m) {
        if (const MountDef* d = R->mount(a.species)) a.body.fatigue = std::clamp(a.body.fatigue + m / 1000.0 * d->tirePerKm, 0.0, 1.0);
    };
    // riders: the beast goes where the rider is
    Json& ride = (*sp)["ride"];
    for (auto it = ride.begin(); it != ride.end();) {
        const EntityId pid = std::stoull(it.key()), bid = it->get<EntityId>();
        Character* p = w.character(pid);
        Character* a = w.character(bid);
        if (gone(pid) || !a || !a->body.alive || (p && p->body.downUntil > w.now())) {
            it = ride.erase(it);
            continue;
        }
        const double moved = dist(a->pos, p->pos);
        tire(*a, std::min(60.0, moved));
        a->pos = p->pos;
        // an NPC who arrived (stood a tick, not walking: at work, in a fight) gets down; the beast goes home
        if (!p->controller.isPlayer() && p->ai.fsm.state != "Move" && moved < 1e-3) {
            a->pos = {p->pos.x + 1.2 * p->facing.y, p->pos.y - 1.2 * p->facing.x};
            it = ride.erase(it);
            continue;
        }
        ++it;
    }
    // vehicles: the vehicle where the driver is, the team ahead of it, the passengers in it
    Json& veh = (*sp)["veh"];
    for (auto it = veh.begin(); it != veh.end();) {
        const EntityId vid = std::stoull(it.key());
        WorldObject* o = w.object(vid);
        if (!o) {
            it = veh.erase(it);
            continue;
        }
        Json& vs = *it;
        if (const EntityId d = vs.value("driver", kNone); d != kNone && gone(d)) vs["driver"] = kNone;
        for (const char* k : {"team", "pass"})
            for (auto b = vs[k].begin(); b != vs[k].end();)
                if (gone(b->get<EntityId>())) b = vs[k].erase(b);
                else ++b;
        const EntityId d = vs.value("driver", kNone);
        const VehicleDef* vd = R->vehicle(o->type);
        const bool pushed = vd && vd->teamMax == 0;
        if (d != kNone)
            if (Character* dc = w.character(d)) {
                // the vehicle under the driver (a handcart a step before the one who pushes it)
                const double px = vs.value("fx", 1.0), py = vs.value("fy", 0.0);
                const Vec2 was = pushed ? Vec2{o->pos.x - px * R->pushAheadM, o->pos.y - py * R->pushAheadM} : o->pos;
                const double m = dist(was, dc->pos);
                if (m > 1e-3) {
                    vs["fx"] = (dc->pos.x - was.x) / m;
                    vs["fy"] = (dc->pos.y - was.y) / m;
                    for (const auto& b : vs["team"])
                        if (Character* a = w.character(b.get<EntityId>())) tire(*a, std::min(60.0, m));
                }
                const double fx = vs.value("fx", 1.0), fy = vs.value("fy", 0.0);
                o->pos = pushed ? Vec2{dc->pos.x + fx * R->pushAheadM, dc->pos.y + fy * R->pushAheadM} : dc->pos;
            }
        const double fx = vs.value("fx", 1.0), fy = vs.value("fy", 0.0);
        int i = 0;
        const int n = static_cast<int>(vs["team"].size());
        for (const auto& b : vs["team"]) {
            Character* a = w.character(b.get<EntityId>());
            if (!a) continue;
            const int row = i / 2, col = i % 2;
            const double side = n - row * 2 >= 2 ? (col ? R->teamSideM : -R->teamSideM) : 0.0;
            const double ahead = R->teamAheadM + R->teamRowM * row;
            a->pos = {o->pos.x + fx * ahead - fy * side, o->pos.y + fy * ahead + fx * side};
            ++i;
        }
        if (pushed) {   // nobody rides a handcart
            ++it;
            continue;
        }
        for (auto b = vs["pass"].begin(); b != vs["pass"].end();) {
            Character* p = w.character(b->get<EntityId>());
            // a passenger who walked off has got down
            if (p && dist(p->pos, o->pos) > 3.0 && p->controller.isPlayer() == false) {
                b = vs["pass"].erase(b);
                continue;
            }
            if (p) p->pos = o->pos;
            ++b;
        }
        if (vs["team"].empty() && vs["pass"].empty() && vs.value("driver", kNone) == kNone) it = veh.erase(it);
        else ++it;
    }
}

void autoRideStart(World& w, Character& c, Vec2 dest) {
    if (c.isAnimal() || c.controller.isPlayer() || !c.body.alive) return;
    const TransportRules* R = transportRules(w.content());
    if (!R || R->mounts.empty() || dist(c.pos, dest) < R->rideMinM) return;
    if (rideOf(w, c.id) != kNone || vehicleOf(w, c.id) != kNone || c.status.family == kNone) return;
    if (c.body.ageDays < 12 * kDaysPerYear) return;
    Character* best = nullptr;
    double bd = R->fetchM;
    for (auto& [id, a] : w.characters()) {
        if (!a.isAnimal() || a.status.family != c.status.family || !a.body.alive) continue;
        const double d = dist(a.pos, c.pos);
        if (d > bd) continue;
        const MountDef* m = R->mount(a.species);
        if (!m || m->ride <= 0.0 || a.body.fatigue > 0.6 || !whyMount(w, c, a).empty()) continue;
        bd = d, best = &a;
    }
    if (best) mountBeast(w, c, *best);
}

void autoRideEnd(World& w, Character& c) {
    if (c.isAnimal() || c.controller.isPlayer()) return;
    if (rideOf(w, c.id) != kNone) dismount(w, c);
}

void installTransport(World& w) {
    w.addCommandHandler([](World& world, Command& cmd) {
        if (cmd.type != "perform" && cmd.type != "transport") return false;
        const std::string act = cmd.type == "transport" ? cmd.args.value("act", std::string())
                                                        : cmd.args.value("interaction", std::string());
        std::string a = act;
        if (cmd.type == "perform") {
            if (act.rfind("transport.", 0) != 0) return false;
            a = act.substr(10);
        }
        if (std::find(std::begin(kActs), std::end(kActs), a) == std::end(kActs)) return cmd.result = "rejected:unknown_act", true;
        Character* c = world.character(cmd.args.at("id").get<EntityId>());
        if (!c) throw std::runtime_error("no such character");
        if (!c->body.alive) return cmd.result = "rejected:dead", true;
        const std::string r = performTransport(world, *c, a, cmd.args.value("target", kNone));
        if (!r.empty()) cmd.result = r;
        return true;
    });
}

} // namespace lr::sim
