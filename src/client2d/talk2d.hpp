#pragma once
// AL-98: the talk and the trade windows of the 2D client (gdd/ui/client_2d/
// ui_h3/UI_H3.md, screens 03 and 05), read from the sim the way the 3D HUD
// reads them (game_view/sim_game.cpp talkSnap / tradeSnap / wheelChances):
//   talk:  both sides, how the other feels about the player's person
//          (affinity, trust, fear, respect, familiarity, enmity), the lines the
//          two said to each other (say.line, assembled by the dialogue system in
//          the page's language), what the other mutters aside (chooseBark), the
//          topics to bring up (dialogueTopics / menuLabel), the acts of the
//          interaction wheel by sphere with the chance in the chosen tone
//          (actChance, the number the sim rolls), an ask of theirs waiting for
//          an answer (answer_social).
//   trade: both sides' goods with the other's own worth and taste of each
//          (worthTo, itemTaste: the personal price), what they like and dislike,
//          the draft weighed by the sim's rule (weighDeal), a counter waiting,
//          the last word of the deal.
// Read-only: the page acts through the commands perform {tone}, say_topic,
// answer_social, trade, trade_accept.

#include <map>
#include <string>
#include <vector>

#include "client2d/acts2d.hpp"
#include "client2d/view2d.hpp"
#include "sim/trade.hpp"
#include "sim/world.hpp"

namespace altera::client2d {

struct TalkCatalog {
    struct Sphere {
        std::string id;
        std::vector<std::string> acts;
    };
    std::vector<Sphere> spheres;   // content/ui/wheel.json, in its order
    // the chances of one pair in one tone, kept a few ticks (fights draw battles)
    struct Cached {
        sim::Tick at = -1000;
        sim::Json chances;
    };
    std::map<std::string, Cached> cache;
    static TalkCatalog make(const sim::Json& wheel);
};

// The talk window of `me` with `target` in `tone` ("" = the person's current tone).
// {ok, me, them: {id, name, role, vy}, rel: {axis: v}, lines: [{t, who, text}],
//  aside, topics: [{i, act, verb, label, rumor}], tones: [{id, name}], tone,
//  spheres: [{id, name, acts: [act + chance, kind]}], ask?: {act, name, tone, left}}
sim::Json talkJson(sim::World& w, const ViewText& t, ActCatalog& acts, TalkCatalog& cat, sim::EntityId me,
                   sim::EntityId target, const std::string& tone);

// The trade window of `me` with `target`, the draft {give, take} weighed.
// {ok, role, mine/theirs: [{item, name, n, worth, taste}], coin, likes, dislikes,
//  mode, verdict, reason, gain, loss, keep, ratio, counter?: {give, take, text}, last: [text]}
sim::Json tradeJson(const sim::World& w, const ViewText& t, sim::EntityId me, sim::EntityId target,
                    const sim::Json& give, const sim::Json& take);

// The offer's mode by what goes each way: gift (nothing back), buy (coin for goods),
// sell (goods for coin), barter.
std::string offerMode(const sim::Content& c, const sim::Goods& give, const sim::Goods& take);

// The player's person brings topic `index` of dialogueTopics up with `target` (said now).
// "" when said, else why not.
std::string sayTopic(sim::World& w, sim::EntityId me, sim::EntityId target, int index);

} // namespace altera::client2d
