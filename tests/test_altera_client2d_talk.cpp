// AL-98: the talk and the trade windows of the 2D client (src/client2d/talk2d):
// the relation, the lines, the topics, the wheel's acts with the chance in a
// tone; the goods with the other side's own price, the draft weighed, a deal
// done and its lines in the talk.
#include <string>

#include "client2d/talk2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/trade.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;

namespace {

const std::string kRoot = ALTERA_SIM_SOURCE_DIR;

struct TalkFixture {
    Content content = Content::load(kRoot + "/content/ai");
    Loc loc = Loc::load(kRoot + "/content/loc");
    Json wheel = loadJsonFile(kRoot + "/content/ui/wheel.json");
};

TalkFixture& fx() {
    static TalkFixture f;
    return f;
}

World makeVillage() {
    Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    World v = makeTestWorld(fx().content, cfg, 7);
    installAi(v);
    return v;
}

// Two grown people of different households, the second standing by the first.
std::pair<EntityId, EntityId> pair2(World& w) {
    EntityId a = kNone, b = kNone;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.ageDays / kDaysPerYear < 18) continue;
        if (a == kNone) a = id;
        else if (c.status.family != w.character(a)->status.family) { b = id; break; }
    }
    if (a != kNone && b != kNone) { Vec2 p = w.character(a)->pos; p.x += 1.0; w.character(b)->pos = p; }
    return {a, b};
}

} // namespace

TEST(altera_client2d_talk_window) {
    World w = makeVillage();
    auto [me, other] = pair2(w);
    CHECK(me != kNone && other != kNone);
    c2::ViewText t;
    t.loc = &fx().loc;
    t.lang = "ru";
    c2::ActCatalog acts = c2::ActCatalog::make(w, fx().wheel);
    c2::TalkCatalog cat = c2::TalkCatalog::make(fx().wheel);
    CHECK(!cat.spheres.empty());
    const Json j = c2::talkJson(w, t, acts, cat, me, other, "warm");
    CHECK(j["ok"].get<bool>());
    CHECK(!j["them"]["name"].get<std::string>().empty());
    CHECK(j["them"]["name_ins"].get<std::string>() != j["them"]["name"].get<std::string>());   // ru: «с Томасом»
    CHECK(j["rel"].contains("affinity") && j["rel"].contains("trust") && j["rel"].contains("fear"));
    CHECK(!j["rel_names"]["affinity"].get<std::string>().empty());
    CHECK_EQ(j["tone"].get<std::string>(), std::string("warm"));
    CHECK(j["tones"].size() >= 2);
    CHECK(!j["spheres"].empty());
    int chances = 0;
    for (const auto& sp : j["spheres"]) {
        CHECK(!sp["name"].get<std::string>().empty());
        for (const auto& a : sp["acts"]) {
            CHECK(!a["name"].get<std::string>().empty());
            if (!a["can"].get<bool>()) CHECK(!a["why"].get<std::string>().empty());
            if (a.contains("chance")) {
                ++chances;
                CHECK(a["chance"].get<double>() >= 0.0 && a["chance"].get<double>() <= 1.0);
            }
        }
    }
    CHECK(chances > 0);
    // the tone moves some chance (warm vs hostile)
    const Json h = c2::talkJson(w, t, acts, cat, me, other, "hostile");
    bool moved = false;
    for (std::size_t s = 0; s < j["spheres"].size() && s < h["spheres"].size(); ++s)
        for (std::size_t k = 0; k < j["spheres"][s]["acts"].size() && k < h["spheres"][s]["acts"].size(); ++k)
            if (j["spheres"][s]["acts"][k].value("chance", -1.0) != h["spheres"][s]["acts"][k].value("chance", -1.0)) moved = true;
    CHECK(moved);
    // nobody to talk to: not ok
    CHECK(!c2::talkJson(w, t, acts, cat, me, me, "")["ok"].get<bool>());
    CHECK(!c2::talkJson(w, t, acts, cat, me, kNone, "")["ok"].get<bool>());
    // a topic out of range is refused
    CHECK(!c2::sayTopic(w, me, other, 999).empty());
}

TEST(altera_client2d_trade_window) {
    World w = makeVillage();
    auto [me, other] = pair2(w);
    CHECK(me != kNone && other != kNone);
    c2::ViewText t;
    t.loc = &fx().loc;
    t.lang = "ru";
    Character& m = *w.character(me);
    Character& o = *w.character(other);
    m.inventory["coin"] = 40;
    o.inventory["bread"] = std::max(o.inventory["bread"], 6);
    const Json empty = c2::tradeJson(w, t, me, other, Json::object(), Json::object());
    CHECK(empty["ok"].get<bool>());
    CHECK_EQ(empty["coin"].get<int>(), 40);
    bool breadPriced = false;
    for (const auto& g : empty["theirs"])
        if (g["item"] == "bread") breadPriced = g["worth"].get<double>() > 0.0 && g["n"].get<int>() >= 6;
    CHECK(breadPriced);
    CHECK(!empty.contains("verdict"));
    // a fair buy is weighed: coin for bread
    const Json buy = c2::tradeJson(w, t, me, other, Json{{"coin", 20}}, Json{{"bread", 1}});
    CHECK_EQ(buy["mode"].get<std::string>(), std::string("buy"));
    CHECK(!buy["verdict"].get<std::string>().empty());
    CHECK(buy["gain"].get<double>() > 0.0);
    // a gift has nothing back
    CHECK_EQ(c2::tradeJson(w, t, me, other, Json{{"coin", 1}}, Json::object())["mode"].get<std::string>(), std::string("gift"));
    // the deal made: goods change hands when accepted, and the two said it aloud
    Offer off;
    off.mode = "buy";
    off.give = {{"coin", 20}};
    off.take = {{"bread", 1}};
    const DealResult r = makeOffer(w, m, o, off);
    if (r.done) CHECK_EQ(m.inventory["coin"], 20);
    c2::ActCatalog acts = c2::ActCatalog::make(w, fx().wheel);
    c2::TalkCatalog cat = c2::TalkCatalog::make(fx().wheel);
    const Json talk = c2::talkJson(w, t, acts, cat, me, other, "");
    CHECK(!talk["lines"].empty());
    for (const auto& l : talk["lines"]) CHECK(!l["text"].get<std::string>().empty());
    CHECK(!c2::tradeJson(w, t, me, other, Json::object(), Json::object())["last"].empty());
}
