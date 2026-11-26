// Peoples as data (AL-114): see species.hpp.
#include "sim/species.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

#include "sim/content.hpp"
#include "sim/magic.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace fs = std::filesystem;

namespace {

Json readFile(const fs::path& p, std::vector<std::string>& errors) {
    if (!data::exists(p)) return Json();
    try {
        return data::readJson(p.string());
    } catch (const std::exception& e) {
        errors.push_back(p.filename().string() + ": " + e.what());
        return Json();
    }
}

std::vector<fs::path> jsonFiles(const fs::path& dir) {
    return data::listPaths(dir); // LR-142: every layer's files, sorted
}

double num(const Json& j, const char* k, double d) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; }
int inum(const Json& j, const char* k, int d) { return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<int>() : d; }
std::vector<std::string> strs(const Json& j, const char* k) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(k) && j[k].is_array())
        for (const auto& s : j[k])
            if (s.is_string()) out.push_back(s.get<std::string>());
    return out;
}
std::map<std::string, double> dmap(const Json& j, const char* k) {
    std::map<std::string, double> out;
    if (j.is_object() && j.contains(k) && j[k].is_object())
        for (auto it = j[k].begin(); it != j[k].end(); ++it)
            if (it.value().is_number()) out[it.key()] = it.value().get<double>();
    return out;
}

// Two kinds' maps averaged (a missing key is 1 for multipliers, 0 for biases).
std::map<std::string, double> avgMap(const std::map<std::string, double>& a, const std::map<std::string, double>& b,
                                     double missing) {
    std::map<std::string, double> out;
    for (const auto& [k, v] : a) out[k] = 0.5 * (v + (b.count(k) ? b.at(k) : missing));
    for (const auto& [k, v] : b)
        if (!a.count(k)) out[k] = 0.5 * (v + missing);
    return out;
}

RaceDef blendOf(const RaceDef& a, const RaceDef& b) {
    RaceDef r;
    auto avg = [](double x, double y) { return 0.5 * (x + y); };
    r.tags = a.tags;
    for (const auto& t : b.tags)
        if (std::find(r.tags.begin(), r.tags.end(), t) == r.tags.end()) r.tags.push_back(t);
    r.sapience = avg(a.sapience, b.sapience);
    r.heightM = avg(a.heightM, b.heightM);
    r.massKg = avg(a.massKg, b.massKg);
    r.nightVision = avg(a.nightVision, b.nightVision);
    r.speedMult = avg(a.speedMult, b.speedMult);
    r.reachMult = avg(a.reachMult, b.reachMult);
    r.spritePx = std::max(a.spritePx, b.spritePx);
    r.skin = b.skin.empty() ? a.skin : b.skin;
    r.pawn = a.pawn;
    for (const auto& [k, v] : b.pawn) r.pawn[k] = v;
    r.adultYears = static_cast<int>(std::lround(avg(a.adultYears, b.adultYears)));
    r.elderYears = static_cast<int>(std::lround(avg(a.elderYears, b.elderYears)));
    r.lifespanYears = static_cast<int>(std::lround(avg(a.lifespanYears, b.lifespanYears)));
    r.fertility = avg(a.fertility, b.fertility);
    r.pregnancyMult = avg(a.pregnancyMult, b.pregnancyMult);
    r.needMult = avgMap(a.needMult, b.needMult, 1.0);
    r.statMods = avgMap(a.statMods, b.statMods, 0.0);
    r.diet = a.diet;
    r.affinity = avgMap(a.affinity, b.affinity, 1.0);
    r.traitsPool = avgMap(a.traitsPool, b.traitsPool, 1.0);
    r.resist = avgMap(a.resist, b.resist, 1.0);
    r.weak = avgMap(a.weak, b.weak, 1.0);
    r.relationsBias = avgMap(a.relationsBias, b.relationsBias, 0.0);
    for (const auto& [k, v] : a.aptitudes) {
        const RaceAptitude o = b.aptitudes.count(k) ? b.aptitudes.at(k) : RaceAptitude{};
        r.aptitudes[k] = {avg(v.learn, o.learn), avg(v.cap, o.cap)};
    }
    for (const auto& [k, v] : b.aptitudes)
        if (!a.aptitudes.count(k)) r.aptitudes[k] = {avg(v.learn, 1.0), avg(v.cap, 1.0)};
    r.giftedChance = avg(a.giftedChance, b.giftedChance);
    r.cultureDefault = a.cultureDefault;
    return r;
}

// LR-166: `over` laid on `base`: objects merge key by key, anything else replaces.
Json mergeOver(const Json& base, const Json& over) {
    if (!base.is_object() || !over.is_object()) return over;
    Json out = base;
    for (auto it = over.begin(); it != over.end(); ++it)
        out[it.key()] = out.contains(it.key()) ? mergeOver(out[it.key()], it.value()) : it.value();
    return out;
}

void readRace(const Json& j, RaceDef& r) {
    if (j.contains("tags")) r.tags = strs(j, "tags");
    r.base = j.value("base", r.base);
    r.kin = j.value("kin", r.kin);
    if (j.contains("region")) {
        if (j["region"].is_string()) r.region = {j["region"].get<std::string>()};
        else r.region = strs(j, "region");
    }
    r.settles = j.value("settles", r.settles);
    if (j.contains("anchor") && j["anchor"].is_object()) r.anchor = j["anchor"];
    if (j.contains("mobility") && j["mobility"].is_object()) r.mobility = j["mobility"];
    r.sapience = num(j, "sapience", r.sapience);
    if (j.contains("glyph")) r.glyph = j.value("glyph", r.glyph);
    const Json b = j.value("body", Json::object());
    r.heightM = num(b, "height_m", r.heightM);
    r.massKg = num(b, "mass_kg", r.massKg);
    r.spritePx = inum(b, "sprite_px", r.spritePx);
    r.skin = b.value("skin", r.skin);
    const Json pawn = b.value("pawn", Json::object());   // a local: items() of a temporary dangles
    for (const auto& [k, v] : pawn.items())
        if (v.is_string()) r.pawn[k] = v.get<std::string>();
        else if (v.is_boolean()) r.pawn[k] = v.get<bool>() ? "yes" : "";
    r.nightVision = num(b, "night_vision", r.nightVision);
    r.speedMult = num(b, "speed_mult", r.speedMult);
    r.reachMult = num(b, "reach_mult", r.reachMult);
    const Json l = j.value("life", Json::object());
    r.adultYears = inum(l, "adult_years", r.adultYears);
    r.elderYears = inum(l, "elder_years", r.elderYears);
    r.lifespanYears = inum(l, "lifespan_years", r.lifespanYears);
    r.fertility = num(l, "fertility", r.fertility);
    r.pregnancyMult = num(l, "pregnancy_mult", r.pregnancyMult);
    if (l.contains("interbreed") && l["interbreed"].is_object())
        r.interbreed = l["interbreed"].get<std::map<std::string, std::string>>();
    r.sexes = l.value("sexes", r.sexes);
    r.otherSex = l.value("other_sex", r.otherSex);
    if (j.contains("needs")) r.needMult = dmap(j, "needs");
    if (j.contains("stats")) r.statMods = dmap(j, "stats"); // LR-141
    if (j.contains("diet")) r.diet = strs(j, "diet");
    if (j.contains("aptitudes") && j["aptitudes"].is_object())
        for (auto it = j["aptitudes"].begin(); it != j["aptitudes"].end(); ++it)
            r.aptitudes[it.key()] = {num(it.value(), "learn", 1.0), num(it.value(), "cap", 1.0)};
    if (j.contains("affinity")) r.affinity = dmap(j, "affinity");
    if (j.contains("traits_pool")) r.traitsPool = dmap(j, "traits_pool");
    if (j.contains("resist")) r.resist = dmap(j, "resist");
    if (j.contains("weak")) r.weak = dmap(j, "weak");
    if (j.contains("relations_bias")) r.relationsBias = dmap(j, "relations_bias");
    r.cultureDefault = j.value("culture_default", r.cultureDefault);
    r.settlementTag = j.value("settlement_tag", r.settlementTag);
    r.giftedChance = num(j, "gifted_chance", r.giftedChance);
    r.might = num(j, "might", r.might);
    if (j.contains("innate_spells")) r.innateSpells = j["innate_spells"];
    if (j.contains("hoard")) r.hoard = j["hoard"];
    if (j.contains("variants")) r.variants = j["variants"];
    if (j.contains("regen")) {
        r.regen.healthPerDay = num(j["regen"], "health_per_day", 0.0);
        r.regen.blockedBy = strs(j["regen"], "blocked_by_moodlets");
    }
    if (j.contains("sun_sleep") && j["sun_sleep"].is_object()) {
        r.sunSleep.on = true;
        r.sunSleep.lightAbove = num(j["sun_sleep"], "light_above", r.sunSleep.lightAbove);
        r.sunSleep.wakeBelow = num(j["sun_sleep"], "wake_below", r.sunSleep.wakeBelow);
        r.sunSleep.state = j["sun_sleep"].value("state", r.sunSleep.state);
    }
}

const RaceDef& builtinHuman() {
    static const RaceDef h = [] {
        RaceDef r;
        r.id = "human";
        return r;
    }();
    return h;
}

} // namespace

SpeciesCatalog parseSpecies(const std::vector<Json>& races, const std::vector<Json>& cultures, const Json& settle,
                            const Json& migrate, const Json& belts) {
    SpeciesCatalog c;
    c.loaded = !races.empty();
    // plain kinds first, blends after (a blend reads its parents' numbers, then its own fields)
    std::vector<const Json*> blends;
    // LR-166: a kind on a base reads the base's file with its own over it (chains allowed)
    std::map<std::string, const Json*> byId;
    for (const Json& j : races)
        if (!j.value("id", std::string()).empty() && !j.contains("blend")) byId[j.value("id", std::string())] = &j;
    std::function<Json(const std::string&, int)> resolved = [&](const std::string& id, int depth) -> Json {
        const Json& j = *byId.at(id);
        const std::string b = j.value("base", std::string());
        if (b.empty()) return j;
        if (depth > 8 || !byId.count(b) || b == id) {
            c.errors.push_back("species " + id + ": base '" + b + "' unknown or circular");
            return j;
        }
        Json parent = resolved(b, depth + 1);
        for (const char* k : {"_doc", "settles", "kin"}) parent.erase(k); // not inherited
        Json out = mergeOver(parent, j);
        out["base"] = b;
        return out;
    };
    for (const Json& j : races) {
        const std::string id = j.value("id", std::string());
        if (id.empty()) {
            c.errors.push_back("species: an entry without id");
            continue;
        }
        if (j.contains("blend")) {
            blends.push_back(&j);
            continue;
        }
        RaceDef r;
        r.id = id;
        readRace(j.contains("base") ? resolved(id, 0) : j, r);
        if (!c.races.emplace(id, r).second) c.errors.push_back("species: duplicate id " + id);
    }
    // the kin: written, else the base's (to the root), else the kind itself
    std::function<std::string(const std::string&, int)> kinFor = [&](const std::string& id, int depth) -> std::string {
        const RaceDef& r = c.races.at(id);
        if (!r.kin.empty()) return r.kin;
        if (!r.base.empty() && c.races.count(r.base) && depth < 8) return kinFor(r.base, depth + 1);
        return id;
    };
    for (auto& [id, r] : c.races) r.kin = kinFor(id, 0);
    for (const Json* jp : blends) {
        const Json& j = *jp;
        const std::string id = j.value("id", std::string());
        const auto parents = strs(j, "blend");
        if (parents.size() != 2 || !c.races.count(parents[0]) || !c.races.count(parents[1])) {
            c.errors.push_back("species " + id + ": blend must name two known kinds");
            continue;
        }
        RaceDef r = blendOf(c.races.at(parents[0]), c.races.at(parents[1]));
        r.id = id;
        r.blend = parents;
        readRace(j, r);
        if (r.kin.empty()) r.kin = id;
        if (!c.races.emplace(id, r).second) c.errors.push_back("species: duplicate id " + id);
    }
    for (const Json& j : cultures) {
        CultureDef d;
        d.id = j.value("id", std::string());
        d.species = j.value("species", std::string());
        const Json n = j.value("names", Json::object());
        d.namesM = strs(n, "m");
        d.namesF = strs(n, "f");
        d.family = strs(n, "family");
        d.speech = j.value("speech", std::string());
        d.buildStyle = j.value("build_style", std::string());
        d.teachAspects = strs(j, "teach_aspects");
        d.forbidAspects = strs(j, "forbid_aspects");
        d.norms = j.value("norms", Json::object());
        if (j.contains("peoples") && j["peoples"].is_object()) {
            const Json& pj = j["peoples"];
            // either one mix {kind: share} or mixes by settlement kind {village: {...}, default: {...}}
            bool flat = true;
            for (auto it = pj.begin(); it != pj.end(); ++it) flat = flat && it.value().is_number();
            if (flat) d.peoples["default"] = dmap(j, "peoples");
            else
                for (auto it = pj.begin(); it != pj.end(); ++it) d.peoples[it.key()] = dmap(pj, it.key().c_str());
        }
        d.welcome = dmap(j, "welcome");
        d.welcomeOther = num(j, "welcome_other", d.welcomeOther);
        d.belt = j.value("belt", std::string());
        d.placeNames = strs(j, "place_names");
        d.data = j;
        if (!d.id.empty()) c.cultures[d.id] = d;
    }
    c.defaultRace = settle.value("default_race", c.defaultRace);
    // LR-166: old ids to new
    auto smap = [](const Json& j, const char* k) {
        std::map<std::string, std::string> out;
        if (j.is_object() && j.contains(k) && j[k].is_object())
            for (auto it = j[k].begin(); it != j[k].end(); ++it)
                if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
        return out;
    };
    c.kinChildMother = settle.value("kin_child", std::string("father")) == "mother";
    c.migrateRace = smap(migrate, "races");
    c.migrateCulture = smap(migrate, "cultures");
    c.migrateFaith = smap(migrate, "faiths");
    c.raceByCulture = smap(migrate, "default_race_by_culture");
    // LR-166: the belts of the continent
    if (belts.is_object()) {
        c.beltKm = num(belts, "belt_km", c.beltKm);
        c.homeBelt = belts.value("home", std::string());
        const Json bl = belts.value("belts", Json::object());
        for (auto it = bl.begin(); it != bl.end(); ++it) {
            if (!it.value().is_object()) continue;
            const Json& b = it.value();
            SpeciesCatalog::Belt d;
            d.id = it.key();
            if (b.contains("at") && b["at"].is_array() && b["at"].size() >= 2)
                d.at = {b["at"][0].get<double>(), b["at"][1].get<double>()};
            d.radiusKm = num(b, "radius_km", d.radiusKm);
            d.mapCulture = b.value("map_culture", std::string());
            d.cultures = strs(b, "cultures");
            d.layout = b.value("layout", Json::array());
            d.data = b;
            c.belts[d.id] = d;
        }
        if (!c.belts.empty() && !c.belts.count(c.homeBelt)) c.homeBelt = c.belts.begin()->first;
    }
    // AL-136: a settlement's mix is either written out {kind: share} or names a culture:
    // {culture, kind?, peoples?} - the culture's mix for that kind of settlement, with
    // `peoples` (if any) over it.
    auto resolve = [&](const Json& e, const std::string& kind, std::string& culture) {
        std::map<std::string, double> mix;
        if (!e.is_object()) return mix;
        if (e.contains("culture") && e["culture"].is_string()) {
            culture = e["culture"].get<std::string>();
            if (!c.cultures.count(culture)) c.errors.push_back("settle.json: unknown culture '" + culture + "'");
            mix = culturePeoples(c, culture, e.value("kind", kind));
            for (const auto& [k, v] : dmap(e, "peoples")) mix[k] = v;
            for (auto it = mix.begin(); it != mix.end();) it = it->second <= 0.0 ? mix.erase(it) : std::next(it);
            return mix;
        }
        for (auto it = e.begin(); it != e.end(); ++it)
            if (it.value().is_number()) mix[it.key()] = it.value().get<double>();
        return mix;
    };
    if (settle.contains("map")) c.mapMix = resolve(settle["map"], "village", c.mapCulture);
    if (settle.contains("realm_templates") && settle["realm_templates"].is_object())
        for (auto it = settle["realm_templates"].begin(); it != settle["realm_templates"].end(); ++it) {
            std::string cu;
            c.templateMix[it.key()] = resolve(it.value(), it.key(), cu);
            if (!cu.empty()) c.templateCulture[it.key()] = cu;
        }
    return c;
}

std::map<std::string, double> culturePeoples(const SpeciesCatalog& s, const std::string& culture, const std::string& kind) {
    const auto cu = s.cultures.find(culture);
    if (cu == s.cultures.end()) return {};
    if (auto it = cu->second.peoples.find(kind); it != cu->second.peoples.end()) return it->second;
    if (auto it = cu->second.peoples.find("default"); it != cu->second.peoples.end()) return it->second;
    if (!cu->second.species.empty()) return {{cu->second.species, 1.0}};
    return {};
}

double cultureWelcome(const SpeciesCatalog& s, const std::string& culture, const std::string& race) {
    const auto cu = s.cultures.find(culture);
    if (cu == s.cultures.end()) return 1.0;
    const std::string r = race.empty() ? s.defaultRace : race;
    if (auto it = cu->second.welcome.find(r); it != cu->second.welcome.end()) return std::clamp(it->second, 0.0, 1.0);
    if (r == cu->second.species) return 1.0;
    // LR-166: a people of a kin the culture names (human for a hellene)
    const std::string k = kinOf(s, r);
    if (k != r)
        if (auto it = cu->second.welcome.find(k); it != cu->second.welcome.end()) return std::clamp(it->second, 0.0, 1.0);
    return std::clamp(cu->second.welcomeOther, 0.0, 1.0);
}

std::string kinOf(const SpeciesCatalog& s, const std::string& race) {
    const std::string r = race.empty() ? s.defaultRace : race;
    auto it = s.races.find(r);
    return it == s.races.end() || it->second.kin.empty() ? r : it->second.kin;
}

std::string cultureBelt(const SpeciesCatalog& s, const std::string& culture) {
    auto it = s.cultures.find(culture);
    return it == s.cultures.end() ? std::string() : it->second.belt;
}

SpeciesCatalog loadSpecies(const std::string& dir) {
    std::vector<std::string> errors;
    std::vector<Json> races, cultures;
    for (const auto& p : jsonFiles(dir)) {
        if (p.filename() == "settle.json" || p.filename() == "migrate.json") continue;
        Json j = readFile(p, errors);
        if (j.is_object()) races.push_back(std::move(j));
    }
    for (const auto& p : jsonFiles(fs::path(dir) / "cultures")) {
        Json j = readFile(p, errors);
        if (j.is_object()) cultures.push_back(std::move(j));
    }
    Json settle = readFile(fs::path(dir) / "settle.json", errors);
    if (!settle.is_object()) settle = Json::object();
    Json migrate = readFile(fs::path(dir) / "migrate.json", errors);
    if (!migrate.is_object()) migrate = Json::object();
    Json belts = readFile(fs::path(dir) / ".." / "world" / "belts.json", errors);
    if (!belts.is_object()) belts = Json::object();
    SpeciesCatalog c = parseSpecies(races, cultures, settle, migrate, belts);
    for (auto& e : errors) c.errors.push_back(e);
    return c;
}

std::vector<std::string> mergeCreatures(const std::string& dir, Json& species, Json& items, Json& interactions) {
    std::vector<std::string> errors;
    auto addAll = [&](Json& into, const char* key, const Json& from, const char* what) {
        if (!into.is_object()) into = Json::object();
        if (!into.contains(key)) into[key] = Json::array();
        for (const auto& e : from.value(key, Json::array())) {
            const std::string id = e.value("id", std::string());
            bool dup = false;
            for (const auto& o : into[key]) dup |= o.value("id", std::string()) == id;
            if (dup) {
                errors.push_back(std::string(what) + " '" + id + "' is there already");
                continue;
            }
            into[key].push_back(e);
        }
    };
    for (const auto& p : jsonFiles(dir)) {
        const std::string fn = p.filename().string();
        Json j = readFile(p, errors);
        if (!j.is_object()) continue;
        if (fn == "_items.json") addAll(items, "items", j, "item");
        else if (fn == "_interactions.json") addAll(interactions, "interactions", j, "interaction");
        else if (fn[0] != '_') {
            Json one = j;
            one["_source"] = fn;
            Json wrap = Json::object();
            wrap["species"] = Json::array({one});
            addAll(species, "species", wrap, "creature");
            // the hunt: a catalog act on the creature whose effects are its loot
            if (j.contains("hunt") && j["hunt"].is_object()) {
                const Json& h = j["hunt"];
                const std::string id = j.value("id", std::string());
                Json eff = Json::array();
                const Json loot = j.value("loot", Json::object());
                for (auto it = loot.begin(); it != loot.end(); ++it)
                    eff.push_back("has:" + it.key() + "+" + std::to_string(it.value().get<int>()));
                eff.push_back("t.group:heads-1");
                eff.push_back("skill:hunting+0.03");
                eff.push_back("body:fatigue+0.18");
                eff.push_back("memory:good_hunt");
                Json act = {{"id", "hunt_" + id},
                            {"category", "gather"},
                            {"target", "creature:" + id},
                            {"pre", h.value("pre", Json::array({"knows:hunting"}))},
                            {"eff", eff},
                            {"duration", h.value("duration", 8)},
                            {"cost", h.value("cost", 9)},
                            {"range_m", h.value("range_m", 40)},
                            {"on_fail", h.value("on_fail", Json::array({"t.moodlet:alarmed", "skill:hunting+0.01",
                                                                         "body:fatigue+0.15"}))},
                            {"_doc", "AL-114: generated from content/creatures/" + fn}};
                if (h.contains("chance")) act["chance"] = h["chance"];
                Json iw = Json::object();
                iw["interactions"] = Json::array({act});
                addAll(interactions, "interactions", iw, "interaction");
            }
        }
    }
    return errors;
}

void validateSpecies(const Content& c, ValidationReport& r) {
    const SpeciesCatalog& s = c.peoples;
    for (const auto& e : s.errors) r.errors.push_back(e);
    if (!s.loaded) return;
    if (!s.races.count(s.defaultRace)) r.errors.push_back("species: default race '" + s.defaultRace + "' is not defined");
    for (const auto& [id, d] : s.races) {
        if (d.adultYears <= 0 || d.elderYears <= d.adultYears || d.lifespanYears < d.elderYears)
            r.errors.push_back("species " + id + ": life years must be 0 < adult < elder <= lifespan");
        if (d.sapience < 0.0 || d.sapience > 1.0) r.errors.push_back("species " + id + ": sapience out of [0,1]");
        if (d.glyph.empty()) r.errors.push_back("species " + id + ": no glyph");
        for (const auto& [o, child] : d.interbreed) {
            if (!s.races.count(o)) r.errors.push_back("species " + id + ": interbreed with unknown kind '" + o + "'");
            if (!s.races.count(child)) r.errors.push_back("species " + id + ": interbreed child '" + child + "' unknown");
        }
        if (!d.cultureDefault.empty() && !s.cultures.count(d.cultureDefault))
            r.errors.push_back("species " + id + ": culture_default '" + d.cultureDefault + "' unknown");
        for (const auto& [a, m] : d.statMods)
            if (!c.expansion.attributes.empty() &&
                std::find(c.expansion.attributes.begin(), c.expansion.attributes.end(), a) == c.expansion.attributes.end())
                r.errors.push_back("species " + id + ": stats names unknown stat '" + a + "'");
        for (const auto& [n, m] : d.needMult)
            if (!c.need(n)) r.warnings.push_back("species " + id + ": needs names unknown need '" + n + "'");
        for (const auto& [t, m] : d.traitsPool)
            if (!c.traits.empty() && !c.traits.count(t))
                r.warnings.push_back("species " + id + ": traits_pool names unknown trait '" + t + "'");
        for (const auto& [o, b] : d.relationsBias)
            if (!s.races.count(o)) r.errors.push_back("species " + id + ": relations_bias unknown kind '" + o + "'");
    }
    for (const auto& [id, cu] : s.cultures)
        if (!cu.species.empty() && !s.races.count(cu.species))
            r.errors.push_back("culture " + id + ": unknown species '" + cu.species + "'");
    auto mix = [&](const std::string& where, const std::map<std::string, double>& m) {
        for (const auto& [k, v] : m)
            if (!s.races.count(k)) r.errors.push_back("settle.json " + where + ": unknown kind '" + k + "'");
            else if (!s.races.at(k).settles)
                r.errors.push_back("settle.json " + where + ": kind '" + k + "' is a base only (settles: false)");
    };
    // LR-166: regions, one-sex kinds, belts, the migration table
    for (const auto& [id, d] : s.races) {
        for (const auto& b : d.region)
            if (!s.belts.empty() && !s.belts.count(b)) r.errors.push_back("species " + id + ": region names unknown belt '" + b + "'");
        if (d.sexes != "" && d.sexes != "f" && d.sexes != "m" && d.sexes != "fm" && d.sexes != "mf")
            r.errors.push_back("species " + id + ": life.sexes must be f, m or empty");
        if (!d.otherSex.empty()) {
            const auto o = s.races.find(d.otherSex);
            if (o == s.races.end()) r.errors.push_back("species " + id + ": other_sex '" + d.otherSex + "' unknown");
            else if (!o->second.sexes.empty() && o->second.sexes == d.sexes)
                r.errors.push_back("species " + id + ": other_sex '" + d.otherSex + "' is of the same one sex");
        }
        if (d.sexes.size() == 1 && d.otherSex.empty())
            r.warnings.push_back("species " + id + ": a one-sex kind without other_sex");
        if (!d.kin.empty() && !s.races.count(d.kin)) r.errors.push_back("species " + id + ": kin '" + d.kin + "' unknown");
    }
    for (const auto& [id, cu] : s.cultures) {
        if (!s.belts.empty() && !cu.belt.empty() && !s.belts.count(cu.belt))
            r.errors.push_back("culture " + id + ": belt '" + cu.belt + "' unknown");
        // §1.1 (the user's rule above every culture): intimacy, marriage and courtship are for
        // adults only, and no custom of a culture may sexualise children
        if (cu.data.contains("intimacy")) {
            const Json& in = cu.data["intimacy"];
            if (!in.is_object() || !in.value("adults_only", false))
                r.errors.push_back("culture " + id + ": intimacy must have adults_only: true");
            else if (in.value("min_age_years", 0.0) < 14.0)
                r.errors.push_back("culture " + id + ": intimacy.min_age_years must be at least 14 (the human measure of adulthood)");
        }
        const std::string dump = cu.data.dump();
        for (const char* banned : {"pederast", "harem", "child_bride", "child_marriage", "concubine_girl"})
            if (dump.find(banned) != std::string::npos)
                r.errors.push_back("culture " + id + ": '" + banned + "' is excluded from the setting (adults only, §1.1)");
    }
    for (const auto& [bid, b] : s.belts) {
        if (!b.mapCulture.empty() && !s.cultures.count(b.mapCulture))
            r.errors.push_back("belts.json " + bid + ": map_culture '" + b.mapCulture + "' unknown");
        for (const auto& e : b.layout) {
            const std::string cu = e.value("culture", b.mapCulture);
            if (!s.cultures.count(cu)) r.errors.push_back("belts.json " + bid + ": layout culture '" + cu + "' unknown");
        }
        for (const auto& cu : b.cultures)
            if (!s.cultures.count(cu)) r.errors.push_back("belts.json " + bid + ": culture '" + cu + "' unknown");
    }
    if (!s.belts.empty() && !s.belts.count(s.homeBelt)) r.errors.push_back("belts.json: home belt unknown");
    for (const auto& [o, n] : s.migrateRace)
        if (!s.races.count(n)) r.errors.push_back("migrate.json races: '" + o + "' -> unknown kind '" + n + "'");
    for (const auto& [o, n] : s.raceByCulture)
        if (!s.races.count(n)) r.errors.push_back("migrate.json default_race_by_culture: '" + o + "' -> unknown kind '" + n + "'");
    for (const auto& [o, n] : s.migrateCulture)
        if (!s.cultures.count(n)) r.errors.push_back("migrate.json cultures: '" + o + "' -> unknown culture '" + n + "'");
    mix("map", s.mapMix);
    for (const auto& [id, cu] : s.cultures) {
        for (const auto& [kind, m] : cu.peoples) mix("culture " + id + " peoples." + kind, m);
        for (const auto& [k, v] : cu.welcome)
            if (!s.races.count(k)) r.errors.push_back("culture " + id + ": welcome names unknown kind '" + k + "'");
    }
    for (const auto& [t, m] : s.templateMix) mix(t, m);
    // monsters: their loot must be items
    for (const auto& [id, sp] : c.species)
        for (const auto& [item, n] : sp.loot)
            if (!c.items.count(item)) r.errors.push_back("creature " + id + ": loot item '" + item + "' unknown");
}

// --- one person -----------------------------------------------------------------------

const RaceDef* findRace(const Content& ct, const std::string& id) { return ct.raceOf(id); }

const RaceDef& raceOf(const Content& ct, const Character& c) {
    const RaceDef* r = ct.raceOf(c.race);
    return r ? *r : builtinHuman();
}

std::string raceId(const Content& ct, const Character& c) {
    return c.race.empty() ? ct.peoples.defaultRace : c.race;
}

bool raceAppliesTo(const Character& c) { return !c.isAnimal() && !c.hasTrait("undead"); }

int raceAgeDays(const RaceDef& r, double humanYears) {
    double y;
    if (humanYears <= 14.0) y = humanYears * r.adultYears / 14.0;
    else y = r.adultYears + (humanYears - 14.0) * (r.elderYears - r.adultYears) / 46.0;
    return std::max(0, static_cast<int>(std::lround(y * kDaysPerYear)));
}

void setRace(const Content& ct, Character& c, const std::string& race0) {
    // LR-166: a one-sex kind's person of the other sex is of its other_sex (a nymph's brother: a satyr)
    const std::string race = c.body.sex ? raceForSex(ct, race0, c.body.sex) : race0;
    const std::string id = race == ct.peoples.defaultRace ? std::string() : race;
    c.race = id;
    const RaceDef& r = raceOf(ct, c);
    c.body.adultYears = r.adultYears;
    c.body.elderYears = r.elderYears;
}

double raceNeedMult(const Content& ct, const Character& c, const std::string& need) {
    if (c.race.empty() && !ct.raceOf("")) return 1.0;
    const RaceDef& r = raceOf(ct, c);
    auto it = r.needMult.find(need);
    return it == r.needMult.end() ? 1.0 : it->second;
}

namespace {
const RaceAptitude* aptitude(const Content& ct, const Character& c, const std::string& skill) {
    const RaceDef& r = raceOf(ct, c);
    if (r.aptitudes.empty()) return nullptr;
    auto it = r.aptitudes.find(skill);
    if (it == r.aptitudes.end()) it = r.aptitudes.find("*");
    return it == r.aptitudes.end() ? nullptr : &it->second;
}
} // namespace

double raceLearnMult(const Content& ct, const Character& c, const std::string& skill) {
    const RaceAptitude* a = aptitude(ct, c, skill);
    return a ? a->learn : 1.0;
}
double raceCapMult(const Content& ct, const Character& c, const std::string& skill) {
    const RaceAptitude* a = aptitude(ct, c, skill);
    return a ? a->cap : 1.0;
}

double raceTraitWeight(const RaceDef& r, const std::string& trait) {
    auto it = r.traitsPool.find(trait);
    return it == r.traitsPool.end() ? 1.0 : it->second;
}

double raceConceiveMult(const Content& ct, const Character& c) { return raceOf(ct, c).fertility; }

std::string kinOf(const Content& ct, const std::string& race) { return kinOf(ct.peoples, race); }

std::string childRace(const Content& ct, const std::string& mother, const std::string& father) {
    const std::string m = mother.empty() ? ct.peoples.defaultRace : mother;
    const std::string f = father.empty() ? ct.peoples.defaultRace : father;
    if (m == f) return m;
    const RaceDef* rm = ct.raceOf(m);
    if (!rm) return m;
    if (auto it = rm->interbreed.find(f); it != rm->interbreed.end()) return it->second;
    // LR-166: two peoples of one kin: the father's (or the mother's) people
    const std::string km = kinOf(ct, m), kf = kinOf(ct, f);
    if (km == kf) return ct.peoples.kinChildMother ? m : f;
    // a kind written for the kin (elf x human) holds for each of its peoples
    if (auto it = rm->interbreed.find(kf); it != rm->interbreed.end()) return it->second;
    if (const RaceDef* rk = ct.raceOf(km); rk && km != m) {
        if (auto it = rk->interbreed.find(f); it != rk->interbreed.end()) return it->second;
        if (auto it = rk->interbreed.find(kf); it != rk->interbreed.end()) return it->second;
    }
    return std::string();
}

std::string raceForSex(const Content& ct, const std::string& race, char sex) {
    const RaceDef* r = ct.raceOf(race.empty() ? ct.peoples.defaultRace : race);
    if (!r || r->sexes.empty() || r->sexes.find(sex) != std::string::npos) return race;
    if (!r->otherSex.empty() && ct.raceOf(r->otherSex)) return r->otherSex;
    return race;
}

std::string childRace(const Content& ct, const std::string& mother, const std::string& father, char sex) {
    const std::string kind = childRace(ct, mother, father);
    if (kind.empty()) return kind;
    const RaceDef* r = ct.raceOf(kind);
    if (!r || r->sexes.empty() || r->sexes.find(sex) != std::string::npos) return kind;
    // a son of a nymph and a man is a man: the other parent's kind when it fits this sex
    for (const std::string& p : {father, mother}) {
        const std::string pk = p.empty() ? ct.peoples.defaultRace : p;
        if (pk == kind) continue;
        const RaceDef* pr = ct.raceOf(pk);
        if (pr && (pr->sexes.empty() || pr->sexes.find(sex) != std::string::npos)) return raceForSex(ct, pk, sex);
    }
    return raceForSex(ct, kind, sex);
}

std::string cultureName(const Content& ct, const std::string& race, Rng& rng, bool female, bool withFamily) {
    const RaceDef* r = ct.raceOf(race);
    if (!r) return {};
    auto cu = ct.peoples.cultures.find(r->cultureDefault);
    if (cu == ct.peoples.cultures.end()) return {};
    const auto& given = female ? cu->second.namesF : cu->second.namesM;
    if (given.empty()) return {};
    std::string name = given[static_cast<size_t>(rng.irange(0, static_cast<int>(given.size()) - 1))];
    if (withFamily && !cu->second.family.empty())
        name += " " + cu->second.family[static_cast<size_t>(rng.irange(0, static_cast<int>(cu->second.family.size()) - 1))];
    return name;
}

double raceBias(const Content& ct, const std::string& from, const std::string& to) {
    const std::string a = from.empty() ? ct.peoples.defaultRace : from;
    const std::string b = to.empty() ? ct.peoples.defaultRace : to;
    if (a == b) return 0.0;
    const RaceDef* r = ct.raceOf(a);
    if (!r) return 0.0;
    if (auto it = r->relationsBias.find(b); it != r->relationsBias.end()) return it->second;
    // LR-166: what the kind holds of a kin holds of its peoples (an elf of any human people)
    const std::string kb = kinOf(ct, b);
    if (kb == kinOf(ct, a)) return 0.0;
    if (auto it = r->relationsBias.find(kb); it != r->relationsBias.end()) return it->second;
    return 0.0;
}

std::vector<std::string> peoplesTags(const Content& ct, const std::map<std::string, int>& count, double major, double minor) {
    std::vector<std::string> out;
    int total = 0;
    for (const auto& [k, n] : count) total += n;
    if (total <= 0) return out;
    int big = 0;
    for (const auto& [k, n] : count) {
        const double share = static_cast<double>(n) / total;
        if (share >= minor) ++big;
        if (share >= major)
            if (const RaceDef* r = ct.raceOf(k); r && !r->settlementTag.empty()) out.push_back(r->settlementTag);
    }
    if (big >= 2) out.push_back("mixed");
    return out;
}

std::string drawRace(const std::map<std::string, double>& mix, Rng& rng, const std::string& fallback) {
    double total = 0.0;
    for (const auto& [k, v] : mix) total += std::max(0.0, v);
    if (total <= 0.0) return fallback;
    double x = rng.uniform() * total;
    for (const auto& [k, v] : mix) {
        x -= std::max(0.0, v);
        if (x <= 0.0) return k;
    }
    return mix.rbegin()->first;
}

void raceAtBirth(World& w, Character& c) {
    const Content& ct = w.content();
    setRace(ct, c, c.race);
    // the gift (trait gifted) is rolled by the magic core (AL-116) from raceOf(..).giftedChance
}

bool innateStrike(World& w, Character* holder, const Json& innateSpells, EntityId target, const Vec2* at,
                  const std::string& salt) {
    if (!innateSpells.is_array() || innateSpells.empty()) return false;
    Rng rng = w.rng(holder ? holder->id : target, "innate:" + salt);
    bool any = false;
    for (const auto& sj : innateSpells) {
        if (!rng.chance(sj.value("chance", 1.0))) continue;
        const Spell s = spellFromJson(sj);
        CastTarget t;
        t.who = target;
        if (at) t.hasPos = true, t.at = *at;
        const double power = sj.contains("mods") ? sj["mods"].value("power", 3.0) : 3.0;
        any |= castStored(w, holder, s, t, power).cast;
    }
    return any;
}

// --- trolls in the sun --------------------------------------------------------------------

bool petrified(const Character& c) { return c.body.petrified; }

void forcePetrify(World& w, Character& c) {
    if (c.body.petrified || !c.body.alive) return;
    c.body.petrified = true;
    c.body.downUntil = std::max(c.body.downUntil, w.now() + 1);
    w.chronicle("petrified", {c.id}, "chronicle.species.petrified", {{"who", c.id}});
}

void wakePetrified(World& w, Character& c) {
    if (!c.body.petrified) return;
    c.body.petrified = false;
    c.body.downUntil = std::min(c.body.downUntil, w.now());
    w.chronicle("woke_from_stone", {c.id}, "chronicle.species.woke", {{"who", c.id}});
}

double lightOn(const World& w, const Character& c, bool underRoof) {
    (void)c;
    if (underRoof) return 0.0;
    return w.isNight() ? 0.0 : 1.0;
}

bool raceBodyTick(World& w, Character& c, bool underRoof) {
    if (!raceAppliesTo(c)) return true;
    const Content& ct = w.content();
    const RaceDef* r = ct.raceOf(c.race);
    if (!r) return true;
    if (r->sunSleep.on) {
        const double light = lightOn(w, c, underRoof);
        if (!c.body.petrified && light > r->sunSleep.lightAbove) forcePetrify(w, c);
        else if (c.body.petrified && light < r->sunSleep.wakeBelow) wakePetrified(w, c);
    }
    if (c.body.petrified) {
        c.body.downUntil = std::max(c.body.downUntil, w.now() + 2); // stone: the mind does not run
        return false;
    }
    if (r->regen.healthPerDay > 0.0 && c.body.health < 1.0) {
        bool blocked = false;
        for (const auto& m : r->regen.blockedBy) blocked |= c.hasMoodlet(m, w.now());
        if (!blocked) c.body.health = std::min(1.0, c.body.health + r->regen.healthPerDay / kTicksPerDay);
    }
    return true;
}

// --- LR-166: old saves -------------------------------------------------------------

int migratePeoples(World& w) {
    const SpeciesCatalog& s = w.content().peoples;
    if (!s.loaded) return 0;
    int changed = 0;
    auto remap = [&](std::string& v, const std::map<std::string, std::string>& m) {
        if (v.empty()) return;
        if (auto it = m.find(v); it != m.end() && it->second != v) v = it->second, ++changed;
    };
    for (auto& [id, st] : w.settlements()) {
        remap(st.culture, s.migrateCulture);
        std::map<std::string, double> mix;
        bool moved = false;
        for (const auto& [k, v] : st.peoples) {
            std::string nk = k;
            if (auto it = s.migrateRace.find(k); it != s.migrateRace.end()) nk = it->second, moved = true;
            const RaceDef* r = w.content().raceOf(nk);
            if (r && !r->settles)
                if (auto bc = s.raceByCulture.find(st.culture); bc != s.raceByCulture.end()) nk = bc->second, moved = true;
            mix[nk] += v;
        }
        if (moved) st.peoples = mix, ++changed;
    }
    // the bare default kind (an old save's human, race "") does not live in the world any more:
    // a person becomes the people of their culture, else of their settlement's, else "*"
    const RaceDef* def = w.content().raceOf(s.defaultRace);
    const bool defaultSettles = !def || def->settles;
    auto person = [&](Character& c) {
        if (c.isAnimal()) return;
        remap(c.culture, s.migrateCulture);
        remap(c.faith, s.migrateFaith);
        std::string r = c.race;
        if (!r.empty())
            if (auto it = s.migrateRace.find(r); it != s.migrateRace.end()) r = it->second;
        const RaceDef* rd = w.content().raceOf(r.empty() ? s.defaultRace : r);
        if ((r.empty() && !defaultSettles) || (rd && !rd->settles)) {
            std::string cu = c.culture;
            if (cu.empty())
                if (auto st = w.settlements().find(c.status.settlement); st != w.settlements().end()) cu = st->second.culture;
            auto bc = s.raceByCulture.find(cu);
            if (bc == s.raceByCulture.end()) bc = s.raceByCulture.find("*");
            if (bc != s.raceByCulture.end()) r = bc->second;
        }
        if (r != c.race) {
            const int age = c.body.ageDays;
            setRace(w.content(), c, r);
            c.body.ageDays = age;
            ++changed;
        }
    };
    for (auto& [id, c] : w.characters()) person(c);
    for (auto& [id, c] : w.offstage()) person(c);
    return changed;
}

} // namespace lr::sim
