// Combat core: round loop of melee duels, clinch, ranged approach, group
// battles, wounds, wear. A port of gdd/combat/tools/combat_sim.py (the
// reference); behaviour, quirks included, follows it, every number comes from
// CombatData. Deterministic: one Rng per combat, the same seed gives the same
// event log byte for byte. No rendering: the output is a stream of events.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sim/combat/combat_data.hpp"
#include "sim/combat/combat_rng.hpp"

namespace altera::combat {

enum class State : std::uint8_t { Fight, Ko, Dead, Yield, Flee };
const char* stateId(State s);

enum class Outcome : std::uint8_t { Hit, Parry, Block, BlockShield, Dodge };
const char* outcomeId(Outcome o);

enum class EventKind : std::uint8_t {
    Attack, Guard, Result, Wound, Riposte, CloseIn, Clinch, GetUp, Exhausted, Wavering, LimpOrCradle, State,
    Shoot, ProjectileHit, ProjectileMiss, WeaponBreak, End
};
const char* eventId(EventKind k);

// One combat event (COMBAT_PRESENTATION.md §2). Ids are indices into
// CombatData; -1 = not set. Strings never live here: the client and the
// chronicle turn ids into loc keys.
struct Event {
    int round = 0;
    EventKind kind = EventKind::Attack;
    std::int64_t actor = -1, target = -1;
    int weapon = -1, direction = -1, zone = -1, dtype = -1, guard = -1, severity = -1, move = -1, ranged = -1;
    State state = State::Fight;
    Outcome outcome = Outcome::Hit;
    double dmg = 0.0, frac = 0.0, absorbed = 0.0;
    double skill = 0.0, stamina = 0.0, pain = 0.0, blood = 0.0, morale = 0.0;
    std::uint32_t zones = 0; // bit per zone (limp_or_cradle)
    // flags
    bool riposte = false, correct = false, nearGuard = false, shield = false, armored = false, success = false;
    int shieldId = -1;
};

// Writes one event as a compact JSON line with data ids (fixed number format).
std::string eventLine(const CombatData& d, const Event& e);

// Round batches go to subscribers (client, chronicle, NPC memory, test log).
class EventStream {
public:
    using Subscriber = std::function<void(const std::vector<Event>& roundBatch)>;
    void subscribe(Subscriber s) { subs_.push_back(std::move(s)); }
    void push(const Event& e) { batch_.push_back(e); }
    void flush() {
        if (batch_.empty()) return;
        for (auto& s : subs_) s(batch_);
        batch_.clear();
    }
    bool active() const { return !subs_.empty(); }

private:
    std::vector<Subscriber> subs_;
    std::vector<Event> batch_;
};

struct FighterSpec {
    double skill = 50.0;
    std::string weapon = "sword";
    std::vector<std::string> armor;
    double know = -1.0;      // <0 = core.default_know
    std::string shield;      // empty = none
};

struct Fighter {
    std::int64_t id = 0;
    int team = 0;
    double skill = 50.0, know = 0.3;
    int weapon = -1, shield = -1;
    std::vector<int> armor;
    std::vector<double> armorCond; // per layer, 1..0
    std::vector<double> hp;        // per zone
    double stam = 100.0, blood = 100.0, pain = 0.0, morale = 100.0, bleed = 0.0, ap = 0.0;
    int down = 0;
    double weaponCond = 1.0;
    int broke = 0, hits = 0;
    State state = State::Fight;
    std::uint32_t disabled = 0;   // bit per zone
    std::vector<int> wounds;      // severity index per wound, in order
    std::vector<int> woundZone;   // zone of each wound (parallel to wounds)
    std::vector<double> woundDmg; // zone hp each wound took

    bool fighting() const { return state == State::Fight; }
};

struct BattleResult {
    int aliveA = 0, aliveB = 0, rounds = 0;
};

class Engine {
public:
    Engine(const CombatData& d, std::uint64_t seed, EventStream* events = nullptr)
        : d_(d), rng_(seed), ev_(events) {}

    Fighter make(const FighterSpec& s, std::int64_t id = 0, int team = 0) const;

    // 1 on 1 melee until someone leaves the fight. Returns rounds played.
    int fight(Fighter& a, Fighter& b, bool useReach = true, bool clinch = true, int maxRounds = -1);
    // Shooter fires at a target that runs in from `distM` metres, then melee.
    void rangedDuel(Fighter& shooter, Fighter& target, int rangedWeapon, double distM);
    // N on M.
    BattleResult battle(std::vector<Fighter>& A, std::vector<Fighter>& B, bool clinch = true, int maxRounds = -1);

    Rng& rng() { return rng_; }
    const CombatData& data() const { return d_; }

    // --- exposed for tests and the statistical (LOD) layer ---------------
    double read(const Fighter& f) const;
    double fatigue(const Fighter& f) const { return 1.0 - f.stam / d_.staminaMax; }
    double panic(const Fighter& f) const { return 1.0 - f.morale / d_.morale.start; }
    double armorPen(const Fighter& f) const;
    int reachOf(const Fighter& f) const { return d_.weapons[f.weapon].reach; }
    // Share of force the armour stops on a zone; rng = roll gaps, nogap = skip jointed layers.
    double absorb(const Fighter& f, int zone, int dtype, Rng* rng = nullptr, double pierce = 0.0, bool nogap = false) const;

private:
    void emit(Event e);
    void act(Fighter& a, Fighter& d, double bonus, bool allowRiposte);
    void clinchAct(Fighter& a, Fighter& d);
    int clinchPick(const Fighter& a, const Fighter& d);
    void takeAction(Fighter& x, Fighter& y, bool closed, bool useReach, bool clinch);
    void wound(Fighter& d, int zone, double dmg);
    void statusCheck(Fighter& f);
    double hitChance(const Fighter& a, const Fighter& d, int guard, int dir, double bonus) const;
    double reachBonus(const Fighter& x, const Fighter& y, bool closed) const;
    double weaponMult(const Fighter& f) const;
    void wearWeapon(Fighter& f, double cost);
    void wearArmor(Fighter& f, int zone);
    void regen(Fighter& f);
    int pickZone(const std::vector<double>& bias);
    int shootZone(const Fighter& a, const Fighter& d, const RangedDef& w);
    bool guardCovers(int guard, int dir) const {
        return guard >= 0 && (guard == d_.counter[dir][0] || guard == d_.counter[dir][1]);
    }

    const CombatData& d_;
    Rng rng_;
    EventStream* ev_;
    int clock_ = 0;
};

} // namespace altera::combat
