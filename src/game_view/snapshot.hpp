#pragma once
// What the sim thread hands the frame (src/game_view/sim_game.cpp builds it
// under its lock; the HUD, src/game_view/hud.cpp, only reads it). Display
// strings in here are already looked up in content/loc.

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/core.hpp"

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
};

} // namespace altera::game
