// Event stream -> animation cues (COMBAT_PRESENTATION.md §2-3). No rendering:
// for each combat event the binder picks the animation key from
// content/combat/animations.json and the clip on the UAL rig from
// content/animations/humanoid_ual/manifest.json (keys.combat), with a start
// time inside the round. A key without a clip is reported as a gap.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/combat/combat.hpp"

namespace altera::combat {

struct AnimCue {
    int round = 0;
    double startMs = 0.0;      // from the start of the combat
    std::int64_t actor = -1;
    EventKind source = EventKind::Attack;
    std::string key;           // animations.json name ("atk_blade_overhead_a")
    std::string clip;          // UAL clip id, empty = gap
    std::string tier;          // novice / trained / master (attacks)
    double speedMult = 1.0;    // clip time scale from weapon speed, fatigue, tier
    bool additive = false;     // hit reactions and wound poses go on an additive layer
};

class AnimBinder {
public:
    // manifest: parsed humanoid_ual/manifest.json
    AnimBinder(const CombatData& d, const nlohmann::json& manifest);

    // Fighters' current kit (weapon, skill, fatigue) comes from the event itself
    // for attacks; reactions need only the event.
    std::vector<AnimCue> bind(const std::vector<Event>& roundBatch);

    // Keys used so far that the manifest has no clip for, and event kinds with no key at all.
    const std::set<std::string>& missingClips() const { return missingClips_; }
    const std::set<std::string>& unboundEvents() const { return unbound_; }

private:
    std::string clipFor(const std::string& key);
    std::string variant(const OJson& list, std::uint64_t salt) const;
    std::string tierOf(double skill) const;

    const CombatData& d_;
    std::map<std::string, std::string> clips_; // key -> first clip
    std::set<std::string> missingClips_, unbound_;
    std::map<std::int64_t, int> lastZone_;     // actor -> zone of the last wound (falls, deaths)
    double roundMs_ = 1500, hitFrameMs_ = 520, windupMs_ = 350;
};

} // namespace altera::combat
