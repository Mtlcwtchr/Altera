// AL-99: the 2D client's session (src/client2d/session2d): a new game by seed
// and size, who the player begins as, and saves that load back to the same world.
#include <filesystem>
#include <string>

#include "client2d/session2d.hpp"
#include "framework.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;
namespace fs = std::filesystem;

namespace {

const std::string kRoot = ALTERA_SIM_SOURCE_DIR;

const Content& contentS() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

const Json& baseCfg() {
    static const Json j = loadJsonFile(kRoot + "/content/sim/test_world.json");
    return j;
}

EntityId led(const World& w) {
    for (const auto& [id, c] : w.characters())
        if (c.controller.isPlayer() && c.controller.playerId == 1) return id;
    return kNone;
}

} // namespace

TEST(altera_client2d_session_newgame) {
    // the seed as digits or as a word; unknown sizes and seasons fall back
    CHECK_EQ(c2::NewGame::fromJson({{"seed", "42"}}, 7).seed, std::uint64_t(42));
    CHECK(c2::NewGame::fromJson({{"seed", "Emberford"}}, 7).seed == hashStr("Emberford"));
    CHECK_EQ(c2::NewGame::fromJson({{"size", "huge"}, {"season", "monsoon"}}, 7).size, std::string("medium"));
    CHECK(c2::NewGame::fromJson({{"season", "winter"}}, 7).season == "winter");
    const Json cfg = c2::worldConfig(baseCfg(), c2::NewGame::fromJson({{"size", "small"}, {"season", "autumn"}}, 3));
    CHECK_EQ(cfg["size_m"].get<int>(), 8000);
    CHECK_EQ(cfg["families"].get<int>(), 3);
    CHECK(cfg["season"] == "autumn" && !cfg.contains("start_day"));

    c2::NewGame g;
    g.seed = 11;
    g.size = "small";
    auto w = c2::makeWorld2d(contentS(), baseCfg(), g, 1.0);
    CHECK_EQ(w->map().sizeM, 8000.0);
    CHECK(led(*w) == kNone);   // nobody led before the pick

    const c2::ViewText t;
    const Json opts = c2::newGameOptions(contentS(), t, g);
    CHECK_EQ(opts["sizes"].size(), std::size_t(3));
    CHECK_EQ(opts["seasons"].size(), std::size_t(4));
    CHECK(opts["backstories"].size() == contentS().backstories.size());

    const Json fams = c2::dynastiesJson(*w, t);
    CHECK(!fams.empty());
    CHECK(fams[0]["full"].get<bool>());
    EntityId adult = kNone;
    for (const auto& p : fams[0]["people"])
        if (p["adult"].get<bool>()) adult = p["id"].get<EntityId>();
    CHECK(adult != kNone);
    CHECK_EQ(c2::beginAs(*w, {{"id", adult}}), std::string("ok"));
    CHECK(led(*w) == adult);
    CHECK_EQ(c2::beginAs(*w, {{"watch", true}}), std::string("ok"));
    CHECK(led(*w) == kNone);
    // a newcomer of a backstory
    CHECK_EQ(c2::beginAs(*w, {{"backstory", contentS().backstories.front().id}}), std::string("ok"));
    CHECK(led(*w) != kNone);
    CHECK(w->character(led(*w))->backstory == contentS().backstories.front().id);
    CHECK(c2::beginAs(*w, {{"backstory", "nobody_knows"}}).rfind("rejected", 0) == 0);
}

TEST(altera_client2d_session_saves) {
    const fs::path dir = fs::temp_directory_path() / "altera_client2d_saves_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    c2::NewGame g;
    g.seed = 5;
    g.size = "small";
    auto w = c2::makeWorld2d(contentS(), baseCfg(), g, 2.0);
    const Json fams = c2::dynastiesJson(*w, c2::ViewText{});
    CHECK_EQ(c2::beginAs(*w, {{"id", fams[0]["people"][0]["id"]}}), std::string("ok"));

    for (const auto& m : c2::listSaves2d(dir)) CHECK(!m["used"].get<bool>());
    std::string why;
    Json game = g.toJson();
    game["view"] = {{"x", 10}, {"y", 20}};
    CHECK(c2::writeSave2d(dir, 2, *w, game, &why));
    const Json list = c2::listSaves2d(dir);
    CHECK_EQ(list.size(), std::size_t(c2::kSlots2d + 1));
    CHECK(list[2]["used"].get<bool>() && !list[1]["used"].get<bool>());
    CHECK(list[2]["size"] == "small" && list[2].contains("person"));
    CHECK(!c2::writeSave2d(dir, c2::kSlots2d + 1, *w, game, &why));

    // the loaded world is the saved one, and it goes on the same way
    const Json save = c2::readSave2d(dir, 2);
    CHECK(save["game"]["view"]["x"] == 10);
    auto back = c2::loadWorld2d(contentS(), save);
    CHECK_EQ(back->stateHash(), w->stateHash());
    CHECK(led(*back) == led(*w));
    w->run(kTicksPerDay / 24);
    back->run(kTicksPerDay / 24);
    CHECK_EQ(back->stateHash(), w->stateHash());

    CHECK(c2::deleteSave2d(dir, 2));
    CHECK(!c2::listSaves2d(dir)[2]["used"].get<bool>());
    bool threw = false;
    try {
        c2::readSave2d(dir, 2);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    fs::remove_all(dir, ec);
}
