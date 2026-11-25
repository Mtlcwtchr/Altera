// Backstories (content/ai/backstories.json): they load and validate, the
// start command reshapes the person and gives the player control, the same
// seed gives the same person.

#include <algorithm>
#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/backstory.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(real(), cfg, seed);
    installAi(w);
    return w;
}

Command start(const std::string& id) {
    Command c;
    c.source = "player:1";
    c.type = "start_backstory";
    c.args = {{"backstory", id}, {"player", 1}};
    return c;
}

} // namespace

TEST(lr_sim_backstory_content) {
    const Content& c = real();
    CHECK(c.backstories.size() >= 8);
    for (const char* id : {"peasant", "smith_apprentice", "hunter", "ex_soldier", "novice", "trader", "vagrant", "herbalist"})
        CHECK(c.backstory(id) != nullptr);
    CHECK(c.items.count("coin"));
    // a broken reference is an error of the validator
    Content bad = c;
    bad.backstories[0].skills.emplace_back("no_such_skill", 0.5);
    bad.backstories[0].start = "the_moon";
    ValidationReport r;
    validateBackstories(bad, r);
    CHECK(r.errors.size() >= 2);
    ValidationReport ok;
    validateBackstories(c, ok);
    CHECK(ok.errors.empty());
}

TEST(lr_sim_backstory_start) {
    for (const BackstoryDef& b : real().backstories) {
        World w = realWorld();
        const EntityId who = backstoryCandidate(w);
        CHECK(who != kNone);
        const Command done = w.applyNow(start(b.id));
        CHECK(done.result.rfind("rejected", 0) != 0);
        const Character& c = *w.character(who);
        CHECK(c.controller.isPlayer());
        CHECK_EQ(c.backstory, b.id);
        for (const auto& [k, lvl] : b.skills) {
            CHECK(c.knowledge.count(k));
            CHECK(std::fabs(c.skills.at(k) - lvl) <= 0.05);
        }
        for (const auto& t : b.traitsAlways) CHECK(c.hasTrait(t));
        CHECK_EQ(c.traits.size(), b.traitsAlways.size() + std::size_t(b.traitPick));
        CHECK_EQ(c.attrs.size(), std::size_t(16)); // LR-141
        for (const auto& [it, n] : b.items) CHECK(c.has(it) >= n);
        CHECK_EQ(c.has("coin"), b.coins);
        CHECK_EQ(c.craft.items.size(), b.made.size());
        for (const auto& x : b.rights) CHECK(c.status.rights.count(x));
        // the world goes on with them in it
        w.run(kTicksPerDay / 4);
        CHECK(w.character(who)->body.alive);
    }
}

TEST(lr_sim_backstory_deterministic) {
    World a = realWorld(11), b = realWorld(11);
    a.applyNow(start("hunter"));
    b.applyNow(start("hunter"));
    for (const auto& [id, c] : a.characters())
        if (c.controller.isPlayer()) {
            const Character& d = *b.character(id);
            CHECK(d.controller.isPlayer());
            CHECK(c.traits == d.traits);
            CHECK(c.attrs == d.attrs);
            CHECK(c.skills == d.skills);
            CHECK(c.pos.x == d.pos.x && c.pos.y == d.pos.y);
        }
    // an unknown backstory is refused
    World u = realWorld();
    CHECK_EQ(u.applyNow(start("dragon")).result, std::string("rejected:unknown_backstory"));
}
