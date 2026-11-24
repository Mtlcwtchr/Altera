#pragma once
// Crafting content (content/ai/craft/*.json, gdd crafting design v0.2): what
// materials are, what processes do to them, which functions a made thing has
// and which archetype (form) it is. Code is the interpreter (craft.hpp).

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/craft/craft_expr.hpp"

namespace altera::sim::craft {

// The ~16 numeric properties of §2.1, plus derived ones the data may give a
// material directly (warmth, cover, waterproof).
const std::vector<std::string>& propNames();
bool isProp(const std::string& n);

struct EffectDef {
    std::string id;
    double k = 0.0;
    std::vector<std::string> extract; // methods that pull it out; empty = any
};

struct MatStateDef {
    std::string id;
    Expr when;            // empty: only set by a process (set_state)
    bool temp = false;    // re-evaluated each step (hot iron), never stored
    std::map<std::string, double> add; // property deltas while in the state
};

struct HeatReactDef {
    std::string id;
    int at = 1;                       // heat level 0..4
    bool noAir = false;               // only without air (mode with no_air)
    bool needsDry = false;            // moisture < config.dry_below
    std::vector<std::string> needs;   // tags any co-input (or the item) must carry
    std::vector<std::string> modes;   // only in these heat modes; empty = any
    Expr when;
    std::string to;                   // becomes this material
    std::string toState;              // enters this state
    std::map<std::string, double> set;// absolute property values
    std::string result;               // destroys the item into this salvage material
};

struct RefineDef {               // the material turns into another during a process
    std::string process;
    Expr when;
    std::string to;
};

struct MaterialDef {
    std::string id;
    std::vector<std::string> tags;
    std::map<std::string, double> p;
    std::vector<MatStateDef> states;
    std::vector<HeatReactDef> heat;
    std::vector<RefineDef> refine;
    std::vector<EffectDef> effects;
    double potency = 1.0;
    std::string shape = "lump";   // the form it comes in raw
    int len = 1, thick = 1;
    std::string salvage;          // what a ruined piece leaves; empty = the material itself
    std::string item;             // items.json id this material comes as (source in the world)
    bool raw = true;              // gathered, not made (validator wants a source)
    double worth = 1.0;           // value component (1 = ordinary), see itemWorth()
};

struct ShapeDef {
    std::string id;
    std::string role;             // the part's role in an assembly (blade, handle, ...)
    std::string stage = "part";   // raw | part | finished
    int lenMin = 1, lenMax = 1, lenDefault = 1;
    int thick = 1;
    std::map<std::string, double> v; // cover, capacity, grip, ... (shape.<name> in expressions)
    double worth = 1.0;              // value component of a piece of this shape
};

struct SlotDef {
    std::string name;
    std::vector<std::string> shapes; // any-of; "any" = all
    std::vector<std::string> tags;   // any-of material/item tags
    std::map<std::string, Constraint> p;
    std::map<std::string, std::string> pSrc;
    bool either = false;             // shape OR tag matches (default: both lists that are given)
    bool optional = false;
    int min = 1, max = 1;
};

struct FlawRule {
    std::string id;
    Expr p;
    Expr when;
};

struct RiskDef {
    std::string id;
    double p = 0.0;
    std::vector<std::string> eff; // sim effect strings ("body:health-0.03")
};

struct AssembleRule {
    std::string a, b; // shapes; "any" matches all
    bool binder = false;
    std::string to;   // result shape
};

enum class ProcOp { Shape, Heat, Blend, Join, Soak };

struct ProcessDef {
    std::string id;
    double worth = 1.0; // value component of having been worked this way (dye, engrave, ...)
    std::string skill;
    std::vector<std::pair<std::string, std::string>> skillBy; // work piece tag -> skill
    ProcOp op = ProcOp::Shape;
    std::vector<std::string> tools;    // any-of tool tags (empty = bare hands)
    std::vector<std::string> stations; // any-of station tags (empty = anywhere)
    int heatMin = 0;
    bool clean = false;
    Expr toolHardness;                 // the tool must be at least this hard (over the work piece)
    std::vector<SlotDef> slots;
    int time = 1;
    double diffBase = 0.3;
    Expr diffAdd;
    std::map<std::string, std::vector<std::string>> shapeMap;
    std::vector<AssembleRule> assemble;
    std::vector<std::pair<std::string, Expr>> transform; // prop -> new value (x = old)
    std::string setState;
    std::string toMaterial;
    std::string extract;              // extraction method this process applies
    std::vector<std::string> marks;   // profile tags added
    std::vector<std::string> fixes;   // flaws a step of q >= 0.5 removes
    bool reacts = false;
    bool risky = false;               // catastrophes possible
    std::vector<FlawRule> flaws;
    std::vector<RiskDef> risks;
    std::vector<std::string> modes;
        std::vector<std::string> noAirModes;
    bool ordered = false;
    double toolWear = 0.01;
    std::vector<std::string> needs; // sim effects on the actor per step
    std::string fsm = "Work";
    std::vector<std::string> chainIn, chainOut; // hints (§16.1)
};

struct ReactDef {
    std::string id;
    std::vector<std::string> when; // tag | tag|tag | no_air | no_yeast | eff:x | prop>=v | heatN
    std::vector<std::pair<std::string, Constraint>> propConds;
    int atHeat = 0;
    std::string make;              // material added to the profile (tags, effects)
    bool replace = false;          // the item becomes the made material
    std::map<std::string, Expr> p; // property transforms after the reaction
    std::string extract;           // extraction method it applies (tincture: all)
    double riskTainted = 0.0;
    std::string ratioMat;          // alloys: mass share of ratioMat must be in ratio
    Constraint ratio;
    bool hasRatio = false;
};

struct StationDef {
    std::string id;
    std::vector<std::string> tags;
    int tempMax = -1;      // -1: no heat
    double qualityBonus = 0.0;
    double even = 0.5;
    double cleanDefault = 1.0;
    std::vector<std::string> needs;
    bool noAir = false;
};

struct FlawDef {
    std::string id;
    bool visible = true;
    std::string fix;
    std::map<std::string, double> mods; // property or function -> multiplier
    std::string salvage;
    double worth = 1.0;                 // value component (< 1: the flaw makes it cheaper)
};

struct FunctionDef {
    std::string id;
    Expr f;
};

struct FormNeed {
    std::string key;   // a function, property or any expression ("blade.len")
    Expr expr;
    Constraint c;
};

struct FormDef {
    std::string id;
    std::string category;
    std::vector<std::string> shape;    // item or any of its parts has one of these
    std::vector<std::string> mat;      // main material id or tag
    std::vector<std::string> state;
    std::vector<std::string> profile;  // each entry "a|b": any present among the profile tags
    std::vector<std::string> effects;  // each entry "a|b": any effect >= config.effect_present
    std::vector<FormNeed> needs;
    bool named = true;                 // name with the material ("iron sword") or plain ("bread")
    std::string item;                  // items.json id the sim counts it as (empty: none)
    double worth = 1.0;                // value component of the archetype itself
    // derive (§5, §10): numbers for the combat core and the sim.
    std::string kind;
    std::map<std::string, std::string> deriveStr; // weapon_id, layer, ...
    std::vector<std::string> deriveTags;
    std::vector<std::pair<std::string, Expr>> derive;
};

struct ImprovisedDef {
    std::string id;
    std::string fn; // function it is named after; "any" = fallback
};

// One step of a starter design / recipe (§7, §17): what goes into each slot.
struct DesignInput {
    std::string ref;   // "mat:<id>", "form:<id>", "shape:<id>", "tag:<t>", "step:<n>" (output of step n)
    int count = 1;
};
struct DesignStep {
    std::string process;
    std::map<std::string, std::vector<DesignInput>> slots;
    std::string shape;  // target shape
    int len = 0;
    std::string mode;
    int heat = -1;
};
struct DesignDef {
    std::string id;     // usually the archetype it makes
    std::string form;   // expected result form
    std::vector<DesignStep> steps;
    std::vector<std::string> tools;    // tool items/forms the design assumes (tag:x)
    std::vector<std::string> stations; // station ids it assumes
};

struct CraftConfig {
    double k = 1.2, noise = 0.15;
    double tCatastrophe = -0.45, tRuin = -0.15, tOk = 0.1, tGood = 0.3, tMaster = 0.55;
    double wTool = 0.15, wStation = 0.15, wKnow = 0.15, wTrait = 0.1, repeatBonus = 0.1;
    double tierCrude = 0.3, tierGood = 0.6, tierMaster = 0.85;
    double tau = 0.6, falloff = 0.25, improvisedMin = 0.25, effectPresent = 0.08;
    double salvageMin = 0.3, salvageMax = 0.6;
    double xpRate = 0.02, xpWindow = 0.25, xpFailRatio = 0.4;
    double knowGain = 0.05;
    double novice = 0.05;      // skill of someone who never learned it
    double toolQualityBare = 0.3;
    double dryBelow = 0.15;
    double massUnit = 1.0;
    double stepQualityMasterBonus = 0.08;
    // worth (value components, AL-87): any def may carry "worth" (a multiplier, 1 = ordinary);
    // tags carry it through worth.tags. A made thing's worth multiplies its trade value.
    double worthAvg = 0.7, worthMax = 0.3;   // parts: weighted mass average + share of the finest part
    double worthMin = 0.05, worthCap = 100.0;
    std::map<std::string, double> tagWorth;  // tag -> worth (material tags, marks, item tags)
    // recipe book (§17)
    double masterThreshold = 0.85;
    int masterSuccesses = 5, fastCraftMaxStages = 12, fragmentsPerSuccess = 1;
    double masteryPerSuccess = 0.12, teachShare = 0.8;
    // breakthrough (§18)
    double brExpert = 0.015, brNovice = 0.001, brCap = 0.05, brNoveltyBoost = 2.0, brQBonus = 0.1;
    int brCooldownDays = 14;
    int maxInputsPerStep = 6, maxPartsPerItem = 8;
    bool goapDesigns = true; // starter designs become GOAP actions (craft:<design>)
    std::map<std::string, std::map<std::string, double>> traitMods; // trait -> quality/speed/risk/xp/breakthrough
    std::map<int, double> heatC;
};

struct CraftData {
    std::map<std::string, MaterialDef> materials;
    std::map<std::string, ShapeDef> shapes;
    std::map<std::string, ProcessDef> processes;
    std::vector<std::string> processOrder;
    std::vector<FunctionDef> functions;          // evaluated in order
    std::map<std::string, std::string> blend;    // prop -> avg_mass | max | min | sum_cap | product
    std::vector<std::string> effStack, effDominant;
    std::vector<std::pair<std::string, std::string>> effCancel;
    std::vector<FormDef> forms;                  // order breaks ties
    std::vector<ImprovisedDef> improvised;
    std::vector<ReactDef> reacts;
    std::map<std::string, StationDef> stations;
    std::map<std::string, FlawDef> flaws;
    std::vector<DesignDef> designs;
    CraftConfig cfg;
    std::vector<std::string> errors, warnings;
    bool loaded = false;

    const MaterialDef* material(const std::string& id) const;
    const ShapeDef* shape(const std::string& id) const;
    const ProcessDef* process(const std::string& id) const;
    const FormDef* form(const std::string& id) const;
    const StationDef* station(const std::string& id) const;
    const FlawDef* flaw(const std::string& id) const;
    const DesignDef* design(const std::string& id) const;
    bool isFunction(const std::string& id) const;

    // content/ai/craft; a missing directory gives an empty, not-loaded set.
    static CraftData load(const std::string& dir);
    static CraftData fromJson(const Json& materials, const Json& shapes, const Json& processes,
                              const Json& functions, const Json& forms, const Json& flaws, const Json& reacts,
                              const Json& stations, const Json& config, const Json& designs);
};

// Every loc key the crafting content needs in the base language.
std::vector<std::string> craftLocKeys(const CraftData& d);

} // namespace altera::sim::craft
