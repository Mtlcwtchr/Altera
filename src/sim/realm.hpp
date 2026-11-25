#pragma once
// The realm (AL-85, AL-92; gdd/realm/REALM_v0.2_unified.md, content/ai/realm.json):
// the world beyond the village map and the village in it, by one set of rules.
//
// One kind of settlement, one kind of person. Every settlement - the map's
// village, the next village, a town, a keep, a monastery, a robbers' nest - is a
// World Settlement; what it is (its size, its tags: raiders, outlaw, cult,
// fortified, farming, trade, vassal...) is classified each season from its
// numbers and its history, never set. Every person is a Character with a
// Standing (role, life, axes, renown, claim...). The only difference is how a
// thing is computed: a settlement in "full" mode is on the map, its people act
// and its stores are real; one in "stat" mode is counted by the same rules
// (people offstage in World::offstage()). A person who walks onto the map is
// moved, not copied: the same object, the same id, every time.
//
// Nothing is scripted. Every story is the state moving on: a bad harvest
// starves the weak, the starving riot or flee (refugees reach the village as
// strangers), outlaws gather where the land is restless and live by raids,
// rulers with grudges, greed or ambition and the strength for it demand tribute
// or go to war, battles kill and take knights and lords, a child heir draws an
// ambitious kinsman's claim, the exile comes back with hired swords. Wanderers
// take the bounties real needs post and earn renown or die.
//
// Deterministic (World::rng by id and salt). Text only through loc keys
// chronicle.realm.* / realm.* (content/loc). Extension point for magic: quest
// and arc kinds carry tags, realm.json disabled_tags switches them off (magic
// is off), Standing::aptitudes, addRealmHook() joins the daily pass.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Content;
struct Settlement;
struct Character;
class Loc;

struct RealmGrievance {
    std::string cause; // raid | war | battle | spurned | tribute | killed_kin | usurped | insult | refused
    double weight = 0.0;
    int day = 0;
};

// How one settlement sees another (directional).
struct RealmView {
    double opinion = 0.0;
    std::vector<RealmGrievance> grievances;
    double grudge() const;
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

// What is between settlements; the settlements themselves are World::settlements()
// (Settlement, its realm fields) and the people are Characters (Standing), on the map
// or offstage (AL-92).
struct RealmState {
    bool inited = false;
    int nextId = 1;
    int year = -1;
    double harvest = 1.0;  // this year's yield over the region (one weather for all)
    std::map<std::string, RealmView> views; // "<a>><b>"
    std::vector<std::pair<EntityId, EntityId>> alliances;
    std::vector<RealmQuest> quests;
    std::vector<RealmWar> wars;
    std::vector<RealmOffer> offers;
    std::map<std::string, int> stats;
    std::map<std::string, int> once;  // dedup marks of the chronicle (a famine per season, unpaid tribute per year)

    RealmView& view(EntityId a, EntityId b);
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
// A quest posted from outside the realm's own pass (AL-118: magic); reward < 0 = rolled from realm.json.
RealmQuest* realmPostQuest(World& w, RealmState& s, const std::string& kind, EntityId polity, EntityId giver,
                           EntityId target, double reward);

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
double realmStrength(const World& w, const RealmState& s, const Settlement& p);
// The settlement the player's map shows (the first one in full mode), null if none.
Settlement* realmLocal(World& w);
// Re-reads what a settlement is (size, tags) from its numbers and history.
void realmClassify(World& w, Settlement& p);
// A raid of `by` on `on` now (for tests and commands): what was taken.
double realmRaid(World& w, RealmState& s, EntityId by, EntityId on);

} // namespace altera::sim
