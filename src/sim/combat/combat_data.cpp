#include "sim/combat/combat_data.hpp"

#include <filesystem>
#include <fstream>
#include <set>

namespace lr::combat {

namespace fs = std::filesystem;

namespace {

std::string joinErrors(const std::vector<std::string>& e) {
    std::string s = "combat data: " + std::to_string(e.size()) + " error(s)";
    for (const auto& x : e) s += "\n  " + x;
    return s;
}

// Collects errors instead of throwing on the first one.
struct Ctx {
    std::vector<std::string> err;

    void fail(const std::string& m) { err.push_back(m); }

    const OJson* obj(const OJson& j, const std::string& key, const std::string& where) {
        if (!j.is_object() || !j.contains(key)) {
            fail(where + ": missing '" + key + "'");
            return nullptr;
        }
        const OJson& v = j[key];
        if (!v.is_object()) {
            fail(where + "." + key + ": must be an object");
            return nullptr;
        }
        return &v;
    }
    const OJson* arr(const OJson& j, const std::string& key, const std::string& where) {
        if (!j.is_object() || !j.contains(key)) {
            fail(where + ": missing '" + key + "'");
            return nullptr;
        }
        const OJson& v = j[key];
        if (!v.is_array()) {
            fail(where + "." + key + ": must be an array");
            return nullptr;
        }
        return &v;
    }
    double num(const OJson& j, const std::string& key, const std::string& where) {
        if (!j.is_object() || !j.contains(key)) {
            fail(where + ": missing '" + key + "'");
            return 0.0;
        }
        if (!j[key].is_number()) {
            fail(where + "." + key + ": must be a number");
            return 0.0;
        }
        return j[key].get<double>();
    }
    double numOr(const OJson& j, const std::string& key, double def, const std::string& where) {
        if (!j.is_object() || !j.contains(key)) return def;
        return num(j, key, where);
    }
    std::string str(const OJson& j, const std::string& key, const std::string& where) {
        if (!j.is_object() || !j.contains(key) || !j[key].is_string()) {
            fail(where + ": missing string '" + key + "'");
            return {};
        }
        return j[key].get<std::string>();
    }
    Span span(const OJson& j, const std::string& key, const std::string& where) {
        if (!j.is_object() || !j.contains(key)) {
            fail(where + ": missing '" + key + "'");
            return {};
        }
        return spanOf(j[key], where + "." + key);
    }
    Span spanOf(const OJson& v, const std::string& where) {
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
            fail(where + ": must be [lo, hi]");
            return {};
        }
        Span s{v[0].get<double>(), v[1].get<double>()};
        if (s.hi < s.lo) fail(where + ": hi < lo");
        return s;
    }
    std::vector<std::string> strings(const OJson& j, const std::string& key, const std::string& where) {
        std::vector<std::string> out;
        const OJson* a = arr(j, key, where);
        if (!a) return out;
        for (const auto& x : *a) {
            if (!x.is_string()) {
                fail(where + "." + key + ": must hold strings");
                continue;
            }
            out.push_back(x.get<std::string>());
        }
        return out;
    }
    // id -> index; an unknown id is an error
    int ref(const std::map<std::string, int>& m, const std::string& id, const std::string& what,
            const std::string& where) {
        auto it = m.find(id);
        if (it == m.end()) {
            fail(where + ": unknown " + what + " '" + id + "'");
            return -1;
        }
        return it->second;
    }
    // Object keyed by ids of one kind (zones, weapons, ...). "_doc" is skipped.
    template <class F>
    void keyed(const OJson& j, const std::map<std::string, int>& m, const std::string& what,
               const std::string& where, F&& f) {
        if (!j.is_object()) {
            fail(where + ": must be an object");
            return;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "_doc") continue;
            const int ix = ref(m, it.key(), what, where);
            if (ix >= 0) f(ix, it.value(), where + "." + it.key());
        }
    }
    // Rejects keys outside an allowed set (typos would otherwise be silent zeros).
    void only(const OJson& j, std::initializer_list<const char*> keys, const std::string& where) {
        if (!j.is_object()) return;
        std::set<std::string> ok{"_doc"};
        for (const char* k : keys) ok.insert(k);
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!ok.count(it.key())) fail(where + ": unknown key '" + it.key() + "'");
    }
};

std::map<std::string, int> index(const std::vector<std::string>& ids, const std::string& what, Ctx& c) {
    std::map<std::string, int> m;
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (!m.emplace(ids[i], static_cast<int>(i)).second) c.fail(what + ": duplicate id '" + ids[i] + "'");
    return m;
}

const OJson& file(const std::map<std::string, OJson>& files, const std::string& name, Ctx& c) {
    static const OJson empty = OJson::object();
    auto it = files.find(name);
    if (it == files.end()) {
        c.fail("missing file " + name + ".json");
        return empty;
    }
    return it->second;
}

} // namespace

DataError::DataError(std::vector<std::string> e) : std::runtime_error(joinErrors(e)), errors(std::move(e)) {}

int CombatData::find(const std::map<std::string, int>& m, const std::string& id, const char* what) {
    auto it = m.find(id);
    if (it == m.end()) throw std::out_of_range(std::string("combat: unknown ") + what + " '" + id + "'");
    return it->second;
}

std::map<std::string, OJson> CombatData::readFiles(const std::string& dir) {
    std::map<std::string, OJson> files;
    for (const char* n : {"combat_config", "weapons", "armor", "shields", "wounds", "clinch_moves",
                          "ranged_weapons", "wear", "animations"}) {
        const fs::path p = fs::path(dir) / (std::string(n) + ".json");
        std::ifstream f(p, std::ios::binary);
        if (!f) continue; // reported as a missing file by fromJson
        try {
            files[n] = OJson::parse(f);
        } catch (const std::exception& e) {
            throw DataError({p.string() + ": " + e.what()});
        }
    }
    return files;
}

CombatData CombatData::load(const std::string& dir) { return fromJson(readFiles(dir)); }

namespace {
CombatData build(const std::map<std::string, OJson>& files);
}

CombatData CombatData::fromJson(const std::map<std::string, OJson>& files) {
    try {
        return build(files);
    } catch (const nlohmann::json::exception& e) {
        throw DataError({std::string("combat data: wrong value type: ") + e.what()});
    }
}

namespace {
CombatData build(const std::map<std::string, OJson>& files) {
    Ctx c;
    CombatData d;
    const OJson& cfg = file(files, "combat_config", c);
    const OJson& wpn = file(files, "weapons", c);
    const OJson& arm = file(files, "armor", c);
    const OJson& shd = file(files, "shields", c);
    const OJson& wnd = file(files, "wounds", c);
    const OJson& cln = file(files, "clinch_moves", c);
    const OJson& rng = file(files, "ranged_weapons", c);
    const OJson& wer = file(files, "wear", c);
    d.animations = files.count("animations") ? files.at("animations") : OJson::object();
    if (!c.err.empty()) throw DataError(c.err);

    // --- id lists ---------------------------------------------------------
    const std::string C = "combat_config";
    d.zones = c.strings(cfg, "zones", C);
    d.directions = c.strings(cfg, "directions", C);
    d.guards = c.strings(cfg, "guards", C);
    const OJson* core = c.obj(cfg, "core", C);
    const OJson coreJ = core ? *core : OJson::object();
    const std::string K = C + ".core";
    d.dtypes = c.strings(coreJ, "damage_types", K);
    d.zoneIx = index(d.zones, "zones", c);
    d.dirIx = index(d.directions, "directions", c);
    d.guardIx = index(d.guards, "guards", c);
    d.dtypeIx = index(d.dtypes, "damage_types", c);
    const std::size_t NZ = d.zones.size(), ND = d.directions.size(), NT = d.dtypes.size();

    for (auto it = wpn.begin(); it != wpn.end(); ++it)
        if (it.key() != "_doc") d.weapons.push_back({it.key()});
    for (auto it = arm.begin(); it != arm.end(); ++it)
        if (it.key() != "_doc" && it.key() != "k_impact") d.armors.push_back({it.key()});
    for (auto it = shd.begin(); it != shd.end(); ++it)
        if (it.key() != "_doc") d.shields.push_back({it.key()});
    std::vector<std::string> ids;
    for (const auto& w : d.weapons) ids.push_back(w.id);
    d.weaponIx = index(ids, "weapons", c);
    ids.clear();
    for (const auto& a : d.armors) ids.push_back(a.id);
    d.armorIx = index(ids, "armor", c);
    ids.clear();
    for (const auto& s : d.shields) ids.push_back(s.id);
    d.shieldIx = index(ids, "shields", c);
    const std::size_t NW = d.weapons.size();

    // --- wounds: zones and severities ------------------------------------
    const std::string WN = "wounds";
    c.only(wnd, {"zone_hp", "severity", "disable", "heal", "treatment"}, WN);
    d.zoneHp.assign(NZ, 0.0);
    std::vector<char> hpSet(NZ, 0);
    if (const OJson* z = c.obj(wnd, "zone_hp", WN))
        c.keyed(*z, d.zoneIx, "zone", WN + ".zone_hp", [&](int i, const OJson& v, const std::string& w) {
            if (!v.is_number() || v.get<double>() <= 0) c.fail(w + ": must be a positive number");
            else d.zoneHp[i] = v.get<double>(), hpSet[i] = 1;
        });
    for (std::size_t i = 0; i < NZ; ++i)
        if (!hpSet[i]) c.fail(WN + ".zone_hp: no hp for zone '" + d.zones[i] + "'");
    d.disable.assign(NZ, {});
    if (const OJson* z = c.obj(wnd, "disable", WN))
        c.keyed(*z, d.zoneIx, "zone", WN + ".disable", [&](int i, const OJson& v, const std::string& w) {
            c.only(v, {"attack_mult", "guard_mult", "speed_mult", "ko", "death_at_zone_hp"}, w);
            ZoneDisable& zd = d.disable[i];
            zd.attackMult = c.numOr(v, "attack_mult", 1.0, w);
            zd.guardMult = c.numOr(v, "guard_mult", 1.0, w);
            zd.speedMult = c.numOr(v, "speed_mult", 1.0, w);
            zd.ko = v.value("ko", false);
            zd.deathAtZero = v.value("death_at_zone_hp", false);
            if (zd.guardMult <= 0) c.fail(w + ".guard_mult: must be > 0");
        });
    const OJson* bleedCfg = c.obj(cfg, "bleed", C);
    if (const OJson* sv = c.arr(wnd, "severity", WN)) {
        double prev = -1.0;
        for (const auto& s : *sv) {
            const std::string w = WN + ".severity[" + std::to_string(d.severity.size()) + "]";
            c.only(s, {"id", "min_frac", "pain", "bleed", "disable_zone"}, w);
            SeverityDef sd;
            sd.id = c.str(s, "id", w);
            sd.minFrac = c.num(s, "min_frac", w);
            sd.pain = c.num(s, "pain", w);
            sd.disableZone = s.value("disable_zone", false);
            if (sd.minFrac < prev) c.fail(w + ": min_frac must not decrease");
            prev = sd.minFrac;
            double rate = 0.0;
            if (s.contains("bleed") && s["bleed"].is_string()) {
                const std::string b = s["bleed"].get<std::string>();
                if (!bleedCfg || !bleedCfg->contains(b) || !(*bleedCfg)[b].is_number())
                    c.fail(w + ": unknown bleed rate '" + b + "' (combat_config.bleed)");
                else rate = (*bleedCfg)[b].get<double>();
            } else if (s.contains("bleed") && s["bleed"].is_number() && s["bleed"].get<double>() != 0.0) {
                c.fail(w + ": bleed must be 0 or a key of combat_config.bleed");
            }
            d.bleedRate.push_back(rate);
            d.severity.push_back(sd);
        }
        if (d.severity.empty() || d.severity[0].minFrac > 0.0)
            c.fail(WN + ".severity: the first entry must start at min_frac 0");
    }
    ids.clear();
    for (const auto& s : d.severity) ids.push_back(s.id);
    d.sevIx = index(ids, "severity", c);

    // --- combat_config ----------------------------------------------------
    c.only(cfg, {"round_seconds", "zones", "zone_weight", "directions", "direction_counter", "guards",
                 "direction_zone_bias", "skill_range", "read", "hit", "stamina", "morale", "knockout", "bleed",
                 "blood_max", "aim_bias_exp", "reach", "riposte", "group", "aim_sharpness", "core"},
           C);
    d.roundSeconds = c.num(cfg, "round_seconds", C);
    d.zoneWeight.assign(NZ, 0.0);
    if (const OJson* z = c.obj(cfg, "zone_weight", C))
        c.keyed(*z, d.zoneIx, "zone", C + ".zone_weight",
                [&](int i, const OJson& v, const std::string&) { d.zoneWeight[i] = v.get<double>(); });
    d.counter.assign(ND, {-1, -1});
    if (const OJson* dc = c.obj(cfg, "direction_counter", C)) {
        c.keyed(*dc, d.dirIx, "direction", C + ".direction_counter", [&](int i, const OJson& v, const std::string& w) {
            if (!v.is_array() || v.size() != 2) return c.fail(w + ": must be [perfect, near]");
            for (int k = 0; k < 2; ++k)
                d.counter[i][k] = v[k].is_string() ? c.ref(d.guardIx, v[k].get<std::string>(), "guard", w) : -1;
        });
        for (std::size_t i = 0; i < ND; ++i)
            if (d.counter[i][0] < 0) c.fail(C + ".direction_counter: no guard for '" + d.directions[i] + "'");
    }
    d.dirZoneBias.assign(ND, std::vector<double>(NZ, 0.0));
    if (const OJson* b = c.obj(cfg, "direction_zone_bias", C)) {
        std::vector<char> seen(ND, 0);
        c.keyed(*b, d.dirIx, "direction", C + ".direction_zone_bias", [&](int i, const OJson& v, const std::string& w) {
            seen[i] = 1;
            c.keyed(v, d.zoneIx, "zone", w, [&](int z, const OJson& x, const std::string&) {
                d.dirZoneBias[i][z] = x.get<double>();
            });
        });
        for (std::size_t i = 0; i < ND; ++i)
            if (!seen[i]) c.fail(C + ".direction_zone_bias: no bias for '" + d.directions[i] + "'");
    }
    if (const OJson* r = c.obj(cfg, "read", C)) {
        const std::string w = C + ".read";
        d.read = {c.num(*r, "base", w), c.num(*r, "k_skill", w), c.num(*r, "k_know", w), c.num(*r, "k_fatigue", w),
                  c.num(*r, "k_panic", w), c.num(*r, "min", w), c.num(*r, "max", w)};
    }
    if (const OJson* h = c.obj(cfg, "hit", C)) {
        const std::string w = C + ".hit";
        d.hit = {c.num(*h, "base", w), c.num(*h, "k_skill_diff", w), c.num(*h, "perfect_guard_mult", w),
                 c.num(*h, "near_guard_mult", w), c.num(*h, "no_guard_mult", w), c.num(*h, "min", w),
                 c.num(*h, "max", w)};
    }
    if (const OJson* s = c.obj(cfg, "stamina", C)) {
        d.staminaMax = c.num(*s, "max", C + ".stamina");
        d.staminaRegen = c.num(*s, "regen_per_round", C + ".stamina");
    }
    if (const OJson* m = c.obj(cfg, "morale", C)) {
        const std::string w = C + ".morale";
        d.morale = {c.num(*m, "start", w), c.num(*m, "flee_below", w), c.num(*m, "yield_below", w),
                    c.num(*m, "loss_per_hit", w), c.num(*m, "loss_per_wound_heavy", w)};
        if (d.morale.start <= 0) c.fail(w + ".start: must be > 0");
    }
    if (const OJson* k = c.obj(cfg, "knockout", C)) {
        d.painThreshold = c.num(*k, "pain_threshold", C + ".knockout");
        d.koZoneMult = c.num(*k, "head_hit_ko_mult", C + ".knockout");
    }
    d.bloodMax = c.num(cfg, "blood_max", C);
    d.aimBiasExp = c.num(cfg, "aim_bias_exp", C);
    d.aimSharpness = c.num(cfg, "aim_sharpness", C);
    if (const OJson* r = c.obj(cfg, "reach", C)) {
        const std::string w = C + ".reach";
        d.reach = {c.num(*r, "hit_bonus_per_level", w), c.num(*r, "close_base", w), c.num(*r, "close_k_skill", w),
                   c.num(*r, "clinch_bonus_per_level", w), c.num(*r, "clinch_long_penalty_per_level", w)};
    }
    if (const OJson* r = c.obj(cfg, "riposte", C)) {
        const std::string w = C + ".riposte";
        d.riposte = {c.num(*r, "base", w), c.num(*r, "k_skill", w), c.num(*r, "hit_bonus", w)};
    }
    if (const OJson* g = c.obj(cfg, "group", C)) {
        const std::string w = C + ".group";
        d.group = {c.num(*g, "flank_bonus_per_extra", w), c.num(*g, "flank_cap", w),
                   c.num(*g, "morale_loss_ally_down", w), c.num(*g, "polearm_rank_bonus", w),
                   c.num(*g, "polearm_rank_cap", w)};
    }
    {
        c.only(coreJ, {"damage_types", "impact_only_types", "unarmed_weapon", "default_know",
                       "indestructible_durability", "attack_armor_stamina_mult", "regen_armor_stamina_mult",
                       "clinch_armor_stamina_mult", "damage_fatigue_floor", "exhausted_below", "wavering_margin",
                       "bleed_per_round_mult", "pain_scale_severity", "heavy_severities", "ko_severity",
                       "aim_floor_weight", "pierce_immune_pen", "clinch_chance", "first_mover_chance",
                       "duel_max_rounds", "battle_max_rounds", "rank_weapons"},
               K);
        auto& k = d.core;
        k.defaultKnow = c.num(coreJ, "default_know", K);
        k.indestructible = c.num(coreJ, "indestructible_durability", K);
        k.attackArmorStamina = c.num(coreJ, "attack_armor_stamina_mult", K);
        k.regenArmorStamina = c.num(coreJ, "regen_armor_stamina_mult", K);
        k.clinchArmorStamina = c.num(coreJ, "clinch_armor_stamina_mult", K);
        k.fatigueFloor = c.num(coreJ, "damage_fatigue_floor", K);
        k.exhaustedBelow = c.num(coreJ, "exhausted_below", K);
        k.waveringMargin = c.num(coreJ, "wavering_margin", K);
        k.bleedPerRound = c.num(coreJ, "bleed_per_round_mult", K);
        k.aimFloor = c.num(coreJ, "aim_floor_weight", K);
        k.pierceImmunePen = c.num(coreJ, "pierce_immune_pen", K);
        const Span cc = c.span(coreJ, "clinch_chance", K);
        k.clinchMin = cc.lo;
        k.clinchMax = cc.hi;
        k.firstMover = c.num(coreJ, "first_mover_chance", K);
        k.duelMaxRounds = static_cast<int>(c.num(coreJ, "duel_max_rounds", K));
        k.battleMaxRounds = static_cast<int>(c.num(coreJ, "battle_max_rounds", K));
        k.unarmed = c.ref(d.weaponIx, c.str(coreJ, "unarmed_weapon", K), "weapon", K + ".unarmed_weapon");
        k.painScaleSeverity = c.ref(d.sevIx, c.str(coreJ, "pain_scale_severity", K), "severity", K + ".pain_scale_severity");
        if (k.painScaleSeverity >= 0 && d.severity[k.painScaleSeverity].minFrac <= 0.0)
            c.fail(K + ".pain_scale_severity: needs min_frac > 0");
        for (const auto& s : c.strings(coreJ, "heavy_severities", K)) {
            const int i = c.ref(d.sevIx, s, "severity", K + ".heavy_severities");
            if (i >= 0) d.severity[i].heavy = true;
        }
        {
            const int i = c.ref(d.sevIx, c.str(coreJ, "ko_severity", K), "severity", K + ".ko_severity");
            if (i >= 0) d.severity[i].ko = true;
        }
        d.impactOnly.assign(NT, 0);
        for (const auto& s : c.strings(coreJ, "impact_only_types", K)) {
            const int i = c.ref(d.dtypeIx, s, "damage type", K + ".impact_only_types");
            if (i >= 0) d.impactOnly[i] = 1;
        }
        d.rankWeapon.assign(NW, 0);
        for (const auto& s : c.strings(coreJ, "rank_weapons", K)) {
            const int i = c.ref(d.weaponIx, s, "weapon", K + ".rank_weapons");
            if (i >= 0) d.rankWeapon[i] = 1;
        }
    }

    // --- weapons ----------------------------------------------------------
    for (auto& w : d.weapons) {
        const OJson& j = wpn[w.id];
        const std::string where = "weapons." + w.id;
        c.only(j, {"reach", "weight", "stamina", "types", "dir_type", "speed", "durability"}, where);
        w.reach = static_cast<int>(c.num(j, "reach", where));
        w.weight = c.num(j, "weight", where);
        w.stamina = c.num(j, "stamina", where);
        w.speed = c.num(j, "speed", where);
        w.durability = c.numOr(j, "durability", d.core.indestructible, where);
        w.indestructible = w.durability >= d.core.indestructible;
        if (w.durability <= 0) c.fail(where + ".durability: must be > 0");
        w.hasType.assign(NT, 0);
        w.types.assign(NT, {});
        if (const OJson* t = c.obj(j, "types", where))
            c.keyed(*t, d.dtypeIx, "damage type", where + ".types", [&](int i, const OJson& v, const std::string& ww) {
                w.types[i] = c.spanOf(v, ww);
                w.hasType[i] = 1;
                if (w.firstType < 0) w.firstType = i;
            });
        w.dirType.assign(ND, -1);
        if (const OJson* dt = c.obj(j, "dir_type", where))
            c.keyed(*dt, d.dirIx, "direction", where + ".dir_type", [&](int i, const OJson& v, const std::string& ww) {
                if (!v.is_string()) return c.fail(ww + ": must be a damage type");
                const int t = c.ref(d.dtypeIx, v.get<std::string>(), "damage type", ww);
                if (t >= 0 && !w.hasType[t]) c.fail(ww + ": weapon has no damage of type '" + v.get<std::string>() + "'");
                w.dirType[i] = t;
                w.dirs.push_back(i);
            });
        if (w.dirs.empty()) c.fail(where + ": no directions");
    }

    // --- armour -----------------------------------------------------------
    const std::string A = "armor";
    d.kImpact.assign(NT, 0.0);
    if (const OJson* k = c.obj(arm, "k_impact", A))
        c.keyed(*k, d.dtypeIx, "damage type", A + ".k_impact",
                [&](int i, const OJson& v, const std::string&) { d.kImpact[i] = v.get<double>(); });
    for (std::size_t t = 0; t < NT; ++t)
        if (!d.impactOnly[t] && (!arm.contains("k_impact") || !arm["k_impact"].contains(d.dtypes[t])))
            c.fail(A + ".k_impact: no value for '" + d.dtypes[t] + "'");
    const OJson* wearArmorDur = nullptr;
    if (const OJson* wa = c.obj(wer, "armor", "wear")) wearArmorDur = c.obj(*wa, "durability", "wear.armor");
    double defaultDur = 0.0;
    if (wearArmorDur) defaultDur = c.num(*wearArmorDur, "default", "wear.armor.durability");
    for (auto& a : d.armors) {
        const OJson& j = arm[a.id];
        const std::string where = A + "." + a.id;
        c.only(j, {"cover", "pen", "trans", "stamina_pen", "gap"}, where);
        a.cover.assign(NZ, 0);
        for (const auto& z : c.strings(j, "cover", where)) {
            const int i = c.ref(d.zoneIx, z, "zone", where + ".cover");
            if (i >= 0) a.cover[i] = 1;
        }
        a.pen.assign(NT, 0.0);
        a.trans.assign(NT, 1.0);
        std::vector<char> penSet(NT, 0), trSet(NT, 0);
        if (const OJson* p = c.obj(j, "pen", where))
            c.keyed(*p, d.dtypeIx, "damage type", where + ".pen", [&](int i, const OJson& v, const std::string&) {
                a.pen[i] = v.get<double>();
                penSet[i] = 1;
            });
        if (const OJson* t = c.obj(j, "trans", where))
            c.keyed(*t, d.dtypeIx, "damage type", where + ".trans", [&](int i, const OJson& v, const std::string&) {
                a.trans[i] = v.get<double>();
                trSet[i] = 1;
            });
        for (std::size_t t = 0; t < NT; ++t) {
            if (!trSet[t]) c.fail(where + ".trans: no value for '" + d.dtypes[t] + "'");
            if (!d.impactOnly[t] && !penSet[t]) c.fail(where + ".pen: no value for '" + d.dtypes[t] + "'");
        }
        a.staminaPen = c.num(j, "stamina_pen", where);
        a.hasGap = j.contains("gap");
        a.gap = c.numOr(j, "gap", 0.0, where);
        a.durability = defaultDur;
        if (wearArmorDur && wearArmorDur->contains(a.id)) a.durability = c.num(*wearArmorDur, a.id, "wear.armor.durability");
    }
    if (wearArmorDur)
        for (auto it = wearArmorDur->begin(); it != wearArmorDur->end(); ++it)
            if (it.key() != "default" && it.key() != "_doc" && !d.armorIx.count(it.key()))
                c.fail("wear.armor.durability: unknown armor '" + it.key() + "'");

    // --- shields ----------------------------------------------------------
    for (auto& s : d.shields) {
        const OJson& j = shd[s.id];
        const std::string where = "shields." + s.id;
        c.only(j, {"guard_mult", "stamina", "weight"}, where);
        s.guardMult = c.num(j, "guard_mult", where);
        s.stamina = c.num(j, "stamina", where);
        s.weight = c.numOr(j, "weight", 0.0, where);
    }

    // --- wear -------------------------------------------------------------
    if (const OJson* ww = c.obj(wer, "weapon", "wear")) {
        const std::string w = "wear.weapon";
        d.wear.perAttack = c.num(*ww, "per_attack", w);
        d.wear.perParry = c.num(*ww, "per_parry", w);
        d.wear.vsHardArmor = c.num(*ww, "vs_hard_armor", w);
        d.wear.hardArmorPen = c.num(*ww, "hard_armor_pen", w);
        d.wear.minDmgMult = c.num(*ww, "min_dmg_mult", w);
    }
    if (const OJson* wa = c.obj(wer, "armor", "wear")) {
        d.wear.armorPerHit = c.num(*wa, "per_hit", "wear.armor");
        d.wear.armorMinEff = c.num(*wa, "min_eff", "wear.armor");
    }

    // --- clinch -----------------------------------------------------------
    const std::string CL = "clinch_moves";
    c.only(cln, {"share", "down_rounds", "armor_weight_factor", "moves"}, CL);
    if (const OJson* sh = c.obj(cln, "share", CL)) {
        const double def = c.num(*sh, "default", CL + ".share");
        for (auto& w : d.weapons) w.clinchShare = def;
        for (auto it = sh->begin(); it != sh->end(); ++it) {
            if (it.key() == "_doc" || it.key() == "default") continue;
            const int i = c.ref(d.weaponIx, it.key(), "weapon", CL + ".share");
            if (i >= 0) d.weapons[i].clinchShare = it.value().get<double>();
        }
    }
    d.downRounds = static_cast<int>(c.num(cln, "down_rounds", CL));
    d.armorWeightFactor = c.num(cln, "armor_weight_factor", CL);
    if (const OJson* mv = c.obj(cln, "moves", CL)) {
        static const std::map<std::string, MoveKind> kinds = {{"grapple_throw", MoveKind::GrappleThrow},
                                                               {"gap_stab", MoveKind::GapStab},
                                                               {"pommel_strike", MoveKind::PommelStrike},
                                                               {"disarm", MoveKind::Disarm}};
        for (auto it = mv->begin(); it != mv->end(); ++it) {
            if (it.key() == "_doc") continue;
            const std::string where = CL + ".moves." + it.key();
            auto k = kinds.find(it.key());
            if (k == kinds.end()) {
                c.fail(where + ": unknown clinch move (known: grapple_throw, gap_stab, pommel_strike, disarm)");
                continue;
            }
            const OJson& j = it.value();
            ClinchMove m;
            m.id = it.key();
            m.kind = k->second;
            m.weaponWeight.assign(NW, 0.0);
            m.baseByWeapon.assign(NW, -1.0);
            if (const OJson* ws = c.obj(j, "weapons", where))
                c.keyed(*ws, d.weaponIx, "weapon", where + ".weapons",
                        [&](int i, const OJson& v, const std::string&) { m.weaponWeight[i] = v.get<double>(); });
            m.base = c.num(j, "base", where);
            m.kSkillDiff = c.num(j, "k_skill_diff", where);
            m.stamCost = c.num(j, "stam_cost", where);
            m.downWeightMult = c.numOr(j, "down_weight_mult", 1.0, where);
            if (j.contains("base_by_weapon"))
                c.keyed(j["base_by_weapon"], d.weaponIx, "weapon", where + ".base_by_weapon",
                        [&](int i, const OJson& v, const std::string&) { m.baseByWeapon[i] = v.get<double>(); });
            m.zoneWeight.assign(NZ, 0.0);
            auto zonesOf = [&] {
                if (const OJson* z = c.obj(j, "zones", where))
                    c.keyed(*z, d.zoneIx, "zone", where + ".zones",
                            [&](int i, const OJson& v, const std::string&) { m.zoneWeight[i] = v.get<double>(); });
            };
            auto dtypeOf = [&] { m.dtype = c.ref(d.dtypeIx, c.str(j, "dtype", where), "damage type", where + ".dtype"); };
            switch (m.kind) {
            case MoveKind::GrappleThrow:
                c.only(j, {"weapons", "base", "k_skill_diff", "armor_resist_per_pen", "stam_cost", "target_stam_loss",
                           "fall_dmg", "effect", "fall_zone", "down_weight_mult"},
                       where);
                m.armorResistPerPen = c.num(j, "armor_resist_per_pen", where);
                m.targetStamLoss = c.num(j, "target_stam_loss", where);
                m.fallDmg = c.span(j, "fall_dmg", where);
                m.fallZone = c.ref(d.zoneIx, c.str(j, "fall_zone", where), "zone", where + ".fall_zone");
                break;
            case MoveKind::GapStab:
                c.only(j, {"weapons", "base", "k_skill_diff", "down_bonus", "stam_cost", "dtype", "armor_weight_mult",
                           "down_weight_mult", "zones", "base_by_weapon", "force"},
                       where);
                m.downBonus = c.num(j, "down_bonus", where);
                m.armorWeightMult = c.num(j, "armor_weight_mult", where);
                m.force = c.numOr(j, "force", 1.0, where);
                dtypeOf();
                zonesOf();
                break;
            case MoveKind::PommelStrike:
                c.only(j, {"weapons", "base", "k_skill_diff", "stam_cost", "dmg", "dtype", "absorb_mult",
                           "down_weight_mult", "zones"},
                       where);
                m.dmg = c.span(j, "dmg", where);
                m.absorbMult = c.num(j, "absorb_mult", where);
                dtypeOf();
                zonesOf();
                break;
            case MoveKind::Disarm:
                c.only(j, {"weapons", "base", "k_skill_diff", "stam_cost", "down_weight_mult"}, where);
                break;
            }
            d.moves.push_back(std::move(m));
        }
    }

    // --- ranged -----------------------------------------------------------
    const std::string R = "ranged_weapons";
    c.only(rng, {"close_speed_m_per_round", "armor_speed_pen_per_pen", "melee_range_m", "moving_penalty",
                 "shield_extra", "weapons", "aim_direction", "min_close_speed_m_per_round", "hit_chance"},
           R);
    {
        auto& r = d.rangedCfg;
        r.closeSpeed = c.num(rng, "close_speed_m_per_round", R);
        r.armorSpeedPen = c.num(rng, "armor_speed_pen_per_pen", R);
        r.meleeRange = c.num(rng, "melee_range_m", R);
        r.movingPenalty = c.num(rng, "moving_penalty", R);
        r.shieldExtra = c.num(rng, "shield_extra", R);
        r.minSpeed = c.num(rng, "min_close_speed_m_per_round", R);
        const Span hc = c.span(rng, "hit_chance", R);
        r.hitMin = hc.lo;
        r.hitMax = hc.hi;
        r.aimDirection = c.ref(d.dirIx, c.str(rng, "aim_direction", R), "direction", R + ".aim_direction");
        if (r.minSpeed <= 0) c.fail(R + ".min_close_speed_m_per_round: must be > 0");
    }
    if (const OJson* ws = c.obj(rng, "weapons", R)) {
        for (auto it = ws->begin(); it != ws->end(); ++it) {
            if (it.key() == "_doc") continue;
            const OJson& j = it.value();
            const std::string where = R + ".weapons." + it.key();
            c.only(j, {"range", "dtype", "dmg", "reload", "ammo", "base_hit", "dist_k", "skill_k", "pierce", "stam"},
                   where);
            RangedDef r;
            r.id = it.key();
            r.range = c.num(j, "range", where);
            r.dtype = c.ref(d.dtypeIx, c.str(j, "dtype", where), "damage type", where + ".dtype");
            r.dmg = c.span(j, "dmg", where);
            r.reload = static_cast<int>(c.num(j, "reload", where));
            r.ammo = static_cast<int>(c.num(j, "ammo", where));
            r.baseHit = c.num(j, "base_hit", where);
            r.distK = c.num(j, "dist_k", where);
            r.skillK = c.num(j, "skill_k", where);
            r.pierce = c.num(j, "pierce", where);
            r.stam = c.num(j, "stam", where);
            if (r.reload < 1) c.fail(where + ".reload: must be >= 1");
            d.ranged.push_back(r);
        }
    }
    ids.clear();
    for (const auto& r : d.ranged) ids.push_back(r.id);
    d.rangedIx = index(ids, "ranged weapons", c);

    // --- treatment (wounds.json) ------------------------------------------
    if (const OJson* t = c.obj(wnd, "treatment", WN)) {
        const std::string w = WN + ".treatment";
        d.treat.recoverDays.assign(d.severity.size(), 0.0);
        d.treat.perm.assign(d.severity.size(), 0.0);
        for (auto it = t->begin(); it != t->end(); ++it) {
            const std::string& k = it.key();
            if (k == "_doc") continue;
            if (k == "recover_days" || k == "perm") {
                auto& dst = k == "perm" ? d.treat.perm : d.treat.recoverDays;
                c.keyed(it.value(), d.sevIx, "severity", w + "." + k,
                        [&](int i, const OJson& v, const std::string&) { dst[i] = v.get<double>(); });
            } else if (k == "infection_extra_days") {
                d.treat.infectionExtraDays = c.num(*t, k, w);
            } else if (k == "infection_death") {
                d.treat.infectionDeath = c.num(*t, k, w);
            } else if (k == "bleed_death_untreated_deep") {
                d.treat.bleedDeathUntreatedDeep = c.num(*t, k, w);
            } else if (it.value().is_object()) {
                const std::string ww = w + "." + k;
                c.only(it.value(), {"p_stop", "infection", "recover_mult", "perm_mult"}, ww);
                d.treatments.push_back({k, c.num(it.value(), "p_stop", ww), c.num(it.value(), "infection", ww),
                                        c.num(it.value(), "recover_mult", ww), c.num(it.value(), "perm_mult", ww)});
            } else {
                c.fail(w + ": unknown key '" + k + "'");
            }
        }
    }
    ids.clear();
    for (const auto& t : d.treatments) ids.push_back(t.id);
    d.treatIx = index(ids, "treatments", c);

    // --- animations: weapon classes must name known weapons ---------------
    if (d.animations.is_object() && d.animations.contains("weapon_class"))
        for (auto it = d.animations["weapon_class"].begin(); it != d.animations["weapon_class"].end(); ++it)
            c.ref(d.weaponIx, it.key(), "weapon", "animations.weapon_class");

    if (!c.err.empty()) throw DataError(c.err);
    return d;
}
} // namespace

} // namespace lr::combat
