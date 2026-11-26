#include "sim/combat/combat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace lr::combat {

namespace {

double clampd(double x, double a, double b) { return std::max(a, std::min(b, x)); }

} // namespace

const char* stateId(State s) {
    switch (s) {
    case State::Fight: return "fight";
    case State::Ko: return "ko";
    case State::Dead: return "dead";
    case State::Yield: return "yield";
    case State::Flee: return "flee";
    }
    return "?";
}

const char* outcomeId(Outcome o) {
    switch (o) {
    case Outcome::Hit: return "hit";
    case Outcome::Parry: return "parry";
    case Outcome::Block: return "block";
    case Outcome::BlockShield: return "block_shield";
    case Outcome::Dodge: return "dodge";
    }
    return "?";
}

const char* eventId(EventKind k) {
    switch (k) {
    case EventKind::Attack: return "attack";
    case EventKind::Guard: return "guard";
    case EventKind::Result: return "result";
    case EventKind::Wound: return "wound";
    case EventKind::Riposte: return "riposte";
    case EventKind::CloseIn: return "close_in";
    case EventKind::Clinch: return "clinch";
    case EventKind::GetUp: return "get_up";
    case EventKind::Exhausted: return "exhausted";
    case EventKind::Wavering: return "wavering";
    case EventKind::LimpOrCradle: return "limp_or_cradle";
    case EventKind::State: return "state";
    case EventKind::Shoot: return "shoot";
    case EventKind::ProjectileHit: return "projectile_hit";
    case EventKind::ProjectileMiss: return "projectile_miss";
    case EventKind::WeaponBreak: return "weapon_break";
    case EventKind::End: return "end";
    }
    return "?";
}

std::string eventLine(const CombatData& d, const Event& e) {
    std::string s;
    char buf[64];
    auto add = [&](const char* k, const std::string& v) {
        s += s.empty() ? "{" : ",";
        s += "\"";
        s += k;
        s += "\":";
        s += v;
    };
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    auto n = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.4f", v);
        return std::string(buf);
    };
    auto id = [&](const std::vector<std::string>& ids, int i) { return q(i >= 0 ? ids[i] : std::string("none")); };
    add("r", std::to_string(e.round));
    add("k", q(eventId(e.kind)));
    if (e.actor >= 0) add("a", std::to_string(e.actor));
    if (e.target >= 0) add("t", std::to_string(e.target));
    std::vector<std::string> wid, mid, rid, sid;
    switch (e.kind) {
    case EventKind::Attack:
        add("weapon", q(d.weapons[e.weapon].id));
        add("dir", id(d.directions, e.direction));
        add("zone", id(d.zones, e.zone));
        add("dtype", id(d.dtypes, e.dtype));
        add("skill", n(e.skill));
        add("stamina", n(e.stamina));
        add("riposte", e.riposte ? "1" : "0");
        break;
    case EventKind::Guard:
        add("guard", id(d.guards, e.guard));
        add("correct", e.correct ? "1" : "0");
        add("near", e.nearGuard ? "1" : "0");
        add("shield", e.shieldId >= 0 ? q(d.shields[e.shieldId].id) : q("none"));
        break;
    case EventKind::Result:
        add("out", q(outcomeId(e.outcome)));
        add("zone", id(d.zones, e.zone));
        add("dir", id(d.directions, e.direction));
        add("dtype", id(d.dtypes, e.dtype));
        if (e.outcome == Outcome::Hit) {
            add("dmg", n(e.dmg));
            add("absorbed", n(e.absorbed));
            add("armored", e.armored ? "1" : "0");
        }
        break;
    case EventKind::Wound:
        add("zone", id(d.zones, e.zone));
        add("sev", q(d.severity[e.severity].id));
        add("dmg", n(e.dmg));
        add("frac", n(e.frac));
        break;
    case EventKind::Clinch:
        add("move", q(d.moves[e.move].id));
        add("ok", e.success ? "1" : "0");
        break;
    case EventKind::Exhausted: break;
    case EventKind::Wavering: add("morale", n(e.morale)); break;
    case EventKind::LimpOrCradle: {
        std::string z = "[";
        for (std::size_t i = 0; i < d.zones.size(); ++i)
            if (e.zones & (1u << i)) z += (z.size() > 1 ? "," : "") + q(d.zones[i]);
        add("zones", z + "]");
        break;
    }
    case EventKind::State:
        add("state", q(stateId(e.state)));
        add("pain", n(e.pain));
        add("blood", n(e.blood));
        add("morale", n(e.morale));
        break;
    case EventKind::Shoot:
    case EventKind::ProjectileMiss: add("weapon", q(d.ranged[e.ranged].id)); break;
    case EventKind::ProjectileHit:
        add("weapon", q(d.ranged[e.ranged].id));
        add("zone", id(d.zones, e.zone));
        break;
    case EventKind::WeaponBreak: add("weapon", q(d.weapons[e.weapon].id)); break;
    default: break;
    }
    return s + "}";
}

// --- fighters -----------------------------------------------------------

Fighter Engine::make(const FighterSpec& s, std::int64_t id, int team) const {
    Fighter f;
    f.id = id;
    f.team = team;
    f.skill = s.skill;
    f.know = s.know < 0 ? d_.core.defaultKnow : s.know;
    f.weapon = d_.weapon(s.weapon);
    f.shield = s.shield.empty() ? -1 : d_.shield(s.shield);
    for (const auto& a : s.armor) f.armor.push_back(d_.armor(a));
    f.armorCond.assign(f.armor.size(), 1.0);
    f.damageMult = s.damageMult;
    f.durabilityMult = s.durabilityMult > 0 ? s.durabilityMult : 1.0;
    f.armorEff.assign(f.armor.size(), 1.0);
    f.armorGap.assign(f.armor.size(), 0.0);
    for (std::size_t i = 0; i < f.armor.size(); ++i) {
        if (i < s.armorEff.size()) f.armorEff[i] = s.armorEff[i];
        if (i < s.armorGap.size()) f.armorGap[i] = s.armorGap[i];
    }
    f.hp = d_.zoneHp;
    f.stamCap = s.staminaCap > 0 ? s.staminaCap : d_.staminaMax;
    f.stam = s.stamina >= 0 ? std::min(s.stamina, f.stamCap) : f.stamCap;
    f.blood = d_.bloodMax;
    f.morale = d_.morale.start;
    return f;
}

double Engine::read(const Fighter& f) const {
    const auto& r = d_.read;
    const double p = r.base + r.kSkill * f.skill + r.kKnow * f.know - r.kFatigue * fatigue(f) - r.kPanic * panic(f);
    return clampd(p, r.min, r.max);
}

double Engine::armorPen(const Fighter& f) const {
    double p = 0.0;
    for (int a : f.armor) p += d_.armors[a].staminaPen;
    return p;
}

double Engine::absorb(const Fighter& f, int zone, int dtype, Rng* rng, double pierce, bool nogap) const {
    double cut = 1.0, tr = 1.0;
    const bool impact = d_.impactOnly[dtype] != 0;
    for (std::size_t i = 0; i < f.armor.size(); ++i) {
        const ArmorDef& A = d_.armors[f.armor[i]];
        if (!A.cover[zone]) continue;
        if (nogap && A.hasGap) continue;
        const double gapAdd = i < f.armorGap.size() ? f.armorGap[i] : 0.0;
        if (rng && rng->random() < A.gap + gapAdd) continue;
        const double made = i < f.armorEff.size() ? f.armorEff[i] : 1.0;
        const double eff = std::min(1.0, made * (d_.wear.armorMinEff + (1.0 - d_.wear.armorMinEff) * f.armorCond[i]));
        tr *= 1.0 - (1.0 - A.trans[dtype]) * eff;
        if (!impact) {
            const double p = A.pen[dtype] * eff;
            cut *= 1.0 - (p < d_.core.pierceImmunePen ? p * (1.0 - pierce) : p);
        }
    }
    if (impact) return 1.0 - tr;
    return 1.0 - (cut + (1.0 - cut) * d_.kImpact[dtype] * tr);
}

double Engine::weaponMult(const Fighter& f) const {
    double m = 1.0;
    for (std::size_t z = 0; z < d_.zones.size(); ++z)
        if (f.disabled & (1u << z)) m *= d_.disable[z].attackMult;
    return m * f.damageMult * (d_.wear.minDmgMult + (1.0 - d_.wear.minDmgMult) * f.weaponCond);
}

void Engine::wearWeapon(Fighter& f, double cost) {
    const WeaponDef& w = d_.weapons[f.weapon];
    if (w.indestructible) return;
    f.weaponCond -= cost / (w.durability * f.durabilityMult);
    if (f.weaponCond <= 0.0) {
        Event e;
        e.kind = EventKind::WeaponBreak;
        e.actor = f.id;
        e.weapon = f.weapon;
        emit(e);
        f.weapon = d_.core.unarmed;
        f.shield = -1;
        f.weaponCond = 1.0;
        ++f.broke;
    }
}

void Engine::wearArmor(Fighter& f, int zone) {
    for (std::size_t i = 0; i < f.armor.size(); ++i) {
        const ArmorDef& A = d_.armors[f.armor[i]];
        if (A.cover[zone]) f.armorCond[i] = std::max(0.0, f.armorCond[i] - d_.wear.armorPerHit / A.durability);
    }
}

void Engine::regen(Fighter& f) {
    f.stam = clampd(f.stam + d_.staminaRegen - armorPen(f) * d_.core.regenArmorStamina, 0.0, f.stamCap);
    f.ap += d_.weapons[f.weapon].speed;
}

void Engine::emit(Event e) {
    if (!ev_ || !ev_->active()) return;
    e.round = clock_;
    ev_->push(e);
}

int Engine::pickZone(const std::vector<double>& bias) { return static_cast<int>(rng_.weighted(bias)); }

// --- one strike -----------------------------------------------------------

double Engine::hitChance(const Fighter& a, const Fighter& d, int guard, int dir, double bonus) const {
    const auto& h = d_.hit;
    const auto& ok = d_.counter[dir];
    double m = guard == ok[0] ? h.perfect : guard == ok[1] && guard >= 0 ? h.nearMult : h.none;
    if (d.shield >= 0 && guardCovers(guard, dir)) m *= d_.shields[d.shield].guardMult;
    for (std::size_t z = 0; z < d_.zones.size(); ++z)
        if ((d.disabled & (1u << z)) && d_.disable[z].guardMult != 1.0) m = std::min(1.0, m / d_.disable[z].guardMult);
    const double base = h.base + h.kSkillDiff * (a.skill - d.skill) + bonus;
    return clampd(base * m, h.min, h.max);
}

void Engine::wound(Fighter& d, int zone, double dmg) {
    d.hp[zone] -= dmg;
    const double frac = dmg / d_.zoneHp[zone];
    int si = 0;
    for (std::size_t i = 0; i < d_.severity.size(); ++i)
        if (frac >= d_.severity[i].minFrac) si = static_cast<int>(i);
    const SeverityDef& sev = d_.severity[si];
    d.wounds.push_back(si);
    d.woundZone.push_back(zone);
    d.woundDmg.push_back(dmg);
    {
        Event e;
        e.kind = EventKind::Wound;
        e.actor = d.id;
        e.zone = zone;
        e.severity = si;
        e.dmg = dmg;
        e.frac = frac;
        emit(e);
    }
    const double sc = std::min(1.0, frac / d_.severity[d_.core.painScaleSeverity].minFrac);
    const ZoneDisable& zd = d_.disable[zone];
    d.pain += sev.pain * sc * (zd.ko ? d_.koZoneMult : 1.0);
    d.bleed += d_.bleedRate[si];
    d.morale -= d_.morale.lossPerHit * sc + (sev.heavy ? d_.morale.lossHeavy : 0.0);
    if (sev.disableZone || d.hp[zone] <= 0) d.disabled |= 1u << zone;
    if (zd.deathAtZero && d.hp[zone] <= 0) d.state = State::Dead;
    if (zd.ko && (d.hp[zone] <= 0 || sev.ko)) d.state = State::Ko;
}

void Engine::act(Fighter& a, Fighter& d, double bonus, bool allowRiposte) {
    const WeaponDef& W = d_.weapons[a.weapon]; // the swing uses the weapon it started with
    const int weaponAtSwing = a.weapon;
    // --- choose the attack
    int dir, zone;
    if (rng_.random() < read(a)) {
        const std::size_t NZ = d_.zones.size();
        std::vector<double> wts;
        wts.reserve(W.dirs.size() * NZ);
        for (int dr : W.dirs) {
            const int t = W.dirType[dr];
            const double mid = (W.types[t].lo + W.types[t].hi) / 2.0;
            for (std::size_t z = 0; z < NZ; ++z) {
                const double ed = mid * (1.0 - absorb(d, static_cast<int>(z), t));
                const double w = std::max(d_.core.aimFloor, ed * std::pow(d_.dirZoneBias[dr][z], d_.aimBiasExp));
                wts.push_back(std::pow(w, d_.aimSharpness));
            }
        }
        const std::size_t k = rng_.weighted(wts);
        dir = W.dirs[k / NZ];
        zone = static_cast<int>(k % NZ);
    } else {
        dir = W.dirs[rng_.below(W.dirs.size())];
        zone = pickZone(d_.dirZoneBias[dir]);
    }
    // --- choose the guard
    int guard = -1;
    if (!d.down) guard = rng_.random() < read(d) ? d_.counter[dir][0] : static_cast<int>(rng_.below(d_.guards.size()));
    const int t = W.dirType[dir];
    const bool covers = guardCovers(guard, dir);
    const bool perfect = guard >= 0 && guard == d_.counter[dir][0];
    {
        Event e;
        e.kind = EventKind::Attack;
        e.actor = a.id;
        e.target = d.id;
        e.weapon = weaponAtSwing;
        e.direction = dir;
        e.zone = zone;
        e.dtype = t;
        e.skill = a.skill;
        e.stamina = a.stam;
        e.riposte = !allowRiposte;
        emit(e);
        Event g;
        g.kind = EventKind::Guard;
        g.actor = d.id;
        g.guard = guard;
        g.correct = perfect;
        g.nearGuard = covers;
        g.shieldId = d.shield;
        emit(g);
    }
    a.stam -= W.stamina + armorPen(a) * d_.core.attackArmorStamina;
    wearWeapon(a, d_.wear.perAttack);
    if (d.shield >= 0 && covers) d.stam -= d_.shields[d.shield].stamina;
    if (rng_.random() < hitChance(a, d, guard, dir, bonus)) {
        const double ab = absorb(d, zone, t, &rng_);
        const double dmg = rng_.uniform(W.types[t].lo, W.types[t].hi) * weaponMult(a) *
                           (d_.core.fatigueFloor + (1.0 - d_.core.fatigueFloor) * (1.0 - fatigue(a))) * (1.0 - ab);
        Event e;
        e.kind = EventKind::Result;
        e.actor = a.id;
        e.target = d.id;
        e.outcome = Outcome::Hit;
        e.zone = zone;
        e.direction = dir;
        e.dtype = t;
        e.dmg = dmg;
        e.absorbed = ab;
        for (int l : d.armor)
            if (d_.armors[l].cover[zone]) e.armored = true;
        emit(e);
        wound(d, zone, dmg);
        ++a.hits;
        wearArmor(d, zone);
        if (ab > d_.wear.hardArmorPen) wearWeapon(a, d_.wear.perAttack * (d_.wear.vsHardArmor - 1.0));
    } else {
        if (covers) wearWeapon(d, d_.wear.perParry);
        Event e;
        e.kind = EventKind::Result;
        e.actor = a.id;
        e.target = d.id;
        e.outcome = perfect ? (d.shield >= 0 ? Outcome::BlockShield : Outcome::Parry)
                            : (covers ? Outcome::Block : Outcome::Dodge);
        e.zone = zone;
        e.direction = dir;
        e.dtype = t;
        emit(e);
        if (allowRiposte && perfect && d.fighting()) {
            if (rng_.random() < d_.riposte.base + d_.riposte.kSkill * d.skill) {
                Event r;
                r.kind = EventKind::Riposte;
                r.actor = d.id;
                r.target = a.id;
                emit(r);
                act(d, a, d_.riposte.hitBonus, false);
            }
        }
    }
}

// --- state after the round --------------------------------------------------

void Engine::statusCheck(Fighter& f) {
    if (!f.fighting()) return;
    if (f.stam < d_.core.exhaustedBelow) {
        Event e;
        e.kind = EventKind::Exhausted;
        e.actor = f.id;
        emit(e);
    }
    f.blood -= f.bleed * d_.core.bleedPerRound;
    if (f.blood <= 0) f.state = State::Dead;
    else if (f.pain >= d_.painThreshold) f.state = State::Ko;
    else if (f.morale < d_.morale.yieldBelow) f.state = State::Yield;
    else if (f.morale < d_.morale.fleeBelow) f.state = State::Flee;
    if (!f.fighting()) {
        Event e;
        e.kind = EventKind::State;
        e.actor = f.id;
        e.state = f.state;
        e.pain = f.pain;
        e.blood = f.blood;
        e.morale = f.morale;
        emit(e);
    } else if (f.morale < d_.morale.fleeBelow + d_.core.waveringMargin) {
        Event e;
        e.kind = EventKind::Wavering;
        e.actor = f.id;
        e.morale = f.morale;
        emit(e);
    }
    if (f.fighting() && f.disabled) {
        Event e;
        e.kind = EventKind::LimpOrCradle;
        e.actor = f.id;
        e.zones = f.disabled;
        emit(e);
    }
}

double Engine::reachBonus(const Fighter& x, const Fighter& y, bool closed) const {
    const auto& R = d_.reach;
    const int diff = reachOf(x) - reachOf(y);
    if (!closed) return R.hitBonusPerLevel * diff;
    return diff > 0 ? -R.clinchLongPenaltyPerLevel * diff : R.clinchBonusPerLevel * -diff;
}

// --- clinch -----------------------------------------------------------------

int Engine::clinchPick(const Fighter& a, const Fighter& d) {
    std::vector<int> ids;
    std::vector<double> ws;
    for (std::size_t i = 0; i < d_.moves.size(); ++i) {
        const ClinchMove& m = d_.moves[i];
        double w = m.weaponWeight[a.weapon];
        if (w == 0.0) continue;
        if (m.kind == MoveKind::GrappleThrow && d.down) continue;
        if (m.kind == MoveKind::Disarm && d.weapon == d_.core.unarmed) continue;
        if (d.down) w *= m.downWeightMult;
        if (m.kind == MoveKind::GapStab) w *= 1.0 + d_.armorWeightFactor * m.armorWeightMult * armorPen(d);
        if (w > 0) {
            ids.push_back(static_cast<int>(i));
            ws.push_back(w);
        }
    }
    if (ids.empty()) return -1;
    if (rng_.random() < read(a)) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < ws.size(); ++i)
            if (ws[i] > ws[best]) best = i;
        return ids[best];
    }
    return ids[rng_.weighted(ws)];
}

void Engine::clinchAct(Fighter& a, Fighter& d) {
    const int mi = clinchPick(a, d);
    if (mi < 0) return act(a, d, 0.0, true);
    const ClinchMove& m = d_.moves[mi];
    const double diff = a.skill - d.skill;
    const double lo = d_.core.clinchMin, hi = d_.core.clinchMax;
    bool ok = false;
    a.stam -= m.stamCost + armorPen(a) * d_.core.clinchArmorStamina;
    switch (m.kind) {
    case MoveKind::GrappleThrow: {
        const double p = clampd(m.base + m.kSkillDiff * diff - m.armorResistPerPen * armorPen(d), lo, hi);
        if (rng_.random() < p) {
            ok = true;
            d.down = d_.downRounds;
            d.stam -= m.targetStamLoss;
            wound(d, m.fallZone, rng_.uniform(m.fallDmg.lo, m.fallDmg.hi));
        }
        break;
    }
    case MoveKind::GapStab: {
        const double base = m.baseByWeapon[a.weapon] >= 0 ? m.baseByWeapon[a.weapon] : m.base;
        const double p = clampd(base + m.kSkillDiff * diff + (d.down ? m.downBonus : 0.0), lo, hi);
        const int zone = pickZone(m.zoneWeight);
        if (rng_.random() < p) {
            ok = true;
            const WeaponDef& W = d_.weapons[a.weapon];
            const Span s = W.hasType[m.dtype] ? W.types[m.dtype] : W.types[W.firstType];
            const double dmg = rng_.uniform(s.lo, s.hi) * weaponMult(a) *
                               (d_.core.fatigueFloor + (1.0 - d_.core.fatigueFloor) * (1.0 - fatigue(a))) *
                               (1.0 - absorb(d, zone, m.dtype, nullptr, 0.0, true)) * m.force;
            wound(d, zone, dmg);
            ++a.hits;
        }
        break;
    }
    case MoveKind::PommelStrike: {
        const double p = clampd(m.base + m.kSkillDiff * diff, lo, hi);
        const int zone = pickZone(m.zoneWeight);
        if (rng_.random() < p) {
            ok = true;
            const double dmg = rng_.uniform(m.dmg.lo, m.dmg.hi) * (1.0 - absorb(d, zone, m.dtype, &rng_) * m.absorbMult);
            wound(d, zone, dmg);
            ++a.hits;
        }
        break;
    }
    case MoveKind::Disarm: {
        const double p = clampd(m.base + m.kSkillDiff * diff, lo, hi);
        if (rng_.random() < p) {
            ok = true;
            d.weapon = d_.core.unarmed;
            d.shield = -1;
        }
        break;
    }
    }
    Event e;
    e.kind = EventKind::Clinch;
    e.actor = a.id;
    e.target = d.id;
    e.move = mi;
    e.success = ok;
    emit(e);
}

void Engine::takeAction(Fighter& x, Fighter& y, bool closed, bool useReach, bool clinch) {
    if (x.down) {
        --x.down;
        Event e;
        e.kind = EventKind::GetUp;
        e.actor = x.id;
        emit(e);
        return;
    }
    if (clinch && closed && rng_.random() < d_.weapons[x.weapon].clinchShare) clinchAct(x, y);
    else act(x, y, useReach ? reachBonus(x, y, closed) : 0.0, true);
}

// --- duel -------------------------------------------------------------------

int Engine::fight(Fighter& a, Fighter& b, bool useReach, bool clinch, int maxRounds) {
    if (maxRounds < 0) maxRounds = d_.core.duelMaxRounds;
    bool closed = !useReach || reachOf(a) == reachOf(b);
    int rd = 0;
    for (rd = 0; rd < maxRounds; ++rd) {
        clock_ = rd;
        regen(a);
        regen(b);
        if (!closed) {
            Fighter& s = reachOf(a) < reachOf(b) ? a : b;
            if (rng_.random() < d_.reach.closeBase + d_.reach.closeKSkill * s.skill) {
                closed = true;
                Event e;
                e.kind = EventKind::CloseIn;
                e.actor = s.id;
                emit(e);
            }
        }
        const bool aFirst = rng_.random() < d_.core.firstMover;
        Fighter* order[2] = {aFirst ? &a : &b, aFirst ? &b : &a};
        for (Fighter* x : order) {
            Fighter* y = x == &a ? &b : &a;
            if (!x->fighting() || !y->fighting()) continue;
            if (x->ap >= 1) {
                x->ap -= 1;
                takeAction(*x, *y, closed, useReach, clinch);
            }
        }
        statusCheck(a);
        statusCheck(b);
        if (ev_) ev_->flush();
        if (!a.fighting() || !b.fighting()) break;
    }
    if (rd == maxRounds) rd = maxRounds - 1;
    Event e;
    e.kind = EventKind::End;
    emit(e);
    if (ev_) ev_->flush();
    return rd + 1;
}

// --- ranged approach --------------------------------------------------------

int Engine::shootZone(const Fighter& a, const Fighter& d, const RangedDef& w) {
    const auto& bias = d_.dirZoneBias[d_.rangedCfg.aimDirection];
    if (rng_.random() < read(a)) {
        const double mid = (w.dmg.lo + w.dmg.hi) / 2.0;
        int best = 0;
        double bv = -1e300;
        for (std::size_t z = 0; z < d_.zones.size(); ++z) {
            const double v = mid * (1.0 - absorb(d, static_cast<int>(z), w.dtype, nullptr, w.pierce)) * bias[z];
            if (v > bv) bv = v, best = static_cast<int>(z);
        }
        return best;
    }
    return pickZone(bias);
}

void Engine::rangedDuel(Fighter& a, Fighter& d, int rangedWeapon, double distM) {
    const RangedDef& wp = d_.ranged[rangedWeapon];
    const auto& R = d_.rangedCfg;
    int ammo = wp.ammo, r = 0;
    double cur = distM;
    clock_ = 0;
    while (cur > R.meleeRange && d.fighting() && a.fighting()) {
        ++r;
        const double speed = std::max(R.minSpeed, R.closeSpeed - R.armorSpeedPen * armorPen(d));
        if (ammo > 0 && r % wp.reload == 0 && cur <= wp.range) {
            --ammo;
            a.stam -= wp.stam;
            Event s;
            s.kind = EventKind::Shoot;
            s.actor = a.id;
            s.target = d.id;
            s.ranged = rangedWeapon;
            emit(s);
            double p = wp.baseHit - wp.distK * cur + wp.skillK * (a.skill - d.skill) - R.movingPenalty;
            if (d.shield >= 0) p *= d_.shields[d.shield].guardMult + R.shieldExtra;
            if (rng_.random() < clampd(p, R.hitMin, R.hitMax)) {
                const int zone = shootZone(a, d, wp);
                const double ab = absorb(d, zone, wp.dtype, &rng_, wp.pierce);
                wound(d, zone, rng_.uniform(wp.dmg.lo, wp.dmg.hi) * (1.0 - ab));
                ++a.hits;
                statusCheck(d);
                Event e;
                e.kind = EventKind::ProjectileHit;
                e.actor = a.id;
                e.target = d.id;
                e.zone = zone;
                e.ranged = rangedWeapon;
                emit(e);
            } else {
                Event e;
                e.kind = EventKind::ProjectileMiss;
                e.actor = a.id;
                e.target = d.id;
                e.ranged = rangedWeapon;
                emit(e);
            }
        }
        cur -= speed;
        a.stam = clampd(a.stam + d_.staminaRegen, 0.0, a.stamCap);
        if (ev_) ev_->flush();
    }
    if (d.fighting() && a.fighting()) fight(a, d);
    else {
        Event e;
        e.kind = EventKind::End;
        emit(e);
        if (ev_) ev_->flush();
    }
}

// --- group battle -----------------------------------------------------------

BattleResult Engine::battle(std::vector<Fighter>& A, std::vector<Fighter>& B, bool clinch, int maxRounds) {
    if (maxRounds < 0) maxRounds = d_.core.battleMaxRounds;
    const auto& G = d_.group;
    std::set<std::pair<const Fighter*, const Fighter*>> closedPairs;
    auto key = [](const Fighter* x, const Fighter* y) { return x < y ? std::make_pair(x, y) : std::make_pair(y, x); };
    BattleResult res;
    std::vector<Fighter*> al, bl, order;
    std::map<const Fighter*, Fighter*> targets;
    std::map<const Fighter*, int> count;
    std::vector<State> before;
    for (auto& f : A) f.team = 0;
    for (auto& f : B) f.team = 1;
    int rd = 0;
    for (rd = 0; rd < maxRounds; ++rd) {
        clock_ = rd;
        for (auto& f : A) regen(f);
        for (auto& f : B) regen(f);
        al.clear();
        bl.clear();
        for (auto& f : A)
            if (f.fighting()) al.push_back(&f);
        for (auto& f : B)
            if (f.fighting()) bl.push_back(&f);
        if (al.empty() || bl.empty()) break;
        targets.clear();
        count.clear();
        for (Fighter* f : al) targets[f] = bl[rng_.below(bl.size())];
        for (Fighter* f : bl) targets[f] = al[rng_.below(al.size())];
        for (auto& [f, t] : targets) ++count[t];
        order = al;
        order.insert(order.end(), bl.begin(), bl.end());
        rng_.shuffle(order);
        for (Fighter* x : order) {
            if (!x->fighting() || x->ap < 1) continue;
            const bool inA = x->team == 0;
            Fighter* y = targets[x];
            if (!y->fighting()) {
                std::vector<Fighter*> foes;
                for (auto& f : inA ? B : A)
                    if (f.fighting()) foes.push_back(&f);
                if (foes.empty()) continue;
                y = foes[rng_.below(foes.size())];
            }
            x->ap -= 1;
            if (x->down) {
                --x->down;
                continue;
            }
            bool closed = reachOf(*x) == reachOf(*y) || closedPairs.count(key(x, y));
            if (!closed) {
                const Fighter* s = reachOf(*x) < reachOf(*y) ? x : y;
                if (rng_.random() < d_.reach.closeBase + d_.reach.closeKSkill * s->skill) {
                    closedPairs.insert(key(x, y));
                    closed = true;
                    Event e;
                    e.kind = EventKind::CloseIn;
                    e.actor = s->id;
                    emit(e);
                }
            }
            auto ci = count.find(y);
            double flank = clampd(G.flankPerExtra * ((ci == count.end() ? 1 : ci->second) - 1), 0.0, G.flankCap);
            if (d_.rankWeapon[x->weapon]) { // spear wall: neighbouring spears cover each other
                int mates = 0;
                for (auto& f : inA ? A : B)
                    if (&f != x && d_.rankWeapon[f.weapon] && f.fighting()) ++mates;
                flank += std::min(G.rankCap, G.rankBonus * mates);
            }
            if (clinch && closed && rng_.random() < d_.weapons[x->weapon].clinchShare) clinchAct(*x, *y);
            else act(*x, *y, flank + reachBonus(*x, *y, closed), true);
        }
        before.clear();
        for (auto& f : A) before.push_back(f.state);
        for (auto& f : B) before.push_back(f.state);
        for (auto& f : A) statusCheck(f);
        for (auto& f : B) statusCheck(f);
        std::size_t bi = 0;
        for (auto* team : {&A, &B}) { // losing an ally weighs on the team's morale
            int downN = 0;
            for (auto& f : *team)
                if (before[bi++] == State::Fight && !f.fighting()) ++downN;
            for (auto& f : *team)
                if (f.fighting()) {
                    f.morale -= G.moraleLossAllyDown * downN;
                    statusCheck(f);
                }
        }
        if (ev_) ev_->flush();
    }
    for (auto& f : A) res.aliveA += f.fighting();
    for (auto& f : B) res.aliveB += f.fighting();
    res.rounds = rd;
    Event e;
    e.kind = EventKind::End;
    emit(e);
    if (ev_) ev_->flush();
    return res;
}

} // namespace lr::combat
