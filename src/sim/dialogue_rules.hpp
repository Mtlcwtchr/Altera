#pragma once
// Dialogue generation, layer 2: choosing the line (AL-48, T2 of doc/design/DIALOGUE_GEN.md §4).
//
//   Utterance (+ World, + what was already said)  ->  ContextFacts  ->  the winning rule  ->  its `say` loc key
//
// Selection: a rule fits when every `when` criterion holds. Among the fitting
// rules that are not blocked by `cooldown` / `once_per`, the ones with the most
// criteria win (the most specific); ties are a weighted pick from the seed.
// Tie order does not depend on the order of the rule files (candidates are
// taken by id). If every fitting rule is blocked, the most specific blocked one
// is returned with `repeat = true`: a line is always found when a fallback exists,
// the caller decides whether a repeat is said.
//
// Context facts (all of them optional; a rule that asks for an absent fact does not fit,
// except `!key` and `key!=v`):
//   from the utterance  act, tone, intent, has_content, pred, pred.tag (list), neg, modality, tense,
//                       qty, unit, fact (1 when the Prop points to a world fact),
//                       role.<r> = ref kind, role.<r>.key = item / place / object type key,
//                       epi.source, epi.conf, epi.hops, epi.age_days, topic.age_days (= epi.age_days),
//                       stance.valence, stance.emphasis, stance.politeness,
//                       speaker.is_<r> / listener.is_<r> = 1 when the role names that character
//   from the world      speaker.* / listener.*: trait (list), age (child|young|adult|old: <14, <25, <60, 60+),
//                       age_years, stage (child|adult|elder), sex (f|m), role (family role), species,
//                       status (noble|clergy|burgher|commoner: a trait with that id, else commoner),
//                       state (list: drunk|scared|tired|angry|..., from moodlets/fatigue by
//                       dialogue_rules/*.json "states"), is_player, mood, reputation.<axis>,
//                       rights (list), verdict (list), is_<r> = 1 also when the role names their family;
//                       reputation.<axis> = the listener's (short form);
//                       rel.<axis> (speaker -> listener, Relation axes), rel_back.<axis> (listener -> speaker),
//                       rel.kin (list: spouse|parent|child|sibling|family), rel.pair (list of status pairs),
//                       memory.<type> = how many memories of that type the speaker keeps with the
//                       listener as doer (who[0]) - "ты ж у мельника муку украл" (AL-38),
//                       time (day|night), season
//   from SaidMemory     said.<rule> = times the speaker said this rule (to anyone),
//                       said_to.<rule> = times the speaker said it to this listener,
//                       said.topic = times the speaker told this listener about this topic (any rule),
//                       heard.topic = times anyone told this listener about this topic
//   from the caller     slot (set by chooseRule from SayContext::slot), outcome, reason, place,
//                       mode (gesture), topic.kind (interaction|terms), commitment, reply_to (act answered):
//                       the dialogue layer (T6) sets them on the facts before choosing.
//   from providers      whatever a registered FactProvider adds (its declared key prefixes).
//
// Slots (parts of a line, LOC {>wrap.epi} etc.): a rule with the criterion slot=<name> is a
// part rule. Choosing a whole line skips every part rule; choosing a slot considers only the
// rules with slot=<that name>, and gives no rule when none fits (the part is left empty).
//
// Everything is deterministic: no clock, no global rng, ties from the seed.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/dialogue_gen.hpp"

namespace altera::sim {

class World;

// "2d" -> 288 ticks, "3h" -> 18 ticks, "40" -> 40 ticks; -1 for a bad text.
Tick parseDialogueDuration(const std::string& s);

// What the utterance is about, for once_per / said.topic: "fact:<id>" when the Prop
// points to a world fact, else "pred:<pred>|<role>=<ref>..." (sorted roles); "" without content.
std::string dialogueTopic(const Utterance& u);

// --- memory of what was said -------------------------------------------------
struct SaidRecord {
    std::string rule;
    EntityId speaker = kNone, listener = kNone;
    std::string topic;
    Tick tick = 0;
    bool operator==(const SaidRecord&) const;
};

// Kept by the caller (the dialogue layer puts it into the world save, T6).
// Records are appended in time order. `once_per` lasts as long as the record is kept:
// prune() drops what is older than `keep` and caps the size.
struct SaidMemory {
    std::vector<SaidRecord> log;

    static constexpr Tick kDefaultKeep = kTicksPerDay * 90; // one season
    static constexpr size_t kDefaultCap = 20000;

    void record(const std::string& rule, EntityId speaker, EntityId listener, const std::string& topic, Tick tick);
    // True when the rule's cooldown or once_per forbids saying it now.
    bool blocked(const DialogueRuleDef& r, EntityId speaker, EntityId listener, const std::string& topic,
                 Tick now) const;
    int count(const std::string& rule, EntityId speaker, EntityId listener = kNone) const; // kNone = any listener
    int topicCount(EntityId speaker, EntityId listener, const std::string& topic) const;   // kNone speaker = anyone
    void prune(Tick now, Tick keep = kDefaultKeep, size_t cap = kDefaultCap);
    bool operator==(const SaidMemory&) const;
};
Json toJson(const SaidMemory& m);
bool fromJson(const Json& j, SaidMemory& out, std::string* error = nullptr);

// --- context facts -------------------------------------------------------------
// The pure part: only the utterance.
void addUtteranceFacts(ContextFacts& f, const DialogueGenData& d, const Utterance& u);
// The world part: speaker, listener, their relation, memories, time.
void addWorldFacts(ContextFacts& f, const World& w, const Utterance& u);
// What was already said (topic = dialogueTopic(u)).
void addSaidFacts(ContextFacts& f, const SaidMemory& said, const Utterance& u);

// A new axis for every rule: code that knows something about the moment (place,
// weather, a quest) registers a provider. `prefixes` are the keys it fills
// ("place", "quest.") - the validator knows them and stops warning.
using FactProvider = std::function<void(ContextFacts&, const World&, const Utterance&)>;
void registerFactProvider(const std::string& name, FactProvider fn, std::vector<std::string> prefixes = {});
void unregisterFactProvider(const std::string& name);
void addProviderFacts(ContextFacts& f, const World& w, const Utterance& u); // by provider name order

// All of the above (said may be null).
ContextFacts dialogueFacts(const DialogueGenData& d, const World* w, const SaidMemory* said, const Utterance& u);

// --- choosing ----------------------------------------------------------------
struct RuleChoice {
    const DialogueRuleDef* rule = nullptr; // null: no rule fits at all
    bool repeat = false;                   // every fitting rule was blocked; `rule` is the best blocked one
    int specificity = 0;
    std::vector<const DialogueRuleDef*> fitting; // every rule whose when holds, most specific first, then id
    std::vector<std::string> blocked;            // fitting rules skipped for cooldown / once_per
    std::vector<std::string> tied;               // the candidates of the weighted pick
    std::string say() const { return rule ? rule->say : std::string(); }
};

struct SayContext {
    EntityId speaker = kNone, listener = kNone;
    std::string topic;
    Tick now = 0;
    std::uint64_t seed = 0;
    std::string slot; // "" = a whole line; else a part (wrap.epi, wrap.voice, ...)
};

RuleChoice chooseRule(const DialogueGenData& d, const ContextFacts& f, const SaidMemory* said, const SayContext& ctx);

// The whole step for an utterance: facts from the world and the memory, choice,
// and (when `said` is given, a rule was found and it is not a repeat or
// `recordRepeat`) the line is written into the memory at w.now().
// The seed is hash(u.seed, speaker, tick) so the same moment gives the same line.
RuleChoice chooseLine(const DialogueGenData& d, const World& w, const Utterance& u, SaidMemory* said,
                      bool recordRepeat = false, ContextFacts* factsOut = nullptr);
// A part of a line for the same moment: the same facts (pass the ones chooseLine gave)
// plus slot=<slot>. Part rules are not written into the memory unless they have a
// cooldown or once_per.
RuleChoice chooseSlot(const DialogueGenData& d, const ContextFacts& lineFacts, const Utterance& u,
                      const std::string& slot, Tick now, SaidMemory* said);

// The rule file section "states" (dialogue_rules/*.json):
//   "states": {"drunk": {"moodlets": ["merry"]}, "tired": {"moodlets": ["exhausted"], "fatigue_ge": 0.7}}
struct DialogueStateDef {
    std::string id;
    std::vector<std::string> moodlets; // any active one
    double fatigueGe = -1.0;           // < 0 = not used
    double moodLe = 2.0;               // > 1 = not used
};
const std::vector<DialogueStateDef>& dialogueStates(const std::string& contentDir);
std::vector<DialogueStateDef> parseDialogueStates(const std::vector<Json>& ruleFiles, std::vector<std::string>* errors = nullptr);

// One line per fact and per candidate, for the debug overlay and the corpus (T7).
std::string explainChoice(const RuleChoice& c, const ContextFacts& f);

// Rule checks on top of validateDialogueGen: criterion keys that no fact
// source fills (warning), acts used by rules without a fallback that always fits
// (a rule with only act=<id>, no cooldown, no once_per) (warning), a fallback with
// cooldown / once_per (warning), rules that can never win because another rule has
// the same criteria and the same say (warning).
ValidationReport validateDialogueRules(const DialogueGenData& d, const Content* c = nullptr);
// True when `key` is filled by some fact source (built-in or a registered provider).
bool knownDialogueFact(const std::string& key);

} // namespace altera::sim
