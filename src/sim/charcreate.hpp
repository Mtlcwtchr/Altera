#pragma once
// AL-136: the player's character created at a new game - over the backstory
// (sim/backstory.hpp): the kind of people (race), sex, age, name, talents and
// flaws for points, the rare magic gift, the attributes for points, and how the
// person looks. The rules are data (content/ai/character_creation.json).
//
// The person is still one of the village (the candidate of that sex, as a
// backstory takes): their household takes the chosen kind with them, and a kind
// the village's culture takes badly (cultures/*.json welcome) starts as a
// stranger to the rest of the village.

#include <string>

#include "sim/world.hpp"

namespace lr::sim {

// The rules resolved for the screen: {points, age, attrs: {ids, base, min, max, step,
// cost, refund}, max, races: [{id, skin, skins, adult, elder, lifespan, gift_cost,
// welcome, aptitudes: [{skill, learn, cap}], affinity}], talents: [{id, cost}],
// flaws: [{id, cost}], nature: [ids], gift: {trait}, exclusive: [[a, b]], look}.
// `culture`: the village's culture (welcome); empty = none.
Json creationRules(const Content& c, const std::string& culture = {});

// The points a choice spends (talents, the gift, attributes; flaws give back),
// -1 and why when the choice breaks the rules. A choice: {backstory, race, sex,
// age, name, traits: [ids], attrs: {id: 0..1}, look: {key: value}}.
int creationCost(const Content& c, const Json& choice, std::string* why = nullptr);

// Whom the choice is made on: a household's head (not the chief) of that sex, else
// a spouse of that sex, else any adult of it, else backstoryCandidate.
EntityId creationCandidate(const World& w, char sex);

// Applies the choice to `c` (the backstory first, then the rest). false + why.
bool applyCreation(World& w, Character& c, const Json& choice, std::string& why);

// The command "create_character" {player, ...the choice, id?}: makes the person and
// gives the player control. false when `cmd` is not that command.
bool handleCreationCommand(World& w, Command& cmd, int player);

// character_creation.json against the content: races, traits, attributes.
void validateCreation(const Content& c, ValidationReport& r);

} // namespace lr::sim
