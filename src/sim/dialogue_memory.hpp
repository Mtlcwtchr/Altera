#pragma once
// Memory -> meaning (AL-49 T3, AL-38; doc/design/DIALOGUE_GEN.md §3, §10).
//
// A MemoryEntry becomes a Prop (who did what to whom) by the table
// predicates.json "memory", and an Epistemic (seen / told / rumour, how sure,
// from whom, how long ago). On top of that:
//   - recall(): what a person remembers about somebody, strongest first:
//     misdeeds (a predicate with legal tags, the somebody as doer) and
//     kindnesses (a good memory with the somebody as doer);
//   - addMemoryFacts(): the context facts of the rule base (layer 2) about the
//     listener - memory.deed, memory.deed.mine, memory.wariness, ... - so a
//     greeting, a trade or a plea for help can bring the deed up
//     ("ты ж у мельника муку украл");
//   - deedWariness(): the same knowledge as a number the sim acts on (a
//     refused favour, a dearer price);
//   - recallUtterance() / renderUtterance(): the Utterance of such a line and a
//     plain render of it until the LOC v0.2 frames (AL-47) take over.
// Weights live in content/ai/dialogue_recall.json (defaults without it).
// The truth stays in the core: Prop::fact = MemoryEntry::fact (AL-55 fills it).

#include <optional>
#include <string>
#include <vector>

#include "sim/dialogue_gen.hpp"

namespace altera::sim {

class World;
class Loc;
struct Character;
struct MemoryEntry;

struct RecallRules {
    std::map<std::string, double> severity = {{"theft", 0.6},   {"trespass", 0.3}, {"poaching", 0.4},
                                              {"banditry", 0.9}, {"insult", 0.3},   {"assault", 0.7},
                                              {"murder", 1.0}};
    double severityDefault = 0.4;  // a tag not in the table
    double mineMult = 1.6;         // the speaker or his household was the one wronged
    double halfLifeDays = 60.0;    // a deed's weight halves in that many days
    double minWeight = 0.04;       // below this a memory is not brought up
    double guessMult = 0.7;        // a suspicion counts for less than knowledge
    double consentWariness = 0.35; // consentChance: p -= this x wariness (AL-38: help, favours)
    double priceWariness = 0.5;    // priceFactor = 1 + this x wariness (AL-38: a trader asks more)
    static RecallRules fromJson(const Json& j);
};

// content/ai/dialogue_recall.json next to the content (cached per content dir).
const RecallRules& recallRules(const Content& c);

// The Prop of a memory (nullopt: no table row, or a required role unknown).
std::optional<Prop> propFromMemory(const World& w, const Character& self, const MemoryEntry& m,
                                   const DialogueGenData& d);
// How `self` knows it. A guess by the one who found the loss is "inferred".
Epistemic epistemicFromMemory(const MemoryEntry& m, Tick now, const MemoryPropMap* map = nullptr);

// The topic of a Prop for once_per / said.*: "fact:<id>" when it points to a
// fact, else "<pred>:<doer>:<object>".
std::string topicOf(const Prop& p);

struct Recollection {
    const MemoryEntry* memory = nullptr;
    Prop prop;
    Epistemic epi;
    std::string kind;     // deed | kindness
    double weight = 0.0;  // severity x confidence x freshness (x mine), 0..~1.6
    bool mine = false;    // the rememberer or his household was the victim / beneficiary
    std::string topic;
};

// What `self` remembers about `about` (kNone: about anybody), strongest first,
// one per topic (the surest memory of it). Deterministic: ties by tick, then order.
std::vector<Recollection> recall(const World& w, const Character& self, EntityId about, const DialogueGenData& d,
                                 const RecallRules& r);
std::vector<Recollection> recall(const World& w, const Character& self, EntityId about);

// 0..1: how wary `self` is of `other` for what he knows of him (1 - prod(1 - w)).
double deedWariness(const World& w, const Character& self, EntityId other);
// AL-38: what a trader asks of `buyer` relative to the fair price (>= 1).
double priceFactor(const World& w, const Character& seller, EntityId buyer);

// Context facts about the listener from the speaker's memory:
//   memory.deed (pred), memory.deed.tag (list), memory.deed.conf, .source,
//   .hops, .days, .mine (1 when the speaker's own were wronged), .topic,
//   memory.deeds (count), memory.wariness, memory.kind (pred of the strongest
//   kindness), memory.kind.conf, memory.kind.mine;
//   listener.reputation.<axis> for every config.reputation_axes.
// Facts absent when there is nothing to say (criteria "!memory.deed").
void addMemoryFacts(const World& w, const Character& speaker, const Character& listener, ContextFacts& out);

// The line that brings a recollection up: act (greet, refuse, trade, inform,
// accuse...), content = the Prop, epi = how the speaker knows it.
Utterance recallUtterance(const World& w, const Character& speaker, const Character& listener,
                          const std::string& act, const Recollection& r, std::uint64_t seed = 0);

// locArgs params of a Prop: each char/object role by its name, plus who (=
// doer), owner (a victim, else the victim family's head), tag (first legal tag).
Json propParams(const World& w, const Prop& p);
// The text of `sayKey` for an Utterance with LOC v0.1 (speaker = self,
// listener, the Prop's params). AL-47/AL-53 replace it with frames.
std::string renderUtterance(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                            const std::string& sayKey);

} // namespace altera::sim
