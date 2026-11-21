#pragma once
// What an act asks and gives, for the player's eyes: every precondition of an
// interaction checked one by one for a person and a target (met or not), what
// it gives the person, how long it takes. The game shows these on the act
// ring and the wheel ("needs: an axe ✗, knows woodcutting ✓ -> +2 logs").
// The same checks the executor runs (ai.hpp canPerform / evalCondition).

#include <string>
#include <vector>

#include "sim/rules.hpp"

namespace altera::sim {

class World;
struct Character;
struct WorldObject;
struct InteractionDef;
struct Content;

struct ActReq {
    Condition cond;
    bool ok = false;
};

struct ActInfo {
    std::string id;
    bool can = false;          // canPerform, the reach left out (the person walks there)
    std::string why;           // the first failing precondition (canPerform's)
    std::vector<ActReq> pre;   // shown preconditions (internal ones left out)
    std::vector<Effect> gains; // effects on the person (not t./w., not target_stock)
    std::vector<Effect> onTarget; // effects on the target person (t.)
    Tick duration = 1;         // ticks
};

ActInfo actInfo(const World& w, const Character& c, const InteractionDef& d, const WorldObject* obj,
                const Character* targetChar);

// The interactions done to an object of this type (target object:<type>), in catalog order.
std::vector<std::string> actsOnObject(const Content& content, const std::string& objectType);

// True for a precondition worth showing a player (not a bookkeeping flag).
bool shownCondition(const Condition& k);

} // namespace altera::sim
