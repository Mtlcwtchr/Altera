#pragma once
// Saying a line in the sim (AL-53, doc/design/DIALOGUE_GEN.md §3.3, §7): an
// Utterance is chosen a rule (dialogue_rules), the choice is remembered in the
// world's SaidMemory, and the line goes to the trace of both sides as
// "say.line" with the key and the Utterance - text is assembled at output by
// traceText (lineText below), so saves keep meaning, not words.
//
// AL-36: no silent "no". A refusal is said aloud with its reason (the deed the
// refuser holds against the asker, if any), a failed social act gets the
// other side's reaction.

#include <optional>
#include <string>

#include "sim/dialogue_gen.hpp"

namespace altera::sim {

class World;
class Loc;
struct Character;
struct InteractionDef;
struct TraceEntry;

struct SaidLine {
    std::string key;   // loc key of the line ("" = nothing fitted)
    std::string rule;  // the rule id
    Utterance utterance;
    bool ok() const { return !key.empty(); }
};

// Registers the fact providers lines need (memory facts of AL-49); say() calls it once.
void installDialogueFacts();

// Choose and say a line; nothing is said when no rule fits. `intent` reaches
// the rules as intent=<v> (refuse lines use consent | fail | order).
SaidLine say(World& w, Character& speaker, Character& listener, const std::string& act,
             std::optional<Prop> content = std::nullopt, Epistemic epi = Epistemic{}, const std::string& tone = "",
             const std::string& intent = "");

// `refuser` says no to `asker`. The strongest deed he holds against the asker
// becomes the content ("Не после того, как ты обокрал мельника").
SaidLine sayRefusal(World& w, Character& refuser, Character& asker, const std::string& why = "consent");

// `target` answers a social act of `actor` that failed (act ignore, intent fail).
SaidLine sayFailReaction(World& w, Character& target, Character& actor, const InteractionDef& d);

// The text of a "say.line" trace entry: "<speaker>: «line»" by trace.say.line.
std::string lineText(const Loc& loc, const std::string& lang, const World& w, const Json& params);
// The line alone (no speaker), from the key and the Utterance.
std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key);

} // namespace altera::sim
