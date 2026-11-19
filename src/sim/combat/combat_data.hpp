// Combat data (content/combat/*.json) parsed into flat tables. Every number of
// the combat core comes from here; ids are resolved to indices once at load.
// The loader is also the validator: an unknown zone, direction, guard, damage
// type, weapon, armour, shield, severity or clinch move is a load error, and so
// is a missing field. Spec: gdd/combat/COMBAT_CORE_SPEC_v0.1.md §1-2.
#pragma once

#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace altera::combat {

using OJson = nlohmann::ordered_json; // key order matters (ties, first type)

struct DataError : std::runtime_error {
    std::vector<std::string> errors;
    explicit DataError(std::vector<std::string> e);
};

struct Span {
    double lo = 0.0, hi = 0.0;
};

struct WeaponDef {
    std::string id;
    int reach = 0;
    double weight = 0.0, stamina = 0.0, speed = 1.0, durability = 0.0;
    bool indestructible = false;
    std::vector<int> dirs;         // directions it can strike, in data order
    std::vector<int> dirType;      // per direction: damage type, -1 = not a move
    std::vector<char> hasType;     // per damage type
    std::vector<Span> types;       // per damage type
    int firstType = -1;            // first damage type in data order
    double clinchShare = 0.0;      // share of actions spent on clinch moves
};

struct ArmorDef {
    std::string id;
    std::vector<char> cover;       // per zone
    std::vector<double> pen;       // per damage type (0 for impact-only types)
    std::vector<double> trans;     // per damage type
    double staminaPen = 0.0;
    double gap = 0.0;
    bool hasGap = false;           // a layer with joints: gap_stab goes around it
    double durability = 0.0;       // wear.json
};

struct ShieldDef {
    std::string id;
    double guardMult = 1.0, stamina = 0.0, weight = 0.0;
};

struct SeverityDef {
    std::string id;
    double minFrac = 0.0, pain = 0.0, bleed = 0.0;
    bool disableZone = false, heavy = false, ko = false;
};

struct ZoneDisable {
    double attackMult = 1.0, guardMult = 1.0, speedMult = 1.0;
    bool ko = false, deathAtZero = false;
};

enum class MoveKind { GrappleThrow, GapStab, PommelStrike, Disarm };

struct ClinchMove {
    std::string id;
    MoveKind kind = MoveKind::GrappleThrow;
    std::vector<double> weaponWeight;  // per weapon, 0 = cannot
    std::vector<double> baseByWeapon;  // per weapon, <0 = use base
    double base = 0.0, kSkillDiff = 0.0, stamCost = 0.0;
    double downWeightMult = 1.0;
    // grapple_throw
    double armorResistPerPen = 0.0, targetStamLoss = 0.0;
    Span fallDmg;
    int fallZone = -1;
    // gap_stab / pommel_strike
    int dtype = -1;
    double downBonus = 0.0, armorWeightMult = 0.0, force = 1.0, absorbMult = 1.0;
    std::vector<double> zoneWeight;    // per zone
    Span dmg;
};

struct RangedDef {
    std::string id;
    double range = 0.0, baseHit = 0.0, distK = 0.0, skillK = 0.0, pierce = 0.0, stam = 0.0;
    int dtype = -1, reload = 1, ammo = 0;
    Span dmg;
};

struct Treatment {
    std::string id;
    double pStop = 0.0, infection = 0.0, recoverMult = 1.0, permMult = 1.0;
};

struct CombatData {
    // --- ids -------------------------------------------------------------
    std::vector<std::string> zones, directions, guards, dtypes;
    std::vector<WeaponDef> weapons;
    std::vector<ArmorDef> armors;
    std::vector<ShieldDef> shields;
    std::vector<SeverityDef> severity;
    std::vector<ClinchMove> moves;
    std::vector<RangedDef> ranged;
    std::vector<Treatment> treatments;

    // --- combat_config.json ----------------------------------------------
    double roundSeconds = 1.5;
    std::vector<double> zoneWeight;
    std::vector<std::array<int, 2>> counter;           // per direction: [perfect, near] guard
    std::vector<std::vector<double>> dirZoneBias;      // [direction][zone]
    struct {
        double base, kSkill, kKnow, kFatigue, kPanic, min, max;
    } read{};
    struct {
        double base, kSkillDiff, perfect, nearMult, none, min, max;
    } hit{};
    double staminaMax = 100.0, staminaRegen = 0.0;
    struct {
        double start, fleeBelow, yieldBelow, lossPerHit, lossHeavy;
    } morale{};
    double painThreshold = 100.0, koZoneMult = 1.0;
    double bloodMax = 100.0;
    double aimBiasExp = 1.0, aimSharpness = 1.0;
    struct {
        double hitBonusPerLevel, closeBase, closeKSkill, clinchBonusPerLevel, clinchLongPenaltyPerLevel;
    } reach{};
    struct {
        double base, kSkill, hitBonus;
    } riposte{};
    struct {
        double flankPerExtra, flankCap, moraleLossAllyDown, rankBonus, rankCap;
    } group{};
    // "core": numbers the prototype kept inline
    struct {
        double defaultKnow, attackArmorStamina, regenArmorStamina, clinchArmorStamina, fatigueFloor;
        double exhaustedBelow, waveringMargin, bleedPerRound, aimFloor, pierceImmunePen;
        double clinchMin, clinchMax, firstMover;
        int duelMaxRounds, battleMaxRounds;
        int unarmed;            // weapon index
        int painScaleSeverity;  // severity index
        double indestructible;
    } core{};
    std::vector<char> impactOnly;   // per damage type
    std::vector<double> kImpact;    // per damage type
    std::vector<char> rankWeapon;   // per weapon: spear wall
    std::vector<double> bleedRate;  // per severity

    // --- wounds.json -----------------------------------------------------
    std::vector<double> zoneHp;
    std::vector<ZoneDisable> disable; // per zone

    // --- clinch_moves.json -----------------------------------------------
    int downRounds = 0;
    double armorWeightFactor = 0.0;

    // --- ranged_weapons.json ---------------------------------------------
    struct {
        double closeSpeed, armorSpeedPen, meleeRange, movingPenalty, shieldExtra, minSpeed, hitMin, hitMax;
        int aimDirection;
    } rangedCfg{};

    // --- wear.json -------------------------------------------------------
    struct {
        double perAttack, perParry, vsHardArmor, hardArmorPen, minDmgMult;
        double armorPerHit, armorMinEff;
    } wear{};

    // --- wounds.json treatment -------------------------------------------
    struct {
        std::vector<double> recoverDays;  // per severity
        std::vector<double> perm;         // per severity
        double infectionExtraDays = 0.0, infectionDeath = 0.0, bleedDeathUntreatedDeep = 0.0;
    } treat;

    // --- lookups (throw std::out_of_range on an unknown id) --------------
    int weapon(const std::string& id) const { return find(weaponIx, id, "weapon"); }
    int armor(const std::string& id) const { return find(armorIx, id, "armor"); }
    int shield(const std::string& id) const { return find(shieldIx, id, "shield"); }
    int zone(const std::string& id) const { return find(zoneIx, id, "zone"); }
    int rangedWeapon(const std::string& id) const { return find(rangedIx, id, "ranged weapon"); }
    int treatment(const std::string& id) const { return find(treatIx, id, "treatment"); }
    int sev(const std::string& id) const { return find(sevIx, id, "severity"); }
    bool hasWeapon(const std::string& id) const { return weaponIx.count(id) > 0; }

    // Raw animations.json (presentation layer reads it; validated for keys it uses).
    OJson animations;

    // files: name without extension -> parsed JSON (combat_config, weapons, ...)
    static CombatData fromJson(const std::map<std::string, OJson>& files);
    static CombatData load(const std::string& dir);  // content/combat
    static std::map<std::string, OJson> readFiles(const std::string& dir);

    std::map<std::string, int> weaponIx, armorIx, shieldIx, zoneIx, dirIx, guardIx, dtypeIx, sevIx, rangedIx, treatIx;

private:
    static int find(const std::map<std::string, int>& m, const std::string& id, const char* what);
};

} // namespace altera::combat
