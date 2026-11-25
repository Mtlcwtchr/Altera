#pragma once
// Declarative content (content/ai/*.json, doc/design/AI_DATA_SCHEMA.md). Code is
// the interpreter; everything that shapes behaviour is loaded from here.

#include <map>
#include <string>
#include <vector>

#include "sim/species.hpp"
#include "sim/core.hpp"
#include "sim/craft/craft_data.hpp"
#include "sim/rules.hpp"

namespace lr::sim {

struct ItemDef {
    std::string id;
    std::vector<std::string> tags;
    double nutrition = 0.0;
    double warmth = 0.0;    // clothing: slows warmth decay while carried (needs.clothing_mult)
    double toolWear = 0.0;  // chance per use that the tool breaks (stage 2)
    double spoilDays = 0.0; // 0 = keeps
    std::string material;   // crafting material it is (content/ai/craft/materials.json); empty = none
    double value = 0.0;     // AL-74: base worth in coin of one plain unit (trade, gifts); 0 = not priced
    double quality = 0.0;   // AL-74: how fine a thing it is 0..1 (0.2 plain fare, 0.5 good, 0.8+ festive)
};

// objects.json `crop`: a field sown from `seed` in a day-of-year window,
// ripening `grow_days` later (farming.cpp). Field state lives in the object's
// stock under kCropKeys; `strips` is the size, set per household by worldgen.
struct CropDef {
    std::string seed;                // item sown (kept apart from food in the store)
    std::string produce = "sheaf";   // what reaping gives
    int seedPerStrip = 10;           // seed units per strip
    int grainPerSheaf = 10;          // seed-equivalent units per sheaf (threshing)
    int sowFrom = 0, sowTo = 0;      // sowing window, day of year (inclusive)
    int growDays = 120;              // from sowing to ripe
    double yieldLo = 3.0, yieldHi = 5.0; // harvest per seed: poorest .. best soil, no stress
    // Stress from the field's own weather (weather.cpp, farming.cpp):
    double droughtAt = 0.4;          // soil water below this share of capacity dries the crop
    double waterlogAt = 1.08;        // above this the roots drown
    double droughtLoss = 1.6;        // yield lost per unit of season-average water stress
    double frostC = -2.0;            // a growing crop is hurt below this
    double frostLoss = 600.0;        // frost units (0.1 degC-days below frost_c) that cost half the crop
    double wetHarvestMm = 6.0;       // rain on a standing ripe crop above this sprouts and lodges it
    int weedsEvery = 12;             // days between weed growth
    double weedLoss = 0.0015;        // yield lost per weed-day above the first weed
    int spoilAfter = 15;             // days a ripe crop stands before it starts to shatter
    double spoilPerDay = 0.05;       // share of the standing crop lost per day after that
    bool staple = true;              // the household's bread: last year's stores and the harvest day
    std::string seedFrom = "grain";  // what the seed is put aside from (1:1)
    double stripsLo = -1.0, stripsHi = -1.0; // strips per person (negative: worldgen config)
};
bool isCropKey(const std::string& key);
struct Content;
// Subject of a farm-like condition: "farm" -> "field", a crop object type ->
// itself, anything else -> empty (farming.cpp).
std::string cropSubjectType(const Content& ct, const std::string& subject);
// Keys of herd:<key> (animals.cpp).
bool isHerdKey(const Content& ct, const std::string& key);

// climate.json: the seasons' weather of the region and the soils by biome.
// Daily weather is drawn from it per settlement (weather.cpp).
struct SeasonClimate {
    double tempC = 10.0, tempSd = 3.0; // mean and spread at the season's middle
    double rainMm = 2.0;               // mean rain per day
    double wetChance = 0.4;            // share of days with rain
};
struct SoilDef {
    double fertility = 0.7, depth = 0.7, drainage = 0.5; // 0..1
    double capacityMm = 120.0;                           // water the root zone holds
};
struct ClimateDef {
    bool loaded = false;
    SeasonClimate seasons[4]; // spring, summer, autumn, winter
    std::map<std::string, double> biomeRainMult;
    std::map<std::string, double> biomeTempAdd;
    std::map<std::string, SoilDef> soils; // by biome; "default" for the rest
    // AL-115: the prevailing wind (direction it blows from, degrees clockwise from north = -y) and its strength
    double windFromDeg = 250.0, windMs = 4.0, windSd = 2.0;
    const SoilDef& soil(const std::string& biome) const;
};

// care.json: dependents (who needs looking after) and how much the carer's
// circle counts (care.cpp).
struct DependentDef {
    std::string id;
    int ageLt = -1, ageGe = -1;    // age in years; -1 = not used
    double healthLt = -1.0;        // -1 = not used
    std::string trait;             // empty = not used
    double weight = 0.5;           // how much their need weighs on a carer
    std::vector<std::string> needs; // the needs others see to
};
struct CareDef {
    double charityDays = 20.0;  // a neighbour's store below this many days per member is short
    double charitySpare = 60.0; // a giver keeps this many days of food for its own household
    std::vector<DependentDef> dependents;
    std::map<std::string, double> circles; // household, kin, neighbour
    double threshold = 0.55;               // a need below this starts to count
};

struct ObjectDef {
    std::string id;
    std::string station; // crafting station it is (content/ai/craft/stations.json); empty = none
    bool hasCrop = false;
    CropDef crop;
    std::map<std::string, int> stock;
    std::map<std::string, int> maxStock;
    std::map<std::string, int> regrow;
    Tick regrowEvery = 0;
    std::vector<std::string> biomes;
    double perKm2 = 0.0;
    double standM = 0.0;   // AL-78: how far from its middle one stands to work at it (the well's rim, a trunk)
    bool owned = false;
    bool settlement = false;
    int perFamily = 0;
    int perPerson = 0;
    bool warmth = false;
    bool shelter = false;              // a roof: needs.sheltered_mult applies nearby
    int perSettlement = 0;             // settlement objects shared by everyone
    bool ownSeed = false;              // placed by a random stream of its own (adding it leaves the rest of the world as it was)
    bool placedLast = false;           // AL-91: a natural source placed after the rest of the world (its ids stay as they were)
    std::vector<std::string> seasons;  // empty = all year; out of season the stock is not gathered
};

// species.json: animals and monsters (characters with a simplified AI).
struct SpeciesDef {
    std::string id;
    std::string habitat = "land"; // land | water
    std::vector<std::string> biomes;
    double perKm2 = 0.0;          // groups per km2 of habitat
    int groupLo = 1, groupHi = 1, maxGroup = 1;
    double speedMPerTick = 0.0;
    double noticeM = 0.0;         // sees people this close
    double fleeM = 0.0;           // runs this far once spooked
    std::string diet = "graze";   // graze | prey
    std::vector<std::string> prey;
    double aggression = 0.0;      // chance a hungry group dares a lone person
    double attackChance = 0.0, attackLo = 0.0, attackHi = 0.0;
    int attackSatedDays = 0;   // AL-30: after killing a person the pack is fed and leaves people alone this long
    double hungerPerDay = 0.0;
    int breedEveryDays = 0;
    double territoryM = 0.0;
    int starveDays = 0;
    // AL-82: population dynamics of wild beasts.
    double growthPerYear = 0.0;   // intrinsic growth per head and year (logistic toward max_group); 0 = breed_every_days
    double meatDays = 1.0;        // predator-head-days of food one head of it gives
    double huntBelow = 0.7;       // a predator hunts only when its hunger is below this
    int leaveHeads = 0;           // a predator never takes a group down below this many heads (the ones it cannot find)
    double refugeShare = 0.0;     // prey: a group down to this share of max_group is too wary to be caught by beasts
    double immigrateBelow = 0.0;  // fewer groups than this share of the land's carrying number: groups come in from beyond the map
    // Livestock (domestic): owned by a household, tame, kept by the house.
    bool domestic = false;
    int perFamilyLo = 0, perFamilyHi = 0; // group size per household at the start (0 = none)
    double perFamilyChance = 1.0;         // share of households that keep this kind at all
    std::vector<std::string> grazeSeasons; // seasons it feeds itself outside
    std::string fodder;                   // what it eats from the trough otherwise
    double fodderPerHead = 0.0;           // fodder units per head per day
    struct Product {
        std::string item;
        double perHeadDay = 0.0; // units per head and day when fed
        int from = 0, to = 359;  // day-of-year window
        double capPerHead = 4.0; // left uncollected, no more than this per head
    };
    std::vector<Product> products;
    int breedFrom = -1, breedTo = -1; // birth season (day of year); -1: breed_every_days
    int broodLo = 1, broodHi = 1;     // young per birth
    int cullFrom = -1, cullTo = -1, keep = 0; // slaughter season and heads kept over winter
    // AL-114: monsters on the same ladder (content/creatures): how much mind, what a kill leaves for crafting.
    double sapience = 0.1;
    std::map<std::string, int> loot;
    std::vector<std::string> tags;
    Json innateSpells = Json::array(); // a built-in spell (a basilisk's gaze): species.hpp innateStrike
};

struct NeedDef {
    std::string id;
    double decayPerDay = 0.5;
    double critical = 0.1;
    double startLo = 0.6, startHi = 0.9;
    double nightMult = 1.0;
    std::map<std::string, double> biomeMult;
    bool mvp = true;
    std::map<std::string, double> seasonMult;
    double shelteredMult = 1.0;
    std::map<std::string, double> clothingMult; // item -> decay multiplier while carried
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
    double inherit = 0.0;      // chance a carrier parent passes it on (history generator)
    double weightInPool = 1.0; // world generator draw weight
};

// knowledge.json: what can be known (knows:/skill: keys) and the start rules.
struct KnowledgeDef {
    std::string id;
    std::vector<std::string> requiresKnowledge;
    Tick learnTicks = 0;
    Tick selfTicks = 0;   // 0 = cannot be learned alone
    std::string start;    // all_from_child | all_from_youth | all_adults | "" (none)
    double startChanceAdult = 0.0; // otherwise: chance an adult starts with it
    bool familyKnowledge = false;  // candidate for a family secret (history generator)
};

// content/ai/expansion: the content expansion (gdd/content_expansion). The base files win on a
// shared id; the expansion adds what is missing. Reference books (interests, attributes,
// temperaments, calendar, social position) are kept as data for the core and the generator.
struct InterestDef {
    std::string id, category;
    std::vector<std::string> skills, interactions, opposes;
    std::map<std::string, double> goalMult, methodBias;
    double weightInPool = 1.0;
};
struct TemperamentDef {
    std::string id;
    double weight = 1.0;
    std::map<std::string, std::pair<double, double>> core, attrs; // ranges
    std::map<std::string, double> traits;                         // trait -> chance
    std::map<std::string, double> interestCategories;             // category -> weight
    std::string speech;                                           // register
};
struct ExpansionData {
    bool loaded = false;
    std::vector<std::string> attributes, interestCategories, skillCategories;
    std::map<std::string, InterestDef> interests;
    std::map<std::string, TemperamentDef> temperaments;
    std::map<std::string, Json> traitExt;     // extended trait fields (kind, skill_mult, attr_mod, ...)
    std::map<std::string, Json> knowledgeExt; // category, attrs
    Json skillModel = Json::object(), attributeModel = Json::object(), interestModel = Json::object(),
         temperamentModel = Json::object(), calendar = Json::object(), socialPosition = Json::object();
    std::vector<std::string> kept;   // ids that were already in the base files (base kept)
    std::vector<std::string> errors; // merge problems (reported by validate)
    int added = 0;
};

// content/ai/backstories.json: who the player was before the game (picked at a new game;
// src/sim/backstory.cpp applies one to the person the player starts as).
struct BackstoryMade {
    std::string design;
    double q = 0.5;
};
struct BackstoryDef {
    std::string id, icon, start = "home";
    std::vector<std::pair<std::string, double>> skills;   // knowledge id -> level 0..1 (file order)
    std::vector<std::string> traitsAlways, traitPool;
    int traitPick = 0;
    std::map<std::string, double> attrs;                  // attribute -> shift from the mean
    std::map<std::string, int> items;
    int coins = 0;
    std::vector<std::string> recipes;                     // craft design ids known
    std::vector<BackstoryMade> made;                      // own made things
    std::vector<std::string> rights;                      // config.rights
    std::map<std::string, double> reputation;             // config.reputation_axes
    std::map<std::string, std::map<std::string, double>> relations; // kin|village -> rel axis -> shift
};
struct BackstoryConfig {
    double otherSkillCap = 0.3, attrSpread = 0.08, attrLo = 0.1, attrHi = 0.95;
    std::map<std::string, double> starts;                 // start place -> radius from home (m)
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
    Json params = Json::object();    // Do: extra parameters (start_dialog intent, ...)
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
// A parameter of a parametric interaction: `$name` in pre/eff/terms.
struct ParamDef {
    std::string name;
    std::string type;            // item | skill | place | need | trait
    std::vector<std::string> of; // allowed values (empty = any of the type)
};

// One extra input of a chance (schema v0.3 §3): k * <input>, the input being
// rel:<axis> | need: | body: | skill: | reputation:, optionally with t.
struct ChanceInput {
    Condition input; // parsed as a condition, only subject/key/onTarget are used
    double k = 0.0;
};

// Success chance: clamp(base + per_skill * skill + sum k * input, min, max).
struct ChanceDef {
    bool set = false;
    double base = 1.0;
    std::string skill; // may be a parameter ($skill)
    double perSkill = 0.0;
    std::vector<ChanceInput> add;
    double min = 0.0, max = 1.0;
};

// Conditional law (schema v0.3 §2). `anyOf[i]` is one list entry; its
// alternatives are the `|`-separated parts. All entries must hold.
struct LegalRule {
    std::string tag;
    std::vector<std::vector<Condition>> all;
    double mult = -1.0; // legal_unless: <0 removes the tag, else multiplies its price
};

// A side risk rolled after the interaction (succeeded or not).
struct RiskDef {
    std::string id; // loc: risk.<id>.name
    double p = 0.0;
    std::vector<Effect> eff;
};

// Violence (stage 2 of the interpreter: parser + validator now).
struct CombatDef {
    bool set = false;
    std::string attackSkill = "fighting";
    std::string defenseSkill = "fighting";
    double damageLo = 0.0, damageHi = 0.0; // health removed from the loser
    bool lethal = false;                   // a loser at health 0 dies (cause: combat)
    std::map<std::string, std::vector<Effect>> outcomes; // win | lose | draw | flee
};

// Terms of a deal between two characters (stage 2).
struct TermsDef {
    bool set = false;
    std::map<std::string, int> give; // actor -> target (items, may be $params)
    std::map<std::string, int> take; // target -> actor
    Tick due = 0;                    // 0 = on the spot; else deferred, becomes a commitment
    std::string commitment;          // config.commitments id when due > 0
    std::vector<Effect> onBreach;
};

struct InteractionDef {
    std::string id;
    std::string baseId; // "<base>@foreign" variants (consequences.hpp): the act they are made from
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
    std::vector<LegalRule> legalIf;
    std::vector<LegalRule> legalUnless;
    std::string consent;                // "" | target (the target's mind decides)
    std::vector<std::string> intents;   // start_dialog: conversation intents it can open
    std::string visibility = "public";
    Json social;
    std::vector<std::string> speech;
    bool continuous = false;
    bool hasUntil = false;
    Condition until;
    std::vector<ParamDef> params;
    ChanceDef chance;
    std::vector<Effect> onFail; // applied instead of eff when the chance roll fails
    std::vector<RiskDef> risk;
    CombatDef combat;
    TermsDef terms;
    double rangeM = -1.0;          // reach of the act (spear throw, line); <0 = config.interaction_range_m
    std::string craft;             // resolved by the crafting resolver (craft_sim.hpp): design:<id> | recipe | attempt | inspect | teach | repair
    bool targetsObject() const { return target.rfind("object:", 0) == 0; }
    // creature:<species> - an animal group (a Character with that species)
    bool targetsCreature() const { return target.rfind("creature:", 0) == 0; }
    // carried - the things a person has on them (an act on things, not a social act)
    bool targetsCarried() const { return target == "carried"; }
    bool targetsCharacter() const { return target == "character"; }
    bool parametric() const { return !params.empty(); }
};

// A parametric interaction with its $params bound (pre, eff, on_fail, risk,
// combat outcomes, terms, chance.skill). Throws on an unbound parameter or a
// value outside the parameter's `of` list.
InteractionDef bindParams(const InteractionDef& d, const ParamBinding& b);

// --- dialogue.json: speech acts and dialogue templates ----------------------
// Texts are never here: dialogue.<template>.<variant> in content/loc/<lang>/dialogue.json.
struct DialogueVariant {
    std::string id;
    std::vector<Condition> pre;
    double weight = 1.0;
};

struct DialogueTemplate {
    std::string id;
    std::string speech;             // speech act (speech_acts)
    std::vector<Condition> pre;     // speaker -> listener (t. = listener, rel = speaker->listener)
    std::vector<Effect> eff;        // applied when said
    std::vector<DialogueVariant> variants;
    std::vector<std::string> replies; // templates the listener may answer with
};

struct CommitmentDef {
    std::string id;
    Tick due = 144;
    std::vector<Effect> onBreach;
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
    // AL-108: acts are done up close. One walks to contactM of a person or a
    // herd and works only within contactM + contactSlackM of it; an act whose
    // range_m is rangedFromM or more (a spear throw, watching deer) is ranged.
    double contactM = 1.2;
    double contactSlackM = 1.3;
    double rangedFromM = 10.0;
    double legalBase = 4.0;
    double perceptionRadiusM = 40.0; // witnesses of public acts
    double noticeChance = 0.25;      // a bystander notices a secret act
    double trackRadiusM = 2500.0;    // animals this close count as known: (tracks, calls)
    int traceSize = 64;
    double modMaxAbsDelta = 0.5;
    Tick modMaxTtl = 288;
    Tick playerIdleResumeTicks = 18; // free-will resume for a paused plan under PlayerController
    Tick playerGoalHoldTicks = 36;   // AI keeps a player-chosen goal this long after a switch
    double playerGoalHoldBonus = 0.3;
    double hintDecayTicks = 72;      // player hints become decaying weights after a switch
    int memoryCap = 200;
    // AL-30: practice gains shrink as one gets better: gain x mult x (1 - level)^curve;
    // the small learn nothing from work yet.
    // Neutral by default (config.json skill_gain sets the game's values).
    double skillGainMult = 1.0, skillGainCurve = 0.0;
    int skillMinAgeYears = 0;
    // AL-30 (2): children up to child_until_years learn at child_mult; each
    // person has a talent per skill (talent_min..talent_max, fixed by id and
    // skill): above it practice gives exp(-(level - talent) x over_talent_falloff).
    // Neutral by default: child_mult 1, no talent cap.
    int skillChildUntilYears = 0;
    double skillChildMult = 1.0;
    double skillTalentMin = 1.0, skillTalentMax = 1.0, skillOverTalentFalloff = 0.0;
    std::map<std::string, double> memoryTypes;
    std::vector<std::string> reputationAxes = {"honesty", "valor", "piety", "generosity", "notoriety"};
    std::vector<std::string> statusPairs = {"betrothed", "married", "master_apprentice", "lord_vassal", "sworn_kin"};
    std::vector<std::string> rights = {"arrest", "exile", "judge", "appoint"};
    std::vector<std::string> verdicts = {"execute", "punish", "exile"};
    std::vector<std::string> householdRoles = {"head", "spouse", "child", "kin", "servant", "ward"};
    std::vector<std::string> deathCauses = {"starvation", "combat", "illness", "old_age", "execution"};
    std::map<std::string, CommitmentDef> commitments;
};

// build.json: the modules a household builds from (gdd/blueprints/
// LR_BLUEPRINTS_SPEC_v0.2 §1-2, Valheim-like): a 3D grid of 2 x 2 x 3 m
// cells; a piece takes a cell (foundation, floor), a cell's volume (a roof
// slope), an edge (a wall, a gable) or a corner (a pillar) of one storey.
struct BuildPieceDef {
    std::string id;
    std::string slot;      // cell | roof | edge | corner
    std::string category;  // foundation | floor | wall | gable | pillar | roof
    std::string material;
    std::string mesh;      // the kit bake's piece name (default: id)
    std::map<std::string, int> cost; // items delivered to the site
    int work = 1;          // construct_piece acts once the materials are in
    double carry = 0.8;    // stability passed on (stability = best support x carry)
    bool shelter = false;  // a roof: the needs' sheltered multiplier applies under it
    bool mirror = false;   // a gable: rotations 4..7 are the mirrored triangle
    bool door = false;
    // slot "thing" (category furniture): a planned piece of furniture on a
    // finer grid (cell_m / sub metres a cell, thing_m); built, it becomes the
    // world object `makes` (a bed, a hearth, an anvil) that tells rooms apart.
    std::string makes;
    int sub = 1;
};
struct BuildCatalog {
    double cellM = 2.0;
    double storeyM = 3.0;
    double minStability = 0.2;
    int maxPerFamily = 400;   // the 3D build mode's own cap; the sim plans without a limit (24.11)
    double thingM = 1.0;      // the furniture grid
    std::vector<BuildPieceDef> pieces; // palette order
    const BuildPieceDef* find(const std::string& id) const {
        for (const auto& p : pieces)
            if (p.id == id) return &p;
        return nullptr;
    }
};

// AL-81: rooms.json - what a room is by what stands in it (RimWorld-like).
// A rule fits when each of its needs is met (n things of any of its types)
// and the room is closed / open as it asks; the highest priority wins.
struct RoomNeed {
    std::vector<std::string> any;
    int n = 1;
};
struct RoomRuleDef {
    std::string id;
    int priority = 0;
    int purposeRank = 0;             // what a building of such rooms is for: the highest (default priority)
    std::vector<RoomNeed> needs;
    std::string enclosure = "any";   // closed | open | any
};
struct RoomCatalog {
    std::vector<RoomRuleDef> rules;  // highest priority first
    std::string closedFallback = "room", openFallback = "shelter";
    double roofCover = 0.75;         // an open space is a room when this share of it is roofed
    std::vector<std::string> thingTypes;   // every object type a rule reads
    const RoomRuleDef* find(const std::string& id) const {
        for (const auto& r : rules)
            if (r.id == id) return &r;
        return nullptr;
    }
};
RoomCatalog parseRooms(const Json& j);

// Relation axes (rel:<axis>), -1..1 except familiarity/debt (0..1 / any).
const std::vector<std::string>& relAxes();

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
    std::vector<std::string> speechActs;
    // Names world generation gives (names.json, AL-61): ids = the Latin form, keys name.<id> of the loc.
    std::vector<std::string> givenNamesM, givenNamesF, familyNames;
    std::map<std::string, DialogueTemplate> dialogue;
    Json dialogueRules = Json::object(); // the rest of dialogue.json (plans, menu, consent, ...)
    std::map<std::string, KnowledgeDef> knowledge;
    std::map<std::string, SpeciesDef> species;
    SpeciesCatalog peoples; // AL-114: content/species (sapient kinds, cultures); empty = all human
    const RaceDef* raceOf(const std::string& id) const {
        auto it = peoples.races.find(id.empty() ? peoples.defaultRace : id);
        return it == peoples.races.end() ? nullptr : &it->second;
    }
    ClimateDef climate;
    CareDef care;
    craft::CraftData craft; // content/ai/craft (not loaded = no crafting content)
    BuildCatalog build;
    Json roomsJson = Json::object();  // AL-81: content/ai/rooms.json (sim/structure_plan.hpp parses the recipes)
    RoomCatalog rooms;                // and the rules that tell rooms apart (sim/rooms.hpp)
    ExpansionData expansion; // content/ai/expansion (not loaded = none)
    std::vector<BackstoryDef> backstories; // content/ai/backstories.json, file order (none = no choice)
    BackstoryConfig backstoryCfg;
    Json charCreation = Json::object(); // AL-136: content/ai/character_creation.json (sim/charcreate.hpp)
    const BackstoryDef* backstory(const std::string& id) const {
        for (const auto& b : backstories)
            if (b.id == id) return &b;
        return nullptr;
    }
    const SpeciesDef* speciesOf(const std::string& id) const {
        auto it = species.find(id);
        return it == species.end() ? nullptr : &it->second;
    }
    std::vector<std::string> householdMust;
    std::vector<std::string> settlementMust;
    AiConfig config;
    std::vector<std::string> warnings;
    std::string dir; // directory load() read (content/ai); empty for fromJson

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
                            const Json& config, const Json& dialogue = Json::object(),
                            const Json& knowledge = Json::object(), const Json& species = Json::object());
};

struct ValidationReport {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok() const { return errors.empty(); }
};

ValidationReport validate(const Content& c);
// Merges content/ai/<expDir> into the base JSON before parsing (base wins on a shared id).
ExpansionData mergeExpansion(const std::string& expDir, Json& items, Json& objects, Json& needs, Json& traits,
                             Json& htn, Json& interactions, Json& config, Json& knowledge);
void validateExpansion(const Content& c, ValidationReport& r);
BuildCatalog parseBuild(const Json& j);
void parseBackstories(const Json& j, Content& c);
void validateBackstories(const Content& c, ValidationReport& r);

} // namespace lr::sim
