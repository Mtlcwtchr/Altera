#pragma once
// Social chains (AL-40, content/ai/social_chains.json), as in The Sims: an act
// one person does to another that the other answers with an act of their own.
// The actor asks, the target listens and decides (its relation to the actor,
// mood, needs, traits, kinship, the player's tone), then either refuses or
// both play their side of the act - in place or at a place they walk to
// together (a bed). An accepted invitation (`follow`) makes the target follow
// the actor - an NPC's plan or a player's WASD - and join what the leader does.
//
// The pair is driven from the actor's tick; the target's own mind is paused
// (its plan keeps its position and resumes afterwards). Everything is
// deterministic (agentRng) and every step leaves a trace code (trace.social.*).

#include <map>
#include <string>
#include <vector>

#include "sim/rules.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;

struct SocialAccept {
    double base = 0.3, kin = 0.0, spouse = 0.0, mood = 0.0, min = 0.02, max = 0.97;
    std::map<std::string, double> rel;    // * the target's relation axis toward the actor
    std::map<std::string, double> needs;  // * (1 - the target's need)
    std::map<std::string, double> traits; // + when the target has the trait
};

struct SocialStage {
    std::string state = "Interact"; // FSM state both sides show
    std::string anim, actorAnim, targetAnim;
    Tick ticks = 0;                 // 0 = the act's duration
    std::vector<Effect> eff;        // at the end of the stage (t. = the target)
};

struct SocialChainDef {
    std::string id; // = the catalog act
    SocialAccept accept;
    std::vector<std::string> forbid; // close_kin | child
    double minFamiliarity = -1.0;
    std::string place;               // object type the pair goes to, "" = where they stand
    std::vector<Effect> onRefuse;    // actor = the asker, t. = the one who refused
    std::vector<Effect> onAccept;
    std::vector<SocialStage> stages;
    bool follow = false;
    Tick followTicks = 36, playerLeaderTicks = 72;
    std::string joinMoodlet;
    // AL-73: a talk (every stage is "Talk"): the lines the pair says, by
    // group (social_chains.json talk.groups); a busy listener answers over
    // its work instead of dropping it.
    std::string talk;
    bool talkOnly = false;
    // A talk act with no chain of its own (talk.default): no question of
    // its own - an act the target consents to (consent: target) is asked as
    // the catalog asks it, any other is simply heard.
    bool byDefault = false;
};

struct SocialRules {
    Tick askTicks = 1, refuseTicks = 1, answerTicks = 6, approachMaxTicks = 12, refusedCooldown = 36;
    double placeRadiusM = 1500.0, togetherM = 2.0, followM = 2.5, followLostM = 1500.0, followSpeedMult = 1.2,
           joinRadiusM = 40.0;
    std::string askAnim = "social_ask", listenAnim = "social_listen", refuseAnim = "social_refuse",
                rejectedAnim = "social_rejected";
    std::map<std::string, SocialChainDef> chains;
    std::map<std::string, std::string> talkGroups; // meaning -> line group (talk.groups)
    std::vector<std::string> asideStates;          // a target in these talks over its work
};

// Loaded once per content directory; nullptr without the file. Throws on a
// chain that names an unknown act or an act that does not target a person.
const SocialRules* socialRules(const Content& c);
// The chain of an act, nullptr when the act is done the plain way.
const SocialChainDef* socialChain(const Content& c, const std::string& act);

// Why the target cannot be asked at all now ("" = it can): forbid rules,
// a stranger, the target busy (non-interruptible, fighting, down, captive,
// already in another chain).
std::string socialBlock(const World& w, const Character& actor, const Character& target, const SocialChainDef& sc);
// The chance the target says yes (what the interaction wheel shows).
double socialAcceptChance(const World& w, const Character& actor, const Character& target, const InteractionDef& d,
                          const SocialChainDef& sc, const std::string& tone);

// The actor starts asking (the act's FSM start). False + why when the chain
// cannot start (blocked); a refusal later is a Failed tick.
bool beginSocial(World& w, Character& actor, Character& target, const InteractionDef& d, std::string& why);

enum class SocialStep {
    None,    // not in a chain (or a player follower left to the player): think as usual
    Running, // the chain holds the body this tick
    Done,    // the actor's act is done (accepted and played out)
    Failed   // the actor's act failed: refused, broken, no place (why)
};
SocialStep tickSocial(World& w, Character& c, std::string& why);

// Breaks the chain `c` is in (both sides). The actor side, when it is the
// partner, fails with `why` at its next tick.
void endSocial(World& w, Character& c, const std::string& why);
// A player's answer to an NPC's ask (the "answer_social" command).
bool answerSocial(World& w, Character& target, bool accept, std::string& why);
// Moves the followers of `leader` after it (the player's steer/WASD).
void pullFollowers(World& w, const Character& leader);
std::vector<EntityId> followersOf(const World& w, EntityId leader);

// Commands: answer_social {id, accept}, dismiss_follower {id, follower?}.
void installSocial(World& w);

} // namespace lr::sim
