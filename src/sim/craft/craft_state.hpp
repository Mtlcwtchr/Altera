#pragma once
// What a person carries of crafting (gdd crafting design v0.2 §7, §17): the
// made things with their history and numbers, the personal recipe book, how
// well each material is known, the last breakthrough. Saved with the character.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/craft/craft_chain.hpp"

namespace altera::sim::craft {

struct CraftState {
    std::vector<Item> items;                 // made things (instances); inventory counts them by form item
    RecipeBook book;
    std::map<std::string, double> knowsMaterial;
    std::int64_t lastBreakthrough = -1;      // tick
    std::map<std::string, int> tried;        // novelty: how often a chain signature was tried
    bool empty() const { return items.empty() && book.recipes.empty() && knowsMaterial.empty(); }
};

void to_json(Json& j, const ItemFlaw& f);
void from_json(const Json& j, ItemFlaw& f);
void to_json(Json& j, const StepRec& s);
void from_json(const Json& j, StepRec& s);
void to_json(Json& j, const Item& it);
void from_json(const Json& j, Item& it);
void to_json(Json& j, const Recipe& r);
void from_json(const Json& j, Recipe& r);
void to_json(Json& j, const CraftState& s);
void from_json(const Json& j, CraftState& s);

} // namespace altera::sim::craft
