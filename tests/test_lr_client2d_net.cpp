// LR-160: the players of a multiplayer game (src/client2d/lobby2d): tokens to
// ids, the host's window as player 1, a full game, one dynasty per player, a
// person handed to free will while their player is away and taken again when
// they are back, and the players kept across a save.
#include <string>

#include "client2d/lobby2d.hpp"
#include "client2d/session2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& contentN() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

std::unique_ptr<World> smallWorld() {
    c2::NewGame g;
    g.seed = 23;
    g.size = "small";
    static const Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    return c2::makeWorld2d(contentN(), cfg, g, 1.0);
}

// two adults of different families of the full village
std::pair<EntityId, EntityId> twoHouses(const World& w, EntityId* kinOfFirst) {
    const c2::ViewText t;
    const Json fams = c2::dynastiesJson(w, t);
    EntityId a = kNone, b = kNone;
    *kinOfFirst = kNone;
    for (const auto& f : fams) {
        if (!f["full"].get<bool>()) continue;
        std::vector<EntityId> adults;
        for (const auto& p : f["people"])
            if (p["adult"].get<bool>()) adults.push_back(p["id"].get<EntityId>());
        if (adults.empty()) continue;
        if (a == kNone) {
            a = adults[0];
            if (adults.size() > 1) *kinOfFirst = adults[1];
        } else if (b == kNone) b = adults[0];
    }
    return {a, b};
}

} // namespace

TEST(lr_client2d_net_lobby_tokens) {
    c2::Lobby L;
    L.maxPlayers = 3;
    CHECK_EQ(L.addLocal("host", 0.0), 1);
    bool joined = false;
    const int a = L.touch("tokA", "Ann", "web", 1.0, &joined);
    CHECK(joined);
    CHECK_EQ(a, 2);
    CHECK_EQ(L.touch("tokA", "", "web", 2.0, &joined), 2);   // the same token: the same player
    CHECK(!joined);
    CHECK_EQ(L.find(2)->name, std::string("Ann"));
    CHECK_EQ(L.touch("tok-B", "Bob", "native", 2.0), 3);
    CHECK_EQ(L.touch("tokC", "Cid", "web", 2.0), 0);           // full: 3 in
    CHECK_EQ(L.onlineCount(2.0), 3);
    // Ann away long enough: her place frees, Cid gets in
    CHECK_EQ(L.onlineCount(2.0 + L.onlineSec + 1), 1);
    CHECK_EQ(L.touch("tokC", "Cid", "web", 2.0 + L.onlineSec + 1), 4);
    // Ann back: same id, counted as a join again
    CHECK_EQ(L.touch("tokA", "", "web", 40.0, &joined), 2);
    CHECK(joined);
    // tokens and names made safe
    CHECK_EQ(c2::cleanToken("a b<c>/../d"), std::string("abcd"));
    CHECK_EQ(c2::cleanName("  Анна\x01  "), std::string("Анна"));
    CHECK(c2::cleanName(std::string(40, 'x')).size() == 32);
    CHECK_EQ(L.touch("", "", "", 41.0), L.touch("", "", "", 41.0));   // no token: one "anon" player
}

TEST(lr_client2d_net_two_dynasties) {
    auto w = smallWorld();
    EntityId kin = kNone;
    const auto [a, b] = twoHouses(*w, &kin);
    CHECK(a != kNone && b != kNone);
    CHECK_EQ(c2::beginAs(*w, {{"id", a}}, 1), std::string("ok"));
    CHECK_EQ(c2::beginAs(*w, {{"id", b}}, 2), std::string("ok"));
    CHECK(c2::controlledBy(*w, 1) == a);
    CHECK(c2::controlledBy(*w, 2) == b);
    // one cannot take the other's person, nor a kin of the other's house
    CHECK(c2::beginAs(*w, {{"id", a}}, 2).rfind("rejected:controlled_by_other", 0) == 0);
    if (kin != kNone) {
        std::string why;
        CHECK(!mayLead(*w, 2, *w->character(kin), &why));
        CHECK_EQ(why, std::string("dynasty_of_other"));
        CHECK(mayLead(*w, 1, *w->character(kin)));   // but the owner of the house may
        // a third player, free to pick anyone, is kept off the houses already played
        CHECK(!mayLead(*w, 3, *w->character(kin), &why));
    }
    // the person JSON says who may lead whom, for the asking player
    c2::ViewText t1, t2;
    t2.player = 2;
    CHECK(c2::personJson(*w, t1, a)["lead"].get<bool>());
    CHECK(!c2::personJson(*w, t2, a)["lead"].get<bool>());
    CHECK_EQ(c2::personJson(*w, t2, b)["pid"].get<int>(), 2);
    // the world runs with two players in it
    w->run(30);
    CHECK(c2::controlledBy(*w, 1) == a || !w->character(a)->body.alive);
}

TEST(lr_client2d_net_away_and_back) {
    auto w = smallWorld();
    EntityId kin = kNone;
    const auto [a, b] = twoHouses(*w, &kin);
    c2::Lobby L;
    L.releaseSec = 60;
    L.addLocal("host", 0.0);
    const int p2 = L.touch("tokB", "Bob", "web", 0.0);
    CHECK_EQ(c2::beginAs(*w, {{"id", a}}, 1), std::string("ok"));
    CHECK_EQ(c2::beginAs(*w, {{"id", b}}, p2), std::string("ok"));
    CHECK(L.sweep(*w, 10.0).empty());
    // Bob gone: after releaseSec his person lives on their own; the host's stays led
    const auto log = L.sweep(*w, 70.0);
    CHECK_EQ(log.size(), std::size_t(1));
    CHECK(c2::controlledBy(*w, p2) == kNone);
    CHECK(c2::controlledBy(*w, 1) == a);
    CHECK(!w->character(b)->controller.isPlayer());
    w->run(6);   // the sim goes on without him
    // his house is still his: nobody else takes it meanwhile
    std::string why;
    CHECK(!mayLead(*w, 3, *w->character(b), &why));
    // Bob back: the same person again
    L.touch("tokB", "", "web", 80.0);
    L.sweep(*w, 81.0);
    CHECK(c2::controlledBy(*w, p2) == b);
    const c2::ViewText t;
    const Json list = L.toJson(*w, t, 81.0);
    CHECK_EQ(list.size(), std::size_t(2));
    CHECK(list[1]["me"].get<EntityId>() == b);
    // a player who leaves on purpose is away at once
    L.leave(p2, 90.0);
    CHECK(!L.online(*L.find(p2), 90.0));
    L.sweep(*w, 90.5);
    CHECK(c2::controlledBy(*w, p2) == kNone);
    // the players in a save: a restarted server knows the tokens again
    c2::Lobby L2;
    L2.load(L.save(), 100.0);
    CHECK_EQ(L2.touch("tokB", "", "web", 101.0), p2);
    CHECK_EQ(L2.find(p2)->name, std::string("Bob"));
}
