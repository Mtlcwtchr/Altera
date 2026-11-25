// AL-115: the fields of the world's cells, fire and destruction (sim/world_fields.hpp).

#include "sim/world_fields.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/elementals.hpp"
#include "sim/world_reactions.hpp"
#include "sim/firefight.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace {

constexpr const char* kFieldNames[kFieldCount] = {"heat", "moisture", "fertility", "integrity", "light", "aether", "taint"};
constexpr int kHeat = 0, kMoist = 1, kFert = 2, kInteg = 3, kLight = 4, kAether = 5, kTaint = 6;
// Fields stepped every tick (the rest: once a day). Dormant chunks read these as the baseline.
constexpr bool kFast[kFieldCount] = {true, true, false, true, true, false, false};

int clampByte(double v) { return std::clamp(static_cast<int>(std::lround(v)), 0, 255); }

std::int64_t chunkKey(int cx, int cy) {
    return (static_cast<std::int64_t>(cy) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(cx));
}
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::map<std::string, int> intMap(const Json& j, const char* key) {
    std::map<std::string, int> out;
    if (j.contains(key) && j[key].is_object())
        for (const auto& [k, v] : j[key].items())
            if (v.is_number()) out[k] = v.get<int>();
    return out;
}

} // namespace

const char* fieldName(Field f) { return kFieldNames[static_cast<int>(f)]; }
bool fieldFromName(const std::string& id, Field& out) {
    for (int i = 0; i < kFieldCount; ++i)
        if (id == kFieldNames[i]) {
            out = static_cast<Field>(i);
            return true;
        }
    return false;
}

// --- config --------------------------------------------------------------------------

FieldsConfig FieldsConfig::defaults() {
    FieldsConfig c;
    c.rules[kHeat] = {20, 0.16, 0.08, 0, 0};
    c.rules[kMoist] = {25, 0.05, 0.02, 0, 90};
    c.rules[kFert] = {15, 0, 0, 2, 120};
    c.rules[kInteg] = {30, 0, 0.25, 0, 255};
    c.rules[kLight] = {25, 0.2, 0.35, 0, 0};
    c.rules[kAether] = {20, 0.02, 0, 3, 0};
    c.rules[kTaint] = {20, 0.02, 0, 2, 0};
    FieldMaterial wood;
    wood.id = "wood";
    wood.flammable = 0.9;
    wood.integrity = 120;
    wood.burnPerTick = 3;
    wood.fireHeat = 215;
    wood.debrisType = "wreckage";
    wood.debris = {{"firewood", 3}};
    c.materials["wood"] = wood;
    c.thingDefault.material = "wood";
    c.untouched = {"water", "riverbank", "corpse", "grave", "build_site", "roof_piece", "village_fire", "campfire"};
    return c;
}

FieldsConfig FieldsConfig::fromJson(const Json& j, std::vector<std::string>* warnings) {
    FieldsConfig c = defaults();
    if (!j.is_object() || j.empty()) return c;
    auto warn = [&](const std::string& s) {
        if (warnings) warnings->push_back("fields.json: " + s);
    };
    c.chunk = std::clamp(j.value("chunk", c.chunk), 8, 64);
    if (j.contains("heat_scale")) {
        c.zeroC = j["heat_scale"].value("zero_c", c.zeroC);
        c.cPerStep = std::max(0.5, j["heat_scale"].value("c_per_step", c.cPerStep));
    }
    if (j.contains("fields"))
        for (const auto& [k, v] : j["fields"].items()) {
            Field f;
            if (!fieldFromName(k, f)) {
                warn("unknown field " + k);
                continue;
            }
            FieldRule& r = c.rules[static_cast<int>(f)];
            r.unit = v.value("unit", r.unit);
            r.diffuse = std::clamp(v.value("diffuse", r.diffuse), 0.0, 0.24);
            r.relax = std::clamp(v.value("relax", r.relax), 0.0, 1.0);
            r.relaxPerDay = v.value("relax_per_day", r.relaxPerDay);
            r.base = v.value("base", r.base);
        }
    if (j.contains("biomes"))
        for (const auto& [k, v] : j["biomes"].items()) {
            if (!v.is_object()) continue;
            BiomeFields b;
            b.moisture = v.value("moisture", b.moisture);
            b.fertility = v.value("fertility", b.fertility);
            b.fuel = v.value("fuel", b.fuel);
            b.aether = v.value("aether", b.aether);
            if (k == "_default") c.biomeDefault = b;
            else if (k.rfind('_', 0) != 0) c.biomes[k] = b;
        }
    c.waterMoisture = j.value("water_moisture", c.waterMoisture);
    c.rainMoisturePerMm = j.value("rain_moisture_per_mm", c.rainMoisturePerMm);
    if (j.contains("fire")) {
        const Json& f = j["fire"];
        FireRule& r = c.fire;
        r.igniteHeat = f.value("ignite_heat", r.igniteHeat);
        r.wetStops = f.value("wet_stops", r.wetStops);
        r.fuelPerTick = std::max(1, f.value("fuel_per_tick", r.fuelPerTick));
        r.grow = f.value("grow", r.grow);
        r.flameHeat = f.value("flame_heat", r.flameHeat);
        r.spread = f.value("spread", r.spread);
        r.windCarry = f.value("wind_carry", r.windCarry);
        r.light = f.value("light", r.light);
        r.dryPerTick = f.value("dry_per_tick", r.dryPerTick);
        r.ashFertility = f.value("ash_fertility", r.ashFertility);
        r.fuelRegrowPerDay = f.value("fuel_regrow_per_day", r.fuelRegrowPerDay);
        r.burnHealth = f.value("burn_health", r.burnHealth);
        r.rainPerMm = f.value("rain_per_mm", r.rainPerMm);
        r.smokeMin = f.value("smoke_min", r.smokeMin);
    }
    if (j.contains("materials"))
        for (const auto& [k, v] : j["materials"].items()) {
            if (!v.is_object()) continue;
            FieldMaterial m;
            m.id = k;
            m.flammable = v.value("flammable", 0.0);
            m.integrity = std::clamp(v.value("integrity", 100), 1, 255);
            m.burnPerTick = std::max(1, v.value("burn_per_tick", 4));
            m.fireHeat = v.value("fire_heat", 200);
            m.debrisType = v.value("debris_type", std::string());
            m.debris = intMap(v, "debris");
            m.burntDebris = intMap(v, "burnt_debris");
            m.rustable = v.value("rustable", 0.0);
            c.materials[k] = m;
        }
    if (j.contains("piece_materials"))
        for (const auto& [k, v] : j["piece_materials"].items())
            if (v.is_string()) c.pieceMaterials[k] = v.get<std::string>();
    auto thing = [](const Json& v) {
        ThingRule t;
        t.material = v.value("material", std::string());
        t.integrity = v.value("integrity", -1);
        t.radiusM = v.value("radius_m", 0.5);
        t.keepsStockAsDebris = v.value("keeps_stock", false);
        return t;
    };
    if (j.contains("things"))
        for (const auto& [k, v] : j["things"].items()) {
            if (!v.is_object()) continue;
            if (k == "_default") c.thingDefault = thing(v);
            else if (k.rfind('_', 0) != 0) c.things[k] = thing(v);
        }
    if (j.contains("untouched")) c.untouched = j["untouched"].get<std::vector<std::string>>();
    c.regrowMinDays = j.value("regrow_min_days", c.regrowMinDays);
    if (const Json g = j.value("ground", Json::object()); g.is_object()) {
        c.ground.stepM = std::max(0.01, g.value("step_m", c.ground.stepM));
        c.ground.pitWaterM = g.value("pit_water_m", c.ground.pitWaterM);
        c.ground.drainPerM = g.value("drain_per_m", c.ground.drainPerM);
        c.ground.crackPerM = g.value("crack_per_m", c.ground.crackPerM);
        c.ground.slopeSlow = g.value("slope_slow", c.ground.slopeSlow);
        c.ground.minMove = g.value("min_move", c.ground.minMove);
    }
    if (const Json ff = j.value("firefight", Json::object()); ff.is_object()) {
        FirefightRule& f = c.firefight;
        f.alarmM = ff.value("alarm_m", f.alarmM);
        f.recruitM = ff.value("recruit_m", f.recruitM);
        f.crew = ff.value("crew", f.crew);
        f.reachM = ff.value("reach_m", f.reachM);
        f.douse = ff.value("douse", f.douse);
        f.waterM = ff.value("water_m", f.waterM);
        f.noWaterMult = ff.value("no_water_mult", f.noWaterMult);
        f.fatigue = ff.value("fatigue", f.fatigue);
        if (ff.contains("water_objects")) f.waterObjects = ff["water_objects"].get<std::vector<std::string>>();
    }
    for (const auto& [k, t] : c.things)
        if (!t.material.empty() && !c.materials.count(t.material)) warn("thing " + k + ": unknown material " + t.material);
    for (const auto& [k, m] : c.pieceMaterials)
        if (!c.materials.count(m)) warn("piece material " + k + ": unknown material " + m);
    return c;
}

int FieldsConfig::heatOfC(double c) const { return clampByte((c - zeroC) / cPerStep); }

const FieldMaterial* FieldsConfig::materialOfObject(const std::string& type, ThingRule* rule) const {
    if (std::find(untouched.begin(), untouched.end(), type) != untouched.end()) return nullptr;
    auto it = things.find(type);
    const ThingRule& t = it == things.end() ? thingDefault : it->second;
    if (rule) *rule = t;
    auto m = materials.find(t.material);
    return m == materials.end() ? nullptr : &m->second;
}

const FieldMaterial* FieldsConfig::materialOfPiece(const std::string& buildMaterial) const {
    auto it = pieceMaterials.find(buildMaterial);
    const std::string id = it == pieceMaterials.end() ? "wood" : it->second;
    auto m = materials.find(id);
    return m == materials.end() ? nullptr : &m->second;
}

const FieldsConfig& fieldsConfig(const World& w) {
    static std::mutex mu;
    static std::map<const Content*, std::unique_ptr<FieldsConfig>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto& slot = cache[&w.content()];
    if (!slot) {
        Json j = Json::object();
        if (!w.content().dir.empty()) {
            std::ifstream in(w.content().dir + "/../world/fields.json");
            if (in) j = Json::parse(in, nullptr, false);
            if (j.is_discarded()) j = Json::object();
        }
        slot = std::make_unique<FieldsConfig>(FieldsConfig::fromJson(j));
    }
    return *slot;
}

// --- the state's save ------------------------------------------------------------------

namespace {

// Run-length pairs [value, count, ...]: a chunk is mostly its baseline.
Json rle(const std::vector<std::uint8_t>& v) {
    Json a = Json::array();
    for (std::size_t i = 0; i < v.size();) {
        std::size_t k = i;
        while (k < v.size() && v[k] == v[i]) ++k;
        a.push_back(v[i]);
        a.push_back(k - i);
        i = k;
    }
    return a;
}
std::vector<std::uint8_t> unrle(const Json& a, std::size_t n) {
    std::vector<std::uint8_t> v;
    v.reserve(n);
    for (std::size_t i = 0; i + 1 < a.size(); i += 2)
        v.insert(v.end(), a[i + 1].get<std::size_t>(), a[i].get<std::uint8_t>());
    v.resize(n, 0);
    return v;
}

} // namespace

Json WorldFields::toJson() const {
    Json out = Json::object();
    Json cs = Json::array();
    for (const auto& [k, c] : chunks) {
        Json cj = {{"cx", c.cx}, {"cy", c.cy}, {"active", c.active}, {"quiet", c.quiet}};
        for (int f = 0; f < kFieldCount; ++f) cj[kFieldNames[f]] = rle(c.v[f]);
        cj["fuel"] = rle(c.fuel);
        cj["fire"] = rle(c.fire);
        cj["burnt"] = rle(c.burnt);
        if (!c.ground.empty()) cj["ground"] = rle(c.ground);
        cs.push_back(std::move(cj));
    }
    out["chunks"] = std::move(cs);
    auto fires = [](const std::map<EntityId, ThingFire>& m) {
        Json a = Json::object();
        for (const auto& [id, f] : m) a[std::to_string(id)] = f.flame;
        return a;
    };
    auto ints = [](const std::map<EntityId, int>& m) {
        Json a = Json::object();
        for (const auto& [id, v] : m) a[std::to_string(id)] = v;
        return a;
    };
    out["burning_objects"] = fires(burningObjects);
    out["burning_pieces"] = fires(burningPieces);
    out["object_integrity"] = ints(objectIntegrity);
    out["piece_integrity"] = ints(pieceIntegrity);
    Json wj = Json::object();
    for (const auto& [s, p] : weather) wj[std::to_string(s)] = {p.rainMm, p.heat, p.wind};
    out["weather"] = std::move(wj);
    Json rg = Json::array();
    for (const auto& r : regrow) rg.push_back({r.type, r.pos.x, r.pos.y, r.day});
    out["regrow"] = std::move(rg);
    if (!firefighters.empty()) {
        Json ff = Json::object();
        for (const auto& [p, s] : firefighters) ff[std::to_string(p)] = s;
        out["firefighters"] = std::move(ff);
    }
    if (!fireAlarm.empty()) out["fire_alarm"] = fireAlarm;
    return out;
}

void WorldFields::fromJson(const Json& j) {
    *this = WorldFields{};
    for (const auto& cj : j.value("chunks", Json::array())) {
        FieldChunk c;
        c.cx = cj.value("cx", 0);
        c.cy = cj.value("cy", 0);
        c.active = cj.value("active", true);
        c.quiet = cj.value("quiet", 0);
        // the size of a chunk is the config's; the arrays carry it
        std::size_t n = 0;
        for (std::size_t i = 1; i < cj["heat"].size(); i += 2) n += cj["heat"][i].get<std::size_t>();
        for (int f = 0; f < kFieldCount; ++f) c.v[f] = unrle(cj.value(kFieldNames[f], Json::array()), n);
        c.fuel = unrle(cj.value("fuel", Json::array()), n);
        c.fire = unrle(cj.value("fire", Json::array()), n);
        c.burnt = unrle(cj.value("burnt", Json::array()), n);
        if (cj.contains("ground")) c.ground = unrle(cj["ground"], n);
        chunks[chunkKey(c.cx, c.cy)] = std::move(c);
    }
    auto fires = [](const Json& a, std::map<EntityId, ThingFire>& m) {
        if (a.is_object())
            for (const auto& [k, v] : a.items()) m[static_cast<EntityId>(std::stoul(k))].flame = v.get<int>();
    };
    auto ints = [](const Json& a, std::map<EntityId, int>& m) {
        if (a.is_object())
            for (const auto& [k, v] : a.items()) m[static_cast<EntityId>(std::stoul(k))] = v.get<int>();
    };
    fires(j.value("burning_objects", Json::object()), burningObjects);
    fires(j.value("burning_pieces", Json::object()), burningPieces);
    ints(j.value("object_integrity", Json::object()), objectIntegrity);
    ints(j.value("piece_integrity", Json::object()), pieceIntegrity);
    if (j.contains("weather") && j["weather"].is_object())
        for (const auto& [k, v] : j["weather"].items())
            weather[static_cast<EntityId>(std::stoul(k))] = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
    for (const auto& r : j.value("regrow", Json::array()))
        regrow.push_back({r[0].get<std::string>(), {r[1].get<double>(), r[2].get<double>()}, r[3].get<int>()});
    if (const Json ff = j.value("firefighters", Json::object()); ff.is_object())
        for (const auto& [k, v] : ff.items()) firefighters[static_cast<EntityId>(std::stoul(k))] = v.get<EntityId>();
    for (const auto& s : j.value("fire_alarm", Json::array())) fireAlarm.insert(s.get<EntityId>());
}

// --- the baseline -------------------------------------------------------------------------

namespace {

struct Ambient {
    int heat = 14;
    double rainMm = 0.0, windFromDeg = 270.0, windMs = 3.0;
};

EntityId firstFullSettlement(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return id;
    return kNone;
}

// Today's weather for the fields (one sky over the map: the first village's).
Ambient ambient(const World& w) {
    WorldFields& f = const_cast<World&>(w).fields();
    if (f.ambientDay != w.day()) {
        const FieldsConfig& cfg = fieldsConfig(w);
        const EntityId s = firstFullSettlement(w);
        const DayWeather d = weatherToday(w, s);
        double tempC = d.tempC, rain = d.rainMm;
        double windMs = d.windMs;
        if (auto it = f.weather.find(s); it != f.weather.end()) {
            rain = std::max(0.0, rain + it->second.rainMm);
            tempC += it->second.heat * cfg.cPerStep;
            windMs = std::max(0.0, windMs + it->second.wind);
        }
        f.ambientDay = w.day();
        f.ambientHeat = cfg.heatOfC(tempC);
        f.rainMmToday = rain;
        f.windFromDeg = d.windFromDeg;
        f.windMs = windMs;
    }
    return {f.ambientHeat, f.rainMmToday, f.windFromDeg, f.windMs};
}

const BiomeFields& biomeFieldsAt(const FieldsConfig& cfg, const World& w, Vec2 p) {
    if (w.map().w <= 0) return cfg.biomeDefault;
    const std::string& b = w.map().biomeAt(p);
    auto it = cfg.biomes.find(b);
    return it == cfg.biomes.end() ? cfg.biomeDefault : it->second;
}

struct Base {
    std::array<int, kFieldCount> v{};
    int fuel = 0;
};

Base baseAt(const World& w, const FieldsConfig& cfg, const Ambient& amb, int x, int y) {
    const Vec2 p{x + 0.5, y + 0.5};
    const BiomeFields& b = biomeFieldsAt(cfg, w, p);
    const bool water = w.map().w > 0 && w.map().waterAt(p);
    Base out;
    out.v[kHeat] = amb.heat;
    out.v[kMoist] = water ? cfg.waterMoisture : clampByte(b.moisture + amb.rainMm * cfg.rainMoisturePerMm);
    out.v[kFert] = water ? 0 : b.fertility;
    out.v[kInteg] = cfg.rules[kInteg].base;
    out.v[kLight] = 0;
    out.v[kAether] = b.aether;
    out.v[kTaint] = 0;
    out.fuel = water ? 0 : b.fuel;
    return out;
}

// AL-115: the lie of the land changes the cell's baseline: a pit holds water, raised land drains.
void groundBase(Base& out, const FieldsConfig& cfg, std::uint8_t g, bool water) {
    if (g == 128) return;
    const double m = (g - 128) * cfg.ground.stepM;
    if (m <= -cfg.ground.pitWaterM && !water) {
        out.v[kMoist] = std::max(out.v[kMoist], cfg.waterMoisture - 20);
        out.fuel = 0;
    } else if (m > 0) {
        out.v[kMoist] = clampByte(out.v[kMoist] - cfg.ground.drainPerM * m);
        if (water && m >= cfg.ground.pitWaterM) out.v[kMoist] = std::min(out.v[kMoist], 200);   // a bank out of the water
        out.v[kFert] = clampByte(out.v[kFert] - 10.0 * m);
    }
}

Base baseOf(const World& w, const FieldsConfig& cfg, const Ambient& amb, const FieldChunk& c, int x, int y) {
    Base b = baseAt(w, cfg, amb, x, y);
    if (!c.ground.empty()) {
        const int n = cfg.chunk;
        const bool water = w.map().w > 0 && w.map().waterAt({x + 0.5, y + 0.5});
        groundBase(b, cfg, c.ground[(y - c.cy * n) * n + (x - c.cx * n)], water);
    }
    return b;
}

FieldChunk* chunkOf(World& w, int x, int y, bool make) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int n = cfg.chunk;
    const int cx = floorDiv(x, n), cy = floorDiv(y, n);
    WorldFields& f = w.fields();
    auto it = f.chunks.find(chunkKey(cx, cy));
    if (it != f.chunks.end()) {
        FieldChunk& c = it->second;
        if (make && !c.active) {
            // waking up: the fast fields were the baseline while it slept
            const Ambient amb = ambient(w);
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    const Base b = baseOf(w, cfg, amb, c, cx * n + i, cy * n + j);
                    for (int fi = 0; fi < kFieldCount; ++fi)
                        if (kFast[fi]) c.v[fi][j * n + i] = static_cast<std::uint8_t>(b.v[fi]);
                }
            c.active = true;
            c.quiet = 0;
        }
        return &c;
    }
    if (!make) return nullptr;
    FieldChunk c;
    c.cx = cx;
    c.cy = cy;
    const std::size_t cells = static_cast<std::size_t>(n * n);
    for (auto& a : c.v) a.assign(cells, 0);
    c.fuel.assign(cells, 0);
    c.fire.assign(cells, 0);
    c.burnt.assign(cells, 0);
    const Ambient amb = ambient(w);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const Base b = baseAt(w, cfg, amb, cx * n + i, cy * n + j);
            for (int fi = 0; fi < kFieldCount; ++fi) c.v[fi][j * n + i] = static_cast<std::uint8_t>(b.v[fi]);
            c.fuel[j * n + i] = static_cast<std::uint8_t>(b.fuel);
        }
    return &(f.chunks[chunkKey(cx, cy)] = std::move(c));
}

const FieldChunk* chunkAt(const World& w, int x, int y) {
    const int n = fieldsConfig(w).chunk;
    auto it = w.fields().chunks.find(chunkKey(floorDiv(x, n), floorDiv(y, n)));
    return it == w.fields().chunks.end() ? nullptr : &it->second;
}

int cellIndexIn(const FieldsConfig& cfg, const FieldChunk& c, int x, int y) { return (y - c.cy * cfg.chunk) * cfg.chunk + (x - c.cx * cfg.chunk); }

int cellOf(double m) { return static_cast<int>(std::floor(m)); }

} // namespace

// --- reading ---------------------------------------------------------------------------------

int fieldByte(const World& w, Field f, Vec2 at) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int x = cellOf(at.x), y = cellOf(at.y);
    const int fi = static_cast<int>(f);
    if (const FieldChunk* c = chunkAt(w, x, y)) {
        if (c->active || !kFast[fi]) return c->v[fi][cellIndexIn(cfg, *c, x, y)];
        return baseOf(w, cfg, ambient(w), *c, x, y).v[fi];
    }
    return baseAt(w, cfg, ambient(w), x, y).v[fi];
}

double fieldAt(const World& w, Field f, Vec2 at) { return fieldByte(w, f, at) / 255.0; }
double heatC(const World& w, Vec2 at) { return fieldsConfig(w).cOfHeat(fieldByte(w, Field::heat, at)); }

int fireAt(const World& w, Vec2 at) {
    const int x = cellOf(at.x), y = cellOf(at.y);
    const FieldChunk* c = chunkAt(w, x, y);
    return c ? c->fire[cellIndexIn(fieldsConfig(w), *c, x, y)] : 0;
}

bool burnable(const World& w, Vec2 at) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int x = cellOf(at.x), y = cellOf(at.y);
    int fuel = 0;
    if (const FieldChunk* c = chunkAt(w, x, y)) fuel = c->fuel[cellIndexIn(cfg, *c, x, y)];
    else fuel = baseAt(w, cfg, ambient(w), x, y).fuel;
    return fuel > 0 && fieldByte(w, Field::moisture, at) < cfg.fire.wetStops;
}

int objectIntegrityMax(const World& w, const WorldObject& o) {
    ThingRule rule;
    const FieldMaterial* m = fieldsConfig(w).materialOfObject(o.type, &rule);
    if (!m) return -1;
    return rule.integrity > 0 ? std::min(255, rule.integrity) : m->integrity;
}

int objectIntegrity(const World& w, const WorldObject& o) {
    const int full = objectIntegrityMax(w, o);
    if (full < 0) return -1;
    auto it = w.fields().objectIntegrity.find(o.id);
    return it == w.fields().objectIntegrity.end() ? full : it->second;
}

namespace {

int pieceIntegrityMax(const World& w, const BuildPiece& p) {
    const BuildPieceDef* d = w.content().build.find(p.kind);
    const FieldMaterial* m = fieldsConfig(w).materialOfPiece(d ? d->material : std::string());
    return m ? m->integrity : -1;
}
int pieceIntegrity(const World& w, const BuildPiece& p) {
    const int full = pieceIntegrityMax(w, p);
    auto it = w.fields().pieceIntegrity.find(p.id);
    return it == w.fields().pieceIntegrity.end() ? full : it->second;
}
const FieldMaterial* pieceMaterial(const World& w, const BuildPiece& p) {
    const BuildPieceDef* d = w.content().build.find(p.kind);
    return fieldsConfig(w).materialOfPiece(d ? d->material : std::string());
}
Vec2 piecePos(const World& w, const BuildPiece& p) {
    const BuildPieceDef* d = w.content().build.find(p.kind);
    return d ? pieceCentre(w.content().build, *d, p) : Vec2{};
}

// The index of the things standing on the chunks (rebuilt when the world's things change).
void refreshIndex(World& w) {
    WorldFields& f = w.fields();
    std::uint64_t sig = hashCombine(w.objects().size(), w.pieces().size());
    if (!w.objects().empty()) sig = hashCombine(sig, w.objects().rbegin()->first);
    if (!w.pieces().empty()) sig = hashCombine(sig, w.pieces().rbegin()->first);
    for (const auto& [id, p] : w.pieces()) sig = hashCombine(sig, p.built ? id : 0);
    if (sig == f.indexSig) return;
    f.indexSig = sig;
    f.objectsByChunk.clear();
    f.piecesByChunk.clear();
    const FieldsConfig& cfg = fieldsConfig(w);
    const int n = cfg.chunk;
    for (const auto& [id, o] : w.objects()) {
        if (!cfg.materialOfObject(o.type)) continue;
        f.objectsByChunk[chunkKey(floorDiv(cellOf(o.pos.x), n), floorDiv(cellOf(o.pos.y), n))].push_back(id);
    }
    for (const auto& [id, p] : w.pieces()) {
        if (!p.built) continue;
        const Vec2 c = piecePos(w, p);
        f.piecesByChunk[chunkKey(floorDiv(cellOf(c.x), n), floorDiv(cellOf(c.y), n))].push_back(id);
    }
}

} // namespace

double integrityAt(const World& w, Vec2 at) {
    double best = 1.0;
    const int x = cellOf(at.x), y = cellOf(at.y);
    for (const auto& [id, o] : w.objects()) {
        if (cellOf(o.pos.x) != x || cellOf(o.pos.y) != y) continue;
        const int full = objectIntegrityMax(w, o);
        if (full > 0) best = std::min(best, objectIntegrity(w, o) / double(full));
    }
    for (const auto& [id, p] : w.pieces()) {
        if (!p.built) continue;
        const Vec2 c = piecePos(w, p);
        if (cellOf(c.x) != x || cellOf(c.y) != y) continue;
        const int full = pieceIntegrityMax(w, p);
        if (full > 0) best = std::min(best, pieceIntegrity(w, p) / double(full));
    }
    return best;
}

// --- destruction ---------------------------------------------------------------------------------

namespace {

void dropDebris(World& w, Vec2 at, const std::string& type, const std::map<std::string, int>& stock) {
    if (type.empty() || !w.content().objects.count(type)) return;
    std::map<std::string, int> s;
    for (const auto& [k, v] : stock)
        if (v > 0 && w.content().items.count(k)) s[k] += v;
    if (s.empty()) return;
    // onto a pile of the same kind already there
    for (auto& [id, o] : w.objects())
        if (o.type == type && dist(o.pos, at) < 1.0) {
            for (const auto& [k, v] : s) o.stock[k] += v;
            return;
        }
    WorldObject d;
    d.type = type;
    d.pos = at;
    d.stock = std::move(s);
    w.addObject(d);
}

bool notable(const World& w, const WorldObject& o) {
    auto it = w.content().objects.find(o.type);
    return o.ownerFamily != kNone || (it != w.content().objects.end() && it->second.settlement);
}

void breakObject(World& w, EntityId id, bool burnt, const std::string& cause) {
    WorldObject* o = w.object(id);
    if (!o) return;
    ThingRule rule;
    const FieldMaterial* m = fieldsConfig(w).materialOfObject(o->type, &rule);
    const Vec2 at = o->pos;
    const std::string type = o->type;
    const bool chron = notable(w, *o);
    std::map<std::string, int> left = m ? (burnt ? m->burntDebris : m->debris) : std::map<std::string, int>{};
    if (rule.keepsStockAsDebris && !burnt)
        for (const auto& [k, v] : o->stock) left[k] += v;
    w.fields().burningObjects.erase(id);
    w.fields().objectIntegrity.erase(id);
    if (auto d = w.content().objects.find(type); d != w.content().objects.end() && d->second.perKm2 > 0 && o->ownerFamily == kNone)
        w.fields().regrow.push_back({type, at, w.day() + fieldsConfig(w).regrowMinDays});
    w.removeObject(id);
    if (m) dropDebris(w, at, m->debrisType, left);
    if (chron)
        w.chronicle("destroyed", {}, burnt ? "chronicle.thing_burnt" : "chronicle.thing_destroyed",
                    {{"object", type}, {"cause", cause}, {"x", at.x}, {"y", at.y}});
}

void breakPiece(World& w, EntityId id, bool burnt, const std::string& cause) {
    auto it = w.pieces().find(id);
    if (it == w.pieces().end()) return;
    const FieldMaterial* m = pieceMaterial(w, it->second);
    const Vec2 at = piecePos(w, it->second);
    w.fields().burningPieces.erase(id);
    w.fields().pieceIntegrity.erase(id);
    destroyPiece(w, id, burnt ? "fire" : cause);
    if (m) dropDebris(w, at, m->debrisType, burnt ? m->burntDebris : m->debris);
}

} // namespace

bool damageObject(World& w, EntityId obj, int dmg, const std::string& cause) {
    const WorldObject* o = w.object(obj);
    if (!o || dmg == 0) return false;
    const int full = objectIntegrityMax(w, *o);
    if (full < 0) return false;
    const int now = std::clamp(objectIntegrity(w, *o) - dmg, 0, full);
    if (now <= 0) {
        breakObject(w, obj, cause == "fire", cause);
        return true;
    }
    if (now >= full) w.fields().objectIntegrity.erase(obj);
    else w.fields().objectIntegrity[obj] = now;
    return false;
}

bool damagePiece(World& w, EntityId piece, int dmg, const std::string& cause) {
    auto it = w.pieces().find(piece);
    if (it == w.pieces().end() || !it->second.built || dmg == 0) return false;
    const int full = pieceIntegrityMax(w, it->second);
    if (full < 0) return false;
    const int now = std::clamp(pieceIntegrity(w, it->second) - dmg, 0, full);
    if (now <= 0) {
        breakPiece(w, piece, cause == "fire", cause);
        return true;
    }
    if (now >= full) w.fields().pieceIntegrity.erase(piece);
    else w.fields().pieceIntegrity[piece] = now;
    return false;
}

// --- writing -----------------------------------------------------------------------------------

namespace {

template <class Fn>
void forDisc(Vec2 at, double r, Fn fn) {
    r = std::max(0.5, r);
    const int x0 = cellOf(at.x - r), x1 = cellOf(at.x + r), y0 = cellOf(at.y - r), y1 = cellOf(at.y + r);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double d = std::hypot(x + 0.5 - at.x, y + 0.5 - at.y);
            if (d > r + 0.5) continue;
            fn(x, y, 1.0 - 0.5 * std::min(1.0, d / r));   // full at the centre, half at the rim
        }
}

void addCell(World& w, int fi, int x, int y, double amount) {
    FieldChunk* c = chunkOf(w, x, y, true);
    auto& v = c->v[fi][cellIndexIn(fieldsConfig(w), *c, x, y)];
    v = static_cast<std::uint8_t>(clampByte(v + amount));
}

// Things on the disc: objects by their position, built pieces by their centre.
template <class Fn>
void thingsInDisc(World& w, Vec2 at, double r, Fn fn) {
    std::vector<EntityId> objs, pcs;
    for (const auto& [id, o] : w.objects())
        if (dist(o.pos, at) <= r + 0.5) objs.push_back(id);
    for (const auto& [id, p] : w.pieces())
        if (p.built && dist(piecePos(w, p), at) <= r + 0.5) pcs.push_back(id);
    for (EntityId id : objs) fn(id, false);
    for (EntityId id : pcs) fn(id, true);
}

} // namespace

void fieldAdd(World& w, Field f, Vec2 at, double radiusM, double amount) {
    if (amount == 0.0) return;
    const int fi = static_cast<int>(f);
    forDisc(at, radiusM, [&](int x, int y, double k) { addCell(w, fi, x, y, amount * k); });
    if (f == Field::integrity) {
        // damage or mending lands on what stands there now; the field keeps the mark a while
        const int dmg = static_cast<int>(std::lround(-amount));
        thingsInDisc(w, at, radiusM, [&](EntityId id, bool piece) {
            if (piece) {
                if (dmg > 0) damagePiece(w, id, dmg, "force");
                else if (auto it = w.fields().pieceIntegrity.find(id); it != w.fields().pieceIntegrity.end())
                    damagePiece(w, id, dmg, "mend");
            } else if (dmg > 0) damageObject(w, id, dmg, "force");
            else if (w.fields().objectIntegrity.count(id)) damageObject(w, id, dmg, "mend");
        });
    }
}

bool fieldsStoredAt(const World& w, Vec2 at) { return chunkAt(w, cellOf(at.x), cellOf(at.y)) != nullptr; }

namespace {
int groundByte(const World& w, int x, int y) {
    const FieldChunk* c = chunkAt(w, x, y);
    if (!c || c->ground.empty()) return 128;
    return c->ground[cellIndexIn(fieldsConfig(w), *c, x, y)];
}
} // namespace

double groundAt(const World& w, Vec2 at) {
    if (w.fields().chunks.empty()) return 0.0;
    return (groundByte(w, cellOf(at.x), cellOf(at.y)) - 128) * fieldsConfig(w).ground.stepM;
}

double slopeAt(const World& w, Vec2 at) {
    if (w.fields().chunks.empty()) return 0.0;
    const int x = cellOf(at.x), y = cellOf(at.y);
    const int g = groundByte(w, x, y);
    int d = 0;
    static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    for (int k = 0; k < 4; ++k) d = std::max(d, std::abs(groundByte(w, x + dx[k], y + dy[k]) - g));
    return d * fieldsConfig(w).ground.stepM;
}

void groundAdd(World& w, Vec2 at, double radiusM, double metres, bool sharp) {
    if (std::abs(metres) < 1e-9) return;
    const FieldsConfig& cfg = fieldsConfig(w);
    const double step = cfg.ground.stepM;
    radiusM = std::max(0.5, radiusM);
    double moved = 0.0;   // the most the ground moved anywhere, metres
    const int x0 = cellOf(at.x - radiusM), x1 = cellOf(at.x + radiusM), y0 = cellOf(at.y - radiusM), y1 = cellOf(at.y + radiusM);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double d = std::hypot(x + 0.5 - at.x, y + 0.5 - at.y);
            if (d > radiusM + 0.5) continue;
            // a mound (cosine) or a wall with steep sides
            const double t = std::min(1.0, d / radiusM);
            const double k = sharp ? (t < 0.8 ? 1.0 : std::max(0.0, (1.0 - t) * 5.0)) : 0.5 * (1.0 + std::cos(t * 3.14159265358979));
            const int dv = static_cast<int>(std::lround(metres * k / step));
            if (dv == 0) continue;
            FieldChunk* c = chunkOf(w, x, y, true);
            if (c->ground.empty()) c->ground.assign(c->fuel.size(), 128);
            const int i = cellIndexIn(cfg, *c, x, y);
            const int before = c->ground[i];
            const int now = std::clamp(before + dv, 0, 255);
            c->ground[i] = static_cast<std::uint8_t>(now);
            moved = std::max(moved, std::abs(now - before) * step);
            const double m = (now - 128) * step;
            const bool water = w.map().w > 0 && w.map().waterAt({x + 0.5, y + 0.5});
            if (m <= -cfg.ground.pitWaterM && !water) {
                // the pit fills from the ground water: nothing burns in it
                c->fuel[i] = 0;
                c->fire[i] = 0;
                c->v[kMoist][i] = static_cast<std::uint8_t>(std::max<int>(c->v[kMoist][i], cfg.waterMoisture - 20));
            } else if (now > before) {
                c->v[kMoist][i] = static_cast<std::uint8_t>(clampByte(c->v[kMoist][i] - cfg.ground.drainPerM * (now - before) * step));
            }
        }
    if (moved < step * 0.99) return;
    // what stands on moving ground cracks: a built piece's supports fail, a tree may topple
    thingsInDisc(w, at, radiusM, [&](EntityId id, bool piece) {
        Vec2 p = at;
        if (piece) {
            if (auto it = w.pieces().find(id); it != w.pieces().end()) p = piecePos(w, it->second);
        } else if (const WorldObject* o = w.object(id)) p = o->pos;
        const double t = std::min(1.0, dist(p, at) / radiusM);
        const double k = sharp ? (t < 0.8 ? 1.0 : std::max(0.0, (1.0 - t) * 5.0)) : 0.5 * (1.0 + std::cos(t * 3.14159265358979));
        const int dmg = static_cast<int>(std::lround(cfg.ground.crackPerM * std::abs(metres) * k));
        if (dmg <= 0) return;
        if (piece) damagePiece(w, id, dmg, "quake");
        else damageObject(w, id, dmg / 2, "quake");
    });
}

int douse(World& w, Vec2 at, double radiusM, int flame) {
    const FieldsConfig& cfg = fieldsConfig(w);
    int before = 0;
    forDisc(at, radiusM, [&](int x, int y, double k) {
        FieldChunk* c = chunkOf(w, x, y, false);
        if (!c) return;
        const int i = cellIndexIn(cfg, *c, x, y);
        if (c->fire[i] == 0) return;
        before += c->fire[i];
        c->fire[i] = static_cast<std::uint8_t>(std::max(0, c->fire[i] - static_cast<int>(std::lround(flame * k))));
        c->v[kMoist][i] = static_cast<std::uint8_t>(clampByte(c->v[kMoist][i] + flame * k * 0.8));
        c->v[kHeat][i] = static_cast<std::uint8_t>(clampByte(c->v[kHeat][i] - flame * k * 0.5));
    });
    auto cool = [&](std::map<EntityId, ThingFire>& m, bool piece) {
        for (auto it = m.begin(); it != m.end();) {
            Vec2 p;
            if (piece) {
                auto bp = w.pieces().find(it->first);
                if (bp == w.pieces().end()) { ++it; continue; }
                p = piecePos(w, bp->second);
            } else {
                const WorldObject* o = w.object(it->first);
                if (!o) { ++it; continue; }
                p = o->pos;
            }
            if (dist(p, at) > radiusM + 1.0) { ++it; continue; }
            before += it->second.flame;
            it->second.flame -= flame;
            it = it->second.flame <= 0 ? m.erase(it) : std::next(it);
        }
    };
    cool(w.fields().burningObjects, false);
    cool(w.fields().burningPieces, true);
    return before;
}

bool ignite(World& w, Vec2 at, int flame) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int x = cellOf(at.x), y = cellOf(at.y);
    FieldChunk* c = chunkOf(w, x, y, true);
    const int i = cellIndexIn(cfg, *c, x, y);
    c->v[kHeat][i] = static_cast<std::uint8_t>(std::max<int>(c->v[kHeat][i], cfg.fire.flameHeat));
    bool lit = false;
    if (c->fuel[i] > 0 && c->v[kMoist][i] < cfg.fire.wetStops) {
        c->fire[i] = static_cast<std::uint8_t>(std::max<int>(c->fire[i], std::clamp(flame, 1, 255)));
        lit = true;
    }
    // what stands there catches if it burns
    for (const auto& [id, o] : w.objects())
        if (dist(o.pos, at) <= 1.0)
            if (const FieldMaterial* m = cfg.materialOfObject(o.type); m && m->flammable > 0) {
                w.fields().burningObjects[id].flame = std::max(w.fields().burningObjects[id].flame, int(flame * m->flammable));
                lit = true;
            }
    for (const auto& [id, p] : w.pieces())
        if (p.built && dist(piecePos(w, p), at) <= 1.2)
            if (const FieldMaterial* m = pieceMaterial(w, p); m && m->flammable > 0) {
                w.fields().burningPieces[id].flame = std::max(w.fields().burningPieces[id].flame, int(flame * m->flammable));
                lit = true;
            }
    return lit;
}

double channelValue(const Json& expr, double power) {
    if (expr.is_number()) return expr.get<double>();
    if (!expr.is_string()) return 0.0;
    std::string s = expr.get<std::string>();
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (s.empty()) return 0.0;
    double sign = 1.0;
    if (s[0] == '+' || s[0] == '-') {
        if (s[0] == '-') sign = -1.0;
        s = s.substr(1);
    }
    double k = 1.0;
    bool hasPower = false;
    const auto star = s.find('*');
    std::string a = star == std::string::npos ? s : s.substr(0, star);
    std::string b = star == std::string::npos ? std::string() : s.substr(star + 1);
    auto term = [&](const std::string& t) {
        if (t.empty()) return;
        if (t == "power") hasPower = true;
        else {
            try {
                k *= std::stod(t);
            } catch (...) {
                k = 0.0;
            }
        }
    };
    term(a);
    term(b);
    return sign * k * (hasPower ? power : 1.0);
}

namespace {

EntityId nearestSettlement(const World& w, Vec2 at) {
    EntityId best = kNone;
    double bd = 1e18;
    for (const auto& [id, s] : w.settlements()) {
        if (!s.full()) continue;
        const double d = dist(s.center, at);
        if (d < bd) {
            bd = d;
            best = id;
        }
    }
    return best;
}

void pushWeather(World& w, Vec2 at, double rainMm, double heat, double wind) {
    const EntityId s = nearestSettlement(w, at);
    auto& p = w.fields().weather[s];
    p.rainMm += rainMm;
    p.heat += heat;
    p.wind += wind;
    w.fields().ambientDay = -1;   // today's sky changes now
}

} // namespace

std::vector<std::string> applyChannels(World& w, Vec2 at, double radiusM, const Json& channels, double power) {
    std::vector<std::string> other;
    if (!channels.is_object()) return other;
    const FieldsConfig& cfg = fieldsConfig(w);
    for (const auto& [key, expr] : channels.items()) {
        Field f;
        if (fieldFromName(key, f)) {
            const double units = channelValue(expr, power);
            fieldAdd(w, f, at, radiusM, units * cfg.rules[static_cast<int>(f)].unit);
            if (f == Field::heat && units > 0) {
                // a hot enough blast lights what it touches
                forDisc(at, radiusM, [&](int x, int y, double) {
                    if (fieldByte(w, Field::heat, {x + 0.5, y + 0.5}) >= cfg.fire.igniteHeat) ignite(w, {x + 0.5, y + 0.5}, 120);
                });
            }
        } else if (key == "transfer" && expr.is_string() && fieldFromName(expr.get<std::string>(), f)) {
            // draws the field out of the ring round the disc into it
            const int fi = static_cast<int>(f);
            const double share = std::clamp(0.2 * power, 0.0, 0.8);
            double taken = 0.0;
            int inside = 0;
            const Ambient amb = ambient(w);
            forDisc(at, radiusM * 2.0, [&](int x, int y, double) {
                const double d = std::hypot(x + 0.5 - at.x, y + 0.5 - at.y);
                if (d <= radiusM) {
                    ++inside;
                    return;
                }
                const int v = fieldByte(w, f, {x + 0.5, y + 0.5});
                const int b = baseAt(w, cfg, amb, x, y).v[fi];
                const double t = share * (v > b ? v - b : v * 0.25);
                if (t < 1.0) return;
                addCell(w, fi, x, y, -t);
                taken += t;
            });
            if (inside > 0) forDisc(at, radiusM, [&](int x, int y, double) { addCell(w, fi, x, y, taken / inside); });
        } else if (key == "rain") pushWeather(w, at, 2.0 * channelValue(expr, power), 0.0, 0.0);
        else if (key == "sun") pushWeather(w, at, -1.0 * std::abs(channelValue(expr, power)), 0.5 * channelValue(expr, power), 0.0);
        else if (key == "wind") pushWeather(w, at, 0.0, 0.0, channelValue(expr, power));
        else if (key == "storm") {
            pushWeather(w, at, 3.0 * std::abs(channelValue(expr, power)), -0.5, 3.0 * std::abs(channelValue(expr, power)));
            // lightning takes the tallest thing near: a tree, else a built piece, else the ground
            EntityId tree = kNone;
            double bd = 1e18;
            for (const auto& [id, o] : w.objects())
                if ((o.type == "tree" || o.type == "bee_tree" || o.type == "oak_grove") && dist(o.pos, at) < bd) {
                    bd = dist(o.pos, at);
                    tree = id;
                }
            const Vec2 strike = tree != kNone && bd <= radiusM * 3.0 ? w.object(tree)->pos : at;
            ignite(w, strike, 220);
            w.chronicle("lightning", {}, "chronicle.lightning", {{"x", strike.x}, {"y", strike.y}});
        } else if (key == "rust") {
            const int dmg = static_cast<int>(std::lround(std::abs(channelValue(expr, power)) * cfg.rules[kInteg].unit));
            thingsInDisc(w, at, radiusM, [&](EntityId id, bool piece) {
                if (piece) return;
                const WorldObject* o = w.object(id);
                const FieldMaterial* m = o ? cfg.materialOfObject(o->type) : nullptr;
                if (m && m->rustable > 0) damageObject(w, id, static_cast<int>(dmg * m->rustable), "rust");
            });
        } else if (key == "raise_ground" || key == "lower_ground" || key == "shape_stone") {
            // Earth~ heaves a mound (or, with a negative power, sinks a hollow); Stone~ a wall
            const double v = channelValue(expr, power);
            const double m = (key == "lower_ground" ? -std::abs(v) : v) * (key == "shape_stone" ? 0.6 : 0.5);
            groundAdd(w, at, radiusM, m, key == "shape_stone");
        } else other.push_back(key);
    }
    return applyLifeChannels(w, at, radiusM, channels, power, other);
}

// --- for the reactions -----------------------------------------------------------------------------

bool anyActiveChunk(const World& w) {
    for (const auto& [k, c] : w.fields().chunks)
        if (c.active) return true;
    return false;
}

void forEachStoredCell(World& w, bool activeOnly, const std::function<void(CellRef&)>& fn) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int n = cfg.chunk;
    for (auto& [key, c] : w.fields().chunks)
        if (c.active || !activeOnly)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int idx = j * n + i;
                CellRef r;
                r.x = c.cx * n + i;
                r.y = c.cy * n + j;
                r.active = c.active;
                r.water = w.map().w > 0 && w.map().waterAt({r.x + 0.5, r.y + 0.5});
                for (int f = 0; f < kFieldCount; ++f) r.v[f] = &c.v[f][idx];
                r.fuel = &c.fuel[idx];
                r.fire = &c.fire[idx];
                r.burnt = &c.burnt[idx];
                fn(r);
            }
}

void fieldAddCell(World& w, Field f, int x, int y, double amount) { addCell(w, static_cast<int>(f), x, y, amount); }

int fuelAt(const World& w, Vec2 at) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int x = cellOf(at.x), y = cellOf(at.y);
    if (const FieldChunk* c = chunkAt(w, x, y)) return c->fuel[cellIndexIn(cfg, *c, x, y)];
    return baseAt(w, cfg, ambient(w), x, y).fuel;
}

bool burntAt(const World& w, Vec2 at) {
    const int x = cellOf(at.x), y = cellOf(at.y);
    const FieldChunk* c = chunkAt(w, x, y);
    return c && c->burnt[cellIndexIn(fieldsConfig(w), *c, x, y)];
}

// --- time ------------------------------------------------------------------------------------------

namespace {

struct Heat {
    int x, y;
    double amount;
};

void stepChunks(World& w, bool daily) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const FireRule& fr = cfg.fire;
    const Ambient amb = ambient(w);
    const int n = cfg.chunk;
    WorldFields& wf = w.fields();
    // wind: where it blows to, as a unit vector (from north = -y, clockwise)
    const double rad = (amb.windFromDeg + 180.0) * 3.14159265358979323846 / 180.0;
    const double wx = std::sin(rad), wy = -std::cos(rad);
    const double rainFlame = amb.rainMm * fr.rainPerMm;
    std::vector<Heat> pushed;
    std::vector<std::int64_t> wake;
    std::map<std::int64_t, std::array<std::vector<std::uint8_t>, kFieldCount>> next;

    // the value of a neighbour cell (another chunk or the baseline)
    auto cellValue = [&](const FieldChunk& c, int fi, int i, int j, const Base* nb) -> int {
        if (i >= 0 && i < n && j >= 0 && j < n) return c.v[fi][j * n + i];
        const int x = c.cx * n + i, y = c.cy * n + j;
        if (const FieldChunk* o = chunkAt(w, x, y); o && (o->active || !kFast[fi])) return o->v[fi][cellIndexIn(cfg, *o, x, y)];
        return nb ? nb->v[fi] : baseAt(w, cfg, amb, x, y).v[fi];
    };

    for (auto& [key, c] : wf.chunks) {
        if (!c.active && !daily) continue;
        auto& out = next[key];
        bool busy = false, scarred = false;
        bool edge[4] = {false, false, false, false};  // -x, +x, -y, +y
        for (int fi = 0; fi < kFieldCount; ++fi) out[fi] = c.v[fi];
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int idx = j * n + i;
                const int x = c.cx * n + i, y = c.cy * n + j;
                const Base b = baseOf(w, cfg, amb, c, x, y);
                const bool rim = i == 0 || j == 0 || i == n - 1 || j == n - 1;
                for (int fi = 0; fi < kFieldCount; ++fi) {
                    const FieldRule& r = cfg.rules[fi];
                    const int v = c.v[fi][idx];
                    if (kFast[fi] && !c.active) {
                        out[fi][idx] = static_cast<std::uint8_t>(b.v[fi]);
                        continue;
                    }
                    double nv = v;
                    if (r.diffuse > 0 && (kFast[fi] || daily)) {
                        const double mean = (cellValue(c, fi, i - 1, j, rim ? nullptr : &b) + cellValue(c, fi, i + 1, j, rim ? nullptr : &b) +
                                             cellValue(c, fi, i, j - 1, rim ? nullptr : &b) + cellValue(c, fi, i, j + 1, rim ? nullptr : &b)) /
                                            4.0;
                        nv += r.diffuse * (mean - v) * (kFast[fi] ? 1.0 : 24.0);
                    }
                    if (kFast[fi] && r.relax > 0) {
                        const double d = r.relax * (b.v[fi] - nv);
                        nv += d;
                        // never stuck a byte short of the baseline
                        if (std::lround(nv) == v && v != b.v[fi]) nv = v + (b.v[fi] > v ? 1 : -1);
                    }
                    if (!kFast[fi] && daily && r.relaxPerDay > 0) {
                        const double d = std::min(r.relaxPerDay, std::abs(b.v[fi] - nv));
                        nv += b.v[fi] > nv ? d : -d;
                    }
                    const int o = clampByte(nv);
                    out[fi][idx] = static_cast<std::uint8_t>(o);
                    if (o != b.v[fi]) {
                        if (kFast[fi]) busy = true;
                        else scarred = true;
                        if (rim && std::abs(o - b.v[fi]) > 6 && kFast[fi]) {
                            if (i == 0) edge[0] = true;
                            if (i == n - 1) edge[1] = true;
                            if (j == 0) edge[2] = true;
                            if (j == n - 1) edge[3] = true;
                        }
                    }
                }
                // fire: read the old values, write the new ones
                int flame = c.fire[idx];
                int fuel = c.fuel[idx];
                if (c.active) {
                    if (flame == 0 && fuel > 0 && c.v[kHeat][idx] >= fr.igniteHeat && c.v[kMoist][idx] < fr.wetStops) flame = fr.grow;
                    if (flame > 0) {
                        flame = fuel > 0 ? std::min(255, flame + fr.grow) : flame - 60;
                        flame -= static_cast<int>(std::lround(rainFlame));
                        if (c.v[kMoist][idx] >= fr.wetStops) flame -= 40;
                        if (flame > 0) {
                            const double k = flame / 255.0;
                            fuel = std::max(0, fuel - std::max(1, static_cast<int>(std::lround(fr.fuelPerTick * k))));
                            out[kHeat][idx] = static_cast<std::uint8_t>(std::max<int>(out[kHeat][idx], clampByte(fr.flameHeat * k)));
                            out[kLight][idx] = static_cast<std::uint8_t>(std::max<int>(out[kLight][idx], clampByte(fr.light * k)));
                            out[kMoist][idx] = static_cast<std::uint8_t>(clampByte(out[kMoist][idx] - fr.dryPerTick * k));
                            static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                            for (int d = 0; d < 4; ++d) {
                                const double along = dx[d] * wx + dy[d] * wy;
                                const double share = std::max(0.0, fr.spread * (1.0 + fr.windCarry * amb.windMs * along * 4.0) *
                                                                       (along < 0 ? 0.6 : 1.0));
                                pushed.push_back({x + dx[d], y + dy[d], share * fr.flameHeat * k});
                            }
                            if (fuel == 0) {
                                c.burnt[idx] = 1;
                                out[kFert][idx] = static_cast<std::uint8_t>(clampByte(out[kFert][idx] + fr.ashFertility));
                            }
                            busy = true;
                            if (rim) {
                                if (i == 0) edge[0] = true;
                                if (i == n - 1) edge[1] = true;
                                if (j == 0) edge[2] = true;
                                if (j == n - 1) edge[3] = true;
                            }
                        } else flame = 0;
                    }
                }
                c.fire[idx] = static_cast<std::uint8_t>(std::max(0, flame));
                if (daily && flame == 0 && fuel < b.fuel) fuel = std::min(b.fuel, fuel + fr.fuelRegrowPerDay);
                if (fuel != b.fuel) scarred = true;
                if (c.burnt[idx] && fuel >= b.fuel) c.burnt[idx] = 0;
                c.fuel[idx] = static_cast<std::uint8_t>(fuel);
            }
        if (c.active) {
            c.quiet = busy ? 0 : c.quiet + 1;
            if (!busy && c.quiet >= 3) c.active = false;
        }
        if (edge[0]) wake.push_back(chunkKey(c.cx - 1, c.cy));
        if (edge[1]) wake.push_back(chunkKey(c.cx + 1, c.cy));
        if (edge[2]) wake.push_back(chunkKey(c.cx, c.cy - 1));
        if (edge[3]) wake.push_back(chunkKey(c.cx, c.cy + 1));
        if (!scarred && std::any_of(c.ground.begin(), c.ground.end(), [](std::uint8_t g) { return g != 128; })) scarred = true;
        if (!c.active && !scarred) c.quiet = -1;   // nothing left: drop
    }
    for (auto& [key, arr] : next) {
        FieldChunk& c = wf.chunks.at(key);
        for (int fi = 0; fi < kFieldCount; ++fi) c.v[fi] = std::move(arr[fi]);
    }
    for (auto it = wf.chunks.begin(); it != wf.chunks.end();)
        it = it->second.quiet < 0 ? wf.chunks.erase(it) : std::next(it);
    for (std::int64_t k : wake) {
        const int cy = static_cast<int>(k >> 32);
        const int cx = static_cast<int>(static_cast<std::int32_t>(k & 0xffffffff));
        chunkOf(w, cx * n, cy * n, true);
    }
    for (const Heat& h : pushed) addCell(w, kHeat, h.x, h.y, h.amount * 0.25);
}

// Burning things: they lose integrity, heat their cells, light what stands near.
void stepThings(World& w) {
    const FieldsConfig& cfg = fieldsConfig(w);
    WorldFields& wf = w.fields();
    const Ambient amb = ambient(w);
    const double rainFlame = amb.rainMm * cfg.fire.rainPerMm;
    refreshIndex(w);
    // catching: things on hot cells of active chunks
    for (const auto& [key, c] : wf.chunks) {
        if (!c.active) continue;
        auto oit = wf.objectsByChunk.find(key);
        if (oit != wf.objectsByChunk.end())
            for (EntityId id : oit->second) {
                const WorldObject* o = w.object(id);
                if (!o || wf.burningObjects.count(id)) continue;
                const FieldMaterial* m = cfg.materialOfObject(o->type);
                if (!m || m->flammable <= 0) continue;
                const int x = cellOf(o->pos.x), y = cellOf(o->pos.y);
                const int idx = cellIndexIn(cfg, c, x, y);
                if (c.v[kHeat][idx] >= cfg.fire.igniteHeat && c.v[kMoist][idx] < cfg.fire.wetStops + 30)
                    wf.burningObjects[id].flame = static_cast<int>(std::max(c.fire[idx], std::uint8_t(60)) * m->flammable);
            }
        auto pit = wf.piecesByChunk.find(key);
        if (pit != wf.piecesByChunk.end())
            for (EntityId id : pit->second) {
                auto p = w.pieces().find(id);
                if (p == w.pieces().end() || !p->second.built || wf.burningPieces.count(id)) continue;
                const FieldMaterial* m = pieceMaterial(w, p->second);
                if (!m || m->flammable <= 0) continue;
                const Vec2 at = piecePos(w, p->second);
                const int x = cellOf(at.x), y = cellOf(at.y);
                const int idx = cellIndexIn(cfg, c, x, y);
                if (c.v[kHeat][idx] >= cfg.fire.igniteHeat) wf.burningPieces[id].flame = static_cast<int>(60 * m->flammable);
            }
    }
    // burning
    auto burn = [&](std::map<EntityId, ThingFire>& list, bool pieces) {
        std::vector<std::pair<EntityId, int>> hurt;
        std::vector<EntityId> out;
        for (auto& [id, f] : list) {
            Vec2 at;
            const FieldMaterial* m = nullptr;
            double radius = 0.5;
            if (pieces) {
                auto p = w.pieces().find(id);
                if (p == w.pieces().end() || !p->second.built) {
                    out.push_back(id);
                    continue;
                }
                at = piecePos(w, p->second);
                m = pieceMaterial(w, p->second);
                radius = 1.0;
            } else {
                const WorldObject* o = w.object(id);
                ThingRule rule;
                m = o ? cfg.materialOfObject(o->type, &rule) : nullptr;
                if (!o) {
                    out.push_back(id);
                    continue;
                }
                at = o->pos;
                radius = rule.radiusM;
            }
            if (!m || m->flammable <= 0) {
                out.push_back(id);
                continue;
            }
            f.flame = std::min(255, f.flame + static_cast<int>(30 * m->flammable)) - static_cast<int>(std::lround(rainFlame));
            if (fieldByte(w, Field::moisture, at) >= cfg.fire.wetStops + 40) f.flame -= 30;
            if (f.flame <= 0) {
                out.push_back(id);
                continue;
            }
            const double k = f.flame / 255.0;
            forDisc(at, radius, [&](int x, int y, double kk) {
                FieldChunk* c = chunkOf(w, x, y, true);
                const int idx = cellIndexIn(cfg, *c, x, y);
                c->v[kHeat][idx] = static_cast<std::uint8_t>(std::max<int>(c->v[kHeat][idx], clampByte(m->fireHeat * k * kk)));
                c->v[kLight][idx] = static_cast<std::uint8_t>(std::max<int>(c->v[kLight][idx], clampByte(cfg.fire.light * k * kk)));
            });
            hurt.emplace_back(id, std::max(1, static_cast<int>(std::lround(m->burnPerTick * k))));
        }
        for (EntityId id : out) list.erase(id);
        for (const auto& [id, dmg] : hurt) {
            if (pieces) damagePiece(w, id, dmg, "fire");
            else damageObject(w, id, dmg, "fire");
        }
    };
    if (!wf.burningObjects.empty()) burn(wf.burningObjects, false);
    if (!wf.burningPieces.empty()) burn(wf.burningPieces, true);
}

// People and beasts standing in flames are burnt.
void stepBodies(World& w) {
    const FieldsConfig& cfg = fieldsConfig(w);
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive) continue;
        if (c.isAnimal() && isElemental(w, c.species)) continue;   // they are what burns
        const int f = fireAt(w, c.pos);
        if (f <= 0) continue;
        c.body.health -= cfg.fire.burnHealth * f / 255.0;
        // one awake on their feet gets out of the flames (a tick is ten minutes)
        Vec2 away;
        if (!c.isAnimal() && c.body.health > 0.0 && w.now() >= c.body.downUntil && fieldDanger(w, c.pos, 40.0, &away)) c.pos = away;
        if (c.body.health <= 0.0) {
            c.body.alive = false;
            c.body.health = 0.0;
            c.body.causeOfDeath = "fire";
            w.chronicle("death", {c.id}, "chronicle.death", {{"who", c.id}, {"cause", "fire"}});
        }
    }
}

} // namespace

void fieldsTick(World& w) {
    WorldFields& wf = w.fields();
    const bool daily = w.now() % kTicksPerDay == 0;
    if (daily && !wf.weather.empty()) {
        // the pushed weather eases off: a third a day
        for (auto it = wf.weather.begin(); it != wf.weather.end();) {
            it->second.rainMm *= 0.66;
            it->second.heat *= 0.66;
            it->second.wind *= 0.66;
            const bool gone = std::abs(it->second.rainMm) < 0.05 && std::abs(it->second.heat) < 0.05 && std::abs(it->second.wind) < 0.05;
            it = gone ? wf.weather.erase(it) : std::next(it);
        }
        wf.ambientDay = -1;
    }
    elementalsTick(w, daily);   // AL-115 §5.7: they live on the cells' fields
    if (wf.empty()) return;
    if (daily && !wf.regrow.empty()) {
        // young growth: the source comes back once its ground is not scorched any more
        std::vector<WorldFields::Regrow> keep;
        for (const auto& r : wf.regrow) {
            if (w.day() < r.day || burntAt(w, r.pos) || fireAt(w, r.pos) > 0) {
                keep.push_back(r);
                continue;
            }
            WorldObject o;
            o.type = r.type;
            o.pos = r.pos;
            if (auto d = w.content().objects.find(r.type); d != w.content().objects.end()) o.stock = d->second.stock;
            w.addObject(o);
        }
        wf.regrow = std::move(keep);
    }
    stepChunks(w, daily);
    const bool live = anyActiveChunk(w);
    if (live || daily) reactionsTick(w, daily);   // M4: steam, ice, mud, melt, tainted water; crops in flames
    if (!wf.burningObjects.empty() || !wf.burningPieces.empty() || live) stepThings(w);
    firefightTick(w);   // AL-115: the village runs to put it out
    if (live) stepBodies(w);
}

namespace {

Vec2 argPos(const Json& a) { return {a.value("x", 0.0), a.value("y", 0.0)}; }

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t == "field_add") {
        Field f;
        if (!fieldFromName(cmd.args.value("field", std::string()), f)) {
            cmd.result = "rejected:unknown_field";
            return true;
        }
        fieldAdd(w, f, argPos(cmd.args), cmd.args.value("r", 1.0), cmd.args.value("amount", 0.0));
        cmd.result = "ok";
        return true;
    }
    if (t == "ignite") {
        cmd.result = ignite(w, argPos(cmd.args), cmd.args.value("flame", 200)) ? "ok" : "rejected:nothing_burns";
        return true;
    }
    if (t == "damage") {
        fieldAdd(w, Field::integrity, argPos(cmd.args), cmd.args.value("r", 1.0), -std::abs(cmd.args.value("amount", 50.0)));
        cmd.result = "ok";
        return true;
    }
    if (t == "apply_channels") {
        const auto other = applyChannels(w, argPos(cmd.args), cmd.args.value("r", 2.0), cmd.args.value("channels", Json::object()),
                                         cmd.args.value("power", 1.0));
        cmd.result = "ok";
        if (!other.empty()) cmd.args["_unhandled"] = other;
        return true;
    }
    return false;
}

} // namespace

void installFields(World& w) { w.addCommandHandler(handle); }

// --- the view ------------------------------------------------------------------------------------------

Json fieldsViewJson(const World& w, const Vec2* near, double r) {
    const FieldsConfig& cfg = fieldsConfig(w);
    const int n = cfg.chunk;
    Json lights = Json::array(), fx = Json::array();
    int fires = 0;
    auto inView = [&](double x, double y) { return !near || r <= 0 || std::hypot(x - near->x, y - near->y) <= r; };
    for (const auto& [key, c] : w.fields().chunks) {
        if (!c.active) continue;
        // one light per 4x4 block of flames (the renderer blends them), smoke where it burns hard
        for (int bj = 0; bj < n; bj += 4)
            for (int bi = 0; bi < n; bi += 4) {
                int sum = 0, cnt = 0, mx = 0;
                double sx = 0, sy = 0;
                for (int j = bj; j < bj + 4; ++j)
                    for (int i = bi; i < bi + 4; ++i) {
                        const int f = c.fire[j * n + i];
                        if (f <= 0) continue;
                        ++cnt;
                        sum += f;
                        mx = std::max(mx, f);
                        sx += c.cx * n + i + 0.5;
                        sy += c.cy * n + j + 0.5;
                    }
                if (cnt == 0) continue;
                fires += cnt;
                const double x = sx / cnt, y = sy / cnt;
                if (!inView(x, y)) continue;
                const double k = sum / (255.0 * 16.0);
                lights.push_back({{"x", x}, {"y", y}, {"r", 3.0 + 4.0 * std::sqrt(cnt / 16.0)}, {"rgb", {1.0, 0.55, 0.2}},
                                  {"i", std::min(1.0, 0.3 + k)}, {"flicker", 0.6}});
                if (mx >= cfg.fire.smokeMin) fx.push_back({{"x", x}, {"y", y}, {"kind", "smoke"}, {"strength", std::min(1.0, k * 1.5)}});
            }
        // steam where hot meets wet (M4 reactions fill more kinds)
        for (int idx = 0; idx < n * n; idx += 7) {
            if (c.v[kHeat][idx] >= 60 && c.v[kMoist][idx] >= 200 && c.fire[idx] == 0) {
                const double x = c.cx * n + idx % n + 0.5, y = c.cy * n + idx / n + 0.5;
                if (inView(x, y)) fx.push_back({{"x", x}, {"y", y}, {"kind", "steam"}, {"strength", 0.5}});
            }
        }
    }
    for (const auto* list : {&w.fields().burningObjects, &w.fields().burningPieces}) {
        const bool pieces = list == &w.fields().burningPieces;
        for (const auto& [id, f] : *list) {
            Vec2 at;
            if (pieces) {
                auto p = w.pieces().find(id);
                if (p == w.pieces().end()) continue;
                at = piecePos(w, p->second);
            } else if (const WorldObject* o = w.object(id)) at = o->pos;
            else continue;
            ++fires;
            if (!inView(at.x, at.y)) continue;
            const double k = f.flame / 255.0;
            lights.push_back({{"x", at.x}, {"y", at.y}, {"r", 4.0 + 3.0 * k}, {"rgb", {1.0, 0.5, 0.15}}, {"i", std::min(1.0, 0.4 + k)}, {"flicker", 0.7}});
            fx.push_back({{"x", at.x}, {"y", at.y}, {"kind", "smoke"}, {"strength", std::min(1.0, 0.3 + k)}});
            fx.push_back({{"x", at.x}, {"y", at.y}, {"kind", "flame"}, {"strength", k}});
        }
    }
    // AL-115: people throwing water (a splash fx over each one at work)
    Json crew = Json::array();
    for (const auto& [pid, sid] : w.fields().firefighters)
        if (const Character* p = w.character(pid); p && p->body.alive) {
            crew.push_back(pid);
            if (inView(p->pos.x, p->pos.y)) fx.push_back({{"x", p->pos.x}, {"y", p->pos.y}, {"kind", "splash"}, {"strength", 0.6}});
        }
    const Ambient amb = ambient(w);
    return {{"lights", lights}, {"fx", fx}, {"fires", fires}, {"firefighters", crew}, {"wind", {{"from_deg", amb.windFromDeg}, {"ms", amb.windMs}}}};
}

Json fieldRasterJson(const World& w, Field f, int x0, int y0, int wCells, int hCells) {
    wCells = std::clamp(wCells, 1, 512);
    hCells = std::clamp(hCells, 1, 512);
    Json data = Json::array();
    for (int y = y0; y < y0 + hCells; ++y)
        for (int x = x0; x < x0 + wCells; ++x) data.push_back(fieldByte(w, f, {x + 0.5, y + 0.5}));
    return {{"x", x0}, {"y", y0}, {"w", wCells}, {"h", hCells}, {"field", fieldName(f)}, {"data", data}};
}

} // namespace altera::sim
