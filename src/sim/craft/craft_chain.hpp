#pragma once
// Chains of steps (gdd crafting design v0.2 §7, §16.1, §17): starter designs,
// the personal recipe book with fragmentary knowledge, fast craft (up to N
// stages, stops with a reason), teaching, and breakthroughs (§18).

#include <cstdint>
#include <string>
#include <vector>

#include "sim/craft/craft.hpp"

namespace lr::sim::craft {

// A recipe step = a design step + what the owner knows of it.
struct RecipeStep {
    DesignStep step;
    bool known = false;
    std::string outForm, outShape, outMat;
};

struct Recipe {
    std::string id;           // r_<hash>
    std::string nameKey;      // loc key (form name) when it makes an archetype
    std::string nameUser;     // the player's own name
    std::string form;         // what it made last time
    std::vector<RecipeStep> steps;
    double mastery = 0.0;
    int successes = 0;
    double bestQ = 0.0;
    std::vector<std::string> notes;
    std::vector<std::string> tags; // player's marks
    bool master(const CraftConfig& c) const {
        return mastery >= c.masterThreshold && successes >= c.masterSuccesses;
    }
    int knownCount() const;
};

struct RecipeBook {
    std::vector<Recipe> recipes;
    Recipe* find(const std::string& id);
    const Recipe* find(const std::string& id) const;
    Recipe* byForm(const std::string& form);
};

// A finished item's history as design steps (what the recipe book stores).
std::vector<RecipeStep> stepsOf(const Item& it);
std::string recipeIdOf(const std::vector<RecipeStep>& steps);
// After a success: a new fragmentary record, or one more fragment and mastery
// for the record of the same chain. Returns the record.
Recipe& learnFromItem(const CraftData& d, RecipeBook& book, const Item& made, std::uint64_t seed, std::uint64_t actor);
// teach_design (§17.3): the pupil gets a copy with mastery <= teacher's skill share
// and fragments by the pupil's level.
Recipe teachRecipe(const CraftData& d, const Recipe& r, double teacherSkill, double pupilSkill);
Recipe recipeFromDesign(const CraftData& d, const DesignDef& ds, double mastery, bool allKnown);

// --- running a chain (fast craft) ------------------------------------------

struct ChainRun {
    bool done = false;
    std::string stop;        // craft.stop.<code>: missing, cannot_start, ruined, hurt, too_long, unknown_step
    std::string stopArg;
    int stopStep = -1;       // 0-based step index
    Item out;                // the final item (when done)
    std::vector<StepResult> results;
    std::vector<int> ran;    // indices of the steps that ran
    std::vector<int> skipped;// stages taken from the store instead
    int time = 0;
    std::vector<Item> leftovers; // salvage and things made but not used
};

struct ChainOpts {
    int maxStages = 12;
    bool rawFromNothing = false; // tests / previews: gathered materials appear as needed
    double rawQ = 0.5;
    bool stopOnRisk = true;      // stop when a risk hurt the actor
    double fillUnknownPenalty = 0.15; // unknown stages (fragmentary record): difficulty added
};

// Runs the steps in order, taking inputs from `pool` (consumed). A stored item
// matching a stage's output (form, or material + shape) replaces that stage
// and everything only it needed. Steps are rolled like any attempt.
ChainRun runChain(const CraftData& d, const std::vector<RecipeStep>& steps, std::vector<Item>& pool, const Actor& a,
                  const Workplace& wp, std::uint64_t seed, std::uint64_t tick, const ChainOpts& opt = {});

struct ChainPlan { // the preview of §17.4: time, use, what is missing
    bool ok = true;
    int stages = 0, time = 0;
    std::vector<std::string> missing;   // refs not found in the pool
    std::vector<std::string> needTools; // tool tags not at hand
    std::vector<std::string> needStations;
    double expectedQ = 0.0;
};
ChainPlan planChain(const CraftData& d, const std::vector<RecipeStep>& steps, const std::vector<Item>& pool,
                    const Actor& a, const Workplace& wp, const ChainOpts& opt = {});

// Breakthrough (§18): chance for an attempt that matches no known recipe.
double breakthroughChance(const CraftData& d, const Actor& a, double skill, double novelty);

} // namespace lr::sim::craft
