#pragma once
// LR-141: the stats of a person (gdd/stats/STATS_SKILLS_v0.3.md).
//
// Stats are the state of body, senses, mind and spirit, 0..1 smooth (0.5 = an
// average grown human; above 1 = superhuman). Skills run beside them and do not
// follow from them. Everything is data:
//
//   content/ai/stats/stats.json   the stats, growth by divisions, skill decay, talents,
//                                 how a missing stat is filled, the age curve
//   content/ai/stats/states.json  shifts of the current value by the person's state
//
// A person keeps the base (Character::attrs). The current value
//   (base + kind + trait attr_mod) * age curve + state shifts, then state mults
// is cached in Character::attrsNow and is what formulas read as attr.<id>
// (attr_base.<id> reads the base).

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/expr.hpp"

namespace lr::sim {

struct Character;
struct Content;
class World;
struct ValidationReport;

struct GrowthCurve {
    double softStart = 0.8, step = 0.1, perStepMult = 0.5;
    // The multiplier of a gain at value v: 1 up to softStart, then perStepMult per
    // division, smooth inside a division. No hard cap.
    double mult(double v) const;
    // The number of divisions v is past softStart (0 below it).
    double divisions(double v) const;
};

struct StatDef {
    std::string id, group, nameKey, descKey;
};

struct StatStateMod {
    std::string id;
    Expr when, scale;
    std::map<std::string, double> add, mult;
};

struct StatRules {
    int version = 2;
    std::vector<StatDef> stats;
    std::vector<std::string> groups;
    GrowthCurve statGrowth, skillGrowth;
    struct Decay {
        double perYear = 0.01, floor = 0.15, overSoftMult = 1.6;
        int graceDays = 60;
    } skillDecay;
    struct Talents {
        double sd = 0.3, lo = 0.35, hi = 2.2;
    } talents;
    std::map<std::string, Expr> fill; // stat -> formula; "" = the default
    Expr fillDefault;
    double clampLo = 0.02, startHi = 0.95;
    struct AgePoint {
        double age = 0;
        std::map<std::string, double> mult;
    };
    std::vector<AgePoint> ageCurve;
    std::vector<StatStateMod> mods;
    int refreshTicks = 10;
    std::map<std::string, std::string> renameSkills; // old save id -> new id
    std::vector<std::string> errors;

    bool has(const std::string& id) const;
    std::vector<std::string> ids() const;
};

// The rules of the content (cached per data generation); null when the content has no stats.json.
const StatRules* statRules(const Content& c);
// The ids of stats.json, or empty (the content keeps expansion attributes.json).
std::vector<std::string> statIdsOf(const std::string& contentAiDir);

// Fills stats the person lacks (deterministic from the world seed and id).
void fillMissingStats(const Content& c, std::uint64_t seed, Character& ch);
// Recomputes Character::attrsNow from the base, kind, traits, age and state.
void refreshStats(const Content& c, Character& ch);
// Age multiplier of a stat at human-measure age `years`.
double statAgeMult(const StatRules& r, const std::string& stat, double years);

// The person's personal talent for a skill (speed of learning, not a cap): by the
// skill's category, rolled once from the id. 1 without rules.
double skillTalentOf(const Content& c, EntityId id, const std::string& skill);
// Practice of a skill: `gain` (the act's raw number) slowed by the growth curve past
// soft start (the kind's aptitude cap moves soft start), sped by the talent and the
// kind's learn multiplier. Returns the new level. No upper cap.
double practiceSkill(const World& w, Character& c, const std::string& skill, double gain);
// Training of a stat's base (an act's attr:<id>+x): slowed by the growth curve on the
// base plus the kind's shift.
double trainStat(const World& w, Character& c, const std::string& stat, double gain);

// The world's step: fills, refreshes attrsNow every refresh_ticks, forgets unused skills daily.
void statsTick(World& w);

// Load-time checks (stats.json / states.json against the content).
void validateStats(const Content& c, ValidationReport& r);

} // namespace lr::sim
