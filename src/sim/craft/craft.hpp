#pragma once
// The crafting resolver (gdd crafting design v0.2 §4-§6, §16): one step of an
// attempt over item instances, assembly, recognition of the archetype,
// flaws and salvage. Deterministic: every roll is a function of (world seed,
// actor, tick, step number). Player and NPC use the same calls.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/craft/craft_data.hpp"

namespace lr::sim::craft {

struct ItemFlaw {
    std::string id;
    bool revealed = false;
};

struct StepRec { // one entry of an item's history (designs and recipes are made of these)
    std::string process;
    std::map<std::string, std::vector<std::string>> slots; // slot -> "mat:<id>" / "form:<id>" / "step:<n>"
    std::string shape, mode;
    int len = 0, heat = -1;
    double q = 0.0;
    std::string outForm, outShape, outMat; // what came out (fast craft takes a stored one instead)
};

// AL-117: a rune cut into a carrier (inscribe) and the aspects laid into it (bind).
struct Rune {
    double q = 0.0;                       // quality of the cut
    double conductivity = 0.0;            // of the carrier piece
    double capacity = 0.0;                // mana_capacity of the carrier piece
    std::map<std::string, double> aspects; // "fire" (+), "fire.minus" (-), "fire.shift" (~) -> power
    std::string trigger;                  // constant | on_hit | on_touch | on_word | on_threat
    double charge = 0.0;                  // 0..capacity: what is left to spend
    bool bound() const { return !aspects.empty(); }
};

struct Item {
    std::string material;                 // main material
    std::map<std::string, double> p;      // current properties (single piece)
    std::string shape = "lump";
    int len = 1, thick = 1;
    double frac = 1.0;                    // salvage: share of the original mass
    double q = 0.5;                       // quality 0..1
    int qSteps = 0;
    double qSum = 0.0, qMin = 2.0;        // qMin 2 = no step yet
    std::vector<ItemFlaw> flaws;
    std::set<std::string> states;
    std::set<std::string> tags;           // material tags + profile marks
    std::map<std::string, double> effects;
    std::set<std::string> extracted;      // extraction methods applied
    struct Part {
        std::string role;
        std::vector<Item> item;           // exactly one (vector: Item is incomplete here)
    };
    std::vector<Part> parts;              // an assembly; empty = a single piece
    double joint = 1.0;
    std::string jointType;
    double wear = 0.0;                    // 0 new .. 1 worn out
    std::uint64_t maker = 0;
    // AL-117: enchantment and the story of the thing (an artifact is a thing with a rich one)
    bool inscribed = false;
    Rune rune;
    int deeds = 0;                        // notable things done with it (fights won, foes slain)
    double resonance = 0.0;               // 1 - exp(-deeds / resonance_deeds): the "soul" of the thing
    std::vector<std::string> history;     // the last deeds ("slew", "won", ...), newest last
    std::vector<StepRec> trace;
    // recognition (recognize())
    std::string form;                     // archetype, improvised_*, or "" (an intermediate)
    double formScore = 0.0;
    std::map<std::string, double> fn;     // function values
    std::string kind;                     // derive kind (weapon, armor, tool, ...)
    std::map<std::string, double> derived;
    std::map<std::string, std::string> derivedStr;
    std::vector<std::string> derivedTags;

    bool isAssembly() const { return !parts.empty(); }
    bool hasFlaw(const std::string& id) const;
    const Item* part(const std::string& role) const;
};

// A fresh piece of raw material as it is gathered.
Item rawItem(const CraftData& d, const std::string& material, double q = 0.5);
double itemMass(const CraftData& d, const Item& it);
// Properties after flaws and temporary states (heat level for "hot").
std::map<std::string, double> effectiveProps(const CraftData& d, const Item& it, int heat = 0);
// Functions, archetype, derive. Called after every step.
void recognize(const CraftData& d, Item& it);
std::string qualityTier(const CraftData& d, double q); // crude | common | good | masterwork
// Worth of a made thing (AL-87): the product of the value components it really holds:
// its archetype, its materials and shapes (an assembly blends its parts: mass average plus
// a share of the finest part), the processes it went through, its flaws and its tags
// (crafting_config worth.tags). 1 = an ordinary thing of its kind; skill (q) is separate.
double itemWorth(const CraftData& d, const Item& it);
// worth.tags over a plain list of tags (items that were not crafted).
double tagsWorth(const CraftData& d, const std::vector<std::string>& tags);

struct Actor {
    std::uint64_t id = 0;
    std::map<std::string, double> skills;
    std::map<std::string, double> knowsMaterial;
    std::vector<std::string> traits;
    double mood = 0.0;        // -1..1
    double repeat = 0.0;      // 0..1: done this way before (design/recipe mastery)
};

struct Tool {
    std::vector<std::string> tags;
    double hardness = 0.5, q = 0.5;
};
Tool toolOf(const CraftData& d, const Item& it); // tags from derive, hardness from its material
std::vector<Tool> bareTools(); // what a person always has (hands)

struct Workplace {
    std::vector<Tool> tools;
    std::vector<std::string> stations; // station ids at hand
    double hygiene = -1.0;             // -1: the station's default
};

struct Target {
    std::string shape;
    int len = 0;        // 0: the shape's default
    std::string mode;
    int heat = -1;      // -1: the highest the stations give
};

struct StepInput {
    std::string process;
    std::map<std::string, std::vector<Item>> slots;
    Target target;
};

struct Roll { // deterministic randomness (§1.7)
    std::uint64_t state;
    Roll(std::uint64_t seed, std::uint64_t actor, std::uint64_t tick, std::uint64_t step);
    double next(); // [0, 1)
};

enum class Outcome { NotStarted, Catastrophe, Ruined, Flawed, Ok, Good, Masterwork };
const char* outcomeName(Outcome o);

struct StepResult {
    Outcome outcome = Outcome::NotStarted;
    std::string reason;          // why it could not start: craft.reason.<code>
    std::string reasonArg;
    Item out;                    // the result (when not ruined)
    bool hasOut = false;
    std::vector<Item> salvage;   // what is left of a ruined attempt
    double difficulty = 0.0, competence = 0.0, margin = 0.0, qStep = 0.0;
    int time = 0;
    std::vector<std::string> risks; // ids that fired
    std::vector<std::string> riskEffects;
    std::map<std::string, double> skillXp;    // skill -> gain
    std::map<std::string, double> knowGain;   // material -> gain
    double toolWear = 0.0;
    std::vector<std::string> newFlaws;
};

struct Preview { // §9: what the actor expects, blurred by what they know
    bool canStart = false;
    std::string reason, reasonArg;
    double expectedQ = 0.0, qSpread = 0.0;
    double pRuin = 0.0, pFlaw = 0.0, pGood = 0.0;
    int time = 0;
    std::string likelyForm;   // "" = no idea (novice) or an intermediate
    double certainty = 0.0;   // 0 = "не знаю", 1 = sure
};

// Can the step start? Empty reason = yes.
std::string checkStep(const CraftData& d, const StepInput& in, const Workplace& wp, std::string* arg = nullptr);
StepResult runStep(const CraftData& d, const StepInput& in, const Actor& a, const Workplace& wp,
                   std::uint64_t seed, std::uint64_t tick, int stepNo);
Preview previewStep(const CraftData& d, const StepInput& in, const Actor& a, const Workplace& wp);

// Inspection (§6): reveals hidden flaws with a chance from the skill.
int inspectItem(const CraftData& d, Item& it, double skill, std::uint64_t seed, std::uint64_t tick, std::uint64_t actor);

} // namespace lr::sim::craft
