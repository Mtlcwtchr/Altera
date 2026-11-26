#pragma once
// Talking over remembered moments (LR-158): people bring up what they lived
// through themselves - the wolf that went for them, the hungry winter, the
// wedding, the fight they won, the raid - and the other answers it.
//
// The truth is the person's own memory (Character::memory, MemoryEntry): only
// what he saw himself (source "seen") of a type listed in
// content/ai/memory_talk.json is a moment worth telling. The file gives every
// such type a kind and a mood (sad | glad | alarm | proud | eerie | shame |
// spicy), an interest and whether it can be lived through together.
//   - recall_moment: the teller tells his own moment ("а на меня третьего дня
//     волк кинулся"); the hearer answers react_moment (by mood; "со мной тоже
//     было такое" when he keeps a moment of the same kind himself);
//   - reminisce: the moment is shared - the hearer is in it (MemoryEntry::who),
//     or keeps the same type within shared_window_days and is kin, or the type is
//     shared_by "day" (a raid on the village) ("а помнишь, как мы...");
//     the other answers react_reminisce;
//   - ask_memory (the player's question, news.hpp askTopics): what the answerer
//     remembers best; no_moment when nothing.
// A told moment is not told the same listener again while the world's
// SaidMemory keeps the record (topic "moment:<type>:<tick>"). Telling brings
// the two closer by memory_talk.json "after" (relation deltas, data).
//
// Lines are rules (content/ai/dialogue_rules/memory.json) over the facts
// moment.* this module registers, with loc keys say.memory.* and the moment's
// text memory.moment.<type> ({moment}), memory.shared.<type> ({moment_we}),
// memory.when.<bucket> ({when}), {with} = the other person in it.
// Deterministic, no RNG state.

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sim/dialogue_rules.hpp"
#include "sim/dialogue_say.hpp"

namespace lr::sim {

class World;
class Loc;
struct Character;
struct Content;
struct MemoryEntry;

struct MomentKind {
    std::string kind = "none";   // beast, hunger, wedding, fight, ... (moment.kind of the lines)
    std::string mood = "neutral"; // sad | glad | alarm | proud | eerie | shame | spicy | neutral
    double interest = 0.0;       // 0 = not worth telling
    bool shareable = true;       // can be lived through together (reminisce)
    // who: together when the other is in it (MemoryEntry::who) or is kin with the same memory
    // that day; day: anybody with the same memory that day lived it too (a raid on the village)
    std::string sharedBy = "who";
};

struct MomentTalkTurn {
    int firstEvery = 0;          // moments go before news on turns turn % n == n - 1 (1 = always; 0 = after)
    bool onlyShared = false;     // only reminisce (tender talk between close ones)
};

struct MomentRules {
    int maxAgeDays = 720;
    double halfLifeDays = 90.0;
    double minWeight = 0.05;
    double sharedMult = 1.5;     // a moment lived together is worth more to bring up
    double sharedWindowDays = 1.0;
    size_t maxMoments = 6;
    std::vector<std::pair<double, std::string>> when; // up to days -> bucket (memory.when.<bucket>)
    std::string whenBeyond = "long";
    std::map<std::string, double> afterTold;   // hearer -> teller relation deltas
    std::map<std::string, double> afterShared; // both ways
    std::map<std::string, MomentTalkTurn> talks;
    std::map<std::string, MomentKind> types;
    const MomentKind* of(const std::string& type) const;
    static MomentRules fromJson(const Json& j);
};

// content/ai/memory_talk.json (cached per content dir; without it nobody talks of moments).
const MomentRules& momentRules(const Content& c);

struct Moment {
    const MemoryEntry* memory = nullptr;
    MomentKind kind;
    double weight = 0.0;  // interest x salience x freshness (x shared)
    bool shared = false;  // the other one lived it too
    std::string topic;    // moment:<type>:<tick>
};

// The moments `c` could tell `other` (kNone: anybody), strongest first, at most
// rules.maxMoments; ones already told `other` left out unless `includeTold`.
std::vector<Moment> momentsToTell(const World& w, const Character& c, const Character* other, bool includeTold = false);
// Whether `other` lived moment `m` of `c` too.
bool sharedMoment(const World& w, const Character& c, const MemoryEntry& m, const Character& other);
// The memory a moment ref points to, in `owner`'s memory (nullptr: forgotten).
const MemoryEntry* momentMemory(const Character& owner, const Ref& r);
Ref momentRef(const MemoryEntry& m);
std::string momentTopic(const MemoryEntry& m);

// A beat of a talk: `teller` brings up a moment with `hearer` (recall_moment or
// reminisce) and the hearer answers. `turn` picks among them. False: no moment
// for this talk (or the talk group has none in memory_talk.json "talks").
bool talkOverMoments(World& w, Character& teller, Character& hearer, const std::string& talk, int turn);
// Whether moments go before the news on this turn of `talk`.
bool momentsFirst(const World& w, const std::string& talk, int turn);
// `answerer` answers the player's ask_memory. Returns the lines said.
int answerMemory(World& w, Character& answerer, Character& asker);

// utteranceArgs(): {moment}, {moment_we}, {when}, {with}.
void addMomentArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u, Json& args);
// Registers the facts moment.* (say() calls it once).
void installMomentFacts();

// --- the dialogue corpus and line tests ---
bool momentFactKey(const std::string& key);
void sampleMomentFacts(const std::string& act, const DialogueRuleDef* target, ContextFacts& f);
void sampleMomentArgs(const Loc& loc, const std::string& lang, Json& args);

} // namespace lr::sim
