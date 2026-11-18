#pragma once
// Declarative content (content/ai/*.json, doc/design/AI_DATA_SCHEMA.md). Code is
// the interpreter; everything that shapes behaviour is loaded from here.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/rules.hpp"

namespace altera::sim {

struct ItemDef {
    std::string id;
    std::vector<std::string> tags;
    double nutrition = 0.0;
};

struct ObjectDef {
    std::string id;
    std::map<std::string, int> stock;
    std::map<std::string, int> maxStock;
    std::map<std::string, int> regrow;
    Tick regrowEvery = 0;
    std::vector<std::string> biomes;
    double perKm2 = 0.0;
    bool owned = false;
    bool settlement = false;
    int perFamily = 0;
    int perPerson = 0;
    bool warmth = false;
};

struct NeedDef {
    std::string id;
    double decayPerDay = 0.5;
    double critical = 0.1;
    double startLo = 0.6, startHi = 0.9;
    double nightMult = 1.0;
    std::map<std::string, double> biomeMult;
};

struct MoodletDef {
    std::string id;
    double value = 0.0;
    Tick ttl = 72;
};

struct TraitDef {
    std::string id;
    std::map<std::string, double> goalMult;
    std::map<std::string, double> needMult;
    std::map<std::string, double> legalPenalty;
    std::map<std::string, double> methodBias;
    double hysteresisMult = 1.0;
};

// --- Layer 1: Utility -------------------------------------------------------
struct Curve {
    std::string type = "linear";
    double m = 1.0, b = 0.0, k = 10.0, x0 = 0.5, p = 2.0, lo = 0.0, hi = 1.0, v = 1.0;
    double eval(double x) const;
};

struct Consideration {
    std::string input; // need:hunger, time:night, body:health, has:meal, home:meal, mood, const
    double norm = 10.0;
    Curve curve;
    double weight = 1.0;
};

struct GoalDef {
    std::string id;
    std::string task;
    std::string priorityClass = "need";
    std::string aggregate = "product";
    std::vector<Consideration> considerations;
    double base = 1.0;
    Tick minTicks = 3;
    bool hasInterrupt = false;
    Condition interrupt;
    Tick cooldownOnFail = 12;
};

// --- Layer 2: HTN -----------------------------------------------------------
struct HtnStep {
    enum class Kind { Do, Achieve, Task } kind = Kind::Do;
    std::string ref;                 // interaction id (Do) or task id (Task)
    std::vector<Condition> achieve;  // Achieve
    std::vector<std::string> allow;  // Achieve: interaction whitelist (empty = default set)
};

struct HtnMethod {
    std::string id;
    std::vector<Condition> pre;
    double cost = 1.0;
    std::vector<std::string> legal;
    std::vector<HtnStep> steps;
};

struct HtnTask {
    std::string id;
    std::vector<HtnMethod> methods;
};

// --- Layers 3/4: interaction catalog, FSM ----------------------------------
struct InteractionDef {
    std::string id;
    std::string category;
    std::string target = "self"; // self | object:<type> | character
    std::string targetType;      // object type when target is object:<type>
    std::vector<Condition> pre;
    std::vector<Effect> eff;
    bool fromTarget = false;
    Tick duration = 1;
    double cost = 1.0;
    std::string fsm = "Interact";
    bool reserve = false;
    bool goap = true;
    std::vector<std::string> legal;
    std::string visibility = "public";
    Json social;
    std::vector<std::string> speech;
    bool continuous = false;
    bool hasUntil = false;
    Condition until;
    bool targetsObject() const { return target.rfind("object:", 0) == 0; }
};

struct FsmStateDef {
    std::string id;
    std::string parent;
    int interruptible = -1; // -1 = inherit from parent (root default: true)
    std::vector<std::string> wakeOn;
};

struct AiConfig {
    int topK = 3;
    double hysteresisBonus = 0.15;
    double switchMargin = 0.1;
    double failPenalty = 0.3;
    int htnMaxDepth = 4;
    int methodMaxFails = 3;
    Tick methodFailTicks = 72;
    int goapMaxDepth = 10;
    int goapMaxNodes = 2000;
    Tick reserveTicks = 36;
    double walkMPerTick = 800.0;
    double interactionRangeM = 15.0;
    double legalBase = 4.0;
    int traceSize = 64;
    double modMaxAbsDelta = 0.5;
    Tick modMaxTtl = 288;
    Tick playerIdleResumeTicks = 18; // free-will resume for a paused plan under PlayerController
    Tick playerGoalHoldTicks = 36;   // AI keeps a player-chosen goal this long after a switch
    double playerGoalHoldBonus = 0.3;
    double hintDecayTicks = 72;      // player hints become decaying weights after a switch
    int memoryCap = 200;
    std::map<std::string, double> memoryTypes;
};

struct Content {
    std::map<std::string, ItemDef> items;
    std::map<std::string, ObjectDef> objects;
    std::vector<NeedDef> needs; // ordered as in the file
    std::map<std::string, MoodletDef> moodlets;
    std::map<std::string, TraitDef> traits;
    std::vector<std::pair<std::string, std::string>> exclusiveTraits;
    std::vector<GoalDef> goals; // ordered as in the file
    std::map<std::string, HtnTask> tasks;
    std::map<std::string, InteractionDef> interactions;
    std::map<std::string, FsmStateDef> fsm;
    AiConfig config;
    std::vector<std::string> warnings;

    const NeedDef* need(const std::string& id) const;
    const GoalDef* goal(const std::string& id) const;
    const InteractionDef* interaction(const std::string& id) const;
    const HtnTask* task(const std::string& id) const;
    bool fsmInterruptible(const std::string& state) const;
    std::string fsmRoot(const std::string& state) const;

    // Throws std::runtime_error with every problem found, if any.
    static Content load(const std::string& dir);
    static Content fromJson(const Json& items, const Json& objects, const Json& needs, const Json& traits,
                            const Json& goals, const Json& htn, const Json& interactions, const Json& fsm,
                            const Json& config);
};

struct ValidationReport {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok() const { return errors.empty(); }
};

ValidationReport validate(const Content& c);

} // namespace altera::sim
