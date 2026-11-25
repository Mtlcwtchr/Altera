// Statistical (LOD) resolution of duels (COMBAT_CORE_SPEC §7): far from the
// player a fight is not played round by round; its result is drawn from a
// table of outcomes the same core produced for that match-up (weapons,
// armour, shields, skill). The table is filled lazily, `samples` full fights
// per cell, seeded from the cell key only, so the result does not depend on
// which fights came first. Skill between two grid points picks the upper or
// the lower cell with the matching probability (linear interpolation of the
// distribution). Losses are laid out per person: a drawn sample carries the
// states, wounds and blood of both sides, as a real fight would.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sim/combat/combat.hpp"

namespace lr::combat {

struct LodConfig {
    int samples = 800;
    double skillStep = 10.0;
};

struct DuelSample {
    State a = State::Fight, b = State::Fight;
    int rounds = 0;
    std::vector<int> woundsA, zoneA, woundsB, zoneB;
    std::vector<double> dmgA, dmgB;
    double bloodA = 100.0, bloodB = 100.0;
};

class DuelTable {
public:
    DuelTable(const CombatData& d, LodConfig cfg) : d_(d), cfg_(cfg) {}

    // One statistical duel; `rng` picks the grid cell and the sample.
    const DuelSample& resolve(const FighterSpec& a, const FighterSpec& b, Rng& rng);

    // Fighters after a statistical duel: fresh fighters with the sample's
    // states, wounds, zone hp and blood (the shape Engine::fight leaves).
    void apply(const DuelSample& s, Fighter& a, Fighter& b) const;

    // Share of fights `a` wins (b leaves the fight, a stays), interpolated
    // over the skill grid the way resolve() draws its cell.
    double winShare(const FighterSpec& a, const FighterSpec& b);

    std::size_t cells() const;

private:
    const std::vector<DuelSample>& cell(const FighterSpec& a, const FighterSpec& b);

    const CombatData& d_;
    LodConfig cfg_;
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<std::vector<DuelSample>>> cells_;
};

} // namespace lr::combat
