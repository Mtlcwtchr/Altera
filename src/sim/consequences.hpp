#pragma once
// Consequences of deeds (AL-31, content/ai/consequences.json).
//
// Nothing a person may do to another household's things or beasts is
// forbidden: the act is done as what it is (theft, trespass) and the world
// answers. Who saw it remembers it, trusts the doer less and tells the
// owners; the doer's repute moves. When nobody saw it the owners still find
// the loss sooner or later, remember it and suspect someone - whoever was
// about, whoever has a bad name, whoever they already hate - and with enough
// certainty they accuse. What people saw or suspect passes on in talk as
// rumour, a little less sure each time.
//
// All of it is deterministic (agentRng from the seed), all text is loc keys
// (memory.*, chronicle.*, act.outcome.*). The trail of an unseen deed is an
// Event ("deed_trail") in the world's queue, so saves and replays keep it.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;
struct WorldObject;
struct LegalTag;

struct ConsequenceRules {
    // tag -> reputation axis -> delta, applied to the doer once per deed when it is known
    std::map<std::string, std::map<std::string, double>> reputation;
    // tags that wrong an owner (a trail is left, the owner looks for the doer)
    std::set<std::string> wrongsOwner = {"theft", "trespass", "poaching", "banditry"};
    double witnessTrust = -0.2, witnessEnmity = 0.05;   // a bystander toward the doer
    double victimTrust = -0.4, victimEnmity = 0.25;      // the wronged household toward the doer (x certainty)
    double tellConfidence = 0.9;                         // a witness's account to the owners
    // the trail of an unseen deed
    double noticeDaily = 0.3;        // an owner finds the loss on a given day
    double noticeAtHome = 0.6;       // ...when one of them is by the thing that day
    double atHomeM = 30.0;
    int trailDays = 20;              // after that the loss is put down to bad luck
    double nearbyM = 90.0;           // who was about when it happened
    // suspicion: score = nearby * min(1, 2 / people about) + bad name + enmity + earlier word of such deeds
    double suspectNearby = 0.55, suspectBadName = 0.5, suspectEnmity = 0.3, suspectPrior = 0.35;
    double suspectMin = 0.2;         // below this nobody is suspected
    double accuseAt = 0.55;          // certainty at which the wronged accuse
    std::string accuseTask = "accuse_publicly";
    double accusePriority = 0.75;
    Tick accuseTtl = 288;
    // rumour
    std::set<std::string> talkActs = {"chat", "gossip", "tell_story", "share_meal", "joke", "greet"};
    double rumorDecay = 0.7, rumorMin = 0.25;
    double rumorTrust = -0.15;       // a listener toward the doer (x certainty)
};

// consequences.json next to the content (cached per content dir); defaults without one.
const ConsequenceRules& consequenceRules(const Content& c);

// AL-32: builds "<id>@foreign" for every act that asks target_owner:family
// (called once by Content::load, after validation).
void addForeignVariants(Content& c);

// A deed just done (applyEffects calls it after the effects): legal tags, the
// witnesses. Witnesses remember and tell, the doer's repute moves, an unseen
// wrong leaves a trail; talk passes rumours on; the player's person gets an
// act.outcome trace either way.
void afterDeed(World& w, Character& doer, const InteractionDef& d, WorldObject* target, Character* targetChar,
               bool success, const std::vector<LegalTag>& tags, const std::vector<EntityId>& witnesses);

// Installs the "deed_trail" event handler.
void installConsequences(World& w);

// One retelling of the teller's strongest word of a misdeed to `to` (AL-35):
// decayed and maybe distorted by the rumour noise (AL-50, sim/rumor_noise.hpp);
// a listener who saw it himself rejects a differing version and remembers the
// teller lied. Called by afterDeed for talk acts.
void passRumor(World& w, Character& from, Character& to, const ConsequenceRules& R);
// How far word of the world's facts has drifted from the truth: facts,
// memories of them, distorted ones, ones naming the wrong doer, mean drift, max chain.
Json rumorReport(const World& w);

// Memory types about somebody's misdeed (who[0] = the doer).
bool isDeedMemory(const std::string& type);

} // namespace altera::sim
