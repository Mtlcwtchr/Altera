#pragma once
// Templates in the data (LR-142, doc/design/DATA_DRIVEN.md §5): inheritance and
// composition for any list of content entries (items, objects, acts, traits, ...).
//
//   { "id": "sword_iron", "extends": "blade_base", "use": ["mix.metal_iron"],
//     "args": {"metal": "iron"}, "+tags": ["weapon"] }
//
// - "extends": one parent, from the template book or from the same list;
// - "use": [...] mixins merged in order over the parent, then the entry itself;
// - merge: objects key by key (deep), arrays and other values replace, "+key" appends
//   to the parent's array under "key";
// - a template's "params" (+ "defaults") are bound by the entry's "args": "$name" in
//   any string is replaced (also inside names: "skill.$skill"); a string that is
//   exactly "$name" takes the argument's JSON value (a number, an object);
// - "abstract": true entries are templates only and leave the list;
// - unknown ids, cycles and unbound parameters are errors.
// The template keys (extends, use, args, params, defaults, abstract) are dropped
// from the result. Expansion runs at load, before validation.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

struct TemplateBook {
    std::map<std::string, Json> byId;
    // content/ai/templates/*.json through the data layers: {"templates": [{"id": ...}, ...]}.
    static TemplateBook load(const std::string& dir, std::vector<std::string>* errors);
    void add(const Json& entries, const std::string& file, std::vector<std::string>* errors);
};

// Expands every entry of `list` (an array of objects with "id") in place; abstract
// entries are removed. `what` names the list in messages ("items.json").
void expandTemplates(Json& list, const TemplateBook& book, const std::string& what, std::vector<std::string>* errors);

// The merge rule above (objects deep, "+key" appends, the rest replaces).
void templateMerge(Json& base, const Json& over);
// "$name" substitution in every string of `j`.
bool bindTemplateParams(Json& j, const std::map<std::string, Json>& args, std::string* why);

} // namespace lr::sim
