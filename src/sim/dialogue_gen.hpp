#pragma once
// Dialogue generation, the contract (AL-46, design: doc/design/DIALOGUE_GEN.md).
//
// A line is built in three layers and only the last one knows words:
//   1. meaning  - Prop (who did what to whom) + Epistemic (how the speaker
//                 knows it) + the speech act: an Utterance. The sim makes and
//                 keeps these; rumour noise works on them, not on text.
//   2. choice   - ContextFacts about the moment are matched against rules
//                 (content/ai/dialogue_rules/*.json); the rule with the most
//                 criteria that all hold wins, ties by weight from the seed.
//   3. surface  - the rule's loc key is rendered by Loc (LOC_FORMAT v0.2):
//                 sub-rules, predicate frames, agreement. Code passes meaning,
//                 never words.
// The truth is kept in the core (World facts, AL-37/AL-55); a Prop points to
// the Fact it is about (or to none: a lie, a guess).
//
// This header is the shared contract of AL-47..AL-55: types, their JSON form
// and the content formats with a validator. Selection, rendering and noise
// live in their own files.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sim/content.hpp"
#include "sim/core.hpp"

namespace lr::sim {

class Loc;

using FactId = EntityId; // World fact id (monotonic, never reused); kNone = no fact

struct Prop;

// What fills a role of a proposition.
struct Ref {
    // char | item | object | family | place | text | num | prop
    std::string kind;
    EntityId id = kNone;      // char, object, family
    std::string key;          // item / object type / place id, or a loc key for text
    double num = 0.0;         // num
    std::vector<Prop> sub;    // prop: exactly one nested proposition
    bool operator==(const Ref&) const;
};

// A proposition: predicate + roles. Role names are the predicate's
// (predicates.json), e.g. theft: doer, victim, victim_family, object.
struct Prop {
    std::string pred;
    std::map<std::string, Ref> roles;
    double qty = 0.0;                // 0 = not said
    std::string unit;                // loc noun id of the measure ("sack"), optional
    bool neg = false;
    std::string modality = "fact";   // fact | plan | wish | must | can | guess
    std::string tense = "past";      // past | present | future
    Tick tick = -1;                  // when (absolute), -1 = not said
    FactId fact = kNone;             // the truth it is about; kNone = none (lie, guess, invented)
    const Ref* role(const std::string& r) const;
    bool operator==(const Prop&) const;
};

// How the speaker knows it.
struct Epistemic {
    std::string source = "seen";     // seen | heard | rumor | told | inferred | invented
    double confidence = 1.0;         // 0..1
    std::vector<EntityId> chain;     // who told it, nearest first (at most kMaxChain)
    Tick age = 0;                    // ticks since it happened / was heard
    static constexpr size_t kMaxChain = 3;
    int hops() const { return static_cast<int>(chain.size()); }
    bool operator==(const Epistemic&) const;
};

struct Stance {
    double valence = 0.0;     // -1..1, how the speaker feels about the content
    double emphasis = 0.0;    // 0..1
    double politeness = 0.5;  // 0..1
    bool operator==(const Stance&) const;
};

// One line, as the sim means it. Text is rendered from it at output time.
struct Utterance {
    EntityId speaker = kNone, listener = kNone;
    std::string act;                 // speech act id (dialogue.json speech_acts)
    std::string tone;                // tones.json id, empty = neutral
    std::string intent;              // dialog_plans id of the session, optional
    std::optional<Prop> content;     // none for greet, farewell, ...
    Epistemic epi;
    Stance stance;
    std::uint64_t seed = 0;
    // The moment's own arguments the content has no role for (LOC v0.1 lines): action
    // (text: interaction.<id>.name), give / get (item), person (char), need, deadline (text).
    std::map<std::string, Ref> args;
    bool operator==(const Utterance&) const;
};

Json toJson(const Ref& r);
Json toJson(const Prop& p);
Json toJson(const Epistemic& e);
Json toJson(const Utterance& u);
// Parsing never throws on content: a malformed value gives `false` and a message.
bool fromJson(const Json& j, Ref& out, std::string* error = nullptr);
bool fromJson(const Json& j, Prop& out, std::string* error = nullptr);
bool fromJson(const Json& j, Epistemic& out, std::string* error = nullptr);
bool fromJson(const Json& j, Utterance& out, std::string* error = nullptr);

// --- context facts and criteria (layer 2) ---------------------------------
// Flat facts about the moment: "act" = "inform", "epi.conf" = 0.49, ... A
// fact is a string or a number; a list fact ("speaker.trait") holds many strings.
struct ContextFacts {
    std::map<std::string, std::string> str;
    std::map<std::string, double> num;
    std::map<std::string, std::vector<std::string>> list;
    void set(const std::string& k, const std::string& v) { str[k] = v; }
    void set(const std::string& k, double v) { num[k] = v; }
    void add(const std::string& k, const std::string& v) { list[k].push_back(v); }
    bool has(const std::string& k) const { return str.count(k) || num.count(k) || list.count(k); }
};

// One criterion of a rule:  key=v  key!=v  key<n  key<=n  key>n  key>=n
// key (fact present and not 0 / empty)  !key (absent or 0).  For a list fact
// key=v means "contains v". v may be a|b|c (any of).
struct Criterion {
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Has, Not };
    std::string key;
    Op op = Op::Has;
    std::vector<std::string> values; // Eq / Ne
    double number = 0.0;             // Lt..Ge
    std::string text;                // as written
    static bool parse(const std::string& s, Criterion& out, std::string* error = nullptr);
    bool match(const ContextFacts& f) const;
};

// --- content formats -------------------------------------------------------
// content/ai/predicates.json
struct PredicateDef {
    std::string id;
    std::map<std::string, std::vector<std::string>> roles; // role -> allowed Ref kinds
    std::vector<std::string> required;                     // roles a Prop must have
    std::vector<std::string> tags;                         // legal tags etc. (pred.tag=...)
    std::vector<std::string> scale;                        // weaker..stronger predicates (rumour exaggerate/minimize)
};
// predicates.json "memory": a memory type -> how a MemoryEntry becomes a Prop.
// Role sources: who0, who1 (MemoryEntry::who[i]), object (MemoryEntry::object),
// object_owner (its owner family), self.
struct MemoryPropMap {
    std::string memoryType;
    std::string pred;
    std::map<std::string, std::string> roles; // role -> source
    std::string modality = "fact";
};
// content/ai/dialogue_rules/*.json: {"rules": [...]}
struct DialogueRuleDef {
    std::string id;
    std::vector<Criterion> when;
    std::string say;               // loc key of the surface (a pattern or a sub-rule)
    double weight = 1.0;
    std::string cooldown;          // "<scope>:<duration>", scope speaker|listener|pair, duration Nd / Nh / N (ticks)
    std::string oncePer;           // listener | topic | listener+topic, empty = no limit
    std::string file;              // where it came from (for messages)
    int specificity() const { return static_cast<int>(when.size()); }
};
// content/ai/rumor_noise.json: operator -> parameters (AL-50 reads them).
struct DialogueGenData {
    std::map<std::string, PredicateDef> predicates;
    std::vector<MemoryPropMap> memory;
    std::vector<DialogueRuleDef> rules;
    Json noise = Json::object();
    std::vector<std::string> loadErrors;
    // contentDir = content/ai; missing files give empty sections.
    static DialogueGenData load(const std::string& contentDir);
    static DialogueGenData fromJson(const Json& predicates, const std::vector<Json>& ruleFiles,
                                    const Json& noise = Json::object());
    const PredicateDef* predicate(const std::string& id) const;
    const MemoryPropMap* forMemory(const std::string& type) const;
};

// Cached per content dir, like consequenceRules().
const DialogueGenData& dialogueGenData(const Content& c);

// Checks: unknown Ref kinds, required roles not declared, scale names unknown
// predicates, memory maps to unknown predicates/roles/sources, rules without
// say or with bad criteria, duplicate rule ids, a bad cooldown. With `loc`:
// every rule's `say` key exists in the base language.
ValidationReport validateDialogueGen(const DialogueGenData& d, const Content* c = nullptr, const Loc* loc = nullptr);
// A Prop against its predicate: known pred, required roles present, role kinds allowed.
bool checkProp(const DialogueGenData& d, const Prop& p, std::string* error = nullptr);

} // namespace lr::sim
