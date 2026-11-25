#pragma once
// AL-75: trade, barter, treats and gifts (content/ai/trade.json).
//
// One model for the player and the NPCs: an offer (what the offerer gives,
// what it asks in return) goes to the other person, who weighs it by its own
// worth of the things - base value, the made thing's quality, rarity in the
// world, its own need (hunger, none of it, too many) and its taste (likes and
// dislikes by item category: traits + the person's seed) - and answers aloud:
// accepts, counters (asks more coin or fewer goods) or refuses with the reason.
// A gift or a treat is weighed by taste: a liked thing warms the relation, a
// disliked one cools it. Trade roles (trader, tavernkeeper) keep a margin and
// sell their stock; the tavernkeeper serves a meal for coin and treats friends.
//
// The catalog acts trade_buy / trade_sell / trade_barter / trade_gift /
// trade_treat / trade_meal (interactions.json) walk to the other person and do
// the deal when the act is done (tradeOnAct, called by the FSM). What exactly is
// offered is the actor's intent (the "trade" command, or the NPC's choice); an
// act started without one (the wheel) offers the default deal of its kind.
// Every step leaves a trace (trace.trade.*) and lines said aloud (say.line,
// content/ai/dialogue_rules/trade.json).

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace lr::sim {

class World;
struct Character;
struct Content;
struct InteractionDef;

using Goods = std::map<std::string, int>;

struct TradeRole {
    std::string id;
    double keep = 1.1;         // margin: what it parts with is worth this much more to it
    double taste = 1.0;        // how much its own taste weighs (a trader buys to sell on)
    bool ignoresNeed = false;  // values goods as wares, not by its own hunger or lack
    std::vector<std::string> sellsTags; // the stock it sells (tavern: food, drink); empty = anything
    std::vector<std::string> pickTraits;
    Goods stock;               // what it starts with when given the role
    std::vector<std::string> restockTags; // daily from the household's store
    int restockMax = 0;
    int autoCount = 0;         // auto_assign: how many per settlement
};

struct TasteCategory {
    std::vector<std::string> tags, materials, items;
    double minValue = -1.0;
    double minQuality = -1.0; // ItemDef.quality (AL-74): a fine thing falls in by itself
};

struct TradeRules {
    std::map<std::string, double> cfg; // config
    std::map<std::string, double> values, tagValues;
    std::map<std::string, TasteCategory> categories;
    std::vector<std::string> seedCategories;
    int seedLikes = 1, seedDislikes = 1;
    std::map<std::string, std::map<std::string, double>> traitTaste; // trait -> category -> +1/-1
    std::map<std::string, double> traitKeep;
    std::map<std::string, TradeRole> roles;
    std::map<std::string, std::string> acts; // catalog act -> mode
    bool autoAssign = false;
    int purseLo = 0, purseHi = 0;
    double c(const std::string& k, double def = 0.0) const {
        auto it = cfg.find(k);
        return it == cfg.end() ? def : it->second;
    }
};

// nullptr without content/ai/trade.json (fixture contents).
const TradeRules* tradeRules(const Content& c);

// --- worth ---------------------------------------------------------------------
// The categories an item falls in (trade.json categories).
std::vector<std::string> itemCategories(const Content& c, const std::string& item);
// The base worth in coins: ItemDef.value (AL-74) when priced, else trade.json values,
// else the best tag value + nutrition.
double baseValue(const Content& c, const std::string& item);
// The quality 0..1 of the best made `item` `holder` has (0.5 when none is made).
double itemQuality(const Character& holder, const std::string& item);
// AL-87: the worth of what the holder would give: a made thing by its components
// (craft::itemWorth), a plain item by its tags (crafting_config worth.tags). 1 = ordinary.
double itemWorthMult(const Content& c, const Character& holder, const std::string& item);
// A person's taste per category, -1..1 (traits + the person's own seed).
std::map<std::string, double> tasteOf(const World& w, const Character& c);
// The person's taste for an item, -1..1 (its categories summed, clamped).
double itemTaste(const World& w, const Character& c, const std::string& item);
// What one `item` (from `holder`) is worth to `judge`, in coins.
double worthTo(const World& w, const Character& judge, const std::string& item, const Character& holder);
// How many of `item` the person can part with (carried minus worn).
int spare(const Character& c, const std::string& item);

// --- a deal ---------------------------------------------------------------------
// mode: buy (coin for their goods), sell (my goods for their coin), barter,
// gift (give only), treat (give food/drink, eaten at once), meal (buy food and
// eat it on the spot - the tavern).
struct Offer {
    std::string mode = "barter";
    Goods give; // offerer -> responder
    Goods take; // responder -> offerer
};

struct DealLine {
    std::string item;
    int n = 0;
    double unit = 0.0;  // worth of one to the responder
    double taste = 0.0; // the responder's taste for it
};

struct DealEval {
    std::string verdict; // accept | counter | refuse
    std::string reason;  // refuse: too_little | insult | no_goods | no_means | no_room | distrust | enmity |
                         // tired | full | dislike | busy | child | self | nothing; accept: generous for a rich one
    double gain = 0.0, loss = 0.0, keep = 1.0, ratio = 0.0;
    std::vector<DealLine> gets, gives; // the responder's view: gets = Offer.give, gives = Offer.take
    double taste = 0.0;                // gift/treat: the taste of what is given (worth-weighted)
    Offer counter;                     // verdict counter: the deal the responder would take
};

// How `responder` weighs `o` from `offerer` - pure, the world is not touched
// (the HUD shows it before the offer is made).
DealEval weighDeal(const World& w, const Character& offerer, const Character& responder, const Offer& o);

// The default offer of a mode (the wheel's act without an intent): buy food when
// hungry or from a tavern, else the thing worth most to me for its price; sell
// what is worth least to me; a gift the other likes most; a treat of liked food.
// Empty give and take = nothing to offer.
Offer defaultOffer(const World& w, const Character& offerer, const Character& responder, const std::string& mode);

struct DealResult {
    DealEval eval;
    bool done = false; // goods changed hands
};

// The offer made now (both within reach): said aloud, weighed, answered,
// carried out. An NPC offerer takes or leaves a counter at once; a player's
// counter waits (trade_accept) for counter_ttl_ticks.
DealResult makeOffer(World& w, Character& offerer, Character& responder, const Offer& o);
// `offerer` takes the counter `responder` made; false + why when there is none
// (expired, another partner) or it cannot be carried out any more.
bool acceptCounter(World& w, Character& offerer, Character& responder, std::string& why);

// The pending counter `responder` made to `offerer` (nullptr = none, or expired).
const Json* pendingCounter(const World& w, const Character& responder, EntityId offerer);

// --- roles ------------------------------------------------------------------------
const TradeRole* roleOf(const World& w, const Character& c);
// Gives the role (and its stock, said in the trace); "" role clears it.
void setOccupation(World& w, Character& c, const std::string& role);

// --- acts --------------------------------------------------------------------------
// The mode of a catalog act ("" = not a trade act).
std::string tradeMode(const Content& c, const std::string& act);
// The catalog act of a mode (trade_<mode>).
std::string tradeAct(const std::string& mode);
// Called by the FSM when a trade act is done: the intent (or the default
// offer) made to the target. False when `d` is not a trade act.
bool tradeOnAct(World& w, Character& c, const InteractionDef& d, Character* target);
// Puts the intent on the actor and starts the act toward `target` (player:
// an order now; NPC: a queued task). False + why when it cannot start.
bool startTrade(World& w, Character& c, Character& target, const Offer& o, bool player, std::string& why);

// NPC trade life, every tick (cheap): hungry people with coin go to the
// tavern, the tavernkeeper treats friends, restocks daily; roles and purses
// are handed out once per settlement (auto_assign).
void tradeTick(World& w);

// Commands:
//   trade {id, target, mode, give?, take?}   the player's person offers (walks there first)
//   trade_accept {id, target}                takes the target's counter
//   trade_quote {id, target, mode, give?, take?} -> result "ok:<verdict>:<reason>:<gain>:<loss>"
//   set_occupation {id, role}                trader | tavernkeeper | ""
void installTrade(World& w);

// Goods as {item: n} JSON and back (unknown items and n <= 0 dropped).
Json goodsJson(const Goods& g);
Goods goodsFrom(const Content& c, const Json& j);

} // namespace lr::sim
