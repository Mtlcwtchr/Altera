#pragma once
// LOC v0.2 (AL-47): the text generator behind Loc::text / formatLoc.
// Superset of LOC_FORMAT v0.1 ({arg}, {arg:case}, {arg:g|..}, {n:plural|..}),
// plus (doc/design/LOC_V02.md):
//   {>key} {>key?}          sub-rule of the same language (a string or an array of
//                           variants, one picked from the seed); ? = empty when an
//                           argument of the sub-rule is missing
//   {x?}                    optional argument
//   {>prop[:clause|:nominal[:case]|:about]}   a proposition argument through frame.<pred>
//   {verb:agree=role}       inside a frame: the frame's verb, agreed with a role, tense of the prop
//   {verb.id:past:agree=r}  a given lexeme lex.verb.id, tense / inf explicit
//   {adj.id:agree=r:acc}    lex.adj.id agreed in gender / number / animacy, in a case
//   {who:pro[:case]}        name on the first mention, a pronoun later; :pron = always a pronoun
//   {x:prep|о}              a preposition by the language's rule (о/об/обо, a/an) + the word
//   {x:ty[:case]} {x:ty|a|b} {x:addr}   address by register (reg: ty | vy) and address word
//   {unit:count=qty}        a noun in the form the number governs (мешок / мешка / мешков)
// Inside a frame a role is rendered with its government ("у+gen" -> у мельника);
// a role that is the speaker / listener becomes я / ты|вы and the verb agrees.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim/loc.hpp"

namespace altera::sim {

struct Prop;
struct Ref;

// Render a pattern. `loc` may be null (no sub-rules, lexemes, frames, grammar:
// v0.1 behaviour). `lang` is the language the pattern came from; lookups of
// sub-rules use `lang` with the loc's fallback. `usedV2` (optional) is set when
// the pattern used a v0.2 feature (callers tidy spaces then).
std::string renderLocPattern(const Loc* loc, const std::string& lang, const std::string& pattern, const Json& args,
                             std::uint64_t seed, std::vector<std::string>* errors = nullptr, bool* usedV2 = nullptr);

// Syntax check; argument names (no sub-rule keys / lexemes) into `names`.
bool locPatternNames(const std::string& pattern, const std::string& lang, std::string& error,
                     std::set<std::string>* names);

// A line of dialogue: key rendered with the seed, spaces tidied, the first letter capitalized.
std::string sayLoc(const Loc& loc, const std::string& lang, const std::string& key, const Json& args,
                   std::uint64_t seed = 0, std::vector<std::string>* errors = nullptr);
// One proposition argument (see propLocArg) in a form: clause | nominal | about.
std::string realizeProp(const Loc& loc, const std::string& lang, const Json& prop, const std::string& form = "clause",
                        const std::string& gcase = "", const Json& args = Json::object(), std::uint64_t seed = 0,
                        std::vector<std::string>* errors = nullptr);

// A Prop as a loc argument: {"pred","tense","neg","modality","qty","unit","roles":{role: noun}}.
// `refNoun` turns a role value into a noun (characters: Loc::personNoun + "id").
// A nested prop role becomes a nested prop argument. `unitNoun` gives the measure noun (may be empty).
Json propLocArg(const Prop& p, const std::function<Json(const Ref&)>& refNoun,
                const std::function<Json(const std::string&)>& unitNoun = nullptr);
bool isPropArg(const Json& v);

// Spaces collapsed, no space before , . ! ? : ; and after an opening quote, trimmed;
// optionally the first letter capitalized (ru and Latin).
std::string tidyLine(const std::string& s, bool capitalize);

// Structure of lexemes (lex.verb.*, lex.adj.*, lex.noun.*), frames (frame.*), variant
// arrays and sub-rule references in every language; with `predicates`: the base
// language has a frame for each (a warning while the base has no frames at all), the
// frame's roles are known to it (`roles`: pred -> role names).
ValidationReport validateLocGen(const Loc& loc, const std::vector<std::string>& predicates = {},
                                const std::map<std::string, std::vector<std::string>>& roles = {});

} // namespace altera::sim
