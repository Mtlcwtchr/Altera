// AL-83: the memory budget (sim/memory_budget.hpp): per-person caps shrink with
// the population, the daily trim keeps what matters, shared memory events are
// written once in a save, and fast craft does not re-bake a finished dish
// (the 5-year run grew a recipe book of 800+ ever longer chains per cook).

#include <algorithm>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/craft_sim.hpp"
#include "sim/memory_budget.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& content() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World testWorld() {
    return makeTestWorld(content(), loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
}

Character* anAdult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.ageDays >= 16 * 360 && c.status.family != kNone) return &c;
    return nullptr;
}

int inv(const Character& c, const std::string& item) {
    auto it = c.inventory.find(item);
    return it == c.inventory.end() ? 0 : it->second;
}

} // namespace

TEST(lr_memory_budget_share_shrinks_with_people) {
    MemoryBudget::Pool p{1000, 10, 100};
    CHECK_EQ(p.per(1), 100);   // the max
    CHECK_EQ(p.per(20), 50);   // total / people
    CHECK_EQ(p.per(500), 10);  // the min
    CHECK(p.per(40) <= p.per(20));
    const MemoryBudget& b = memoryBudget(content());
    CHECK(b.memories.perMax > 0 && b.recipes.perMax > 0 && b.maxRecipeSteps > 0);
}

TEST(lr_memory_budget_trim_keeps_what_matters) {
    World w = testWorld();
    Character* c = anAdult(w);
    CHECK(c != nullptr);
    if (!c) return;
    MemoryBudget b;
    b.memories = {10, 5, 5};
    b.recipes = {3, 3, 3};
    b.tried = {4, 4, 4};
    c->memory.clear();
    for (int i = 0; i < 12; ++i) {
        MemoryEntry m;
        m.tick = i;
        m.type = "good_chat";
        m.salience = i == 3 ? 0.9 : 0.1 * (i % 3);
        c->memory.push_back(m);
    }
    c->craft = {};
    const craft::CraftConfig& cfg = w.content().craft.cfg;
    for (int i = 0; i < 6; ++i) {
        craft::Recipe r;
        r.id = "r_" + std::to_string(i);
        r.mastery = 0.1 * i;
        r.successes = i == 0 ? cfg.masterSuccesses : 1;
        if (i == 0) r.mastery = 1.0; // a master: always kept
        if (i == 1) r.tags.push_back("breakthrough");
        c->craft.book.recipes.push_back(r);
    }
    for (int i = 0; i < 9; ++i) c->craft.tried["sig" + std::to_string(i)] = i;
    BudgetTrim t = trimToBudget(b, cfg, 2, *c);
    CHECK_EQ(c->memory.size(), std::size_t{5});
    CHECK_EQ(t.memories, std::size_t{7});
    CHECK(std::any_of(c->memory.begin(), c->memory.end(), [](const MemoryEntry& m) { return m.salience > 0.8; }));
    CHECK(std::is_sorted(c->memory.begin(), c->memory.end(),
                         [](const MemoryEntry& a, const MemoryEntry& b) { return a.tick < b.tick; })); // order kept
    CHECK_EQ(c->craft.book.recipes.size(), std::size_t{3});
    CHECK(c->craft.book.find("r_0") && c->craft.book.find("r_1") && c->craft.book.find("r_5"));
    CHECK_EQ(c->craft.tried.size(), std::size_t{4});
    CHECK(c->craft.tried.count("sig8") && !c->craft.tried.count("sig0"));
}

TEST(lr_memory_budget_shared_events_round_trip) {
    World w = testWorld();
    installAi(w);
    // one event seen by everyone: one row in the save, every memory back on load
    int people = 0;
    EntityId teller = kNone;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        if (teller == kNone) teller = id;
        MemoryEntry m;
        m.type = "told_stories";
        m.who = {teller};
        m.object = 42;
        m.goods = {{"meal", 1}};
        m.salience = 0.3;
        m.source = people % 2 ? "heard" : "seen";
        w.remember(c, m);
        ++people;
    }
    const Json j = w.toJson();
    CHECK(j.contains("memory_events"));
    CHECK(j["memory_events"].size() < static_cast<std::size_t>(people) + 1); // shared, not one per person
    World back = testWorld();
    back.loadJson(j);
    CHECK_EQ(back.stateHash(), w.stateHash());
    for (const auto& [id, c] : back.characters()) {
        const Character& orig = w.characters().at(id);
        CHECK_EQ(c.memory.size(), orig.memory.size());
        for (std::size_t i = 0; i < c.memory.size() && i < orig.memory.size(); ++i) {
            CHECK_EQ(c.memory[i].type, orig.memory[i].type);
            CHECK_EQ(c.memory[i].tick, orig.memory[i].tick);
            CHECK_EQ(c.memory[i].source, orig.memory[i].source);
            CHECK(c.memory[i].goods == orig.memory[i].goods && c.memory[i].who == orig.memory[i].who);
        }
    }
    CHECK(back.toJson() == j);
}

TEST(lr_memory_budget_no_rebaking_finished_dish) {
    // cooking the same dish again and again: the finished bread is never taken for
    // the dough, the made thing keeps the design's length, the book stays small
    World w = testWorld();
    Character* c = anAdult(w);
    CHECK(c != nullptr);
    if (!c) return;
    const InteractionDef& d = w.content().interactions.at("craft:nut_bread");
    WorldObject* h = w.object(w.families().at(c->status.family).hearth);
    CHECK(h != nullptr);
    if (!h) return;
    c->pos = h->pos;
    c->craft = {};
    c->skills["cooking"] = 0.95;
    int made = 0;
    for (int i = 0; i < 12; ++i) {
        for (const auto& k : d.pre)
            if (k.subject == "has") c->inventory[k.key] = std::max(inv(*c, k.key), static_cast<int>(k.value) + 1);
        const int before = inv(*c, "nut_bread");
        craftAct(w, *c, d, h, nullptr, Json::object());
        if (inv(*c, "nut_bread") > before) ++made;
        w.run(1);
    }
    CHECK(made >= 6);
    std::size_t longest = 0;
    for (const auto& it : c->craft.items) longest = std::max(longest, it.trace.size());
    for (const auto& r : c->craft.book.recipes) longest = std::max(longest, r.steps.size());
    CHECK(longest <= 6);
    CHECK(c->craft.book.recipes.size() <= 3);
}
