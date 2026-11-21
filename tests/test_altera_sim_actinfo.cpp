// What an act asks and gives (sim/act_info.hpp): the game's act ring shows
// each precondition met or not and what the person gets. Chopping a tree
// wants an axe and the craft; with both it can be done and gives logs.

#include <algorithm>

#include "framework.hpp"
#include "sim/act_info.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld() {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 7);
    installAi(w);
    return w;
}

} // namespace

TEST(altera_sim_actinfo_object_acts) {
    const auto tree = actsOnObject(realContent(), "tree");
    CHECK(std::find(tree.begin(), tree.end(), "chop_tree") != tree.end());
    const auto wood = actsOnObject(realContent(), "deadwood");
    CHECK(std::find(wood.begin(), wood.end(), "gather_firewood") != wood.end());
    CHECK(std::find(wood.begin(), wood.end(), "chop_tree") == wood.end());
}

TEST(altera_sim_actinfo_chop_tree) {
    World w = realWorld();
    Character* c = nullptr;
    for (auto& [id, ch] : w.characters())
        if (!ch.isAnimal() && ch.body.alive && ch.body.stage() == "adult") {
            c = &ch;
            break;
        }
    const WorldObject* tree = nullptr;
    for (const auto& [id, o] : w.objects())
        if (o.type == "tree" && o.stock.count("log") && o.stock.at("log") >= 2) {
            tree = &o;
            break;
        }
    CHECK(c && tree);
    if (!c || !tree) return;
    const InteractionDef* d = w.content().interaction("chop_tree");
    CHECK(d != nullptr);
    if (!d) return;
    auto reqOk = [](const ActInfo& a, const std::string& subject, const std::string& key) {
        for (const auto& r : a.pre)
            if (r.cond.subject == subject && r.cond.key == key) return r.ok ? 1 : 0;
        return -1;
    };
    c->inventory.erase("axe");
    ActInfo a = actInfo(w, *c, *d, tree, nullptr);
    CHECK(!a.can);
    CHECK_EQ(reqOk(a, "has", "axe"), 0);
    CHECK_EQ(reqOk(a, "target_stock", "log"), 1);
    CHECK(a.duration == d->duration);
    bool logs = false;
    for (const auto& e : a.gains) logs |= e.subject == "has" && e.key == "log" && e.op == '+';
    CHECK(logs);
    // with an axe and the craft it can be done
    c->inventory["axe"] = 1;
    c->knowledge.insert("woodcutting");
    a = actInfo(w, *c, *d, tree, nullptr);
    CHECK_EQ(reqOk(a, "has", "axe"), 1);
    CHECK_EQ(reqOk(a, "knows", "woodcutting"), 1);
}
