// AL-115: elementals (sim/elementals.hpp).

#include "sim/elementals.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/world.hpp"
#include "sim/world_fields.hpp"

namespace lr::sim {

namespace {

struct Bound {
    Field field = Field::heat;
    bool atLeast = true;
    int value = 0;
};
struct ElementalDef {
    std::string species, aspect;
    std::vector<Bound> feed, kill;
    double rainKillsMm = 0.0, feedWindMs = 0.0;
    double gain = 0.03, loss = 0.02, power = 1.0, radiusM = 1.0;
    bool needsLand = false, ignites = false;
    Json writes = Json::object();
    // wild birth
    bool spawnByField = false, spawnNotWater = false;
    Field spawnField = Field::heat;
    int spawnMin = 255, spawnCells = 1 << 20;
    double spawnWindMs = 0.0, spawnChance = 0.0;
};
struct ElementalsConfig {
    std::vector<ElementalDef> defs;
    const ElementalDef* bySpecies(const std::string& s) const {
        for (const auto& d : defs)
            if (d.species == s) return &d;
        return nullptr;
    }
    const ElementalDef* byAspect(const std::string& a) const {
        for (const auto& d : defs)
            if (d.aspect == a) return &d;
        return nullptr;
    }
};

std::vector<Bound> bounds(const Json& j) {
    std::vector<Bound> out;
    if (!j.is_object()) return out;
    for (const auto& [k, v] : j.items()) {
        Bound b;
        if (!fieldFromName(k, b.field) || !v.is_array() || v.size() != 2) continue;
        b.atLeast = v[0].get<std::string>() == ">=";
        b.value = v[1].get<int>();
        out.push_back(b);
    }
    return out;
}

const ElementalsConfig& config(const World& w) {
    static std::mutex mu;
    static std::map<const Content*, std::unique_ptr<ElementalsConfig>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto& slot = cache[&w.content()];
    if (!slot) {
        slot = std::make_unique<ElementalsConfig>();
        if (w.content().dir.empty()) return *slot;
        std::ifstream in(w.content().dir + "/../world/elementals.json");
        if (!in) return *slot;
        const Json j = Json::parse(in, nullptr, false);
        if (j.is_discarded()) return *slot;
        const Json els = j.value("elementals", Json::object());
        for (const auto& [id, e] : els.items()) {
            if (!w.content().species.count(id)) continue;   // the creature is not loaded
            ElementalDef d;
            d.species = id;
            d.aspect = e.value("aspect", std::string());
            d.feed = bounds(e.value("feed", Json::object()));
            d.kill = bounds(e.value("kill", Json::object()));
            d.rainKillsMm = e.value("rain_kills_mm", 0.0);
            d.feedWindMs = e.value("feed_wind_ms", 0.0);
            d.gain = e.value("gain", d.gain);
            d.loss = e.value("loss", d.loss);
            d.power = e.value("power", d.power);
            d.radiusM = e.value("radius_m", d.radiusM);
            d.needsLand = e.value("needs_land", false);
            d.ignites = e.value("ignites", false);
            d.writes = e.value("writes", Json::object());
            const Json sp = e.value("spawn", Json::object());
            if (sp.contains("field")) {
                d.spawnByField = fieldFromName(sp.value("field", std::string()), d.spawnField);
                d.spawnMin = sp.value("min", d.spawnMin);
                d.spawnCells = sp.value("cells", d.spawnCells);
                d.spawnNotWater = sp.value("not_water", false);
            }
            d.spawnWindMs = sp.value("wind_ms", 0.0);
            d.spawnChance = sp.value("chance_per_day", 0.0);
            slot->defs.push_back(d);
        }
    }
    return *slot;
}

bool holds(const std::vector<Bound>& bs, const World& w, Vec2 p, bool all) {
    if (bs.empty()) return all;
    for (const auto& b : bs) {
        const int v = fieldByte(w, b.field, p);
        const bool ok = b.atLeast ? v >= b.value : v <= b.value;
        if (all && !ok) return false;
        if (!all && ok) return true;
    }
    return all;
}

// today's sky of the fields (reading any cell brings it up to date)
void touchSky(const World& w) { fieldByte(w, Field::heat, {0.5, 0.5}); }
double windMs(const World& w) {
    touchSky(w);
    return w.fields().windMs;
}

bool fed(const World& w, const ElementalDef& d, Vec2 p) {
    if (d.needsLand && w.map().w > 0 && w.map().waterAt(p)) return false;
    if (d.feedWindMs > 0 && windMs(w) < d.feedWindMs) return false;
    // a fire elemental also feeds on flames it stands in
    if (d.aspect == "fire" && fireAt(w, p) > 0) return true;
    return holds(d.feed, w, p, true);
}

bool killed(const World& w, const ElementalDef& d, Vec2 p) {
    if (holds(d.kill, w, p, false) && !d.kill.empty()) return true;
    touchSky(w);
    if (d.rainKillsMm > 0 && w.fields().rainMmToday >= d.rainKillsMm) return true;
    if (d.aspect == "fire" && w.map().w > 0 && w.map().waterAt(p)) return true;
    return false;
}

Character& born(World& w, const ElementalDef& d, Vec2 at, double strength) {
    Character a;
    a.species = d.species;
    a.name = d.species;
    a.group = 1;
    a.pos = at;
    a.den = at;
    a.needs["hunger"] = 1.0;
    a.body.health = std::clamp(strength, 0.1, 1.0);
    a.body.sex = 'f';
    return w.addCharacter(std::move(a));
}

} // namespace

bool isElemental(const World& w, const std::string& species) { return config(w).bySpecies(species) != nullptr; }

EntityId summonElemental(World& w, const std::string& aspect, Vec2 at, double power) {
    const ElementalDef* d = config(w).byAspect(aspect);
    if (!d || killed(w, *d, at)) return kNone;
    Character& c = born(w, *d, at, 0.25 + 0.15 * power);
    return c.id;
}

void elementalsTick(World& w, bool daily) {
    const ElementalsConfig& cfg = config(w);
    if (cfg.defs.empty()) return;
    std::vector<EntityId> gone;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || !c.isAnimal()) continue;
        const ElementalDef* d = cfg.bySpecies(c.species);
        if (!d) continue;
        if (killed(w, *d, c.pos)) {
            c.body.health = 0.0;
        } else if (fed(w, *d, c.pos)) {
            c.body.health = std::min(1.0, c.body.health + d->gain);
        } else {
            c.body.health -= d->loss;
        }
        if (c.body.health <= 0.0) {
            gone.push_back(id);
            continue;
        }
        if (!d->writes.empty()) applyChannels(w, c.pos, d->radiusM, d->writes, d->power * c.body.health);
        if (d->ignites && burnable(w, c.pos) && fireAt(w, c.pos) == 0) ignite(w, c.pos, static_cast<int>(120 + 100 * c.body.health));
    }
    for (EntityId id : gone)
        // it falls apart into its aspect: nothing is left, not even a corpse
        if (Character* c = w.character(id)) {
            c->body.alive = false;
            c->body.health = 0.0;
            c->body.causeOfDeath = "dispersed";
        }
    if (!daily) return;
    // wild births where a field runs strong
    const int n = fieldsConfig(w).chunk;
    for (const auto& d : cfg.defs) {
        if (d.spawnChance <= 0) continue;
        if (d.spawnWindMs > 0) {
            if (windMs(w) < d.spawnWindMs) continue;
            for (const auto& [sid, s] : w.settlements()) {
                if (!s.full()) continue;
                Rng r = w.rng(sid, "elemental:" + d.species);
                if (!r.chance(d.spawnChance)) continue;
                const Vec2 at{s.center.x + r.range(-600.0, 600.0), s.center.y + r.range(-600.0, 600.0)};
                Character& c = born(w, d, at, 0.5);
                w.chronicle("elemental", {c.id}, "chronicle.elemental_born", {{"who", c.id}});
            }
            continue;
        }
        if (!d.spawnByField) continue;
        for (const auto& [key, ch] : w.fields().chunks) {
            if (!ch.active && (d.spawnField == Field::heat || d.spawnField == Field::moisture)) continue;
            const auto& v = ch.v[static_cast<int>(d.spawnField)];
            int strong = 0;
            double sx = 0, sy = 0;
            for (int idx = 0; idx < n * n; ++idx) {
                if (v[idx] < d.spawnMin) continue;
                const double x = ch.cx * n + idx % n + 0.5, y = ch.cy * n + idx / n + 0.5;
                if (d.spawnNotWater && w.map().w > 0 && w.map().waterAt({x, y})) continue;
                ++strong;
                sx += x;
                sy += y;
            }
            if (strong < d.spawnCells) continue;
            Rng r = w.rng(static_cast<EntityId>(key & 0xffffffff), "elemental:" + d.species);
            if (!r.chance(d.spawnChance)) continue;
            Character& c = born(w, d, {sx / strong, sy / strong}, 0.6);
            w.chronicle("elemental", {c.id}, "chronicle.elemental_born", {{"who", c.id}});
        }
    }
}

} // namespace lr::sim
