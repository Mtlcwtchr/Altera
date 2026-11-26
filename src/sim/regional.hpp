#pragma once
// LR-168: things of a belt (content/ai/regional/regions.json).
//
// The world is sewn of belts (mediterranean, nile, forest, north, steppe: the
// culture's own field `belt`, content/species/cultures/*.json). What a person
// of a belt starts dressed in and what crafts his people know is data:
//   belts.<belt>.clothes.{m,f,child}  - the plain clothes (instead of gear.json base_clothes)
//   belts.<belt>.knowledge            - know-how everyone of the belt starts with
//   cultures.<culture>.clothes/knowledge - a culture's own, over its belt's
//   cultures.<culture>.belt           - a belt for a culture file without the field (old ids)
//   races.<race>.skip_slots           - slots a body has no use for (a centaur's legs and feet)
// Nothing here knows any belt or item by name.

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lr::sim {

class World;
struct Character;
struct Content;

struct RegionalSet {
    std::map<std::string, std::map<std::string, int>> clothes; // who (m, f, child) -> item -> n
    std::vector<std::string> knowledge;
    bool hasClothes = false;
};

struct RegionalRules {
    bool loaded = false;
    std::map<std::string, RegionalSet> belts, cultures;
    std::map<std::string, std::string> cultureBelt;              // fallback when the culture file has no belt
    std::map<std::string, std::set<std::string>> skipSlots;      // race -> body slots it does not dress
};

const RegionalRules& regionalRules(const Content& c);

// The belt of a culture id: the culture file's `belt`, else regions.json cultures.<id>.belt, else "".
std::string regionalBelt(const Content& c, const std::string& culture);

// The plain clothes of this person (`who` = m, f or child): the culture's own, else its belt's;
// nullptr = none named (gear.json base_clothes apply).
const std::map<std::string, int>* regionalClothes(const World& w, const Character& c, const std::string& who);

// A slot the person's body is not dressed on (regions.json races.<race>.skip_slots).
bool regionalSkipsSlot(const Content& c, const Character& ch, const std::string& slot);

// Know-how of the person's belt and culture (once; knowing it already is fine).
void giveRegionalKnowledge(const World& w, Character& c);

} // namespace lr::sim
