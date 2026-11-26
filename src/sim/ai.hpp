#pragma once
// The four-layer mind (AI_ARCHITECTURE_v0.1): Utility -> HTN -> GOAP -> FSM.
// Every layer returns a reason on failure and writes one trace line per
// decision. The same code runs for every character; controllers (control.hpp)
// only decide where layer 1's goal comes from.

#include <string>
#include <vector>

#include "sim/world.hpp"

namespace lr::sim {

// ---- shared evaluation ---------------------------------------------------
struct EvalCtx {
    const World& w;
    const Character& c;
    const WorldObject* target = nullptr;
    const Character* targetChar = nullptr;
};
double evalSubject(const EvalCtx& ctx, const Condition& k);
bool evalCondition(const EvalCtx& ctx, const Condition& k);
// Conditions of these subjects are tracked by GOAP, everything else is static.
bool goapFact(const Condition& k);

// The interaction catalog's executor. Players and NPCs both go through here.
// `why` gets the first failing precondition.
std::string gearRefusal(const World& w, const Character& c, const InteractionDef& d);
bool canPerform(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                std::string* why, bool checkRange = true, const Character* targetChar = nullptr);
// The person a character-targeted act is done with: `prefer` when valid
// (the goal's target), else the best known person who passes the act's
// preconditions (family first, the needy for gifts, then the nearest).
// strict (a player's order on a named person): only `prefer`, never somebody else (AL-23).
EntityId findPerson(const World& w, const Character& c, const InteractionDef& d, EntityId prefer, std::string* why,
                    bool strict = false);
// consent: "target" - the other's mind decides (relation, kinship, own need).
bool consents(const World& w, const Character& actor, const Character& target, const InteractionDef& d);
// The chance consents() rolls against (1 for acts without consent); `tone`
// is the player's tone of the act (tones.json), "" = none.
double consentChance(const World& w, const Character& actor, const Character& target, const InteractionDef& d,
                     const std::string& tone = "");
// A conversation opened by start_dialog, run by the built-in provider until a
// dialogue runtime/agent takes over: small talk feeds both sides' social need;
// ask_favor for food puts a "share food with me" task in the other's queue.
void runConversation(World& w, Character& c, Character& other, const Json& params);
// The reach of an act: its range_m, else config.interaction_range_m.
double reachOf(const World& w, const InteractionDef& d);
// AL-108: an act with range_m >= config.ranged_from_m (a hunt, watching deer)
// is done from afar; every other act is done up close.
bool isRanged(const World& w, const InteractionDef& d);
// How far from the target one stops to do the act: the range for a ranged act,
// else config.contact_m (a thing: its stand_m when it has one).
double approachOf(const World& w, const InteractionDef& d, const WorldObject* target = nullptr);
// How far the target may be while the act runs: the range for a ranged act,
// else contact_m + contact_slack_m (a thing: at least its stand_m + 0.25).
double workReachOf(const World& w, const InteractionDef& d, const WorldObject* target = nullptr);
// AL-108: turn everyone to what they do (the act's target) or where they walk.
void faceAll(World& w);
// The nearest live group of a species within the tracking radius of `from`.
// Livestock (domestic species) is found only among `family`'s own herds.
EntityId findCreature(const World& w, const std::string& species, Vec2 from, double radius, EntityId family = kNone);
// Where the target of a plan action is now (object or creature).
bool targetPos(const World& w, EntityId id, Vec2& out);
// Animals (animals.cpp): reactive drivers per tick, daily breeding and
// migration, spawning at world generation, losing heads to hunters/predators.
struct AnimalTick {
    std::vector<const Character*> people;    // alive persons this tick
    std::vector<const Character*> predators; // alive groups of diet:prey species
};
void animalThink(World& w, Character& a, const AnimalTick& tick);
void animalsDaily(World& w);
void spawnAnimals(World& w, std::uint64_t seed);
void loseHeads(World& w, Character& herd, int n, const std::string& cause);
// Fields (farming.cpp): `farm:<key>` conditions and utility inputs - of the
// target field when there is one, else summed over the household's fields
// (sow_window, free, tilled, sown, ripe, weeds, strips, seed_need, work).
// `type` is the crop object type summed over: farm:<key> reads the household's
// "field"s, <type>:<key> any other crop object type (garden:ripe).
double farmValue(const World& w, const Character& c, const WorldObject* target, const std::string& key,
                 const std::string& type = "field");
// Livestock (animals.cpp): `herd:<key>` of the target herd when there is one,
// else summed over the household's herds - work (how pressing the chores are,
// 0..1), trough (fodder missing in the troughs of herds kept in), fodder_need
// (fodder still to put by for the stalled season), cull (heads over what is
// kept, in the slaughter season), <product item> (waiting to be collected).
double herdValue(const World& w, const Character& c, const Character* target, const std::string& key);
// Starting livestock of every household (species with per_family).
void spawnLivestock(World& w, std::uint64_t seed);
// Harvest per seed of a field now: what its soil can give (fertility, depth)
// less the season's water stress, frost and weeds.
double cropYield(const World& w, const WorldObject& field);
// Weather (weather.cpp): a settlement's day, a pure function of the world
// seed, the settlement and the day counted from year 0 (start_day + day).
struct DayWeather {
    double tempC = 10.0;
    double rainMm = 0.0;
    bool wet = false;
    // AL-115: the day's wind - where it blows from (degrees clockwise from north, -y) and how hard
    double windFromDeg = 250.0, windMs = 4.0;
};
DayWeather weatherAt(const World& w, EntityId settlement, int epochDay);
DayWeather weatherToday(const World& w, EntityId settlement);
// A past season of a field replayed day by day from that year's weather
// (`year` counts from year 0 of start_day; -1 is the year before the start):
// the harvest per seed it gave. Worldgen's stand-in for last year's harvest.
double pastSeasonYield(const World& w, const WorldObject& field, int year);
// Once a day: growth, weeds, ripening, shattering of a standing crop.
void cropsDaily(World& w);
// World generation: field sizes per household and their state on the start
// day (sown, ripe, seed corn in the store).
void seedFields(World& w, std::uint64_t seed, const Json& cfg);
// AL-82: a field and a garden for a household that settles later, with their seed.
void openFields(World& w, EntityId family, Vec2 at, std::uint64_t seed);
// Care (care.cpp): the dependent category a character falls in (nullptr: none),
// and the most pressing ward of a carer - who, which need, how urgent
// (circle x category weight x shortfall). `care:need` is that urgency,
// `care:<need>` the ward's shortfall in that need.
const DependentDef* dependencyOf(const World& w, const Character& c);
struct CareChoice {
    EntityId ward = kNone;
    std::string need;
    double urgency = 0.0;
};
CareChoice bestWard(const World& w, const Character& carer);
double careValue(const World& w, const Character& carer, const std::string& key);
// Help between households (care.cpp, care.json charity_*): charity:need is how
// short the neediest other household of the village is, times how much the
// giver's own store can spare; charity:target (on a person) is 1 for a member
// of another household of the village that is running out of food.
double charityValue(const World& w, const Character& giver, const Character* target, const std::string& key);
// Food left in a household store, in days per member (items' nutrition).
double homeFoodDays(const World& w, const Character& c);
// AL-93: days from today to the next harvest of the staple crop (the settlement
// field's sowing window + grow_days); 0 when no staple crop is defined.
int daysToStapleHarvest(const World& w);
// Daily spoilage of perishable food (items' spoil_days) in packs and stores.
void spoilFood(World& w);
// AL-93, diagnostics for the runner's ledger: how many times each interaction
// applied its effects in this process (all worlds). Never read by the sim.
std::map<std::string, long>& interactionTally();
// Applies `eff` once (a continuous interaction calls it every tick), after the
// `chance` roll (on_fail instead of eff when it fails), then the `risk` rolls.
// `targetChar` is the target character of character-targeted interactions
// (t.* effects, rel, rel_back).
void applyEffects(World& w, Character& c, const InteractionDef& d, WorldObject* target,
                  Character* targetChar = nullptr);
// `witnesses` receive the w.* effects (nullptr: none are applied).
void applyEffectList(World& w, Character& actor, const std::vector<Effect>& list, WorldObject* target,
                     Character* targetChar = nullptr, const std::vector<EntityId>* witnesses = nullptr);
// Who perceives the act by its visibility (schema v0.3 §1): public - everyone
// alive within config.perception_radius_m; private - nobody but the two
// participants (who are not witnesses); secret - those who pass a notice roll.
std::vector<EntityId> witnessesOf(const World& w, const Character& actor, const InteractionDef& d,
                                  const Character* targetChar);
// p = clamp(base + per_skill * skill + sum k * input, min, max) (schema v0.3 §3).
double chanceOf(const World& w, const Character& c, const InteractionDef& d, const Character* targetChar);
// Legal tags of the act right now: `legal` plus legal_if minus/scaled by legal_unless (v0.3 §2).
struct LegalTag {
    std::string tag;
    double mult = 1.0;
};
std::vector<LegalTag> legalTags(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                                const Character* targetChar);
// What breaking `tag` costs this character (traits, desperation). The law
// only changes the price, never removes the option.
double legalPrice(const World& w, const Character& c, const std::string& tag, double mult = 1.0);
// The goal's own target when the player's order named it and it is of the
// act's target type (object type or creature species); kNone otherwise.
EntityId pickedTarget(const World& w, const Character& c, const InteractionDef& d);
// Why a player's order cannot be done to `target` at all ("" = it can):
// wrong kind of target (a hen for a fight, a person for a well), dead,
// oneself, or a person failing the act's conditions (AL-23).
std::string orderTargetProblem(const World& w, const Character& c, const InteractionDef& d, EntityId target,
                               std::string& why);
// AL-32: the act done to another household's thing or beast. For an act that
// asks target_owner:family the content keeps "<id>@foreign" (built at load):
// the same act without that condition, taking from the target's stock, with a
// theft or trespass tag. nullptr when the target is ours, nobody's, or the
// act has no such variant.
const InteractionDef* foreignVariant(const World& w, const Character& c, const InteractionDef& d, EntityId target);

// Known, free (or ours), condition-satisfying instance of the interaction's
// target type nearest `from`; kNone if there is none.
EntityId findTarget(const World& w, const Character& c, const InteractionDef& d, Vec2 from, std::string* why,
                    bool ignoreReservations = false);

// ---- layer 1: Utility -----------------------------------------------------
struct GoalScore {
    std::string goal;
    std::string task;
    std::string source = "utility";
    EntityId target = kNone;
    double score = 0.0;
    Json why = Json::object(); // consideration values and multipliers
};
std::vector<GoalScore> scoreGoals(const World& w, const Character& c);

// ---- layer 2: HTN ---------------------------------------------------------
// Decomposes `task` into primitive steps. False + reason when no method fits.
// `log` receives the method choice and the rejected methods with reasons.
bool decompose(const World& w, const Character& c, const std::string& task, Plan& out, std::string& why,
               Json& log);

// At least one method of `task` passes its preconditions now (AI_ARCHITECTURE
// §1.1: layer 1 never picks a goal no method can serve).
bool anyMethodFeasible(const World& w, const Character& c, const std::string& task, std::string* why);
double methodScore(const World& w, const Character& c, const std::string& task, const HtnMethod& m);

// ---- layer 3: GOAP --------------------------------------------------------
struct GoapResult {
    bool ok = false;
    std::vector<PlanAction> actions;
    double cost = 0.0;
    int nodes = 0;
    std::string why;
};
GoapResult planStep(const World& w, const Character& c, const PlanStep& step);

// ---- the loop -------------------------------------------------------------
void trace(World& w, Character& c, char layer, std::string code, Json params = Json::object());
// One tick of one mind (all four layers). Called by World::step via the hook.
void thinkCharacter(World& w, Character& c);
// Installs the think hook and the AI/controller command handlers on a world.
void installAi(World& w);

// Layer 1 per controller: AiController (Utility) or PlayerController
// (ai_control.cpp). Layers 2-4 are shared: executePlan.
void aiThink(World& w, Character& c);
void playerThink(World& w, Character& c);
// Hands `player`'s control to `to` (releasing the one held before); false + why when it cannot.
bool switchControl(World& w, int player, EntityId to, std::string& why);
// LR-138: whether `player` may lead `c` - alive, a person, free, and of the
// player's house (their own kin) unless that house has died out.
bool mayLead(const World& w, int player, const Character& c, std::string* why = nullptr);
// A new game's choice of whom to be: the player's house is chosen anew.
void forgetHouse(World& w, int player);
void executePlan(World& w, Character& c);
bool adoptGoal(World& w, Character& c, const GoalScore& gs, const std::string& source);
void installControl(World& w);

// Abort the running action: release reservations, FSM back to Idle. The plan
// keeps its position, so the action restarts if the plan is resumed.
void abortAction(World& w, Character& c, const std::string& reason);
void releaseReservations(World& w, const Character& c);
void dropGoal(World& w, Character& c, const std::string& reason, bool failed);

// Human-readable / JSON report of why characters did what they did.
Json aiReport(const World& w);

} // namespace lr::sim
