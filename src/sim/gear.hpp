#pragma once
// What a person carries and wears (content/ai/gear.json).
//
// Every thing weighs something and takes room: a cell or more of a grid, as
// in Diablo. The hands hold a small grid of their own; a worn belt, satchel
// or pack adds its grid. Worn things sit on the body in slots by layers
// (torso: under, padding, armour, over), one thing a layer, each layer's
// bulk counted against the slot's: a gambeson under a mail shirt is fine,
// two armours are not. What one carries counts against what a body can bear:
// past the comfortable load every tiring act tires more, and past the most
// one can carry nothing more is picked up (the player's person: the act
// refuses with "too heavy" / "no room").
//
// Character::inventory stays the one count of things; `worn` names what of
// it is on the body, `pins` where the player put a thing in the grids. The
// arrangement is made from these each time (arrange()): pinned things first,
// then the rest, biggest first, first fit, turned when it fits so.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;

struct GearItem {
    double kg = 0.5;
    int w = 1, h = 1;              // cells
    std::string slot, layer;       // worn: body slot and layer ("" not wearable)
    int bulk = 0;                  // the layer's cost against the slot's bulk
    int gridW = 0, gridH = 0;      // worn: the grid it adds (a belt, a satchel, a pack)
    double warmth = 0;
};
struct GearSlot {
    std::string id;
    std::vector<std::string> layers;   // inner to outer
    int bulk = 1;
};
struct GearRules {
    double comfortKg = 18, maxKg = 40;
    std::string strengthAttr = "strength";
    double perAttrKg = 10;             // max load from the attribute (0..1 around 0.5)
    double loadFatigue = 0.9;          // tiring acts tire this much more at the most load
    double walkPerKm = 0.02, runPerKm = 0.07, climbPer100m = 0.05;   // fatigue of walking (steer)
    int handsW = 2, handsH = 3;
    std::vector<GearSlot> slots;
    std::map<std::string, GearItem> items;
    std::map<std::string, int> stacks;   // items.json stack: small things a cell holds
    GearItem fallback;
    const GearItem& item(const std::string& id) const {
        auto it = items.find(id);
        return it == items.end() ? fallback : it->second;
    }
    const GearSlot* slot(const std::string& id) const {
        for (const auto& s : slots)
            if (s.id == id) return &s;
        return nullptr;
    }
};

// gear.json next to the content (cached per content dir); null without one.
const GearRules* gearRules(const Content& c);

struct GearGrid {
    std::string id;               // "hands" or the worn container's item id
    int w = 0, h = 0;
};
struct GearSpot {
    std::string item;
    int count = 1;                // things of a stack in this spot
    std::string grid;
    int x = 0, y = 0, w = 1, h = 1;
    bool turned = false;
};
struct GearLayout {
    std::vector<GearGrid> grids;
    std::vector<GearSpot> spots;
    std::map<std::string, int> overflow;   // what found no room
};

double loadKg(const GearRules& R, const Character& c);
double maxLoadKg(const GearRules& R, const Character& c);
// 1 up to the comfortable load, up to 1 + loadFatigue at the most.
double loadFatigueMult(const GearRules& R, const Character& c);
// Worn things in the grids' order: what of the inventory is on the body.
int wornCount(const Character& c, const std::string& item);
GearLayout arrange(const GearRules& R, const Character& c, const std::map<std::string, int>* extra = nullptr);

// "" when it can be put on, else why: not_wearable, not_carried, layer_taken:<item>, too_bulky.
std::string canWear(const GearRules& R, const Character& c, const std::string& item);
// "" when the gains of an act fit (weight, room), else too_heavy / no_room.
std::string canTake(const GearRules& R, const Character& c, const std::map<std::string, int>& gains);
// Keeps `worn` and `pins` in step with the inventory (a worn thing used up comes off).
void pruneGear(Character& c);
// Puts on whatever carried thing can be worn, inner layers first.
void wearAll(const GearRules& R, Character& c);

// Commands: wear {id, item}, take_off {id, item}, gear_pin {id, item, grid, x, y, turned?} | {id, item, clear: true}.
void installGear(World& w);

} // namespace altera::sim
