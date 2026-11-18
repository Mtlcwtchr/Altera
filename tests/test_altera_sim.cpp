// Altera life-sim core (src/sim): world skeleton, determinism, persistence.

#include <fstream>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& content() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/ai");
    return c;
}

const Json& worldCfg() {
    static const Json j = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/test_world.json");
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

// ---- step 2: the four-layer mind ----------------------------------------

namespace {

World aiWorld(std::uint64_t seed = 0) {
    World w = freshWorld(seed);
    installAi(w);
    return w;
}

bool traceHas(const Character& c, char layer, const std::string& code) {
    for (const auto& e : c.ai.trace)
        if (e.layer == layer && e.code == code) return true;
    return false;
}

std::int64_t methodUses(const World& w, const std::string& m) {
    std::int64_t n = 0;
    for (const auto& [id, c] : w.characters()) {
        auto it = c.ai.stats.methodUse.find(m);
        if (it != c.ai.stats.methodUse.end()) n += it->second;
    }
    return n;
}

// Everyone's storage empty, the character knows no bushes.
void forgetObjects(World& w, Character& c, const std::string& type) {
    for (auto it = c.knownObjects.begin(); it != c.knownObjects.end();) {
        const WorldObject* o = w.object(*it);
        it = (o && o->type == type) ? c.knownObjects.erase(it) : std::next(it);
    }
}

} // namespace

TEST(altera_sim_ai_lives_three_days) {
    World w = aiWorld();
    // No two people ever work one reserved object at the same time.
    bool clash = false;
    for (Tick t = 0; t < 3 * kTicksPerDay; ++t) {
        w.step();
        std::map<EntityId, int> users;
        for (const auto& [id, c] : w.characters()) {
            const InteractionDef* d = w.content().interaction(c.ai.fsm.interaction);
            if (d && d->reserve && c.ai.fsm.target != kNone && c.ai.fsm.state != "Move")
                clash |= ++users[c.ai.fsm.target] > 1;
        }
    }
    CHECK(!clash);
    double hunger = 0.0;
    for (const auto& [id, c] : w.characters()) {
        CHECK(c.body.alive);
        hunger += c.need("hunger");
        CHECK(traceHas(c, 'U', "utility.choose"));
        CHECK(traceHas(c, 'H', "htn.plan"));
        CHECK(traceHas(c, 'F', "fsm.enter"));
    }
    CHECK(hunger / static_cast<double>(w.characters().size()) > 0.35);
    // The chains run: people cook (gather -> wood -> hearth) and sleep in beds.
    CHECK(methodUses(w, "stock_food/cook_and_store") > 0);
    CHECK(methodUses(w, "rest/sleep_in_bed") > 0);
    bool goap = false;
    for (const auto& [id, c] : w.characters()) goap |= traceHas(c, 'G', "goap.plan");
    CHECK(goap);
    const Json rep = aiReport(w);
    CHECK_EQ(rep["alive"].get<int>(), static_cast<int>(w.characters().size()));
}

TEST(altera_sim_ai_is_deterministic) {
    World a = aiWorld(), b = aiWorld();
    a.run(2 * kTicksPerDay);
    b.run(2 * kTicksPerDay);
    CHECK_EQ(a.stateHash(), b.stateHash());
    World c = aiWorld(99);
    c.run(2 * kTicksPerDay);
    CHECK(c.stateHash() != a.stateHash());
    // Snapshot in the middle of plans, reload, continue.
    World d(content());
    d.loadJson(a.toJson());
    installAi(d);
    a.run(kTicksPerDay);
    d.run(kTicksPerDay);
    CHECK_EQ(a.stateHash(), d.stateHash());
}

TEST(altera_sim_hungry_family_cooks) {
    World w = aiWorld();
    // Empty every storage and starve family #1: they must gather, fetch wood and cook.
    for (auto& [id, o] : w.objects())
        if (o.type == "storage") o.stock.clear();
    const EntityId fam = w.families().begin()->first;
    for (auto& [id, c] : w.characters())
        if (c.status.family == fam) c.needs["hunger"] = 0.2;
    w.run(kTicksPerDay);
    for (const auto& [id, c] : w.characters()) {
        if (c.status.family != fam) continue;
        CHECK(c.need("hunger") > 0.2);
    }
    CHECK(methodUses(w, "obtain_food/forage") + methodUses(w, "obtain_food/cook_then_eat") > 0);
}

TEST(altera_sim_why_hungry_is_reported) {
    World w = aiWorld();
    for (auto& [id, o] : w.objects())
        if (o.type == "storage") o.stock.clear();
    Character& c = w.characters().begin()->second;
    forgetObjects(w, c, "berry_bush");
    forgetObjects(w, c, "storage");
    c.needs["hunger"] = 0.05;
    w.run(kTicksPerDay / 2);
    const Json rep = aiReport(w);
    bool found = false;
    for (const auto& h : rep["summary"]["why_hunger"])
        if (h["id"].get<EntityId>() == c.id) found = true;
    CHECK(found);
    // Either no food method is feasible at all (Utility never picks eat), or
    // the tried ones failed and the report carries the reason code.
    CHECK(c.need("hunger") < 0.1);
    const bool explained = c.ai.stats.goalTicks.count("eat") == 0 || c.ai.stats.lastGoalFail.count("eat") > 0;
    CHECK(explained);
    for (const auto& h : rep["summary"]["why_hunger"])
        if (h["id"].get<EntityId>() == c.id && c.ai.stats.lastGoalFail.count("eat")) CHECK(!h["why"].empty());
}

TEST(altera_sim_law_changes_price_not_options) {
    // Two hungry neighbours, own storage empty, the other family's full.
    World w = aiWorld();
    auto famIt = w.families().begin();
    const Family& famA = famIt->second;
    const Family& famB = std::next(famIt)->second;
    w.object(famA.storage)->stock = {};
    w.object(famB.storage)->stock = {{"meal", 10}};
    Character& thief = *w.character(famA.members[0]);
    Character& honest = *w.character(famA.members[1]);
    thief.traits = {"desperate_thief"};
    honest.traits = {"honest"};
    for (Character* c : {&thief, &honest}) {
        c->needs["hunger"] = 0.1;
        c->inventory.clear();
    }
    w.run(kTicksPerDay / 3);
    CHECK(thief.ai.stats.methodUse.count("obtain_food/steal_food") > 0);
    CHECK(honest.ai.stats.methodUse.count("obtain_food/steal_food") == 0);
    bool remembers = false;
    for (const auto& m : thief.memory) remembers |= m.type == "stole_food";
    CHECK(remembers);
}
