#pragma once
// AL-117 (LivelyRealms §7-§8) in the life-sim: drinking a potion, using a rune,
// the deeds that make a made thing an artifact. The crafting side (aspects in
// materials, distill/calcine/dissolve, inscribe/bind, instability) is in
// sim/craft/craft_magic.hpp and the crafting data.
//
// A potion works as a spell of the form "on oneself": its aspects (vector +
// by default, aspect.<id>.minus / .shift for the others) act by the `body` table
// of content/magic/aspects.json, with no knowledge of the aspect and no mana,
// through the magic core (applyAspectsTo, AL-116). Without content/magic only the
// body fields the life-sim has (health, fatigue) apply, the rest become moodlets.

#include <string>
#include <vector>

#include "sim/craft/craft_magic.hpp"
#include "sim/magic.hpp"
#include "sim/world.hpp"

namespace altera::sim {

struct AspectUse {
    bool ok = false;
    std::string reason;                    // not_a_potion, no_charge, no_such_item, not_enchanted
    std::vector<craft::AspectDose> doses;
    double health = 0.0;                   // applied change
    double fatigue = 0.0;
    std::vector<std::string> fields;       // body fields given beyond health/fatigue ("burn:0.3")
    bool blast = false;                    // an unstable flask burst
    bool consumed = false;
};

// Applies doses to `c` as a self-spell (magic core; a fallback without content/magic).
AspectUse applyAspectDoses(World& w, Character& c, const std::vector<craft::AspectDose>& doses);

// Drinks the n-th made thing (a potion, a draught): its aspects and its heal.
AspectUse drinkPotion(World& w, Character& c, int index);
// Spends a use of the rune of the n-th made thing; a scroll burns away when read.
// The doses go to `c` for on_touch/on_word runes of the self vectors (life+, essence+,
// earth+...); other uses only return the doses for the caller (combat, the magic core).
AspectUse useRune(World& w, Character& c, int index, bool onSelf);
// Fires the rune of the n-th made thing through the magic core (castStored, AL-116):
// at a creature (touch), a place (area) or, with neither, its holder (aura).
AspectUse fireRune(World& w, Character& holder, int index, const CastTarget& at);
// After a fight: an on_hit (or constant) rune on the holder's best weapon strikes the foe.
void fireHitRunes(World& w, Character& holder, Character& foe);
// A night's rest gives every carried rune back this share of its capacity.
void rechargeRunes(Character& c, double share);
// A fight won / a foe slain with the best made weapon `c` carries.
void noteWeaponDeed(World& w, Character& c, const std::string& what);

void installAlchemy(World& w); // commands alchemy_drink, alchemy_use_rune

} // namespace altera::sim
