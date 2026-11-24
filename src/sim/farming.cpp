// Fields (objects.json `crop`): tilling and sowing in a window of the year,
// growth and weeds, ripening into a standing crop, shattering when it is left
// too long. The work itself is ordinary interactions (till_field, sow_field,
// weed_field, reap_field...); this file only moves the field's state along
// once a day and answers the `farm:` conditions and utility inputs.
//
// Field state in the object's stock (kCropKeys): strips (size), tilled,
// sown, grown (days since sowing), weeds, weed_load (weed-days above the
// first), ripe (standing sheaves), ripe_age (days standing ripe); the soil:
// moisture and capacity (mm), drainage, soil_depth, fertility and
// soil_fertility (per mille; fertility is worn by each crop and rests back
// toward soil_fertility); the season's water_stress and frost.
//
// Nobody rolls the harvest. It is what the soil can give, less what the
// season's weather (weather.cpp) did to the crop: thirst when the soil water
// runs low, drowned roots on poorly drained ground after long rains, frost
// after sowing, heat at flowering, rain on the standing ripe crop. One
// village shares one sky, so a dry spring is dry for every household.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

const char* const kCropKeys[] = {"strips",    "tilled",     "sown",      "grown",          "weeds",
                                 "weed_load", "ripe",       "ripe_age",  "moisture",       "capacity",
                                 "drainage",  "soil_depth", "fertility", "soil_fertility", "water_stress",
                                 "frost"};

int dayOfYear(const World& w) { return (w.startDay() + w.day()) % kDaysPerYear; }

bool inWindow(int doy, int from, int to) {
    return from <= to ? (doy >= from && doy <= to) : (doy >= from || doy <= to);
}

int st(const WorldObject& o, const char* key) {
    auto it = o.stock.find(key);
    return it == o.stock.end() ? 0 : it->second;
}

const CropDef* cropOf(const World& w, const WorldObject& o) {
    auto d = w.content().objects.find(o.type);
    return d != w.content().objects.end() && d->second.hasCrop ? &d->second.crop : nullptr;
}

// Every field (object type with a crop), through the type index - never a
// scan of all objects (tens of thousands of trees).
template <class W, class F>
void forEachField(W& w, F&& f, const std::string& only = std::string()) {
    for (const auto& [type, def] : w.content().objects) {
        if (!def.hasCrop || (!only.empty() && type != only)) continue;
        for (EntityId id : w.objectsOfType(type))
            if (auto* o = w.object(id)) f(*o, def.crop);
    }
}

// Seed the household has: in the store and in the actor's pack.
int seedHave(const World& w, const Character& c, const std::string& seed) {
    int n = c.has(seed);
    if (const WorldObject* h = w.object(c.home)) {
        auto it = h->stock.find(seed);
        if (it != h->stock.end()) n += it->second;
    }
    return n;
}

// Tillable strips of one field now: in the sowing window, nothing standing.
int freeStrips(const World& w, const WorldObject& o, const CropDef& cd) {
    if (!inWindow(dayOfYear(w), cd.sowFrom, cd.sowTo) || st(o, "ripe") > 0) return 0;
    return std::max(0, st(o, "strips") - st(o, "tilled") - st(o, "sown"));
}

struct FarmSums {
    int strips = 0, tilled = 0, sown = 0, ripe = 0, weeds = 0, free = 0, seedNeed = 0;
    bool window = false;
    int seedPerStrip = 10;
    std::string seed, seedFrom = "grain";
};

FarmSums familyFarm(const World& w, const Character& c, const std::string& type) {
    FarmSums s;
    if (c.status.family == kNone) return s;
    forEachField(w, [&](const WorldObject& o, const CropDef& crop) {
        if (o.ownerFamily != c.status.family) return;
        const CropDef* cd = &crop;
        s.seed = cd->seed;
        s.seedPerStrip = cd->seedPerStrip;
        s.strips += st(o, "strips");
        s.tilled += st(o, "tilled");
        s.sown += st(o, "sown");
        s.ripe += st(o, "ripe");
        s.weeds = std::max(s.weeds, st(o, "weeds"));
        s.free += freeStrips(w, o, *cd);
        s.window = s.window || inWindow(dayOfYear(w), cd->sowFrom, cd->sowTo);
        s.seedFrom = cd->seedFrom;
    }, type);
    if (s.seed.empty()) return s;
    const int have = seedHave(w, c, s.seed);
    // Nobody tills what they cannot sow.
    s.free = std::max(0, std::min(s.free, have / s.seedPerStrip - s.tilled));
    s.seedNeed = std::max(0, (s.strips - s.sown) * s.seedPerStrip - have);
    return s;
}

} // namespace

bool isCropKey(const std::string& key) {
    for (const char* k : kCropKeys)
        if (key == k) return true;
    return false;
}

std::string cropSubjectType(const Content& ct, const std::string& subject) {
    if (subject == "farm") return "field";
    auto d = ct.objects.find(subject);
    return d != ct.objects.end() && d->second.hasCrop ? subject : std::string();
}

double farmValue(const World& w, const Character& c, const WorldObject* target, const std::string& key,
                 const std::string& type) {
    const CropDef* cd = target ? cropOf(w, *target) : nullptr;
    if (target && cd) {
        if (key == "sow_window") return inWindow(dayOfYear(w), cd->sowFrom, cd->sowTo) ? 1.0 : 0.0;
        if (key == "free") {
            const int seedStrips = seedHave(w, c, cd->seed) / cd->seedPerStrip;
            return std::max(0, std::min(freeStrips(w, *target, *cd), seedStrips - st(*target, "tilled")));
        }
        if (isCropKey(key)) return st(*target, key.c_str());
        return 0.0;
    }
    const FarmSums s = familyFarm(w, c, type);
    if (key == "sow_window") return s.window ? 1.0 : 0.0;
    if (key == "free") return s.free;
    if (key == "tilled") return s.tilled;
    if (key == "sown") return s.sown;
    if (key == "ripe") return s.ripe;
    if (key == "weeds") return s.weeds;
    if (key == "strips") return s.strips;
    if (key == "seed_need") return s.seedNeed;
    if (key == "work") {
        // How pressing field work is today, 0..1 (utility input farm:work).
        if (s.ripe > 0) return 1.0;                                  // the crop will not wait
        if (s.tilled > 0 && seedHave(w, c, s.seed) >= s.seedPerStrip) return 0.9;
        if (s.free > 0) return 0.8;
        if (s.weeds >= 2) return 0.5;
        if (s.seedNeed >= s.seedPerStrip) {
            if (const WorldObject* h = w.object(c.home)) {
                auto it = h->stock.find(s.seedFrom);
                if (it != h->stock.end() && it->second >= s.seedPerStrip) return 0.4;
            }
        }
        return 0.0;
    }
    return 0.0;
}

namespace {

EntityId settlementOf(const World& w, const WorldObject& field) {
    auto f = w.families().find(field.ownerFamily);
    return f == w.families().end() ? kNone : f->second.settlement;
}

double permille(const WorldObject& o, const char* key, double fallback) {
    auto it = o.stock.find(key);
    return it == o.stock.end() ? fallback : it->second / 1000.0;
}

} // namespace

double cropYield(const World& w, const WorldObject& field) {
    const CropDef* cd = cropOf(w, field);
    if (!cd) return 0.0;
    // What the soil can give: fertility (worn by cropping, rested by fallow)
    // and the depth of the root zone.
    const double fert = permille(field, "fertility", 0.6);
    const double depth = permille(field, "soil_depth", 0.7);
    const double potential =
        cd->yieldLo + (cd->yieldHi - cd->yieldLo) * std::clamp(0.55 * fert + 0.45 * depth, 0.0, 1.0);
    // What the season took: water stress (drought or drowned roots), frost, weeds.
    const double season = std::max(1, cd->growDays) * 1000.0;
    const double water = std::clamp(1.0 - cd->droughtLoss * st(field, "water_stress") / season, 0.35, 1.0);
    const double frost = std::clamp(1.0 - 0.5 * st(field, "frost") / std::max(1.0, cd->frostLoss), 0.2, 1.0);
    const double weeds = std::max(0.5, 1.0 - cd->weedLoss * st(field, "weed_load"));
    return potential * water * frost * weeds;
}

// One day of the soil and, when sown, of the crop: soil water as a bucket
// (rain in, evaporation and the crop out, the excess draining away as fast as
// the ground lets it), then growth, weeds and the day's stress. Shared by the
// live fields and by worldgen's replay of last year's season.
void growDay(WorldObject& o, const CropDef& crop, const DayWeather& sky) {
    const CropDef* cd = &crop;
    auto& s = o.stock;
    const double cap = std::max(20.0, static_cast<double>(s.count("capacity") ? s["capacity"] : 120));
    const double drain = permille(o, "drainage", 0.5);
    double m = s.count("moisture") ? s["moisture"] : cap * 0.7;
    const bool growing = s["sown"] > 0;
    const double cover =
        growing ? 0.6 + 0.8 * std::min(1.0, s["grown"] / (0.5 * std::max(1, cd->growDays))) : 0.6;
    const double et = sky.tempC <= 0.0 ? 0.2 : (0.6 + 0.13 * sky.tempC) * cover;
    m += sky.rainMm - et;
    if (m > cap) m -= (m - cap) * drain;
    m = std::clamp(m, 0.0, cap * 1.3);
    s["moisture"] = static_cast<int>(std::round(m));
    if (!growing) return;
    const int grown = ++s["grown"];
    if (grown % cd->weedsEvery == 0) s["weeds"] = std::min(4, s["weeds"] + 1);
    s["weed_load"] += std::max(0, s["weeds"] - 1);
    // Stress, weighted by stage: the middle of the season (tillering to
    // flowering) is when thirst and heat cost the most.
    const double stage = grown / static_cast<double>(std::max(1, cd->growDays));
    const double weight = (stage > 0.35 && stage < 0.75) ? 1.5 : 0.8;
    const double rel = m / cap;
    double stress = 0.0;
    if (rel < cd->droughtAt) stress = (cd->droughtAt - rel) / cd->droughtAt;
    else if (rel > cd->waterlogAt) stress = std::min(1.0, (rel - cd->waterlogAt) / 0.2) * 0.8;
    if (sky.tempC > 30.0 && stage > 0.5 && stage < 0.8) stress += 0.3;
    s["water_stress"] += static_cast<int>(std::round(1000.0 * stress * weight));
    if (sky.tempC < cd->frostC && grown > 8 && stage < 0.7)
        s["frost"] += static_cast<int>(std::round((cd->frostC - sky.tempC) * 10.0));
}

double pastSeasonYield(const World& w, const WorldObject& field, int year) {
    const CropDef* cd = cropOf(w, field);
    if (!cd) return 0.0;
    WorldObject f = field; // a copy: the season is replayed, not lived
    const int sowLen = (cd->sowTo - cd->sowFrom + kDaysPerYear) % kDaysPerYear + 1;
    const int sowDay = year * kDaysPerYear + cd->sowFrom + sowLen / 2;
    f.stock["sown"] = std::max(1, f.stock["strips"]);
    for (const char* k : {"grown", "weeds", "water_stress", "frost"}) f.stock[k] = 0;
    f.stock["weed_load"] = 30 * cd->growDays / 100; // weeded now and then
    f.stock["moisture"] = static_cast<int>(std::round(f.stock["capacity"] * 0.8));
    const EntityId st0 = settlementOf(w, field);
    for (int d = 0; d < cd->growDays; ++d) growDay(f, *cd, weatherAt(w, st0, sowDay + d));
    return cropYield(w, f);
}

// AL-93: a household works as much land as it has mouths. Worldgen and a
// settling family size the plots by the people then; the family grows (births,
// marriages, kin taking in), so between the harvest and the sowing - nothing
// standing, nothing tilled - the plot is taken up (or let go) to the eaters it
// feeds now. The seed for the new strips is put aside from the harvest by the
// usual seed_need. Strips per person are the crop's strips_per_person (the
// worldgen default otherwise), the same draw per plot every year.
void fitToHousehold(World& w, WorldObject& o, const CropDef& cd) {
    auto f = w.families().find(o.ownerFamily);
    if (f == w.families().end()) return;
    int people = 0;
    for (EntityId m : f->second.members)
        if (const Character* c = w.character(m); c && c->body.alive) ++people;
    if (people <= 0) return;
    int plots = 0;
    forEachField(w, [&](const WorldObject& other, const CropDef&) {
        if (other.ownerFamily == o.ownerFamily) ++plots;
    }, o.type);
    const double lo = cd.stripsLo >= 0.0 ? cd.stripsLo : 6.0;
    const double hi = cd.stripsHi >= 0.0 ? cd.stripsHi : 8.0;
    Rng fr(hashCombine(w.seed(), hashCombine(static_cast<std::uint64_t>(o.id), hashStr("field_fit"))));
    const int want = std::max(1, static_cast<int>(std::round(people * fr.range(lo, hi) / std::max(1, plots))));
    o.stock["strips"] = want;
}

int daysToStapleHarvest(const World& w) {
    const int doy = dayOfYear(w);
    int best = 0;
    for (const auto& [type, def] : w.content().objects) {
        if (!def.hasCrop || !def.crop.staple || !def.settlement) continue;
        const CropDef& cd = def.crop;
        const int sowLen = (cd.sowTo - cd.sowFrom + kDaysPerYear) % kDaysPerYear + 1;
        const int harvest = (cd.sowFrom + sowLen / 2 + cd.growDays) % kDaysPerYear;
        const int d = (harvest - doy + kDaysPerYear) % kDaysPerYear;
        best = best == 0 ? d : std::min(best, d);
    }
    return best;
}

void cropsDaily(World& w) {
    const int doy = dayOfYear(w);
    const int epoch = w.startDay() + w.day();
    forEachField(w, [&](WorldObject& o, const CropDef& crop) {
        const CropDef* cd = &crop;
        auto& s = o.stock;
        const DayWeather sky = weatherAt(w, settlementOf(w, o), epoch);
        const bool window = inWindow(doy, cd->sowFrom, cd->sowTo);
        // Tilled ground nobody sowed goes back to weeds once the window closes.
        if (!window && s["tilled"] > 0) s["tilled"] = 0;
        const bool growing = s["sown"] > 0;
        if (!growing && s["ripe"] <= 0 && s["tilled"] <= 0 && o.ownerFamily != kNone) fitToHousehold(w, o, crop);
        growDay(o, crop, sky);
        if (growing) {
            const int grown = s["grown"];
            if (grown >= cd->growDays) {
                const double ratio = cropYield(w, o);
                const int sheaves = static_cast<int>(
                    std::round(s["sown"] * cd->seedPerStrip * ratio / static_cast<double>(cd->grainPerSheaf)));
                s["ripe"] += std::max(0, sheaves);
                s["ripe_age"] = 0;
                // A crop takes from the soil; fallow gives it back (below).
                s["fertility"] = std::max(100, s["fertility"] - 60);
                const int stressPct =
                    static_cast<int>(std::round(100.0 * s["water_stress"] / (1000.0 * cd->growDays)));
                const int frost = s["frost"];
                for (const char* k : {"sown", "grown", "weeds", "weed_load", "water_stress", "frost"}) s[k] = 0;
                const Family* fam = nullptr;
                if (auto f = w.families().find(o.ownerFamily); f != w.families().end()) fam = &f->second;
                w.chronicle("crop_ripe", {fam && !fam->members.empty() ? fam->members.front() : kNone},
                            "chronicle.crop_ripe",
                            {{"family", fam ? fam->name : std::string()}, {"crop", o.type}, {"sheaves", sheaves},
                             {"ratio", std::round(ratio * 10.0) / 10.0}, {"stress", stressPct}, {"frost", frost}});
            }
        } else {
            // Resting ground slowly comes back toward what the soil can hold.
            const double base = permille(o, "soil_fertility", 0.7);
            const double fert = permille(o, "fertility", base);
            // Per mille is too coarse for a day's recovery: the fraction is a chance.
            const double gain = 1000.0 * (base - fert) * 0.0015;
            int step = static_cast<int>(gain);
            if (w.rng(o.id, "fallow").uniform() < gain - step) ++step;
            s["fertility"] = static_cast<int>(std::round(1000.0 * fert)) + step;
        }
        if (!growing && s["ripe"] > 0) {
            // Left standing, a ripe crop shatters, lodges and feeds the birds;
            // a wet day sprouts it in the ear.
            const int age = ++s["ripe_age"];
            double share = age > cd->spoilAfter ? cd->spoilPerDay : 0.0;
            if (sky.rainMm > cd->wetHarvestMm) share += 0.5 * cd->spoilPerDay;
            if (share > 0.0) {
                const double expect = s["ripe"] * share;
                int lost = static_cast<int>(expect);
                Rng r = w.rng(o.id, "crop_shatter");
                if (r.uniform() < expect - lost) ++lost;
                s["ripe"] -= std::min(lost, s["ripe"]);
            }
            if (s["ripe"] <= 0) s["ripe_age"] = 0;
        }
    });
}

void seedFields(World& w, std::uint64_t seed, const Json& cfg) {
    // Until history generation runs, a field's state at the start is a cheap
    // model of this year so far: sown in spring if the world starts after the
    // sowing, standing ripe if it starts in the harvest, seed corn kept if it
    // starts before the sowing.
    const Json per = cfg.value("field_strips_per_person", Json::array({6.0, 8.0}));
    const int doy = (w.startDay()) % kDaysPerYear;
    for (auto& [id, o] : w.objects()) {
        const CropDef* cd = cropOf(w, o);
        if (!cd || o.ownerFamily == kNone) continue;
        auto f = w.families().find(o.ownerFamily);
        if (f == w.families().end()) continue;
        Family& fam = f->second;
        int fields = 0; // the household's land of this kind split over its plots
        forEachField(w, [&](const WorldObject& other, const CropDef&) {
            if (other.ownerFamily == fam.id) ++fields;
        }, o.type);
        const double perLo = cd->stripsLo >= 0.0 ? cd->stripsLo : per.at(0).get<double>();
        const double perHi = cd->stripsHi >= 0.0 ? cd->stripsHi : per.at(1).get<double>();
        Rng fr(hashCombine(seed, hashCombine(o.id, hashStr("field_strips"))));
        const double people = std::max<size_t>(1, fam.members.size());
        const int strips = std::max(1, static_cast<int>(std::round(
                                           people * fr.range(perLo, perHi) /
                                           std::max(1, fields))));
        auto& s = o.stock;
        s["strips"] = strips;
        // The soil under the field: the biome's, varied a little field to field.
        const SoilDef& soil = w.content().climate.soil(w.map().biomeAt(o.pos));
        auto pm = [&](double v) { return static_cast<int>(std::round(1000.0 * std::clamp(v, 0.05, 1.0))); };
        s["soil_fertility"] = pm(soil.fertility + fr.range(-0.1, 0.1));
        s["fertility"] = pm(s["soil_fertility"] / 1000.0 - fr.range(0.1, 0.2)); // worn by years of cropping
        s["soil_depth"] = pm(soil.depth + fr.range(-0.1, 0.1));
        s["drainage"] = pm(soil.drainage + fr.range(-0.1, 0.1));
        s["capacity"] = static_cast<int>(std::round(soil.capacityMm * fr.range(0.85, 1.15)));
        s["moisture"] = static_cast<int>(std::round(s["capacity"] * 0.7));
        const int sowLen = (cd->sowTo - cd->sowFrom + kDaysPerYear) % kDaysPerYear + 1;
        const int sinceSow = (doy - cd->sowFrom + kDaysPerYear) % kDaysPerYear; // days since the window opened
        const int sowMid = sowLen / 2;
        int seedLeft = strips; // strips' worth of seed still in the store
        if (sinceSow < sowLen) {
            // In the window: part of it is done.
            const double done = std::clamp(sinceSow / static_cast<double>(sowLen), 0.0, 1.0) * fr.range(0.6, 1.0);
            s["sown"] = static_cast<int>(std::round(strips * done));
            s["grown"] = std::max(0, sinceSow - sowMid / 2);
            seedLeft = strips - s["sown"];
        } else if (sinceSow - sowMid < cd->growDays) {
            s["sown"] = strips; // growing
            s["grown"] = sinceSow - sowMid;
            s["weeds"] = std::min(4, s["grown"] / cd->weedsEvery / 2);
            seedLeft = 0;
        } else if (sinceSow - sowMid - cd->growDays < 30) {
            // Harvest time: ripe, partly reaped.
            const int age = sinceSow - sowMid - cd->growDays;
            const double ratio = pastSeasonYield(w, o, 0); // this year's season so far, replayed
            const double left = std::clamp(1.0 - age / 25.0, 0.1, 1.0);
            s["ripe"] = static_cast<int>(std::round(strips * cd->seedPerStrip * ratio / cd->grainPerSheaf * left));
            s["ripe_age"] = age;
            seedLeft = 0; // the seed comes out of this harvest
        }
        // Between the harvest and the sowing the seed corn sits in the store
        // (a few households kept back too little).
        if (seedLeft > 0 && fam.storage != kNone)
            if (WorldObject* store = w.object(fam.storage)) {
                const double kept = fr.range(0.0, 1.0) < 0.2 ? fr.range(0.6, 0.9) : 1.0;
                store->stock[cd->seed] += static_cast<int>(std::round(seedLeft * cd->seedPerStrip * kept));
            }
    }
}

void openFields(World& w, EntityId family, Vec2 at, std::uint64_t seed) {
    // AL-82: a household that settles takes up land like the others have: a
    // field and a garden by its house, fresh (nothing sown yet), and the seed for
    // them in its store - without them a newcomer family lives off foraging and
    // starves in its first winter.
    auto f = w.families().find(family);
    if (f == w.families().end()) return;
    const double people = std::max<size_t>(1, f->second.members.size());
    WorldObject* store = w.object(f->second.storage);
    int k = 0;
    for (const auto& [type, def] : w.content().objects) {
        if (!def.hasCrop || !def.settlement || def.perFamily <= 0) continue;
        const CropDef& cd = def.crop;
        Rng fr(hashCombine(seed, hashCombine(static_cast<std::uint64_t>(family), hashStr("open_field:" + type))));
        WorldObject o;
        o.type = type;
        const double ang = 0.9 * ++k;
        o.pos = {at.x + std::cos(ang) * (25.0 + 15.0 * k), at.y + std::sin(ang) * (25.0 + 15.0 * k)};
        o.ownerFamily = family;
        o.stock = def.stock;
        const double perLo = cd.stripsLo >= 0.0 ? cd.stripsLo : 6.0;
        const double perHi = cd.stripsHi >= 0.0 ? cd.stripsHi : 8.0;
        auto& s = o.stock;
        s["strips"] = std::max(1, static_cast<int>(std::round(people * fr.range(perLo, perHi))));
        const SoilDef& soil = w.content().climate.soil(w.map().biomeAt(o.pos));
        auto pm = [&](double v) { return static_cast<int>(std::round(1000.0 * std::clamp(v, 0.05, 1.0))); };
        s["soil_fertility"] = pm(soil.fertility + fr.range(-0.1, 0.1));
        s["fertility"] = s["soil_fertility"]; // new-broken ground
        s["soil_depth"] = pm(soil.depth + fr.range(-0.1, 0.1));
        s["drainage"] = pm(soil.drainage + fr.range(-0.1, 0.1));
        s["capacity"] = static_cast<int>(std::round(soil.capacityMm * fr.range(0.85, 1.15)));
        s["moisture"] = static_cast<int>(std::round(s["capacity"] * 0.7));
        if (store && !cd.seed.empty()) store->stock[cd.seed] += static_cast<int>(std::round(s["strips"] * cd.seedPerStrip));
        const EntityId id = w.addObject(o).id;
        for (EntityId m : f->second.members)
            if (Character* c = w.character(m)) c->knownObjects.insert(id);
    }
}

} // namespace altera::sim
