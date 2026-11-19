#include "sim/wounds.hpp"

#include <algorithm>

#include "sim/character.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/content.hpp"
#include "sim/world.hpp"

namespace altera::sim {

int RecoveryRules::rankOf(const std::string& treatment) const {
    for (std::size_t i = 0; i < rank.size(); ++i)
        if (rank[i] == treatment) return static_cast<int>(i);
    return 0;
}

RecoveryRules loadRecovery(const Json& j, const combat::CombatData& d, const Content& c, std::vector<std::string>& err) {
    RecoveryRules r;
    auto need = [&](const Json& o, const char* key, const std::string& where) -> const Json& {
        static const Json null;
        if (!o.contains(key)) {
            err.push_back("recovery." + where + ": missing '" + key + "'");
            return null;
        }
        return o[key];
    };
    auto num = [&](const Json& o, const char* key, const std::string& where, double& out) {
        const Json& v = need(o, key, where);
        if (v.is_number()) out = v.get<double>();
        else if (!v.is_null()) err.push_back("recovery." + where + "." + key + ": not a number");
    };
    const Json& carry = need(j, "carry", "");
    if (carry.is_object()) {
        num(carry, "pain_share", "carry", r.painShare);
        num(carry, "disable_while_left", "carry", r.disableWhileLeft);
        num(carry, "min_zone_hp_share", "carry", r.minZoneHpShare);
    }
    const Json& heal = need(j, "heal", "");
    if (heal.is_object()) {
        num(heal, "first_aid_hours", "heal", r.firstAidHours);
        num(heal, "infection_check_hours", "heal", r.infectionCheckHours);
        num(heal, "fever_days", "heal", r.feverDays);
        num(heal, "bleed_death_blood_below", "heal", r.bleedDeathBloodBelow);
        num(heal, "bleed_death_mult", "heal", r.bleedDeathMult);
        num(heal, "infection_death_ref", "heal", r.infectionDeathRef);
        num(heal, "blood_regen_per_day", "heal", r.bloodRegenPerDay);
        const std::string minSev = heal.value("treat_min_severity", std::string("cut"));
        if (d.sevIx.count(minSev)) r.treatMinSeverity = d.sevIx.at(minSev);
        else err.push_back("recovery.heal.treat_min_severity: unknown severity '" + minSev + "'");
        const Json& pace = need(heal, "pace", "heal");
        if (pace.is_object()) {
            num(pace, "base", "heal.pace", r.paceBase);
            num(pace, "hungry_below", "heal.pace", r.hungryBelow);
            num(pace, "hungry_mult", "heal.pace", r.hungryMult);
            num(pace, "rest_mult", "heal.pace", r.restMult);
            r.hungryNeed = pace.value("hungry_need", r.hungryNeed);
            r.restFsm = pace.value("rest_fsm", std::vector<std::string>{});
            bool needOk = false;
            for (const auto& n : c.needs) needOk |= n.id == r.hungryNeed;
            if (!needOk) err.push_back("recovery.heal.pace.hungry_need: unknown need '" + r.hungryNeed + "'");
            for (const auto& s : r.restFsm)
                if (!c.fsm.count(s)) err.push_back("recovery.heal.pace.rest_fsm: unknown fsm state '" + s + "'");
        }
        r.playerWoundDeath = heal.value("player_wound_death", false);
        r.deathCause = heal.value("death_cause", r.deathCause);
        if (std::find(c.config.deathCauses.begin(), c.config.deathCauses.end(), r.deathCause) == c.config.deathCauses.end())
            err.push_back("recovery.heal.death_cause: '" + r.deathCause + "' is not in config.death_causes");
    }
    r.rank = j.value("treatment_rank", std::vector<std::string>{});
    for (const auto& t : r.rank)
        if (!d.treatIx.count(t)) err.push_back("recovery.treatment_rank: unknown treatment '" + t + "'");
    if (r.rank.empty()) err.push_back("recovery.treatment_rank: empty");
    for (const auto& s : j.value("treatment_sources", Json::array())) {
        TreatmentSource ts;
        ts.treatment = s.value("treatment", std::string());
        if (!d.treatIx.count(ts.treatment)) err.push_back("recovery.treatment_sources: unknown treatment '" + ts.treatment + "'");
        if (s.contains("helper")) {
            ts.helperSkill = s["helper"].value("skill", std::string());
            ts.helperMin = s["helper"].value("min", 0.0);
            ts.helperScope = s["helper"].value("scope", std::string("settlement"));
            if (ts.helperScope != "settlement" && ts.helperScope != "family")
                err.push_back("recovery.treatment_sources." + ts.treatment + ".helper.scope: settlement | family");
        }
        ts.consumesAny = s.value("consumes_any", std::vector<std::string>{});
        for (const auto& item : ts.consumesAny)
            if (!c.items.count(item)) err.push_back("recovery.treatment_sources." + ts.treatment + ": unknown item '" + item + "'");
        r.sources.push_back(std::move(ts));
    }
    r.consumeFrom = j.value("consume_from", std::vector<std::string>{"self"});
    for (const auto& f : r.consumeFrom)
        if (f != "self" && f != "home" && f != "helper") err.push_back("recovery.consume_from: self | home | helper, not '" + f + "'");
    const Json injuries = j.value("injuries", Json::object());
    for (const auto& [zone, kind] : injuries.items()) {
        if (zone.rfind('_', 0) == 0) continue;
        if (!d.zoneIx.count(zone)) err.push_back("recovery.injuries: unknown zone '" + zone + "'");
        r.injuries[zone] = kind.get<std::string>();
    }
    for (const auto& z : d.zones)
        if (!r.injuries.count(z)) err.push_back("recovery.injuries: no injury for zone '" + z + "'");
    const Json mem = j.value("memory", Json::object());
    r.memTreated = mem.value("treated", std::string());
    r.memTreater = mem.value("treater", std::string());
    r.memInjury = mem.value("injury", std::string());
    const Json chron = j.value("chronicle", Json::object());
    r.chronInjury = chron.value("injury", std::string("chronicle.maimed"));
    r.chronDeath = chron.value("death", std::string("chronicle.died_of_wounds"));
    return r;
}

std::vector<WoundHit> woundHits(const combat::Fighter& f, std::size_t from) {
    std::vector<WoundHit> out;
    for (std::size_t i = from; i < f.wounds.size(); ++i) out.push_back({f.woundZone[i], f.wounds[i], f.woundDmg[i]});
    return out;
}

namespace {

double leftShare(const Wound& w) { return w.daysTotal > 0.0 ? std::clamp(w.daysLeft / w.daysTotal, 0.0, 1.0) : 1.0; }

} // namespace

void carryWounds(const combat::CombatData& d, const RecoveryRules& r, const Character& c, combat::Fighter& f) {
    for (const Wound& w : c.wounds) {
        auto zi = d.zoneIx.find(w.zone);
        auto si = d.sevIx.find(w.severity);
        if (zi == d.zoneIx.end() || si == d.sevIx.end()) continue;
        const int z = zi->second;
        const double left = leftShare(w);
        f.hp[z] -= w.dmg * left;
        f.pain += d.severity[si->second].pain * left * r.painShare;
        if (d.severity[si->second].disableZone && left > r.disableWhileLeft) f.disabled |= 1u << z;
    }
    for (std::size_t z = 0; z < f.hp.size(); ++z) f.hp[z] = std::max(f.hp[z], d.zoneHp[z] * r.minZoneHpShare);
    for (const Injury& in : c.injuries) {
        auto zi = d.zoneIx.find(in.zone);
        if (zi != d.zoneIx.end()) f.disabled |= 1u << zi->second;
    }
    f.blood = std::clamp(c.body.blood, 1.0, d.bloodMax);
}

void addCombatWounds(World& w, Character& c, const std::vector<WoundHit>& hits, double bloodAfter, double healthDebt) {
    if (!c.body.alive) return;
    const CombatBridge* b = combatBridge(w.content());
    if (!b) return;
    const combat::CombatData& d = *b->data;
    c.body.blood = std::clamp(bloodAfter, 0.0, d.bloodMax);
    double total = 0.0;
    for (const auto& h : hits) total += h.dmg;
    for (const auto& h : hits) {
        Wound wd;
        wd.zone = d.zones[h.zone];
        wd.severity = d.severity[h.severity].id;
        wd.dmg = h.dmg;
        wd.healthDebt = total > 0.0 ? std::max(0.0, healthDebt) * h.dmg / total : 0.0;
        wd.daysTotal = wd.daysLeft = d.treat.recoverDays[h.severity];
        wd.bleeding = d.bleedRate[h.severity] > 0.0;
        wd.aided = !wd.bleeding;
        wd.since = w.now();
        c.wounds.push_back(std::move(wd));
    }
}


namespace {

constexpr Tick kTicksPerHour = kTicksPerDay / 24;

bool mortal(const RecoveryRules& r, const Character& c) { return r.playerWoundDeath || !c.controller.isPlayer(); }

void dieOfWounds(World& w, const RecoveryRules& r, Character& c, const Wound& wd, const char* why) {
    if (!c.body.alive) return;
    c.body.alive = false;
    c.body.health = 0.0;
    c.body.causeOfDeath = r.deathCause;
    w.chronicle("death", {c.id}, "chronicle.death",
                {{"who", c.id}, {"cause", r.deathCause}, {"wound", wd.zone}, {"severity", wd.severity}, {"by", why}});
}

int countOf(const std::map<std::string, int>& m, const std::string& item) {
    auto it = m.find(item);
    return it == m.end() ? 0 : it->second;
}

// The best helper with the skill: alive, awake, someone else, in scope.
Character* findHelper(World& w, const Character& c, const TreatmentSource& s) {
    Character* best = nullptr;
    double bestSkill = -1.0;
    for (auto& [id, h] : w.characters()) {
        if (id == c.id || !h.body.alive || h.isAnimal() || h.body.downUntil > w.now()) continue;
        if (s.helperScope == "family" ? (c.status.family == kNone || h.status.family != c.status.family)
                                      : (c.status.settlement == kNone || h.status.settlement != c.status.settlement))
            continue;
        auto sk = h.skills.find(s.helperSkill);
        const double v = sk == h.skills.end() ? 0.0 : sk->second;
        if (v >= s.helperMin && v > bestSkill) {
            bestSkill = v;
            best = &h;
        }
    }
    return best;
}

// Takes one unit of any of `items` from the places in consume_from.
bool consumeOne(World& w, const RecoveryRules& r, Character& c, Character* helper, const std::vector<std::string>& items) {
    if (items.empty()) return true;
    for (const auto& place : r.consumeFrom)
        for (const auto& item : items) {
            std::map<std::string, int>* inv = nullptr;
            if (place == "self") inv = &c.inventory;
            else if (place == "helper" && helper) inv = &helper->inventory;
            else if (place == "home" && c.home != kNone) {
                if (WorldObject* o = w.object(c.home)) inv = &o->stock;
            }
            if (inv && countOf(*inv, item) > 0) {
                if (--(*inv)[item] <= 0) inv->erase(item);
                return true;
            }
        }
    return false;
}

void tryTreat(World& w, const RecoveryRules& r, Character& c, Wound& wd) {
    const int current = r.rankOf(wd.treatment);
    for (const auto& s : r.sources) {
        if (r.rankOf(s.treatment) <= current) continue;
        Character* helper = nullptr;
        if (!s.helperSkill.empty() && !(helper = findHelper(w, c, s))) continue;
        if (!consumeOne(w, r, c, helper, s.consumesAny)) continue;
        wd.treatment = s.treatment;
        if (!r.memTreated.empty()) {
            MemoryEntry m;
            m.type = r.memTreated;
            if (helper) m.who = {helper->id};
            m.valence = 0.2;
            w.remember(c, std::move(m));
        }
        if (helper && !r.memTreater.empty()) {
            MemoryEntry m;
            m.type = r.memTreater;
            m.who = {c.id};
            m.valence = 0.1;
            w.remember(*helper, std::move(m));
        }
        return;
    }
}

} // namespace

void healWounds(World& w) {
    const CombatBridge* b = combatBridge(w.content());
    if (!b) return;
    const RecoveryRules& R = b->recovery;
    const combat::CombatData& d = *b->data;
    const Tick now = w.now();
    const double dtDays = 1.0 / static_cast<double>(kTicksPerDay);
    const bool hourly = now % kTicksPerHour == 0;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive) continue;
        if (c.body.downUntil != 0 && c.body.downUntil <= now) {
            c.body.downUntil = 0;
            if (c.ai.fsm.state == "Incapacitated") c.ai.fsm.state = "Idle";
        }
        bool bleeding = false;
        for (const Wound& wd : c.wounds) bleeding |= wd.bleeding;
        if (!bleeding && c.body.blood < d.bloodMax)
            c.body.blood = std::min(d.bloodMax, c.body.blood + R.bloodRegenPerDay * dtDays);
        if (c.wounds.empty()) continue;
        double pace = R.paceBase;
        if (!c.isAnimal() && c.need(R.hungryNeed) < R.hungryBelow) pace *= R.hungryMult;
        if (std::find(R.restFsm.begin(), R.restFsm.end(), c.ai.fsm.state) != R.restFsm.end()) pace *= R.restMult;
        for (std::size_t i = 0; i < c.wounds.size() && c.body.alive;) {
            Wound& wd = c.wounds[i];
            auto si = d.sevIx.find(wd.severity);
            if (si == d.sevIx.end()) {
                c.wounds.erase(c.wounds.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            const int sev = si->second;
            const bool open = sev >= R.treatMinSeverity; // a bruise needs no care and does not fester
            if (open && hourly && !c.isAnimal()) tryTreat(w, R, c, wd);
            const combat::Treatment& t = d.treatments[d.treatment(wd.treatment)];
            Rng rng = w.rng(c.id, "wound:" + std::to_string(i) + ":" + std::to_string(wd.since));
            // First aid: the blood stops or does not (heal_sim: a heavy wound,
            // the stop fails, little blood left -> may bleed out).
            if (!wd.aided && now >= wd.since + static_cast<Tick>(R.firstAidHours * kTicksPerHour)) {
                wd.aided = true;
                const bool failed = rng.uniform() > t.pStop;
                if (d.severity[sev].heavy && failed && c.body.blood < R.bleedDeathBloodBelow &&
                    rng.uniform() < d.treat.bleedDeathUntreatedDeep * R.bleedDeathMult && mortal(R, c)) {
                    dieOfWounds(w, R, c, wd, "bleeding");
                    break;
                }
                wd.bleeding = false;
            }
            if (open && !wd.infectionRolled &&
                now >= wd.since + static_cast<Tick>(R.infectionCheckHours * kTicksPerHour)) {
                wd.infectionRolled = true;
                if (rng.uniform() < t.infection) {
                    wd.infected = true;
                    wd.daysLeft += d.treat.infectionExtraDays;
                    wd.daysTotal += d.treat.infectionExtraDays;
                    wd.feverAt = now + static_cast<Tick>(R.feverDays * kTicksPerDay);
                }
            }
            if (wd.infected && wd.feverAt != 0 && now >= wd.feverAt) {
                wd.feverAt = 0;
                const double p =
                    d.treat.infectionDeath * (R.infectionDeathRef > 0 ? t.infection / R.infectionDeathRef : 1.0);
                if (rng.uniform() < p && mortal(R, c)) {
                    dieOfWounds(w, R, c, wd, "infection");
                    break;
                }
            }
            const double prog = std::min(wd.daysLeft, dtDays * pace / std::max(1e-6, t.recoverMult));
            if (wd.daysTotal > 0.0) c.body.health = clamp01(c.body.health + wd.healthDebt * prog / wd.daysTotal);
            wd.daysLeft -= prog;
            if (wd.daysLeft > 1e-9) {
                ++i;
                continue;
            }
            // Healed. A heavy wound may heal badly into a permanent injury.
            const double perm = d.treat.perm[sev] * t.permMult;
            if (perm > 0.0 && rng.uniform() < perm) {
                const std::string& kind = R.injuries.at(wd.zone);
                bool had = false;
                for (const Injury& in : c.injuries) had |= in.zone == wd.zone && in.kind == kind;
                if (!had) {
                    c.injuries.push_back({wd.zone, kind, now});
                    if (!R.memInjury.empty()) {
                        MemoryEntry m;
                        m.type = R.memInjury;
                        m.valence = -0.5;
                        w.remember(c, std::move(m));
                    }
                    w.chronicle("injury", {c.id}, R.chronInjury, {{"who", c.id}, {"zone", wd.zone}, {"kind", kind}});
                }
            }
            c.wounds.erase(c.wounds.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

} // namespace altera::sim
