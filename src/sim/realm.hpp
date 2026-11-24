#pragma once
// The realm (AL-85, gdd/realm/REALM_v0.1.md, content/ai/realm.json): the
// world beyond the village map. A handful of settlements around it - other
// villages, a town, a keep, a monastery, a bandit camp - each with its people,
// stores, wealth, levy, morale and unrest, a ruler and an heir; and the people
// who matter in them (rulers, heirs, knights, wanderers, outlaws) as permanent
// figures with traits, axes, renown, wealth, kin and grudges.
//
// Nothing is scripted. Every story is the state moving on: a bad harvest
// across the region starves the weak, the starving riot or flee (refugees
// reach the village as strangers), bandits grow from the outcast, rulers who
// have grudges, greed or ambition and the strength for it demand tribute or go
// to war, battles kill and take knights and lords, a dead lord's child heir
// draws an ambitious kinsman's claim, the exile comes back with hired swords.
// Wanderers take the bounties that real needs post (beasts that mauled
// villagers, a camp that raids, a lord who needs swords) and earn renown or
// die.
//
// The village is a settlement of the realm too: its numbers come from the
// real sim, and what happens to it is real - a raid takes grain from the real
// storages and hurts real people, envoys talk to the real chief, a wanderer
// comes in as an ordinary Character with the figure's own id (the same person
// every time: leaving, the Character is parked whole in the figure and comes
// back as it was) and fights the real pack.
//
// Far away the step is daily and aggregate; deterministic (agentRng from the
// seed). Text only through loc keys chronicle.realm.* / realm.* (content/loc).
// Extension point for magic: quest and arc kinds carry tags, realm.json
// disabled_tags switches them off (magic is off), figures carry aptitudes,
// addRealmHook() lets later modules join the daily pass.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Content;
class Loc;

struct RealmGrievance {
    std::string cause; // raid | war | battle | spurned | tribute | killed_kin | usurped | insult | refused
    double weight = 0.0;
    int day = 0;
};

// How one polity sees another (directional).
struct RealmView {
    double opinion = 0.0;
    std::vector<RealmGrievance> grievances;
    double grudge() const;
};

struct RealmPolity {
    EntityId id = kNone;
    std::string kind;  // village | town | keep | monastery | bandit_camp (realm.json kinds)
    std::string name;  // realm.place.<name>
    Vec2 pos;          // km; the village map is at (0, 0)
    EntityId settlement = kNone; // the real Settlement (the village of the map)
    double pop = 0.0, food = 0.0, wealth = 0.0, militia = 0.0;
    double morale = 0.6, unrest = 0.1;
    EntityId ruler = kNone, heir = kNone; // figures, or Characters for the village
    std::string house;                    // the ruling family name
    std::string institution;              // chief | lord | abbot | ataman | council
    EntityId liege = kNone;               // whose vassal it is
    EntityId tributeTo = kNone;
    double tribute = 0.0;                 // paid each month
    bool ruined = false;
    int lastRaid = -9999, lastRaided = -9999, lastRevolt = -9999, lastRefugees = -9999;
    int unrestDays = 0;                   // days unrest stayed over the revolt line
    int founded = 0;
};

struct RealmFigure {
    EntityId id = kNone;   // the same id as its Character when it comes to the village
    std::string name;      // "Given House" (name.<word> keys)
    std::string house;
    char sex = 'm';
    int born = 0;          // world day of birth (may be negative)
    EntityId home = kNone, at = kNone; // polities
    std::string role;      // ruler | consort | heir | kin | knight | elder | abbot | ataman | outlaw | wanderer | commoner
    std::string life = "settled"; // settled | road | outlaw (reconsidered each season)
    std::vector<std::string> traits;
    double martial = 0.3, ambition = 0.3, greed = 0.3, honor = 0.5, wanderlust = 0.2;
    double renown = 0.0, wealth = 0.0, health = 1.0;
    bool alive = true;
    std::string death;
    int deathDay = 0;
    EntityId spouse = kNone, father = kNone, mother = kNone;
    std::vector<EntityId> children;
    EntityId travelTo = kNone;
    int arrive = 0;
    EntityId quest = kNone;
    EntityId claim = kNone;   // a polity it claims (exiled heir, usurper)
    EntityId captor = kNone;  // held by that polity
    std::map<std::string, double> opinions; // figure id -> -1..1
    std::map<std::string, double> aptitudes; // magic extension (empty)
    std::vector<Json> deeds;                 // {day, code, ...}
    bool present = false;     // a Character in the village now
    int leaveDay = 0;
    Json parked;              // the Character while away (empty: never came)
    int ageYears(int today) const { return (today - born) / kDaysPerYear; }
};

struct RealmQuest {
    EntityId id = kNone;
    std::string kind;            // realm.json quests
    EntityId giver = kNone;      // figure or Character
    EntityId polity = kNone;     // whose need it is
    EntityId target = kNone;     // a beast (Character), a camp or a polity
    double reward = 0.0;
    int posted = 0, deadline = 0;
    std::vector<EntityId> takers;
    std::string status = "open"; // open | done | failed | expired
    int closed = 0;
};

struct RealmWar {
    EntityId id = kNone;
    EntityId attacker = kNone, defender = kNone;
    EntityId claimant = kNone;  // a war for a claim: who would rule
    std::string cause;
    int start = 0, last = 0;
    double score = 0.0;         // > 0 the attacker is winning
    int battles = 0, raids = 0;
    double startMilA = 0.0, startMilD = 0.0;
    std::vector<EntityId> alliesA, alliesD;
    bool siege = false;
    bool over = false;
    std::string outcome;
};

struct RealmOffer {
    EntityId id = kNone;
    std::string kind;    // tribute | alliance | marriage | peace
    EntityId from = kNone, to = kNone; // polities
    EntityId envoy = kNone;
    double amount = 0.0;
    EntityId groom = kNone, bride = kNone;
    EntityId war = kNone;
    int day = 0, deadline = 0;
    std::string status = "pending"; // pending | accepted | refused
};

struct RealmState {
    bool inited = false;
    int nextId = 1;
    int year = -1;
    double harvest = 1.0;  // this year's yield over the region (one weather for all)
    std::vector<RealmPolity> polities;
    std::vector<RealmFigure> figures;
    std::map<std::string, RealmView> views; // "<a>><b>"
    std::vector<std::pair<EntityId, EntityId>> alliances;
    std::vector<RealmQuest> quests;
    std::vector<RealmWar> wars;
    std::vector<RealmOffer> offers;
    std::map<std::string, int> stats;

    RealmPolity* polity(EntityId id);
    const RealmPolity* polity(EntityId id) const;
    RealmFigure* figure(EntityId id);
    const RealmFigure* figure(EntityId id) const;
    RealmView& view(EntityId a, EntityId b);
    const RealmPolity* local() const;
    RealmPolity* local();
};

void to_json(Json& j, const RealmState& s);
void from_json(const Json& j, RealmState& s);

// The state lives in World::moduleState("realm"); these read and write it.
RealmState loadRealm(const World& w);
void saveRealm(World& w, const RealmState& s);

// Problems of realm.json against the content (unknown traits, kinds); empty = fine.
std::vector<std::string> checkRealm(const Content& c);
// Loc keys the module writes.
std::vector<std::string> realmLocKeys();

// The daily pass (installAi's think hook, once a day): the first one lays the region out.
void realmDaily(World& w);
// Installs the realm.* command handlers.
void installRealm(World& w);

// A later module's part of the daily pass (magic, cults...): called after the realm's own.
using RealmHook = std::function<void(World&, RealmState&)>;
void addRealmHook(RealmHook h);

// For output: a figure's or polity's display noun ({nom, gen, ..., g}), null when `id` is neither.
Json realmNoun(const Loc& loc, const std::string& lang, const World& w, EntityId id);

// Tests and tools
struct RealmRules;
const RealmRules& realmRules(const Content& c);
// Whether the content has realm.json (the layer is off without it).
bool realmLoaded(const Content& c);
// Whether a quest kind is on (known and none of its tags disabled: magic is off).
bool realmQuestOn(const Content& c, const std::string& kind);
// Strength in the field (levy x morale x the leader, hired swords).
double realmStrength(const World& w, const RealmState& s, const RealmPolity& p);
// A raid of `by` on `on` now (for tests and commands): what was taken.
double realmRaid(World& w, RealmState& s, EntityId by, EntityId on);

} // namespace altera::sim
