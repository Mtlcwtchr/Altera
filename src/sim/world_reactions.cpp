// AL-115 M4: reactions of the elements in the world (sim/world_reactions.hpp).

#include "sim/world_reactions.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>

#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

Json readIf(const std::string& path) {
    data::InFile in(path);
    if (!in) return Json::object();
    Json j = Json::parse(in, nullptr, false);
    return j.is_discarded() ? Json::object() : j;
}

void fieldBytes(const Json& j, std::array<int, kFieldCount>& out) {
    if (!j.is_object()) return;
    for (const auto& [k, v] : j.items()) {
        Field f;
        if (fieldFromName(k, f) && v.is_number()) out[static_cast<int>(f)] = v.get<int>();
    }
}

} // namespace

std::string ReactionsConfig::key(const std::string& a, const std::string& b) { return a < b ? a + "+" + b : b + "+" + a; }

const PairReaction* ReactionsConfig::pair(const std::string& a, const std::string& b) const {
    auto it = pairs.find(key(a, b));
    return it == pairs.end() ? nullptr : &it->second;
}

ReactionsConfig ReactionsConfig::fromJson(const Json& j, const Json& aspectsJson) {
    ReactionsConfig c;
    for (const auto& a : aspectsJson.value("aspects", Json::array())) {
        const std::string id = a.value("id", std::string());
        if (id.empty()) continue;
        c.aspects.push_back(id);
        c.channels[id] = a.value("world_channels", Json::object());
    }
    if (!j.is_object() || j.empty()) return c;
    c.loaded = true;
    const Json pairsJ = j.value("pairs", Json::object()); // a named copy: items() of a temporary dangles
    for (const auto& [k, v] : pairsJ.items()) {
        if (v.is_string()) {
            c.nonePairs.push_back(k);
            continue;
        }
        PairReaction r;
        r.result = v.value("result", std::string());
        r.fx = v.value("fx", std::string());
        r.channels = v.value("channels", Json::object());
        r.cancel = v.value("cancel", false);
        r.minPower = v.value("min_power", 0.0);
        c.pairs[k] = r;
    }
    for (const auto& cj : j.value("cells", Json::array())) {
        CellRule r;
        r.id = cj.value("id", std::string());
        r.state = cj.value("state", std::string());
        r.fx = cj.value("fx", std::string());
        const Json wh = cj.value("when", Json::object());
        r.heatCMin = wh.value("heat_c_min", r.heatCMin);
        r.heatCMax = wh.value("heat_c_max", r.heatCMax);
        r.heatMin = wh.value("heat_min", r.heatMin);
        r.moistureMin = wh.value("moisture_min", r.moistureMin);
        r.moistureMax = wh.value("moisture_max", r.moistureMax);
        r.taintMin = wh.value("taint_min", r.taintMin);
        r.fuelMax = wh.value("fuel_max", r.fuelMax);
        if (wh.contains("water")) r.water = wh["water"].get<bool>() ? 1 : 0;
        if (wh.contains("burnt")) r.burnt = wh["burnt"].get<bool>() ? 1 : 0;
        fieldBytes(cj.value("do", Json::object()), r.doTick);
        fieldBytes(cj.value("do_per_day", Json::object()), r.doDay);
        r.pieceDamagePerDay = cj.value("piece_damage_per_day", 0);
        r.spreadWithWater = cj.value("spread_with_water", 0.0);
        c.cells.push_back(r);
    }
    c.moveFactor = j.value("move_factor", std::map<std::string, double>{});
    return c;
}

std::vector<std::string> ReactionsConfig::problems() const {
    std::vector<std::string> out;
    std::set<std::string> known;
    for (std::size_t i = 0; i < aspects.size(); ++i)
        for (std::size_t k = i; k < aspects.size(); ++k) {
            const std::string kk = key(aspects[i], aspects[k]);
            known.insert(kk);
            if (!pairs.count(kk) && std::find(nonePairs.begin(), nonePairs.end(), kk) == nonePairs.end())
                out.push_back("no entry for " + kk);
        }
    for (const auto& [k, r] : pairs)
        if (!known.count(k)) out.push_back("unknown pair " + k);
    for (const auto& k : nonePairs)
        if (!known.count(k)) out.push_back("unknown pair " + k);
    return out;
}

const ReactionsConfig& reactionsConfig(const World& w) {
    static std::mutex mu;
    static std::map<const Content*, std::unique_ptr<ReactionsConfig>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto& slot = cache[&w.content()];
    if (!slot) {
        const std::string dir = w.content().dir.empty() ? std::string() : w.content().dir + "/../magic/";
        slot = std::make_unique<ReactionsConfig>(
            ReactionsConfig::fromJson(dir.empty() ? Json::object() : readIf(dir + "reactions.json"),
                                      dir.empty() ? Json::object() : readIf(dir + "aspects.json")));
    }
    return *slot;
}

// --- cell rules --------------------------------------------------------------------------

namespace {

struct CellRead {
    std::array<int, kFieldCount> v{};
    int fuel = 0;
    bool water = false, burnt = false;
};

bool fits(const CellRule& r, const CellRead& c, const FieldsConfig& cfg) {
    const double hc = cfg.cOfHeat(c.v[0]);
    if (hc < r.heatCMin || hc > r.heatCMax) return false;
    if (c.v[0] < r.heatMin) return false;
    if (c.v[1] < r.moistureMin || c.v[1] > r.moistureMax) return false;
    if (c.v[6] < r.taintMin) return false;
    if (c.fuel > r.fuelMax) return false;
    if (r.water >= 0 && (r.water == 1) != c.water) return false;
    if (r.burnt >= 0 && (r.burnt == 1) != c.burnt) return false;
    return true;
}

// Does a rule read the fields a dormant chunk does not keep?
bool readsFast(const CellRule& r) {
    return r.heatCMin > -1e8 || r.heatCMax < 1e8 || r.heatMin >= 0 || r.moistureMin >= 0 || r.moistureMax < 256;
}

CellRead readAt(const World& w, Vec2 at) {
    CellRead c;
    for (int f = 0; f < kFieldCount; ++f) c.v[f] = fieldByte(w, static_cast<Field>(f), at);
    c.fuel = fuelAt(w, at);
    c.water = w.map().w > 0 && w.map().waterAt(at);
    c.burnt = burntAt(w, at);
    return c;
}

} // namespace

std::string cellStateAt(const World& w, Vec2 at) {
    const ReactionsConfig& rc = reactionsConfig(w);
    const FieldsConfig& cfg = fieldsConfig(w);
    const CellRead c = readAt(w, at);
    for (const auto& r : rc.cells)
        if (fits(r, c, cfg)) return r.state;
    return "";
}

double moveFactorAt(const World& w, Vec2 at) {
    double f = 1.0;
    const std::string s = cellStateAt(w, at);
    if (!s.empty())
        if (auto it = reactionsConfig(w).moveFactor.find(s); it != reactionsConfig(w).moveFactor.end()) f = it->second;
    if (const double sl = slopeAt(w, at); sl > 0.0) {
        const GroundRule& g = fieldsConfig(w).ground;
        f = std::min(f, std::max(g.minMove, 1.0 / (1.0 + sl * g.slopeSlow)));
    }
    return f;
}

double stepMoveFactor(const World& w, Vec2 from, Vec2 to, double step) {
    if (w.fields().chunks.empty()) return 1.0;
    const double d = std::min(step, dist(from, to));
    if (d < 1e-9) return fieldsStoredAt(w, from) ? std::max(0.1, moveFactorAt(w, from)) : 1.0;
    // the way's first `step` metres in up to 24 samples: time = sum of dx / factor
    const int k = std::clamp(static_cast<int>(std::ceil(d)), 1, 24);
    const double dx = (to.x - from.x) / dist(from, to), dy = (to.y - from.y) / dist(from, to);
    double time = 0.0;
    bool any = false;
    for (int i = 0; i < k; ++i) {
        const double t = d * (i + 0.5) / k;
        const Vec2 p{from.x + dx * t, from.y + dy * t};
        double f = 1.0;
        if (fieldsStoredAt(w, p)) {
            f = std::max(0.1, moveFactorAt(w, p));   // molten rock: one wades round it, not through
            any = true;
        }
        time += 1.0 / f;
    }
    return any ? k / time : 1.0;
}

namespace {

double danger(const World& w, Vec2 p) {
    const FieldsConfig& cfg = fieldsConfig(w);
    double d = 0.0;
    if (const int f = fireAt(w, p); f > 0) d += 2.0 + f / 255.0;
    const int h = fieldByte(w, Field::heat, p);
    if (h >= cfg.fire.igniteHeat / 2) d += (h - cfg.fire.igniteHeat / 2) / 64.0;
    const int t = fieldByte(w, Field::taint, p);
    if (t >= 120) d += (t - 120) / 64.0;
    return d;
}

} // namespace

bool fieldDanger(const World& w, Vec2 at, double lookM, Vec2* away) {
    if (w.fields().chunks.empty()) return false;
    const double here = danger(w, at);
    if (here <= 0.0) return false;
    if (away) {
        // the lowest danger on rings round, upwind preferred on a tie
        const double rad = w.fields().windFromDeg * 3.14159265358979323846 / 180.0;
        const Vec2 up{std::sin(rad), -std::cos(rad)};
        Vec2 best = at;
        double bd = here;
        for (double r = 6.0; r <= lookM + 1e-6; r += 6.0)
            for (int k = 0; k < 12; ++k) {
                const double a = k * 3.14159265358979323846 / 6.0;
                const Vec2 dir{std::cos(a), std::sin(a)};
                const Vec2 p{at.x + dir.x * r, at.y + dir.y * r};
                const double d = danger(w, p) - 0.05 * (dir.x * up.x + dir.y * up.y) + 0.002 * r;
                if (d < bd) {
                    bd = d;
                    best = p;
                }
            }
        const double s = w.map().sizeM;
        *away = {std::clamp(best.x, 1.0, s - 1.0), std::clamp(best.y, 1.0, s - 1.0)};
    }
    return true;
}

bool drinksTaintedWater(const World& w, const Character& c) {
    if (w.fields().chunks.empty()) return false;
    const Settlement* home = nullptr;
    if (auto it = w.settlements().find(c.status.settlement); it != w.settlements().end()) home = &it->second;
    const Vec2 from = home ? home->center : c.pos;
    // the nearest well or water the household would draw from
    const WorldObject* best = nullptr;
    double bd = 1e18;
    for (const char* t : {"well", "water"})
        for (EntityId id : w.objectsOfType(t))
            if (const WorldObject* o = w.object(id); o && dist(o->pos, from) < bd) {
                bd = dist(o->pos, from);
                best = o;
            }
    return best && fieldByte(w, Field::taint, best->pos) >= 40;
}

// --- spells -------------------------------------------------------------------------------

SpellWorldResult applySpellToWorld(World& w, Vec2 at, double radiusM, const std::vector<WorldSpellPart>& parts, double power) {
    SpellWorldResult out;
    const ReactionsConfig& rc = reactionsConfig(w);
    std::vector<bool> dropped(parts.size(), false);
    std::vector<const PairReaction*> extra;
    for (std::size_t i = 0; i < parts.size(); ++i)
        for (std::size_t k = i + 1; k < parts.size(); ++k) {
            const PairReaction* r = rc.pair(parts[i].aspect, parts[k].aspect);
            if (!r || power < r->minPower) continue;
            out.reactions.push_back(r->result);
            if (r->cancel) dropped[i] = dropped[k] = true;
            else extra.push_back(r);
        }
    auto addUnhandled = [&](const std::vector<std::string>& v) {
        for (const auto& k : v)
            if (std::find(out.unhandled.begin(), out.unhandled.end(), k) == out.unhandled.end()) out.unhandled.push_back(k);
    };
    bool any = false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (dropped[i]) continue;
        auto ch = rc.channels.find(parts[i].aspect);
        if (ch == rc.channels.end() || !ch->second.contains(parts[i].vec)) continue;
        addUnhandled(applyChannels(w, at, radiusM, ch->second.at(parts[i].vec), power));
        any = true;
    }
    for (const PairReaction* r : extra) addUnhandled(applyChannels(w, at, radiusM, r->channels, power));
    out.cancelled = !any && !parts.empty();
    return out;
}

namespace {

bool isCrop(const World& w, const WorldObject& o) {
    auto it = w.content().objects.find(o.type);
    return it != w.content().objects.end() && it->second.hasCrop;
}

} // namespace

std::vector<std::string> applyLifeChannels(World& w, Vec2 at, double radiusM, const Json& channels, double power,
                                           const std::vector<std::string>& keys) {
    std::vector<std::string> rest;
    auto has = [&](const char* k) { return channels.is_object() && channels.contains(k); };
    // crops and wild plants in the disc (a field is several metres: its middle counts)
    std::vector<EntityId> plants;
    for (const auto& [id, o] : w.objects())
        if (dist(o.pos, at) <= radiusM + 4.0) plants.push_back(id);
    for (const auto& k : keys) {
        if (k == "growth") {
            const double v = channelValue(channels.at("growth"), power);
            for (EntityId id : plants) {
                WorldObject* o = w.object(id);
                if (!o) continue;
                if (isCrop(w, *o)) {
                    auto& s = o->stock;
                    if (s["sown"] <= 0) continue;
                    // days of growth now, paid for by the soil (a hungry year after a miracle)
                    s["grown"] = std::max(0, s["grown"] + static_cast<int>(std::lround(3.0 * v)));
                    if (v > 0) s["fertility"] = std::max(100, s["fertility"] - static_cast<int>(std::lround(25.0 * v)));
                } else if (const ObjectDef* d = &w.content().objects.at(o->type); !d->maxStock.empty() && v > 0) {
                    for (const auto& [item, mx] : d->maxStock) o->stock[item] = std::min(mx, o->stock[item] + static_cast<int>(std::lround(v)));
                }
            }
            fieldAdd(w, Field::fertility, at, radiusM, v > 0 ? -6.0 * v : 0.0);
        } else if (k == "lure") {
            // beasts round come to the caster or run from them
            const double v = channelValue(channels.at("lure"), power);
            const double reach = std::max(30.0, radiusM * 15.0 * std::abs(v));
            for (auto& [id, c] : w.characters()) {
                if (!c.isAnimal() || !c.body.alive) continue;
                const double d = dist(c.pos, at);
                if (d > reach || d < 1e-6) continue;
                const double step = std::min(d - 2.0, 0.5 * d) * (v > 0 ? 1.0 : -1.0);
                c.pos.x += (at.x - c.pos.x) / d * step;
                c.pos.y += (at.y - c.pos.y) / d * step;
            }
        } else rest.push_back(k);
    }
    // fertility and water reach a sown field's own soil numbers too
    if (has("fertility") || has("moisture"))
        for (EntityId id : plants) {
            WorldObject* o = w.object(id);
            if (!o || !isCrop(w, *o)) continue;
            if (has("fertility"))
                o->stock["fertility"] =
                    std::clamp(o->stock["fertility"] + static_cast<int>(std::lround(40.0 * channelValue(channels.at("fertility"), power))), 0, 1000);
            if (has("moisture"))
                o->stock["moisture"] = std::max(0, o->stock["moisture"] + static_cast<int>(std::lround(10.0 * channelValue(channels.at("moisture"), power))));
        }
    return rest;
}

// --- the tick -----------------------------------------------------------------------------

void reactionsTick(World& w, bool daily) {
    if (w.fields().chunks.empty()) return;
    const ReactionsConfig& rc = reactionsConfig(w);
    const FieldsConfig& cfg = fieldsConfig(w);
    struct Spill {
        int x, y;
        double amount;
    };
    std::vector<Spill> spills;
    std::vector<std::pair<Vec2, int>> soaked;   // mud under pieces: day's damage
    forEachStoredCell(w, !daily, [&](CellRef& c) {
        if (!c.active && !daily) return;
        CellRead r;
        for (int f = 0; f < kFieldCount; ++f) r.v[f] = *c.v[f];
        r.fuel = *c.fuel;
        r.water = c.water;
        r.burnt = *c.burnt != 0;
        for (const auto& rule : rc.cells) {
            if (!c.active && readsFast(rule)) continue;
            if (!fits(rule, r, cfg)) continue;
            if (c.active)
                for (int f = 0; f < kFieldCount; ++f)
                    if (rule.doTick[f]) *c.v[f] = static_cast<std::uint8_t>(std::clamp(*c.v[f] + rule.doTick[f], 0, 255));
            if (daily) {
                for (int f = 0; f < kFieldCount; ++f)
                    if (rule.doDay[f]) *c.v[f] = static_cast<std::uint8_t>(std::clamp(*c.v[f] + rule.doDay[f], 0, 255));
                if (rule.pieceDamagePerDay > 0) soaked.push_back({{c.x + 0.5, c.y + 0.5}, rule.pieceDamagePerDay});
                if (rule.spreadWithWater > 0) {
                    const double give = r.v[6] * rule.spreadWithWater;
                    static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d)
                        if (w.map().waterAt({c.x + dx[d] + 0.5, c.y + dy[d] + 0.5})) spills.push_back({c.x + dx[d], c.y + dy[d], give / 4.0});
                    *c.v[6] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(*c.v[6] - give * 0.5), 0, 255));
                }
            }
        }
    });
    for (const Spill& s : spills) fieldAddCell(w, Field::taint, s.x, s.y, s.amount);
    if (!soaked.empty()) {
        // walls on soaked ground lose their footing
        std::vector<std::pair<EntityId, int>> hit;
        for (const auto& [id, p] : w.pieces()) {
            if (!p.built) continue;
            const BuildPieceDef* d = w.content().build.find(p.kind);
            if (!d) continue;
            const Vec2 c = pieceCentre(w.content().build, *d, p);
            for (const auto& [at, dmg] : soaked)
                if (std::abs(c.x - at.x) < 1.0 && std::abs(c.y - at.y) < 1.0) {
                    hit.emplace_back(id, dmg);
                    break;
                }
        }
        for (const auto& [id, dmg] : hit) damagePiece(w, id, dmg, "flood");
    }
    // a standing crop in flames is lost; a field under strong taint stops growing
    std::vector<EntityId> crops;
    for (const auto& [id, o] : w.objects())
        if (isCrop(w, o)) crops.push_back(id);
    for (EntityId id : crops) {
        WorldObject* o = w.object(id);
        if (!o) continue;
        auto& s = o->stock;
        if (fireAt(w, o->pos) > 0 && (s["sown"] > 0 || s["ripe"] > 0)) {
            for (const char* k : {"sown", "grown", "ripe", "ripe_age", "weeds", "weed_load", "water_stress"}) s[k] = 0;
            const Family* fam = nullptr;
            if (auto f = w.families().find(o->ownerFamily); f != w.families().end()) fam = &f->second;
            w.chronicle("crop_burnt", {fam && !fam->members.empty() ? fam->members.front() : kNone}, "chronicle.crop_burnt",
                        {{"family", fam ? fam->name : std::string()}, {"object", o->type}});
        } else if (daily && s["sown"] > 0 && fieldByte(w, Field::taint, o->pos) >= 60) {
            s["grown"] = std::max(0, s["grown"] - 1);
        }
    }
}

} // namespace lr::sim
