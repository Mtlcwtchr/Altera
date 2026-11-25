#pragma once
// AL-117 (LivelyRealms §7-§8): aspects of magic inside made things. Alchemy
// carries aspects as effects of materials ("aspect.<id>", "aspect.<id>.minus",
// "aspect.<id>.shift" = the vectors + - ~); a potion is a thing whose effects hold
// aspects; a rune (Item::rune) holds aspects laid into a carrier that conducts.
// Pure functions over the crafting data: the sim side is sim/alchemy.hpp.

#include <string>
#include <vector>

#include "sim/craft/craft.hpp"

namespace altera::sim::craft {

struct AspectDose {
    std::string aspect;  // aspect id (fire)
    char vec = '+';      // + - ~
    double power = 0.0;
};

// "aspect.fire.minus" -> {fire, '-'}; false for an effect that is no aspect.
bool parseAspectEffect(const std::string& effect, std::string& aspect, char& vec);
std::string aspectEffectId(const std::string& aspect, char vec);
std::string runeKey(const std::string& aspect, char vec); // "fire", "fire.minus"
bool parseRuneKey(const std::string& key, std::string& aspect, char& vec);

// The aspects a thing carries in its effects (a potion, a reagent), strongest first.
std::vector<AspectDose> aspectDoses(const Item& it);
// Sum of the aspect effects.
double aspectTotal(const Item& it);
// 0..1: how much opposed aspects (fire and water, or fire+ and fire-) fight in it.
double instabilityOf(const CraftData& d, const std::vector<AspectDose>& doses);
double instability(const CraftData& d, const Item& it);

// The piece of a thing that carries a rune: the best conductor among its pieces.
struct Carrier {
    double conductivity = 0.0, capacity = 0.0;
    std::string material;
};
Carrier carrierOf(const CraftData& d, const Item& it);

// What the rune gives now (power grows with resonance), and the doses of one use.
double runePower(const CraftData& d, const Item& it);
std::vector<AspectDose> runeDoses(const CraftData& d, const Item& it);
// Spends one use of charge; returns the doses that use releases (empty: no charge).
std::vector<AspectDose> spendRune(const CraftData& d, Item& it);
void rechargeRune(Item& it, double amount);

// A notable deed with the thing: history, resonance. Returns the new resonance.
double noteDeed(const CraftData& d, Item& it, const std::string& what);

} // namespace altera::sim::craft
