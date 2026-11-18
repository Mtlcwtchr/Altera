#pragma once
// Localization (doc/design/LOC_FORMAT.md). Data and code carry keys only;
// display text lives in content/loc/<lang>/*.json and is assembled here:
//   {arg}                       value of an argument
//   {arg:dat}                   case form (nom gen dat acc ins pre)
//   {arg:g|m-form|f-form[|n|pl]} choice by the argument's grammatical gender
//   {qty:plural|one|few|many}   ru plural forms; en: {qty:plural|one|other}
//   {{ and }}                   literal braces
// Lookup falls back requested language -> fallback language -> base language;
// a key missing everywhere renders as [key].

#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/content.hpp"

namespace altera::sim {

class World;
struct Character;
struct TraceEntry;

class Loc {
public:
    // dir = content/loc: loc.json ({"base", "fallback", "languages"}) and one
    // directory per language with any number of *.json files (nested objects
    // are flattened into dotted keys; an object with "nom" is a noun entry).
    static Loc load(const std::string& dir);
    // lang -> list of file trees (already parsed), for tests.
    static Loc fromJson(const std::string& base, const std::string& fallback,
                        const std::map<std::string, std::vector<Json>>& langs);

    const std::string& base() const { return base_; }
    const std::string& fallback() const { return fallback_; }
    std::vector<std::string> languages() const;
    bool hasLanguage(const std::string& lang) const { return tables_.count(lang) > 0; }

    // The entry for `key` in `lang` only (no fallback); nullptr if absent.
    const Json* entry(const std::string& lang, const std::string& key) const;
    // With fallback; `from` gets the language the entry came from.
    const Json* find(const std::string& lang, const std::string& key, std::string* from = nullptr) const;
    // Formatted text with fallback; [key] when missing everywhere.
    std::string text(const std::string& lang, const std::string& key, const Json& args = Json::object()) const;
    bool has(const std::string& lang, const std::string& key) const { return find(lang, key) != nullptr; }
    // A noun argument for format(): the entry as an object with case forms
    // and "g" (a plain string entry becomes {"nom": text}); `fallbackText`
    // when the key is missing.
    Json noun(const std::string& lang, const std::string& key, const std::string& fallbackText) const;
    // A character's name as a noun {nom, gen, ..., g} (schema v0.3 §4): each
    // word is declined by the language's _grammar.json name_rules; keys
    // name.<word>.<case> override the rules; no rule -> nom everywhere.
    Json personNoun(const std::string& lang, const std::string& name, char sex) const;
    const Json* grammar(const std::string& lang) const;
    std::set<std::string> keys(const std::string& lang) const;

    std::vector<std::string> loadWarnings; // duplicate keys across files, etc.

private:
    std::string base_ = "ru";
    std::string fallback_ = "ru";
    std::map<std::string, std::map<std::string, Json>> tables_; // lang -> key -> string | noun object
    std::map<std::string, Json> grammar_;                       // lang -> _grammar.json
};

// The formatter itself. `lang` picks the plural rule (the language the pattern
// came from). Errors (bad placeholders, missing arguments) go to `errors` and
// the placeholder is rendered as {name}.
std::string formatLoc(const std::string& pattern, const Json& args, const std::string& lang,
                      std::vector<std::string>* errors = nullptr);
// Syntax check of a pattern for `lang`: placeholder names into `names`.
bool checkLocPattern(const std::string& pattern, const std::string& lang, std::string& error,
                     std::set<std::string>* names = nullptr);
// Plural category: "one", "few", "many" (ru) / "one", "other" (en and default).
std::string pluralCategory(const std::string& lang, double n);

// Keys the content and the code require in the base language.
std::vector<std::string> requiredLocKeys(const Content& c);
// Every trace code the code emits (trace.<code>); a test keeps it complete.
const std::vector<std::string>& traceCodes();
// Heads of reason codes (reason.<head>; "fsm.lost:pre:has:meal>=1" -> fsm.lost, pre).
const std::vector<std::string>& reasonCodes();
// Idle reasons of the report (report.idle.<code>).
const std::vector<std::string>& idleCodes();

// Base language: every required key present (error), patterns parse (error);
// other languages: missing keys and keys unknown to the base (warnings),
// placeholders unknown to the base pattern (warning).
ValidationReport validateLoc(const Content& c, const Loc& loc);

// --- output-time assembly -------------------------------------------------
// Arguments for a key's pattern: ids of known kinds become nouns (goal,
// task, action/interaction, state, need, item, object/to, cause; who/speaker/
// listener/actor/self are characters), reason-like params (why, for) become
// their localized text.
Json locArgs(const Loc& loc, const std::string& lang, const World& w, const Json& params,
             const Character* self = nullptr);
std::string reasonText(const Loc& loc, const std::string& lang, const std::string& reason);
std::string traceText(const Loc& loc, const std::string& lang, const World& w, const Character& c,
                      const TraceEntry& e);
// Adds "text" fields to aiReport() output (why_idle, plan_failures, why_hunger, deaths).
void localizeReport(Json& report, const Loc& loc, const std::string& lang, const World& w);

} // namespace altera::sim
