#pragma once
// Dialogue corpus and quality checks (AL-54, T7 of doc/design/DIALOGUE_GEN.md §9).
//
// generateDialogueCorpus() makes a small world (people of both sexes, ages,
// traits, status, families, memories of misdeeds) and says N lines through the
// whole chain (every other moment aimed at one rule, so the specific rules are reached): Utterance -> context facts -> rule (dialogue_rules) -> say key ->
// LOC v0.2 render, with the slot parts (noise / tone / voice) around it. The
// moments are drawn from the seed: act (round robin over speech_acts), tone,
// proposition (every predicate, roles filled with people, items, families),
// epistemics (seen ... rumour by confidence bands, chains), and the facts the
// dialogue layer sets (reason, outcome, intent, place, ...: the values the rules
// ask for). Same seed and content -> the same corpus.
//
// checkDialogueCorpus() reads it like a QA would:
//   broken     - [key], {slot}, stray | or #id in the text, empty line, render errors,
//                no rule for a moment                                        (error)
//   agreement  - the verb of the content agrees with the doer (m / f / pl, я / ты / вы):
//                a past form of the other gender in the line                  (error)
//              + checkFrameAgreement(): every frame on a table of names (m/f,
//                hard ones like Добрыня) and items (m/f/n): doer in nom, verb in
//                the doer's gender, each role in its government case          (error)
//   style      - doubled word, ",," ".," "..", no final punctuation, a capital inside a
//                sentence that is not a name (parts glued wrong), Latin in ru /
//                Cyrillic in en, a very long line                             (warning)
//                the listener named twice, two colons in one sentence          (warning)
//   repeats    - the same text twice to the same listener in one run of talk  (warning),
//                duplicate share of the corpus (metric, a test bounds it)
//   coverage   - whole-line rules never fit / fit but always shadowed, part rules and
//                say.* keys never used                                        (metrics)
//   diversity  - distinct lines, distinct bigrams per act                    (metrics)

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/dialogue_gen.hpp"

namespace lr::sim {

class Loc;

struct CorpusOptions {
    int count = 400;
    std::uint64_t seed = 1;
    std::string lang;   // empty = the loc base language
    bool parts = true;  // slot parts around the line
    int run = 4;        // lines in a row between the same speaker and listener
    bool aim = true;    // every other line aims at one rule (its criteria made true), round robin
    // false: the line gets every argument the content may use (speaker/listener with reg and
    // addr, prop, src, reason, item, price and the v0.1 ones: topic, action, give, get, need,
    // person, text, deadline, self) - checks the content. true: the line as the game renders it
    // now (dialogue_say utteranceText) - checks what the player would read.
    bool game = false;
};

struct CorpusLine {
    int index = 0;
    Utterance u;
    std::map<std::string, std::string> caller; // facts the dialogue layer would set (reason, outcome, ...)
    std::vector<std::string> states;           // speaker.state values set for the moment
    std::string aim;                           // the rule the moment was made for (empty: a random moment)
    std::string rule, say;                     // empty rule: nothing fits
    bool repeat = false;
    std::vector<std::string> parts;            // part rule ids in the order said
    std::vector<std::string> fitting;          // whole-line rules that fit the moment (coverage)
    std::string text;
    std::vector<std::string> errors;           // render errors
    std::string doerAgree;                     // m | f | pl | "" (no check): what the content's verb must agree with
    Json listenerNoun;                         // the listener argument (address checks)
};

struct CorpusIssue {
    std::string severity; // error | warning
    std::string check;    // broken | agreement | style | repeat | coverage
    int line = -1;        // corpus index, -1 = not a line (frame table)
    std::string message;
};

struct CorpusReport {
    int lines = 0, distinct = 0;
    double dupShare = 0.0;   // 1 - distinct / lines
    double distinct2 = 0.0;  // distinct word bigrams / all bigrams
    std::map<std::string, int> perAct, distinctPerAct;
    int rulesTotal = 0, rulesFired = 0;         // whole-line rules
    int partsTotal = 0, partsFired = 0;         // slot rules
    int sayKeysTotal = 0, sayKeysUsed = 0;      // say.* keys of the language
    std::vector<std::string> rulesNever;    // whole-line rules that never fit a corpus moment
    std::vector<std::string> rulesShadowed; // fit, but a more specific rule always won
    std::vector<std::string> partsNever, sayNever;
    int agreementChecked = 0;                   // corpus lines whose verb was checked
    int frameChecks = 0;                        // frame-table renders
    std::vector<CorpusIssue> issues;
    int errors() const;
    int warnings() const;
};

std::vector<CorpusLine> generateDialogueCorpus(const Content& c, const Loc& loc, const CorpusOptions& o);

// The pieces of a line (noise / opener / the line / tail) as one line: a piece after one
// that does not end a sentence (',' ':' "но" ...; not . ! ? … —) starts in lower case
// unless its first word is in `names` or is the English "I"
// (lower case); spaces tidied, the first letter capitalized. The dialogue layer (T6)
// glues the parts the same way.
std::string joinDialogueParts(const std::vector<std::string>& pieces, const std::set<std::string>& names = {});
// The lower-case words of the string forms of the given person arguments (names for joinDialogueParts).
std::set<std::string> dialogueNameWords(const Json& args, const std::vector<std::string>& keys);

// Problems of one rendered line: "error: ..." / "warning: ..." prefixed. With `names`
// (lower case words), a capital inside a sentence that is not a name is a warning.
std::vector<std::string> lintDialogueLine(const std::string& text, const std::string& lang,
                                         const std::set<std::string>* names = nullptr);

// Every frame.<pred> of `lang` on a table of names and items (clause, nominal, about).
std::vector<CorpusIssue> checkFrameAgreement(const Content& c, const Loc& loc, const std::string& lang,
                                             int* renders = nullptr);

CorpusReport checkDialogueCorpus(const std::vector<CorpusLine>& lines, const Content& c, const Loc& loc,
                                 const std::string& lang, int run = 4);

// One line per utterance, for reading: "#12 inform/rude theft seen 1.00 | rule -> key | text".
std::string corpusText(const std::vector<CorpusLine>& lines);
Json corpusJson(const std::vector<CorpusLine>& lines);
Json toJson(const CorpusReport& r);
std::string reportText(const CorpusReport& r, size_t maxIssues = 60);

} // namespace lr::sim
