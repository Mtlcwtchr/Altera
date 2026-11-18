#pragma once
// The four-layer mind (AI_ARCHITECTURE_v0.1): Utility -> HTN -> GOAP -> FSM.
// Every layer returns a reason on failure and writes one trace line per
// decision. The same code runs for every character; controllers (control.hpp)
// only decide where layer 1's goal comes from.

#include <string>
#include <vector>

#include "sim/world.hpp"

namespace altera::sim {

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
bool canPerform(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                std::string* why, bool checkRange = true);
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

} // namespace altera::sim
