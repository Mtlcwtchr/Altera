#pragma once
// Character - one data type for the player's people and every NPC
// (CORE_SPEC §2). Only `controller` tells them apart.

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/craft/craft_state.hpp"

namespace lr::sim {

struct Body {
    double health = 1.0;
    double fatigue = 0.0;
    int ageDays = 0;
    char sex = 'f';
    bool alive = true;
    std::string causeOfDeath;
    double blood = 100.0; // combat units (combat_config stamina/blood scale); bleeding and wounds lower it
    Tick downUntil = 0;   // knocked out / carried off after a fight: the mind does not run until then
    // AL-114: the kind's life (sim/species.hpp setRace): when it grows up and old.
    int adultYears = 14, elderYears = 60;
    bool petrified = false; // a troll asleep as stone in the sun (species.cpp)
    // LR-152 (sim/stamina.hpp): the strength for actions right now, spent by every act,
    // by running and by blows, and back within seconds; the cap falls with fatigue,
    // lack of sleep, hunger, thirst, wounds. Combat units (combat_config stamina.max).
    // Not saved: a loaded body starts rested (stamina < 0 = full at the next tick).
    double stamina = -1.0, staminaMax = 0.0;
    bool winded = false; // spent to the bottom: no run until back above stamina.json move.run_min_share
    Tick stalledAt = -1; // the tick a spent body's act stood still while it caught its breath
    Tick steeredAt = -1; // the last tick the player walked this body by hand (its stamina runs in real seconds then)
    std::string stage() const {
        const int years = ageDays / kDaysPerYear;
        return years < adultYears ? "child" : (years >= elderYears ? "elder" : "adult");
    }
    // Age in human years (14 = grown, 60 = old): what the rules that count years read.
    int lifeYears() const {
        const int years = ageDays / kDaysPerYear;
        if (adultYears == 14 && elderYears == 60) return years;
        const double y = static_cast<double>(ageDays) / kDaysPerYear;
        if (y <= adultYears) return static_cast<int>(14.0 * y / std::max(1, adultYears));
        return static_cast<int>(14.0 + (y - adultYears) * 46.0 / std::max(1, elderYears - adultYears));
    }
};

// A wound kept in the character's profile after a fight (Kenshi-like, COMBAT_DESIGN §6):
// it heals in the ordinary 10-minute ticks (src/sim/wounds.cpp, content/combat/recovery.json),
// weakens the zone in the next fight and may leave a permanent injury.
struct Wound {
    std::string zone, severity;   // combat_config zones, wounds.json severity ids
    double dmg = 0.0;             // zone hp the wound took
    double healthDebt = 0.0;      // share of body.health it took; given back while it heals
    double daysTotal = 0.0, daysLeft = 0.0;
    std::string treatment = "none"; // wounds.json treatment ids
    bool bleeding = false, aided = false, infectionRolled = false, infected = false;
    Tick since = 0, feverAt = 0;
};

// Permanent result of a badly healed wound (limp, crippled arm, scar...).
struct Injury {
    std::string zone, kind;
    Tick since = 0;
};

// Held by someone after a fight or an arrest (content/combat/captivity.json):
// the captor decides the fate at decideAt (release, ransom, trial), the
// captive tries to escape meanwhile; a trial may end in an execution.
struct Captivity {
    EntityId captor = kNone;
    Tick since = 0, decideAt = 0, nextTry = 0;
    std::string stage;   // held | trial
    std::string verdict; // after a trial: execute | punish | exile | acquit
    int tries = 0;
    bool held() const { return captor != kNone; }
};

struct Moodlet {
    std::string id;
    double value = 0.0;
    Tick until = 0;
};

struct MemoryEntry {
    Tick tick = 0;
    std::string type;
    std::vector<EntityId> who;
    EntityId object = kNone;
    double valence = 0.0;
    double salience = 0.0;
    std::string source = "seen"; // seen | heard | rumor | told
    double confidence = 1.0;
    // AL-49 (doc/design/DIALOGUE_GEN.md §3.2, §6.4): who told it, nearest first
    // (at most Epistemic::kMaxChain), and the World fact it is about (kNone =
    // none known: a guess, a lie, or written before the fact registry).
    std::vector<EntityId> chain;
    EntityId fact = kNone;
    // AL-58: what the rememberer believes changed hands (item -> count): the
    // flour, not the storage it was taken from (that stays in `object`).
    std::map<std::string, int> goods;
    // AL-50: what the rememberer believes when a retelling distorted it (a
    // Prop in its JSON form, sim/dialogue_gen.hpp); null = as the type says
    // (who/object). who/object still hold the believed doer and object.
    Json prop;
};

// Directed: A->B is not B->A (AI_ARCHITECTURE §7).
struct Relation {
    double familiarity = 0.0;
    double trust = 0.0;
    double affinity = 0.0;
    double fear = 0.0;
    double debt = 0.0;
    double enmity = 0.0;
    double respect = 0.0;
    double attraction = 0.0;
    // rel:<axis> by name (relAxes() in content.hpp); nullptr for an unknown axis.
    double* axis(const std::string& a);
    double get(const std::string& a) const;
};

struct Status {
    EntityId family = kNone;
    EntityId dynasty = kNone;
    EntityId settlement = kNone;
    std::string role = "member";
    EntityId spouse = kNone;
    EntityId mother = kNone;
    EntityId father = kNone;
    std::vector<EntityId> children;
    // Social layer (schema v0.3 §2). Status pairs are stored on both sides;
    // `married` also mirrors `spouse`.
    std::map<EntityId, std::set<std::string>> pairs; // other -> status pair ids
    std::set<std::string> rights;                    // arrest | exile | judge | appoint (mandate or office)
    std::set<std::string> verdicts;                  // sentences against this character
    bool hasPair(EntityId other, const std::string& id) const;
    bool hasPairAny(const std::string& id) const;
};

struct Controller {
    enum class Kind { Ai, Player };
    Kind kind = Kind::Ai;
    int playerId = 0;
    bool autonomy = false; // LR-157: free will, off unless the player turns it on (set_free_will); a save's old "freeWill" is ignored
    Tick since = 0;
    Tick idleSince = 0; // player: since when no player order is running
    // Player focus for the end of a won fight (COMBAT_DESIGN §4 "взять живым / убить"):
    // spare | kill | bind; empty = the character decides by its own motivation.
    std::string combatFinish;
    // Tone the player picked on the interaction wheel (content/ai/tones.json):
    // it changes the chance and the effects of the acts this character does to
    // a person; empty = no tone (NPCs), the plain rules.
    std::string tone;
    // LR-128: how far steer moved the person in the tick `steerTick` (a budget per tick, not per call)
    Tick steerTick = -1;
    double steerUsed = 0.0;
    bool isPlayer() const { return kind == Kind::Player; }
};

// --- AI state (four layers + controller bookkeeping) ------------------------
struct GoalInstance {
    std::string id;           // goal type from goals.json
    std::string source = "utility"; // utility | player | queued
    double score = 0.0;
    Tick started = 0;
    std::string task;         // root HTN task (a queued task may override the goal's own)
    EntityId target = kNone;  // optional target (queued task / player command)
    bool valid() const { return !id.empty(); }
};

struct PlanStep {
    std::string kind; // do | achieve
    std::string ref;  // interaction for do
    std::vector<std::string> achieve;
    std::vector<std::string> allow;
    std::string method; // HTN method this step came from
    Json params = Json::object(); // HTN step params (start_dialog intent, ...)
};

struct PlanAction {
    std::string interaction; // catalog id, or "goto"
    EntityId target = kNone;
    std::string targetType;
    double cost = 0.0;
};

struct Plan {
    std::vector<std::string> methods; // task/method chosen at each decomposition
    std::vector<PlanStep> steps;
    size_t stepIndex = 0;
    std::vector<PlanAction> actions; // GOAP chain for the current step
    size_t actionIndex = 0;
    int stepReplans = 0; // GOAP replans of the current step
    int htnReplans = 0;  // method switches inside the current goal
    int waits = 0;       // ticks spent waiting for a reserved object in this step
    bool valid() const { return !steps.empty(); }
};

struct FsmState {
    std::string state = "Idle";
    std::string interaction;
    EntityId target = kNone;
    Vec2 dest;
    Tick timer = 0;
    Tick started = 0;
    std::string anim; // social chains: the clip key of this side of a joint act (social_chains.json); "" = by interaction/state
};

// A social chain this character takes part in (sim/social_chain.hpp, AL-40):
// an act that asks the other's answer and then is done by both together, or
// an accepted invitation to follow someone. The actor's side drives the pair.
struct SocialLink {
    std::string chain;     // social_chains.json id (= the catalog act)
    std::string role;      // actor | target | follower
    EntityId partner = kNone; // the other side (for a follower: the leader)
    std::string phase;     // ask | answer | refused | approach | stage | follow | join
    int stage = 0;
    Tick until = 0;        // end of the current phase / stage
    Tick since = 0;
    Tick expires = 0;      // follower: the invitation runs out
    EntityId place = kNone; // where the pair does it (a bed), kNone = where they stand
    std::string tone;      // the actor's tone when asked
    std::string result;    // actor: why the other side broke the chain (phase "broken")
    bool aside = false;    // target: talks over its own work, which goes on (AL-73)
    bool active() const { return !chain.empty(); }
};

struct WeightMod {
    std::string target; // goal:<id>
    double delta = 0.0;
    Tick created = 0;
    Tick ttl = 0;
    std::string decay = "linear"; // linear | none
    std::string reason;
    std::string source;
    double at(Tick now) const {
        if (now >= created + ttl) return 0.0;
        if (decay == "none" || ttl <= 0) return delta;
        return delta * (1.0 - static_cast<double>(now - created) / static_cast<double>(ttl));
    }
};

struct QueuedTask {
    std::string task; // HTN task id, or a catalog interaction id
    EntityId target = kNone;
    double priority = 0.5;
    Tick created = 0;
    Tick ttl = 0;
    std::string reason;
    std::string source;
};

// Trace lines keep a code and parameters, never display text: the text is
// assembled through the localization tables (content/loc) when shown.
struct TraceEntry {
    Tick tick = 0;
    char layer = 'U'; // U H G F C(ontroller) E(vent)
    std::string code; // trace.<code> in content/loc/<lang>/trace.json
    Json params = Json::object();
};

struct MethodFail {
    int count = 0;
    Tick until = 0;
};

struct AiStats {
    std::map<std::string, std::int64_t> goalTicks;
    std::map<std::string, std::int64_t> failReasons;
    std::int64_t idleTicks = 0;
    std::int64_t starvingTicks = 0;
    std::int64_t replans = 0;
    std::int64_t goalSwitches = 0;
    std::int64_t planFailures = 0;
    std::map<std::string, std::int64_t> idleReasons;
    std::map<std::string, std::string> lastGoalFail; // goal -> reason code of its last failure
    std::map<std::string, std::int64_t> methodUse;   // task/method -> times chosen
};

struct AiState {
    GoalInstance goal;
    Plan plan;
    FsmState fsm;
    // A plan put aside while a player drives the body (CORE_SPEC §3.2.5).
    bool suspended = false;
    GoalInstance suspendedGoal;
    Plan suspendedPlan;
    std::vector<WeightMod> mods;
    std::vector<QueuedTask> queue;
    std::map<std::string, MethodFail> methodFails;
    std::map<std::string, Tick> goalCooldown;
    std::deque<TraceEntry> trace;
    std::string lastFailReason;
    Tick nextThink = 0;
    AiStats stats;
    SocialLink social;      // a joint act or a follow in progress (AL-40)
    SocialLink followAfter; // a follow put aside while its leader does a joint act with this follower
};

// AL-92: a person's place in the wider world (sim/realm.hpp) - the same fields
// for a villager of the map and a lord beyond it. Default = nobody of note yet.
struct Standing {
    std::string role;              // ruler | consort | heir | kin | knight | outlaw | wanderer | commoner ("" = none)
    std::string life = "settled";  // settled | road | outlaw (reconsidered each season)
    std::string house;             // the family name
    bool drawn = false;            // the axes below were rolled (else read from the traits)
    double martial = 0.3, ambition = 0.3, greed = 0.3, honor = 0.5, wanderlust = 0.2;
    double renown = 0.0;
    double purse = 0.0;            // coin while away from the map (on it: the coin item)
    EntityId at = kNone;           // the settlement where the person is
    EntityId travelTo = kNone;
    int arrive = 0, leaveDay = 0, deathDay = 0;
    EntityId quest = kNone, claim = kNone, captor = kNone;
    std::map<std::string, double> aptitudes; // magic extension (empty)
    std::vector<Json> deeds;                 // {day, code, ...}, the last 24
    bool empty() const { return role.empty() && !drawn && deeds.empty() && at == kNone && purse == 0.0; }
};
void to_json(Json& j, const Standing& s);
void from_json(const Json& j, Standing& s);

struct Character {
    EntityId id = kNone;
    std::string name;
    // A name the player gave, with its case forms: lang -> case -> form (AL-61, setOwnName).
    // Empty: the name is world generation's (its words are keys name.<word> of the loc).
    std::map<std::string, std::map<std::string, std::string>> nameForms;
    // Animals and monsters (species.json): a group of `group` heads driven by
    // animals.cpp; empty species = a person with the four-layer mind.
    std::string species;
    // AL-114: the sapient kind (content/species, sim/species.hpp); "" = the default (human).
    std::string race;
    int group = 1;
    Vec2 den;          // territory centre
    int starveDays = 0;
    bool isAnimal() const { return !species.empty(); }
    Body body;
    std::map<std::string, double> needs; // 0..1, 1 = satisfied
    std::vector<Moodlet> moodlets;
    std::vector<std::string> traits;
    std::map<std::string, double> skills;
    std::map<std::string, double> attrs; // LR-141: the stats' base (content/ai/stats/stats.json), 0..1, may pass 1; empty = not rolled
    std::map<std::string, double> attrsNow;  // LR-141: the current stats (base + kind + traits, age, state); not saved, sim/stats.cpp
    std::map<std::string, int> skillDay;     // LR-141: the age in days a skill was last practised (forgetting)
    std::string backstory;               // backstories.json id the player started as; empty = none
    // AL-136: how the person looks, as chosen at the character's creation (body, skin, hair,
    // hair_colour, beard, cloth; character_creation.json look). Empty = the client's own pick.
    std::map<std::string, std::string> look;
    // LR-155 (sim/culture.hpp): the person's culture (content/species/cultures) and the bond
    // to it 0..1, the faith (content/culture/religions) and the piety 0..1. Empty / < 0:
    // not given yet (cultureOf/faithOf derive it; the daily pass stores it).
    std::string culture, faith;
    double cultureBond = -1.0, piety = -1.0;
    // AL-75 (sim/trade.hpp): the trade role (trader | tavernkeeper, "" = none) and the
    // deal state: {intent: the offer the next trade act makes, counter: the counter
    // this person made and waits an answer to, rounds: partner -> ticks of recent offers}.
    std::string occupation;
    Json deal = Json::object();
    std::set<std::string> knowledge;
    std::set<EntityId> knownObjects;
    std::vector<MemoryEntry> memory;
    std::map<EntityId, Relation> relations;
    std::map<std::string, double> reputation; // reputation:<axis>, -1..1 (config.reputation_axes)
    Status status;
    std::map<std::string, int> inventory;
    // What of the inventory is worn ("slot:layer" -> item) and where the
    // player put things in the grids (sim/gear.hpp); saved only when set.
    std::map<std::string, std::string> worn;
    struct GearPin {
        std::string grid;
        int x = 0, y = 0;
        bool turned = false;
    };
    std::map<std::string, GearPin> gearPins;
    std::vector<Wound> wounds;
    std::vector<Injury> injuries;
    Vec2 pos;
    // AL-108: where the character looks (a unit vector, +y = south on the map):
    // at the target of the act going on, else where it walks (faceAll in the
    // AI tick). Not saved: the first tick after a load sets it again.
    Vec2 facing{0.0, 1.0};
    Vec2 seenPos;          // pos at the last faceAll (the walk's direction)
    EntityId home = kNone; // the family's storage object
    Controller controller;
    Captivity captivity;
    craft::CraftState craft; // made things, recipe book, known materials (sim/craft)
    AiState ai;
    Standing standing; // AL-92

    bool hasTrait(const std::string& t) const;
    int has(const std::string& item) const;
    double need(const std::string& n) const;
    double mood(Tick now) const;
    bool hasMoodlet(const std::string& id, Tick now) const;
};

void to_json(Json& j, const Character& c);
void from_json(const Json& j, Character& c);

} // namespace lr::sim
