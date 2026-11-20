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
    bool animal = false, alive = true, player = false, down = false, fighting = false, captive = false;
    char sex = 'f';
    int age = 0, group = 1, wounds = 0;
    double px = 0, py = 0, x = 0, y = 0;   // sim metres, last tick and this one
    double hp = 1;
    double lowNeed = 1;                    // the lowest need, 0..1
    std::string lowNeedId;                 // and which
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

struct Detail {
    EntityId id = sim::kNone;
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
};

} // namespace altera::game
