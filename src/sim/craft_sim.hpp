#pragma once
// Crafting in the life-sim (gdd crafting design v0.2 §7, §8, §17, §18; K3):
// a character's attempt with what they carry and what stands around them,
// fast craft from the personal recipe book, inspection, teaching, repair and
// breakthroughs. The resolver (sim/craft) decides; this applies the outcome
// to the character: things in hand, skill, knowledge of materials, wounds.
//
// Refs to inputs in commands: "inv:<item>" = one unit of an inventory item
// that is a crafting material (items.json `material`); "made:<n>" = the n-th
// made thing the character carries (Character::craft.items).

#include <map>
#include <string>
#include <vector>

#include "sim/craft/craft_chain.hpp"
#include "sim/loc.hpp"
#include "sim/world.hpp"

namespace lr::sim {

craft::Actor craftActor(const World& w, const Character& c);
// Stations of the objects within `radiusM` (objects.json `station`), tools
// and portable stations (items whose id is a station id, e.g. a loom) the
// character carries, and the made things that are tools.
craft::Workplace craftWorkplace(const World& w, const Character& c, double radiusM = 12.0);

struct CraftCommand {
    std::string process;
    std::map<std::string, std::vector<std::string>> slots; // slot -> refs
    craft::Target target;
};

struct CraftOutcome {
    bool started = false;
    std::string reason, reasonArg;      // craft.reason.<code> / craft.stop.<code>
    craft::StepResult step;             // single attempt
    craft::ChainRun chain;              // fast craft
    int made = -1;                      // index into c.craft.items of the result
    std::string item;                   // inventory item counted for it (form item), if any
    bool breakthrough = false;
    std::string recipe;                 // recipe id learnt or improved
    int time = 0;
};

CraftOutcome craftAttempt(World& w, Character& c, const CraftCommand& cmd);
craft::Preview craftPreview(const World& w, const Character& c, const CraftCommand& cmd);
// Fast craft (§17.4): the whole recipe `count` times; stops with a reason.
CraftOutcome fastCraft(World& w, Character& c, const std::string& recipeId, int count = 1);
craft::ChainPlan fastCraftPlan(const World& w, const Character& c, const std::string& recipeId);
// The same preview for a starter design the person does not know yet (the
// novice helper of the workshop): what of its chain the person has at hand.
craft::ChainPlan designPlan(const World& w, const Character& c, const std::string& designId);
// A starter design as a mastered recipe in the book (professions, §8).
std::string giveDesign(const World& w, Character& c, const std::string& designId, double mastery = 1.0);
int inspectMade(World& w, Character& c, int index);
bool teachRecipe(World& w, Character& teacher, Character& pupil, const std::string& recipeId);
CraftOutcome repairMade(World& w, Character& c, int index, const std::string& mode = "");

// The act hook: an interaction with a `craft` field ("design:<id>", "recipe",
// "attempt", "inspect", "teach", "repair") ran; params come from the plan step.
void craftAct(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar,
              const Json& params);

// Generated GOAP actions from the starter designs (§7 "образцы → GOAP"):
// craft:<design> for every design whose gathered inputs, tools and stations
// the world has as items and objects. Called by Content::load.
std::vector<InteractionDef> designInteractions(const Content& c);

// Player commands (World::command): craft_attempt {id, process, slots, shape,
// len, mode, heat}, craft_fast {id, recipe, count}, craft_inspect {id, item},
// craft_repair {id, item, mode}, craft_rename {id, recipe, name}. The outcome
// goes to cmd.result and to Character::craft.lastOutcome for the interface.
void installCrafting(World& w);

// Display name of a made thing (loc keys only, gdd crafting §5): an archetype
// named with its material ("железный меч") or plain ("хлеб"), an improvised
// thing, or material + shape for an intermediate.
std::string craftItemName(const Loc& loc, const std::string& lang, const craft::CraftData& d, const craft::Item& it);

} // namespace lr::sim
