// Altera life-sim core (src/sim): world skeleton, determinism, persistence.

#include <fstream>
#include <set>

#include "framework.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& content() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Json& worldCfg() {
    static const Json j = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return j;
}

World freshWorld(std::uint64_t seed = 0) { return makeTestWorld(content(), worldCfg(), seed); }

} // namespace

TEST(altera_sim_content_validates) {
    const ValidationReport r = validate(content());
    for (const auto& e : r.errors) std::cerr << "    " << e << "\n";
    CHECK(r.ok());
    CHECK(content().goals.size() >= 4);
    CHECK(content().interaction("gather_berries") != nullptr);
}

TEST(altera_sim_validator_catches_broken_data) {
    // An achieve no chain can satisfy, a dangling interaction, a bad curve.
    const Json items = Json::parse(R"({"items": [{"id": "meal"}, {"id": "gold"}]})");
    const Json objects = Json::parse(R"({"objects": [{"id": "storage"}]})");
    const Json needs = Json::parse(R"({"needs": [{"id": "hunger"}]})");
    const Json goals = Json::parse(R"({"goals": [{"id": "eat", "task": "obtain_food",
        "considerations": [{"input": "need:hunger", "curve": {"type": "logistic", "k": 99}}]}]})");
    const Json htn = Json::parse(R"({"tasks": [{"id": "obtain_food", "methods": [{"id": "m",
        "steps": [{"achieve": ["has:gold>=1"]}, {"do": "no_such_action"}]}]}]})");
    const Json inter = Json::parse(R"({"interactions": [{"id": "eat_meal", "category": "household",
        "pre": ["has:meal>=1"], "duration": 1}]})");
    const Content c = Content::fromJson(items, objects, needs, Json::object(), goals, htn, inter, Json::object(),
                                        Json::object());
    const ValidationReport r = validate(c);
    CHECK(!r.ok());
    auto mentions = [&](const std::string& s) {
        for (const auto& e : r.errors)
            if (e.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(mentions("no_such_action"));
    CHECK(mentions("not obtainable from an empty start"));
    CHECK(mentions("curve k"));
}

TEST(altera_sim_syntax_errors_throw) {
    bool threw = false;
    try {
        parseCondition("has:meal>>1");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    const Condition c = parseCondition("!need:hunger<0.3");
    CHECK(c.negate);
    CHECK(c.test(0.5));
    CHECK(!c.test(0.1));
    const Effect e = parseEffect("target_stock:berries-2");
    CHECK_EQ(e.op, '-');
    CHECK(std::abs(e.signedDelta() + 2.0) < 1e-12);
}

TEST(altera_sim_test_world_shape) {
    World w = freshWorld();
    CHECK(w.map().sizeM >= 8000.0 && w.map().sizeM <= 16000.0);
    CHECK_EQ(w.families().size(), size_t(3));
    CHECK(w.characters().size() >= 12 && w.characters().size() <= 18);
    std::set<std::string> biomes;
    for (auto b : w.map().biome) biomes.insert(w.map().biomeNames[b]);
    CHECK(biomes.size() >= 2);
    int bushes = 0;
    for (const auto& [id, o] : w.objects()) bushes += o.type == "berry_bush";
    CHECK(bushes > 50);
    for (const auto& [id, c] : w.characters()) {
        CHECK(c.home != kNone);
        CHECK(!c.knownObjects.empty());
        CHECK(c.relations.size() == w.characters().size() - 1);
    }
}

TEST(altera_sim_same_seed_same_world) {
    CHECK_EQ(freshWorld().stateHash(), freshWorld().stateHash());
    CHECK(freshWorld(7).stateHash() != freshWorld(8).stateHash());
}

TEST(altera_sim_snapshot_roundtrip) {
    World a = freshWorld();
    a.run(300);
    const Json snap = a.toJson();
    World b(content());
    b.loadJson(snap);
    CHECK_EQ(b.toJson().dump(), snap.dump());
    // Continuing from the snapshot equals never having stopped.
    a.run(200);
    b.run(200);
    CHECK_EQ(a.stateHash(), b.stateHash());
}

TEST(altera_sim_command_log_replays) {
    World a = freshWorld();
    const EntityId someone = a.characters().begin()->first;
    a.run(10);
    Command c;
    c.source = "scenario";
    c.type = "set_need";
    c.args = {{"id", someone}, {"need", "hunger"}, {"value", 0.05}};
    a.submit(c);
    a.run(50);
    Command bad;
    bad.source = "scenario";
    bad.type = "no_such_command";
    a.applyNow(bad);
    a.run(50);
    CHECK_EQ(a.commandLog().size(), size_t(2));
    CHECK_EQ(a.commandLog()[1].result, std::string("rejected:unknown command"));

    World b = freshWorld();
    b.replay(a.commandLog());
    b.run(110);
    CHECK_EQ(a.stateHash(), b.stateHash());
}
