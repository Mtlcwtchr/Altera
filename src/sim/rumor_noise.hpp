#pragma once
// Rumour noise (AL-50, doc/design/DIALOGUE_GEN.md §6): what a retelling does
// to the meaning. Operators work on a Prop, never on text, so the sim sees the
// distortion (a wrong doer is suspected, a loss grows in the telling) and the
// text simply says what the teller now believes. The truth stays in the
// world's facts; Prop::fact is kept through every distortion, so any rumour
// can be checked against what really happened.
//
// This is the pure core: no World, everything the operators need comes in
// NoiseContext (the caller - the retelling in consequences.cpp - fills it).
// Deterministic for a given Rng. Parameters: content/ai/rumor_noise.json.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sim/dialogue_gen.hpp"

namespace lr::sim {

struct NoiseParams {
    struct Op {
        double base = 0.0;
        double perHop = 0.0;        // + per hop already in the chain
        double belowConf = 1.0;     // only when confidence < this
        int minHops = 0;
        double confDelta = 0.0;     // added to confidence when it fires
        std::map<std::string, double> trait; // teller trait -> chance multiplier
        double relOver = 2.0;       // minimize: teller's affinity to the doer above this
        std::vector<std::string> roles; // drop_detail: what may be dropped (tick, qty, place roles...)
    };
    std::map<std::string, Op> ops;  // drop_detail vague_theme exaggerate minimize swap_agent swap_victim invert attribute_shift
    bool discardIfSeen = true;
    double trustPenalty = -0.1;
    bool surface = true;
    double decay = 0.7;             // confidence kept per retelling (ConsequenceRules::rumorDecay)
    static NoiseParams fromJson(const Json& noise, double decay = 0.7);
    const Op* op(const std::string& id) const;
};

// Everything about this retelling the operators may weigh.
struct NoiseContext {
    EntityId teller = kNone, listener = kNone;
    std::vector<std::string> tellerTraits;
    double tellerAffinityToDoer = 0.0;
    // Who the teller would sooner believe guilty instead, with a weight
    // (enmity, bad name, "always about"); empty = swap_agent cannot fire.
    std::vector<std::pair<EntityId, double>> agentCandidates;
    std::vector<std::pair<EntityId, double>> victimCandidates;
    // item id -> its first category tag (vague_theme: "flour" -> "food"); missing = straight to "something"
    std::map<std::string, std::string> itemCategory;
};

struct NoiseResult {
    Prop prop;
    Epistemic epi;                    // confidence decayed, chain extended with the teller
    std::vector<std::string> applied; // operator ids that fired, in order
};

// One retelling: `heard` as the teller holds it -> what the listener gets.
NoiseResult retell(const DialogueGenData& d, const NoiseParams& p, const Prop& heard, const Epistemic& epi,
                   const NoiseContext& ctx, Rng& rng);

// The listener already holds `own` about the same thing. Returns true when the
// heard version should be thrown away (the listener saw it himself and the
// versions differ); `contradicts` is set when they differ at all.
bool reconcile(const NoiseParams& p, const Prop& heard, const Prop& own, const Epistemic& ownEpi,
               bool* contradicts = nullptr);

// How far a rumour has drifted from the truth: 0 = same, 1 = nothing in common
// (different predicate). Roles weigh by importance (doer/victim most).
double drift(const Prop& truth, const Prop& rumour);

} // namespace lr::sim
