// AL-75: trade, barter, treats and gifts by the NPC's own worth of things and
// taste (sim/trade.hpp). Headless scenes: a trader sells, buys and haggles; a
// tavernkeeper serves a meal for coin and treats a friend; a liked and a
// disliked gift move the relation apart; refusals say why; barter thing for
// thing; the player's order walks over and makes the offer.

#include <algorithm>
#include <map>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/trade.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

Command cmdOf(const std::string& type, Json args, int player = 1) {
    Command c;
    c.source = "player:" + std::to_string(player);
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = player;
    return c;
}

// Adults of different households, not kin, side by side; their inventories and
// relations cleared so each scene starts from what it sets.
std::vector<Character*> adults(World& w, size_t n) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        bool ok = true;
        for (const Character* o : out) ok = ok && o->status.family != c.status.family;
        if (!ok) continue;
        out.push_back(&c);
        if (out.size() == n) break;
    }
    for (size_t i = 0; i < out.size(); ++i) {
        Character& c = *out[i];
        c.inventory.clear();
        c.worn.clear();
        c.gearPins.clear();
        c.craft.items.clear();
        c.relations.clear();
        c.occupation.clear();
        c.deal = Json::object();
        c.ai.fsm = FsmState{};
        c.ai.goal = GoalInstance{};
        c.ai.plan = Plan{};
        c.ai.queue.clear();
        for (auto& [k, v] : c.needs) v = 0.9;
        c.pos = {100.0 + 1.5 * static_cast<double>(i), 100.0};
    }
    return out;
}

bool traced(const Character& c, const std::string& code) {
    return std::any_of(c.ai.trace.begin(), c.ai.trace.end(), [&](const TraceEntry& e) { return e.code == code; });
}

// The keys of the lines `speaker` said (say.line in its own trace).
std::vector<std::string> saidKeys(const Character& speaker) {
    std::vector<std::string> v;
    for (const auto& e : speaker.ai.trace)
        if (e.code == "say.line" && e.params.value("speaker", kNone) == speaker.id)
            v.push_back(e.params.value("key", std::string()));
    return v;
}

bool said(const Character& c, const std::string& prefix) {
    for (const auto& k : saidKeys(c))
        if (k.rfind(prefix, 0) == 0) return true;
    return false;
}

double affinity(const Character& of, const Character& to) {
    auto it = of.relations.find(to.id);
    return it == of.relations.end() ? 0.0 : it->second.affinity;
}

// An item of a category the person likes (taste > 0.5) / dislikes (< -0.5), "" if none.
std::string itemOfTaste(const World& w, const Character& c, bool liked) {
    for (const auto& [id, d] : w.content().items) {
        if (std::find(d.tags.begin(), d.tags.end(), "money") != d.tags.end()) continue;
        const double t = itemTaste(w, c, id);
        if (liked ? t > 0.5 : t < -0.5) return id;
    }
    return "";
}

std::string neutralItem(const World& w, const Character& c) {
    for (const std::string id : {"cord", "stone", "firewood", "branch", "clay", "thatch"})
        if (w.content().items.count(id) && std::abs(itemTaste(w, c, id)) < 0.01) return id;
    return "";
}

} // namespace

TEST(lr_sim_trade_rules_load_and_worth) {
    const Content& ct = realContent();
    const TradeRules* R = tradeRules(ct);
    CHECK(R != nullptr);
    if (!R) return;
    CHECK(R->roles.count("trader") == 1);
    CHECK(R->roles.count("tavernkeeper") == 1);
    for (const char* act : {"trade_buy", "trade_sell", "trade_barter", "trade_gift", "trade_treat", "trade_meal"})
        CHECK(!tradeMode(ct, act).empty());
    auto in = [&](const std::string& item, const std::string& cat) {
        const auto v = itemCategories(ct, item);
        return std::find(v.begin(), v.end(), cat) != v.end();
    };
    // categories by tags first: any item tagged food/drink/tool falls in by itself
    CHECK(in("meal", "food"));
    CHECK(in("meal", "cooked"));
    CHECK(in("ale", "drink"));
    CHECK(in("ale", "alcohol"));
    CHECK(in("honey", "sweet"));
    CHECK(in("axe", "tool"));
    CHECK(!in("stone", "food"));
    CHECK_EQ(baseValue(ct, "coin"), 1.0);
    CHECK(baseValue(ct, "axe") > baseValue(ct, "meal"));
    // an item with no listed value still has a worth from its tags
    CHECK(baseValue(ct, "bread") >= 1.0);
    World w = realWorld();
    auto p = adults(w, 2);
    Character& a = *p[0];
    Character& b = *p[1];
    // the same person has the same taste whenever asked (seed, not tick)
    const auto t1 = tasteOf(w, a);
    w.step();
    CHECK(t1 == tasteOf(w, a));
    CHECK(!t1.empty());
    // hunger raises the worth of food; quality raises the worth of a made thing
    b.inventory["meal"] = 1;
    a.needs["hunger"] = 0.9;
    const double fed = worthTo(w, a, "meal", b);
    a.needs["hunger"] = 0.1;
    const double hungry = worthTo(w, a, "meal", b);
    CHECK(hungry > fed * 1.3);
    b.inventory["axe"] = 1;
    const double plain = worthTo(w, a, "axe", b);
    craft::Item made;
    made.form = "axe";
    made.q = 0.95;
    b.craft.items.push_back(made);
    CHECK(worthTo(w, a, "axe", b) > plain * 1.2);
}

TEST(lr_sim_trade_trader_sells_haggles_buys) {
    World w = realWorld();
    auto p = adults(w, 2);
    Character& buyer = *p[0];
    Character& trader = *p[1];
    setOccupation(w, trader, "trader");
    CHECK(trader.has("axe") >= 1);
    CHECK(traced(trader, "trade.role.trader"));
    buyer.inventory["coin"] = 40;
    // what the trader asks for the axe (his worth x his margin)
    Offer ask = defaultOffer(w, buyer, trader, "buy");
    CHECK(ask.take.count("axe") == 1 || !ask.take.empty());
    Offer o;
    o.mode = "buy";
    o.take["axe"] = 1;
    // a lowball well under the price: a counter asking more coin, said aloud
    const DealEval full = weighDeal(w, buyer, trader, {"buy", {{"coin", 30}}, {{"axe", 1}}});
    CHECK_EQ(full.verdict, std::string("accept"));
    const int fair = static_cast<int>(std::ceil(full.loss * full.keep));
    CHECK(fair > 3);
    o.give["coin"] = static_cast<int>(fair * 0.8);
    DealResult r = makeOffer(w, buyer, trader, o);
    CHECK_EQ(r.eval.verdict, std::string("counter"));
    CHECK(said(buyer, "say.trade.offer.buy"));
    CHECK(said(trader, "say.trade.counter.coin"));
    CHECK(traced(buyer, "trade.weigh"));
    CHECK(traced(buyer, "trade.countered_coin"));
    // an NPC buyer weighs the counter at once: worth it to him, he takes it aloud
    if (r.done) CHECK(said(buyer, "say.trade.accept.counter"));
    // the buyer (an NPC) took the counter at once when it was worth it to him; else
    // the player's way: take the counter
    // (an NPC buyer that found the counter too dear said no aloud; then he pays the full price)
    if (buyer.has("axe") == 0) {
        CHECK(said(buyer, "say.trade.counter_no"));
        CHECK(makeOffer(w, buyer, trader, {"buy", {{"coin", fair}}, {{"axe", 1}}}).done);
    }
    CHECK_EQ(buyer.has("axe"), 1);
    CHECK(buyer.has("coin") <= 40 - static_cast<int>(fair * 0.8));
    CHECK(said(trader, "say.trade.accept"));
    CHECK(traced(buyer, "trade.done"));
    // an insulting offer is refused with the reason, and the trader is put out
    const double before = affinity(trader, buyer);
    trader.inventory["needle"] = 2;
    DealResult insult = makeOffer(w, buyer, trader, {"buy", {{"coin", 1}}, {{"cloak", 1}}});
    CHECK_EQ(insult.eval.verdict, std::string("refuse"));
    CHECK_EQ(insult.eval.reason, std::string("insult"));
    CHECK(said(trader, "say.trade.refuse.insult"));
    CHECK(affinity(trader, buyer) < before);
    // the trader buys a thing for less than he sells it: a margin both ways
    buyer.inventory.erase("axe");
    buyer.inventory["tunic"] = 1;
    const Offer sell = defaultOffer(w, buyer, trader, "sell");
    CHECK_EQ(sell.give.count("tunic"), size_t{1});
    CHECK(sell.take.count("coin") == 1);
    const int paid = sell.take.at("coin");
    const int coinsBefore = buyer.has("coin");
    DealResult sold = makeOffer(w, buyer, trader, sell);
    CHECK(sold.done);
    CHECK_EQ(buyer.has("coin"), coinsBefore + paid);
    CHECK_EQ(trader.has("tunic"), 1);
    const Offer back = defaultOffer(w, buyer, trader, "buy");
    const DealEval resale = weighDeal(w, buyer, trader, {"buy", {{"coin", paid}}, {{"tunic", 1}}});
    CHECK(resale.verdict != "accept"); // he will not sell it back for what he paid
    (void)back;
    // haggling on and on: after max_rounds the trader is tired of it
    DealEval last;
    for (int i = 0; i < 4; ++i) last = makeOffer(w, buyer, trader, {"buy", {{"coin", 1}}, {{"salt", 1}}}).eval;
    CHECK_EQ(last.reason, std::string("tired"));
    CHECK(said(trader, "say.trade.refuse.tired"));
}

TEST(lr_sim_trade_tavern_meal_for_coin_and_treat) {
    World w = realWorld();
    auto p = adults(w, 3);
    Character& guest = *p[0];
    Character& keeper = *p[1];
    Character& pal = *p[2];
    setOccupation(w, keeper, "tavernkeeper");
    CHECK(keeper.has("meal") > 0);
    const int meals = keeper.has("meal");
    const int kcoins = keeper.has("coin");
    guest.inventory["coin"] = 10;
    guest.needs["hunger"] = 0.2;
    // the default meal: food the keeper sells, for what he asks; eaten on the spot
    const Offer o = defaultOffer(w, guest, keeper, "meal");
    CHECK(!o.take.empty());
    CHECK(o.give.count("coin") == 1);
    const int price = o.give.at("coin");
    DealResult r = makeOffer(w, guest, keeper, o);
    CHECK(r.done);
    CHECK(guest.need("hunger") > 0.3);
    CHECK_EQ(guest.has("coin"), 10 - price);
    CHECK_EQ(keeper.has("coin"), kcoins + price);
    CHECK(keeper.has("meal") < meals || keeper.has("bread") < 3 || keeper.has("ale") < 6);
    CHECK(guest.has("meal") == 0); // eaten, not carried off
    CHECK(said(keeper, "say.trade.accept.meal"));
    // a guest with too few coins: the keeper counters or refuses, never serves for less
    guest.inventory["coin"] = 1;
    guest.needs["hunger"] = 0.2;
    DealResult poor = makeOffer(w, guest, keeper, {"meal", {{"coin", 1}}, {{"meal", 1}}});
    CHECK(!poor.done);
    CHECK(poor.eval.verdict == "refuse");
    // the keeper treats a friend: the friend eats, likes him more, says thanks
    pal.needs["hunger"] = 0.3;
    const double aff0 = affinity(pal, keeper);
    const Offer treat = defaultOffer(w, keeper, pal, "treat");
    CHECK(!treat.give.empty());
    DealResult t = makeOffer(w, keeper, pal, treat);
    if (t.done) {
        CHECK(pal.need("hunger") > 0.3);
        CHECK(said(pal, "say.trade.treat."));
        CHECK(affinity(pal, keeper) != aff0);
    } else {
        CHECK_EQ(t.eval.reason, std::string("dislike")); // a treat it cannot stand is refused aloud
        CHECK(said(pal, "say.trade.refuse.dislike"));
    }
    // a full person refuses a treat of food: "I am full"
    pal.needs["hunger"] = 0.99;
    keeper.inventory["bread"] = 2;
    DealResult full = makeOffer(w, keeper, pal, {"treat", {{"bread", 1}}, {}});
    if (itemTaste(w, pal, "bread") > -0.5) {
        CHECK_EQ(full.eval.reason, std::string("full"));
        CHECK(said(pal, "say.trade.refuse.full"));
    }
}

TEST(lr_sim_trade_npc_goes_to_tavern_when_hungry) {
    World w = realWorld();
    auto p = adults(w, 2);
    Character& guest = *p[0];
    Character& keeper = *p[1];
    setOccupation(w, keeper, "tavernkeeper");
    keeper.status.settlement = guest.status.settlement;
    guest.inventory["coin"] = 12;
    guest.needs["hunger"] = 0.15;
    guest.pos = {keeper.pos.x + 25.0, keeper.pos.y};
    bool ate = false;
    for (int i = 0; i < 200 && !ate; ++i) {
        w.step();
        guest.needs["hunger"] = std::min(guest.need("hunger"), 0.3); // stays hungry until fed by the deal
        ate = traced(guest, "trade.done");
    }
    CHECK(ate);
    CHECK(guest.has("coin") < 12);
    CHECK(said(guest, "say.trade.offer.meal"));
}

TEST(lr_sim_trade_gift_liked_vs_disliked) {
    World w = realWorld(11);
    auto p = adults(w, 2);
    Character& giver = *p[0];
    Character& friendP = *p[1];
    std::string liked = itemOfTaste(w, friendP, true);
    std::string disliked = itemOfTaste(w, friendP, false);
    // a trait-free fallback: give the person a known taste
    if (liked.empty() || disliked.empty()) {
        friendP.traits.push_back("hard_drinker");
        friendP.traits.push_back("ascetic");
        liked = itemOfTaste(w, friendP, true);
        disliked = itemOfTaste(w, friendP, false);
    }
    CHECK(!liked.empty());
    CHECK(!disliked.empty());
    const std::string plain = neutralItem(w, friendP);
    CHECK(!plain.empty());
    giver.inventory[liked] = 1;
    giver.inventory[disliked] = 1;
    giver.inventory[plain] = 1;
    auto delta = [&](const std::string& item) {
        const double a0 = affinity(friendP, giver);
        DealResult r = makeOffer(w, giver, friendP, {"gift", {{item, 1}}, {}});
        CHECK(r.done);
        return affinity(friendP, giver) - a0;
    };
    const double dLiked = delta(liked);
    const double dPlain = delta(plain);
    const double dDisliked = delta(disliked);
    CHECK(dLiked > 0.0);
    CHECK(dDisliked < 0.0);
    CHECK(dLiked > dPlain);
    CHECK(dPlain >= 0.0);
    CHECK(said(friendP, "say.trade.gift.like"));
    CHECK(said(friendP, "say.trade.gift.dislike"));
    CHECK(traced(giver, "trade.gift_like"));
    CHECK(traced(giver, "trade.gift_dislike"));
    CHECK_EQ(friendP.has(disliked), 1); // taken, if grudgingly
    // the default gift picks what the other likes most
    giver.inventory[liked] = 1;
    giver.inventory[disliked] = 1;
    const Offer d = defaultOffer(w, giver, friendP, "gift");
    CHECK(d.give.count(liked) == 1 || itemTaste(w, friendP, d.give.begin()->first) >= itemTaste(w, friendP, liked));
}

TEST(lr_sim_trade_refusals_say_why) {
    World w = realWorld();
    auto p = adults(w, 2);
    Character& a = *p[0];
    Character& b = *p[1];
    // asking for what the other has not got
    a.inventory["coin"] = 5;
    DealEval e = makeOffer(w, a, b, {"buy", {{"coin", 5}}, {{"axe", 1}}}).eval;
    CHECK_EQ(e.reason, std::string("no_goods"));
    CHECK(said(b, "say.trade.refuse.no_goods"));
    // offering what one has not got
    b.inventory["axe"] = 1;
    e = makeOffer(w, a, b, {"buy", {{"coin", 50}}, {{"axe", 1}}}).eval;
    CHECK_EQ(e.reason, std::string("no_means"));
    // no trust, no trade
    b.relations[a.id].trust = -0.8;
    e = makeOffer(w, a, b, {"buy", {{"coin", 5}}, {{"axe", 1}}}).eval;
    CHECK_EQ(e.reason, std::string("distrust"));
    CHECK(said(b, "say.trade.refuse.distrust"));
    // the refusal reads in words in the journal (no raw keys)
    const Loc& L = realLoc();
    for (const auto& t : a.ai.trace) {
        if (t.code != "trade.refused") continue;
        const std::string s = traceText(L, "ru", w, a, t);
        CHECK(s.find("trade_") == std::string::npos);
        CHECK(s.find("trace.") == std::string::npos);
    }
    for (const auto& t : b.ai.trace) {
        if (t.code != "say.line") continue;
        const std::string s = traceText(L, "ru", w, b, t);
        CHECK(s.find("say.trade") == std::string::npos);
    }
}

TEST(lr_sim_trade_barter_thing_for_thing) {
    World w = realWorld();
    auto p = adults(w, 2);
    Character& a = *p[0];
    Character& b = *p[1];
    a.inventory["cloak"] = 1;
    b.inventory["honey"] = 1;
    DealResult r = makeOffer(w, a, b, {"barter", {{"cloak", 1}}, {{"honey", 1}}});
    CHECK_EQ(r.eval.verdict, std::string("accept"));
    CHECK(r.done);
    CHECK_EQ(a.has("honey"), 1);
    CHECK_EQ(b.has("cloak"), 1);
    CHECK(said(a, "say.trade.offer.barter"));
    // the other way round (a cheap thing for a dear one) is not taken
    a.inventory["stone"] = 1;
    b.inventory["axe"] = 1;
    DealResult no = makeOffer(w, a, b, {"barter", {{"stone", 1}}, {{"axe", 1}}});
    CHECK(!no.done);
    CHECK_EQ(no.eval.verdict, std::string("refuse"));
}

TEST(lr_sim_trade_player_order_walks_and_offers) {
    World w = realWorld();
    auto p = adults(w, 2);
    Character& me = *p[0];
    Character& other = *p[1];
    other.pos = {me.pos.x + 12.0, me.pos.y};
    me.inventory["honey"] = 2;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me.id}})).result, std::string("ok"));
    const Command q = w.applyNow(cmdOf("trade_quote", {{"id", me.id}, {"target", other.id}, {"mode", "gift"}, {"give", {{"honey", 1}}}}));
    CHECK(q.result.rfind("ok:accept", 0) == 0);
    const Command c = w.applyNow(cmdOf("trade", {{"id", me.id}, {"target", other.id}, {"mode", "gift"}, {"give", {{"honey", 1}}}}));
    CHECK_EQ(c.result, std::string("ok"));
    for (int i = 0; i < 60 && other.has("honey") == 0; ++i) w.step();
    CHECK_EQ(other.has("honey"), 1);
    CHECK_EQ(me.has("honey"), 1);
    CHECK(traced(me, "trade.offer_gift"));
    // set_occupation from a command; a counter accepted by the player
    CHECK_EQ(w.applyNow(cmdOf("set_occupation", {{"id", other.id}, {"role", "trader"}})).result, std::string("ok"));
    me.inventory["coin"] = 30;
    me.pos = other.pos;
    DealResult r = makeOffer(w, me, other, {"buy", {{"coin", 9}}, {{"axe", 1}}});
    if (r.eval.verdict == "counter") {
        CHECK(pendingCounter(w, other, me.id) != nullptr);
        CHECK_EQ(w.applyNow(cmdOf("trade_accept", {{"id", me.id}, {"target", other.id}})).result, std::string("ok"));
        CHECK_EQ(me.has("axe"), 1);
    }
    // the deal state survives a save
    const Json saved = w.toJson();
    World w2(realContent());
    w2.loadJson(saved);
    CHECK_EQ(w2.character(other.id)->occupation, std::string("trader"));
}
