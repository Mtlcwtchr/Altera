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

#include <map>
#include <optional>
#include <string>
#include <vector>

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
    // the parts said around the line (DIALOGUE_GEN §5.4): noise.open, tone|voice.open
    // before; voice|tone.tail after. Loc keys, in the order said.
    std::vector<std::string> pre, post;
    bool ok() const { return !key.empty(); }
};

// Registers the fact providers lines need (memory facts of AL-49); say() calls it once.
void installDialogueFacts();

// Choose and say a line; nothing is said when no rule fits. `intent` reaches
// the rules as intent=<v> (refuse lines use consent | fail | order). `args` are the
// moment's own arguments (Utterance::args: action, give, get, person, need, deadline).
SaidLine say(World& w, Character& speaker, Character& listener, const std::string& act,
             std::optional<Prop> content = std::nullopt, Epistemic epi = Epistemic{}, const std::string& tone = "",
             const std::string& intent = "", std::map<std::string, Ref> args = {});

// `refuser` says no to `asker`. The strongest deed he holds against the asker
// becomes the content ("Не после того, как ты обокрал мельника"). `actionKey`: the loc
// key of what was asked ("interaction.<id>.name"), the lines' {action}.
SaidLine sayRefusal(World& w, Character& refuser, Character& asker, const std::string& why = "consent",
                    const std::string& actionKey = "");

// `target` answers a social act of `actor` that failed (act ignore, intent fail).
SaidLine sayFailReaction(World& w, Character& target, Character& actor, const InteractionDef& d);

// --- the menu without an agent (dialogue.json menu, topic_kinds) -------------
// What `speaker` could bring up with `listener`: word of somebody else's deed
// (inform), the listener's own deed (accuse) or kindness (thank); strongest
// first, one per topic, at most `max`.
struct MenuTopic {
    std::string act;
    Prop prop;
    Epistemic epi;
    std::string topic;
    double weight = 0.0;
};
std::vector<MenuTopic> dialogueTopics(const World& w, const Character& speaker, const Character& listener,
                                      size_t max = 6);
// The menu caption: dialogue.menu.<act> ("Рассказать о краже муки у мельника").
std::string menuLabel(const Loc& loc, const std::string& lang, const World& w, const Character& speaker,
                      const Character& listener, const MenuTopic& t);

// --- barks: what a person mutters to himself ---------------------------------
// Presentation only: chosen from the moment (needs, state, time, season) and
// deterministic for the tick, but not recorded - the client asks for it when
// it shows a person, so it never changes the world. "" = nothing to say.
SaidLine chooseBark(const World& w, const Character& c);

// --- Agent API (AGENT_API §4.3 acts[].content) --------------------------------
// {kind: fact|none, ref: fact:<id> | <topic>, params: <Prop JSON>, epi: <Epistemic JSON>}
Json agentContent(const Utterance& u);

// The text of a "say.line" trace entry: "<speaker>: «line»" by trace.say.line.
std::string lineText(const Loc& loc, const std::string& lang, const World& w, const Json& params);
// The line alone (no speaker), from the key and the Utterance.
std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key);
// The same with the parts around it, glued by joinDialogueParts (dialogue_corpus).
std::string utteranceText(const Loc& loc, const std::string& lang, const World& w, const Utterance& u,
                          const std::string& key, const std::vector<std::string>& pre,
                          const std::vector<std::string>& post);
// Every loc argument a line of the moment may use (LOC v0.1 and v0.2): speaker, self,
// listener (with id; reg=vy and addr toward the nobility, DIALOGUE_GEN §11.2), src (the
// teller), prop, reason, topic, text, action, give, get, need, person, deadline.
Json utteranceArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u);
// Is `listener` spoken to as "вы" (reg=vy) by `speaker`: the nobility.
bool addressedVy(const World& w, EntityId speaker, EntityId listener);

} // namespace altera::sim
