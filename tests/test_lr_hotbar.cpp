// AL-80: the HUD's quick bars as data (src/game_view/hotbar.hpp): a new
// person gets the starting layout, the layout goes through the save and back,
// a slot used with nobody chosen asks for an aim, an empty one does nothing.

#include "framework.hpp"
#include "game_view/hotbar.hpp"

using lr::game::BarRef;
using lr::game::barUse;
using lr::game::BarUse;
using lr::game::Hotbars;

TEST(lr_hotbar_new_person_gets_the_starting_layout) {
    Hotbars h;
    CHECK_EQ(h.get(7, 0, 0, 0).id, std::string("chat"));
    CHECK_EQ(h.get(7, 0, 0, 1).kind, std::string("goal"));
    CHECK_EQ(h.get(7, 0, 0, 3).id, std::string("rest"));
    CHECK(h.get(7, 0, 0, 4).empty());
    CHECK(h.get(7, 1, 0, 0).empty());   // the second bar starts empty
    CHECK_EQ(h.get(7, 0, Hotbars::kFightPage, 0).id, std::string("attack"));
    CHECK(h.get(7, 9, 0, 0).empty());   // out of range: nothing
}

TEST(lr_hotbar_layout_goes_through_the_save) {
    Hotbars h;
    h.set(7, 1, 2, 5, {"item", "bread", 0});
    h.set(7, 0, 0, 1, {});              // the person cleared a starting slot
    h.set(9, 0, 1, 0, {"act", "hug", 2});
    h.swap(7, 0, 0, 0, 1, 0, 7);        // between the two bars
    h.turnPage(7, 1, -1);               // wraps to the last page
    h.locked = true;
    Hotbars r;
    r.fromJson(nlohmann::json::parse(h.toJson().dump()));
    CHECK(r.locked);
    CHECK_EQ(r.get(7, 1, 2, 5).id, std::string("bread"));
    CHECK(r.get(7, 0, 0, 1).empty());   // an emptied slot stays empty after a load
    CHECK(r.get(7, 0, 0, 0).empty());
    CHECK_EQ(r.get(7, 1, 0, 7).id, std::string("chat"));
    CHECK_EQ(r.of(7).page[1], Hotbars::kPages - 1);
    CHECK_EQ(r.get(9, 0, 1, 0).aim, 2);
    // a broken save is skipped, not thrown on
    Hotbars bad;
    bad.fromJson(nlohmann::json::parse(R"({"people": {"x": 1, "5": [{"pages": [[{"kind": "spell", "id": "x"}]]}]}})"));
    CHECK(bad.get(5, 0, 0, 0).empty());
}

TEST(lr_hotbar_use_without_a_target_aims) {
    const BarRef talk{"act", "chat", 0};
    BarUse u = barUse(talk, "character", "", 0);
    CHECK(u.kind == BarUse::Aim);
    u = barUse(talk, "character", "", 42);
    CHECK(u.kind == BarUse::Command);
    CHECK_EQ(u.args.value("target", std::uint64_t(0)), std::uint64_t(42));
    u = barUse({"act", "chat", 3}, "character", "", 42);   // "ask" even with someone chosen
    CHECK(u.kind == BarUse::Aim);
    // an act on oneself or on a place needs no aim: the sim finds the place
    u = barUse({"act", "pray_alone", 0}, "self", "", 0);
    CHECK(u.kind == BarUse::Command && !u.args.contains("target"));
    u = barUse({"act", "gather_firewood", 0}, "object:deadwood", "", 0);
    CHECK(u.kind == BarUse::Command && !u.args.contains("target"));
    // a goal and a thing of the bag
    u = barUse({"goal", "eat", 0}, "", "", 0);
    CHECK(u.type == "set_goal" && u.args.value("goal", std::string()) == "eat");
    u = barUse({"item", "bread", 0}, "", "eat_bread", 0);
    CHECK(u.kind == BarUse::Command && u.args.value("interaction", std::string()) == "eat_bread");
    CHECK(barUse({"item", "stone", 0}, "", "", 0).kind == BarUse::None);   // nothing uses it up
    CHECK(barUse({}, "", "", 0).kind == BarUse::None);
}
