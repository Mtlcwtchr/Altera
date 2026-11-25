#pragma once
// LR-140: derived needs and the tiers of needs (gdd/motivation/motivation.json:
// derived_needs, tiers, tier_gate).
//
// Derived needs (safety, provision, kinship, acceptance, standing, legacy, grace)
// do not fall by themselves: every few ticks each is worked out from the state of
// the world - a weighted mean of its inputs (content/ai/needs.json "derived") -
// and the value goes toward it smoothly (tier_gate.rate_per_day of the gap a
// day). They are kept in Character::needs beside hunger and the rest, so
// `need:<id>` reads them in Utility like any need and saves/loads them.
//
// The tiers group all needs into a pyramid (body, safety, belonging, esteem,
// purpose; spirit beside it). A goal of tier k is scored times
// prod gate(sat_j) over the tiers j below k (sat = the least of the tier's gate
// needs): a starving person still may pray, only less (the floor).
// Every character is the same here - animals have their own drives (animals.cpp).

#include <string>
#include <vector>

#include "sim/world.hpp"

namespace lr::sim {

// How much `c` cares about a weighted derived need (0..1; 1 when not weighted).
double needCare(const Content& ct, const Character& c, const std::string& need);
// What Utility reads as need:<id>: the value, for a weighted derived need
// 1 - care * (1 - value) (someone who does not care is not pulled by it).
double needPull(const Content& ct, const Character& c, const std::string& need);
// The value a derived need of `c` goes toward now (0..1).
double derivedTarget(const World& w, const Character& c, const DerivedNeedDef& d);
// Satisfaction of a tier: the least of its gate needs (pulls), 1 when it has none.
double tierSat(const World& w, const Character& c, const NeedTierDef& t);
// The multiplier a goal's score gets from the tiers below its own (1 = none).
double tierGate(const World& w, const Character& c, const std::string& goal);
// Works the derived needs of everyone out and moves them toward it (World::step).
void derivedNeedsTick(World& w);

// For the client: the tiers in pyramid order (spirit last) with their needs.
struct NeedView {
    std::string id;
    double v = 1.0;     // the value
    double care = 1.0;  // how much the person cares (derived, weighted)
    double crit = 0.0;  // critical level (base needs)
    bool derived = false;
};
struct TierView {
    std::string id;
    int order = 0;
    double sat = 1.0;
    std::vector<NeedView> needs;
};
std::vector<TierView> needTiersOf(const World& w, const Character& c);
// The tier a need belongs to ("" = none).
std::string needTier(const Content& ct, const std::string& need);

} // namespace lr::sim
