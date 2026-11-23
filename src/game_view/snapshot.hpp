#pragma once
// What the sim thread hands the frame (src/game_view/sim_game.cpp builds it
// under its lock; the HUD, src/game_view/hud.cpp, only reads it). Display
// strings in here are already looked up in content/loc.

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/building.hpp"

namespace altera::game {

using sim::EntityId;
using sim::Tick;
using SnapClock = std::chrono::steady_clock;

struct ActorSnap {
    EntityId id = sim::kNone;
    std::string name, species, family, state, act, goal, interaction, role;
    std::string occupation;   // AL-75: trader | tavernkeeper | "" (sim Character::occupation)
    EntityId faces = sim::kNone;   // AL-73: whom they talk with / act on (turned to them when standing)
    std::string anim; // AL-40: the clip key of this side of a joint social act (sim FsmState::anim)
    bool animal = false, alive = true, player = false, down = false, fighting = false, captive = false;
    char sex = 'f';
    int age = 0, group = 1, wounds = 0;
    double px = 0, py = 0, x = 0, y = 0;   // sim metres, last tick and this one
    double hp = 1;
    double lowNeed = 1;                    // the lowest need, 0..1
    std::string lowNeedId;                 // and which
    // the act going on: how far along (0..1 at the last tick, -1: none),
    // its length in ticks, what it is done to
    double progress = -1;
    int actTicks = 0;
    EntityId actTarget = sim::kNone;
    EntityId spouse = sim::kNone, mother = sim::kNone, father = sim::kNone;
};

struct Line {
    int day = 0;
    std::string time, text, kind;
    std::vector<EntityId> who;
};

struct NeedSnap {
    std::string id, name;
    double value = 1;
};

struct RelSnap {
    EntityId id = sim::kNone;
    std::string name, kin;   // kin: spouse | mother | father | child | "" (loc key part)
    double affinity = 0, trust = 0, fear = 0, enmity = 0, respect = 0, familiarity = 0;
};

struct ItemSnap {
    std::string id, name;
    int count = 0;
};

// What the person carries and wears (sim/gear.hpp): the load, the body's
// slots by layers, the grids of the hands and the worn bags with every
// thing's place in them.
struct GearSnap {
    bool on = false;                      // gear.json is there
    double kg = 0, maxKg = 0, comfortKg = 0, tireMult = 1;
    struct Grid {
        std::string id, name;
        int w = 0, h = 0;
    };
    struct Spot {
        std::string item, name, grid;
        int count = 1, x = 0, y = 0, w = 1, h = 1;
        bool turned = false;
        double kg = 0;
        bool wearable = false;
    };
    struct Layer {
        std::string id, name, item, itemName;   // item "" = nothing on it
        int bulk = 0;
    };
    struct Slot {
        std::string id, name;
        int bulk = 0, used = 0;
        std::vector<Layer> layers;
    };
    std::vector<Grid> grids;
    std::vector<Spot> spots;
    std::vector<Slot> slots;
    std::vector<std::pair<std::string, int>> overflow;   // display name, count
    std::vector<std::pair<std::string, int>> hauled;     // AL-73: on the shoulder (a log)
};

struct Detail {
    EntityId id = sim::kNone;
    GearSnap gear;
    std::vector<NeedSnap> needs;
    double mood = 0, fatigue = 0, blood = 100;
    bool freeWill = true;
    std::vector<Line> trace;
    std::vector<ItemSnap> inventory;
    std::vector<std::string> traits;
    std::vector<std::pair<std::string, double>> moodlets, skills, reputation;
    std::vector<RelSnap> relations;
    std::vector<std::string> wants;   // goal now, queued tasks
};

// How the wheel's target feels about the player's own character, and the
// sim's own chance of every act of the wheel per tone (content/ai/tones.json
// order): what the sim rolls with (sim/tones.hpp actChance).
struct WheelSnap {
    EntityId target = sim::kNone, actor = sim::kNone;
    RelSnap toActor;
    std::vector<std::string> tones;                    // tone ids, the wheel's chips
    std::map<std::string, std::vector<float>> chance;  // interaction -> per tone
    std::map<std::string, std::string> kind;           // certain | chance | consent | reception | win
};

// The colony as the "Колония" panel shows it (GDD §2.2): who rules, who
// judges now, every grown person with the rights they hold.
struct ColonyMember {
    EntityId id = sim::kNone;
    std::string name, family;
    int age = 0;
    bool chief = false;
    std::vector<std::string> rights;
};
struct ColonySnap {
    EntityId settlement = sim::kNone, chief = sim::kNone, judge = sim::kNone;
    std::string institution, chiefName, chiefFamily, judgeName;
    bool judgeByRight = false;
    bool mandate = false;        // the player's dynasty holds it
    EntityId actor = sim::kNone; // who signs the player's orders (the chief of the player's dynasty)
    std::vector<std::string> rights; // config.rights
    std::vector<ColonyMember> members;
};

// The workshop (gdd crafting §9, §17): what the person has to work with, the
// processes and their targets, the draft's forecast, the recipe book.
struct CraftThing {
    std::string ref, name, shape, note; // ref: inv:<item> | made:<n>; note: quality tier, flaws
    int count = 1;
    double q = -1;
    std::string icon;                   // icons.json name (content/ui/craft.json)
    double wear = 0;                    // made things: wear 0..1
    bool flawed = false;                // a revealed flaw
};
struct CraftProcessSnap {
    std::string id, name;
    std::vector<std::pair<std::string, std::string>> slots;   // slot, its loc name
    std::vector<std::pair<std::string, std::string>> modes;   // mode, name
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> targets; // from shape -> (shape, name)
    std::vector<std::string> stations;   // any-of station ids it needs (empty: anywhere)
    bool available = true;               // the stations it needs stand near (or are carried)
    std::string need;                    // what is missing, for the tooltip
    int heatMin = 0, time = 1;
};
struct CraftRecipeSnap {
    std::string id, name;
    double mastery = 0, bestQ = 0;
    int known = 0, total = 0;
    bool master = false;
    int successes = 0, stages = 0, time = 0;
    double expectedQ = 0;
    std::vector<std::string> steps;      // process ids, in order
    std::vector<bool> stepKnown;
    std::vector<std::string> missing;    // display names of what the fast craft lacks
};
// A station of the workplace (gdd crafting §9) with where it stands, so the
// workshop can draw its gauge in the world (board 10).
struct CraftStationSnap {
    std::string id, name;
    int tempMax = -1;                    // heat steps it gives (-1: none)
    bool placed = false;                 // an object in the world (else carried)
    double x = 0, y = 0;                 // sim metres
};
// The last outcome (boards 11, 13): shown once per seq.
struct CraftResultSnap {
    int seq = 0;
    std::string code, head, what, tier;
    double q = -1;
    int made = -1;
    bool started = false, breakthrough = false;
    std::string recipe, recipeName;
    std::vector<std::pair<std::string, double>> links;   // the chain's steps: process name, quality
    std::vector<std::string> flaws;                      // revealed flaw names
};
// The novice helper (board 10b): designs that answer a wish.
struct CraftCandidate {
    std::string id, name, icon;
    double fit = 0;                      // 0..1
    std::vector<std::pair<std::string, std::string>> steps; // process id, station id it uses ("" anywhere)
    int next = -1;                       // the step the person can do now (-1: none)
    std::vector<std::string> missing;    // display names
    sim::Json draft;                     // the next step as a workshop draft
};
struct CraftSnap {
    EntityId actor = sim::kNone;
    std::vector<CraftThing> materials, made;
    std::vector<CraftProcessSnap> processes;
    std::vector<std::string> stations;        // names of what stands near
    std::vector<CraftStationSnap> stationsAt;
    int heatMax = -1;                         // the hottest station at hand
    std::vector<CraftRecipeSnap> recipes;
    bool canStart = false;
    std::string forecast, why, last;          // display texts
    // the draft's forecast as numbers (§9), blurred by `certainty`
    double expectedQ = 0, qSpread = 0, pRuin = 0, pFlaw = 0, certainty = 0;
    int time = 0;
    std::string likely, tier;                 // display names ("" = no idea)
    CraftResultSnap result;
    std::string wish;
    std::vector<CraftCandidate> candidates;
};

// A thing of the world near the camera or the player's person that one can
// work on: a tree, deadwood, a stony bank, a bush, a patch of herbs.
struct ThingSnap {
    EntityId id = sim::kNone;
    std::string type, name;
    double x = 0, y = 0;                                  // sim metres
    std::vector<std::pair<std::string, int>> stock;       // display name, count
    bool busy = false;                                    // someone else works on it
    bool faced = false;                                   // AL-76: set in a building, facing yaw
    float yaw = 0;
    bool held = false, felled = false;                    // AL-78: a tree of the drawn forest; felled: no logs left
    float height = 0;                                     // AL-78: as drawn (m), 0 = the type's own
};

// An act for the act ring and the wheel: what it needs (met or not), what it
// gives, how long it takes, as the sim checks it for the player's person.
struct ActInfoSnap {
    std::string id, name;
    bool can = false;
    std::string why;                                      // display text of the first unmet need
    std::vector<std::pair<std::string, bool>> needs;      // display text, met
    std::vector<std::string> gains, toTarget;             // display texts: what the person gets, what the target
    int minutes = 0;
};

// What just came of the player's person's doing: a thing gained or spent, an
// act done or broken off. Shown once per seq, floating over them.
struct FeedbackSnap {
    int seq = 0;
    EntityId who = sim::kNone;
    std::string text;
    int kind = 0;   // 0 info, 1 good, 2 bad
};

// AL-65: social chains around the player's person (sim/social_chain.hpp): an
// NPC's ask waiting for the player's answer (answer_social), and who follows
// the person (dismiss_follower).
struct SocialSnap {
    EntityId askFrom = sim::kNone;
    std::string askName, askAct, askActName, askTone;
    int askTicksLeft = 0, askTicks = 1;
    std::vector<std::pair<EntityId, std::string>> followers;   // id, name
};

// AL-67: what the player's person could bring up with the wheel's target
// (sim/dialogue_say.hpp dialogueTopics, the menu without an agent), and the
// cases opened against the person (sim/justice.hpp case_pending).
struct TopicSnap {
    std::string act, label;
};
struct CaseSnap {
    EntityId accuser = sim::kNone;
    std::string accuserName, judgeName, stage, what;
    int day = 0;
};

// A line someone said (AL-57, say.line of the sim): a bubble over the speaker.
struct SpeechSnap {
    int seq = 0;
    EntityId speaker = sim::kNone, listener = sim::kNone;
    std::string line;
};

// A row of the journal (AL-57): what the player's person said and heard, what
// came of their doing. Newest last.
struct JournalSnap {
    int seq = 0;
    std::string time, text;
    int kind = 0;   // 0 info, 1 good, 2 bad, 3 speech
};

// AL-75: the trade window (sim/trade.hpp) with the wheel's target: the goods of
// both sides with the other's worth and taste of each, the draft the player is
// putting together as the other side weighs it, a counter-offer waiting, and
// the last word of the deal. All display texts but the numbers.
struct TradeGoodSnap {
    std::string item, name;
    int n = 0;          // spare (carried, not worn)
    double worth = 0;   // one of it, to the other side, in coins
    int taste = 0;      // the other's taste: 1 likes, -1 dislikes, 0
};
struct TradeSnap {
    EntityId with = sim::kNone;
    std::string role;                          // display name of their trade role, "" = none
    std::vector<TradeGoodSnap> mine, theirs;
    std::vector<std::string> likes, dislikes;  // category names, as they say them
    std::string mode, verdict, reason;          // the draft weighed: accept | counter | refuse, reason text
    double gain = 0, loss = 0, keep = 1;
    bool counter = false;                       // they made a counter the player may take
    std::string counterText;
    std::vector<std::string> last;              // the last trade lines of the player's trace (display)
};

struct Snapshot {
    Tick tick = 0;
    SnapClock::time_point tickAt = SnapClock::now();
    double tickSeconds = 1.0;   // real seconds one tick takes at this speed
    std::string season;
    double tempC = 0, rain = 0;
    std::vector<ActorSnap> actors;
    std::vector<Line> chronicle;   // newest last, a bounded tail
    Detail detail;
    WheelSnap wheel;
    EntityId controlled = sim::kNone;
    bool paused = false;
    int speed = 1;
    std::string result;
    std::string settlement;
    double centre[2] = {0, 0};   // the settlement's centre, sim metres
    double mapSize = 0;
    // Building (sim/building.hpp): every piece, what planned ones still lack,
    // and the family the player builds for.
    sim::PieceMap pieces;
    std::map<EntityId, std::map<std::string, int>> missing;
    EntityId builder = sim::kNone, builderFamily = sim::kNone;
    ColonySnap colony;
    CraftSnap craft;
    // things one can work on near the camera and the person; the acts of the
    // target the act ring or the wheel is open on; what just came of it
    std::vector<ThingSnap> things;
    EntityId actsFor = sim::kNone;
    std::vector<ActInfoSnap> acts;
    std::vector<FeedbackSnap> feedback;   // a bounded tail, newest last
    std::vector<int> felled;              // AL-78: the drawn forest's trees felled (indices, ascending)
    SocialSnap social;                    // AL-65
    EntityId topicsFor = sim::kNone;      // AL-67: the target the topics are for
    std::vector<TopicSnap> topics;
    std::vector<CaseSnap> cases;
    std::vector<std::string> caseAnswers; // the accused's possible answers (consequences.json case.answers)
    std::vector<SpeechSnap> speech;       // AL-57: lines said near the camera, a bounded tail
    std::vector<JournalSnap> journal;     // AL-57: the player's journal, a bounded tail
    TradeSnap trade;                      // AL-75: the trade window's target (wheel target)
};

} // namespace altera::game
