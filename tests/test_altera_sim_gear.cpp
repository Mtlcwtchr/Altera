// What a person carries and wears (sim/gear.hpp, content/ai/gear.json):
// layers with their cost (a gambeson under armour, not two armours), grids
// of the hands and the worn bags (Diablo-like room), the load a body bears
// and how it tires.

#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/gear.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& gearContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World gearWorld() {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(gearContent(), cfg, 7);
    installAi(w);
    return w;
}

Character* adult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") return &c;
    return nullptr;
}

Command gearCmd(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    return c;
}

} // namespace

TEST(altera_sim_gear_rules_load) {
    const GearRules* R = gearRules(gearContent());
    CHECK(R != nullptr);
    if (!R) return;
    CHECK(R->slot("torso") != nullptr);
    CHECK(R->item("log").kg > 10);
    CHECK(R->item("backpack").gridW > 0);
    // every thing of items.json has a weight and a size (the expansion's fall back to the default)
    CHECK(R->stacks.size() >= 60);
    for (const auto& [id, n] : R->stacks) CHECK(R->items.count(id) == 1);
}

TEST(altera_sim_gear_layers) {
    const GearRules* R = gearRules(gearContent());
    if (!R) return;
    World w = gearWorld();
    Character* c = adult(w);
    CHECK(c != nullptr);
    if (!c) return;
    c->inventory = {{"gambeson", 1}, {"leather_armour", 1}, {"mail_shirt", 1}, {"tunic", 1}, {"cloak", 1}};
    c->worn.clear();
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "tunic"}})).result, std::string("ok"));
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "gambeson"}})).result, std::string("ok"));
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "leather_armour"}})).result, std::string("ok"));
    // a second armour: its layer is taken
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "mail_shirt"}})).result,
             std::string("rejected:layer_taken:leather_armour"));
    // a cloak over it all still fits the torso's bulk (1 + 1 + 2 + 1 = 5)
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "cloak"}})).result, std::string("ok"));
    CHECK_EQ(wornCount(*c, "leather_armour"), 1);
    CHECK_EQ(canWear(*R, *c, "tunic"), std::string("not_carried"));   // the one tunic is on
    CHECK_EQ(canWear(*R, *c, "log"), std::string("not_wearable"));
    // the armour taken off: the mail goes on
    CHECK_EQ(w.applyNow(gearCmd("take_off", {{"id", c->id}, {"item", "leather_armour"}})).result, std::string("ok"));
    CHECK_EQ(w.applyNow(gearCmd("wear", {{"id", c->id}, {"item", "mail_shirt"}})).result, std::string("ok"));
    // a worn thing used up comes off
    c->inventory.erase("cloak");
    pruneGear(*c);
    CHECK_EQ(wornCount(*c, "cloak"), 0);
}

TEST(altera_sim_gear_grid_and_load) {
    const GearRules* R = gearRules(gearContent());
    if (!R) return;
    World w = gearWorld();
    Character* c = adult(w);
    if (!c) return;
    c->worn.clear();
    c->gearPins.clear();
    c->attrs.clear();
    // AL-73: logs go on the shoulder (haul), outside the grids; three spears fill the hands (4 x 3, a spear 1 x 4)
    c->inventory = {{"log", 2}};
    GearLayout L = arrange(*R, *c);
    CHECK(L.overflow.empty());
    CHECK(L.spots.empty());
    CHECK_EQ(L.hauled["log"], 2);
    // a third log would be too heavy (45 kg)
    CHECK_EQ(canTake(*R, *c, {{"log", 1}}), std::string("too_heavy"));
    c->inventory = {{"spear", 3}};
    L = arrange(*R, *c);
    CHECK(L.overflow.empty());
    CHECK_EQ(L.spots.size(), std::size_t(3));
    CHECK_EQ(canTake(*R, *c, {{"stone", 1}}), std::string("no_room"));
    // a pack worn: room for the stones
    c->inventory["backpack"] = 1;
    c->worn["back:pack"] = "backpack";
    CHECK_EQ(canTake(*R, *c, {{"stone", 4}}), std::string(""));
    // small things stack in a cell
    c->inventory = {{"berries", 30}};
    c->worn.clear();
    L = arrange(*R, *c);
    CHECK_EQ(L.spots.size(), std::size_t(2));   // 20 + 10
    // a load past the comfortable one tires more
    CHECK(std::fabs(loadFatigueMult(*R, *c) - 1.0) < 1e-9);
    c->inventory = {{"log", 2}};
    CHECK(loadFatigueMult(*R, *c) > 1.3);
    // a strong body carries more
    c->attrs["strength"] = 0.9;
    CHECK(maxLoadKg(*R, *c) > R->maxKg);
}

TEST(altera_sim_gear_pin_and_bag_off) {
    const GearRules* R = gearRules(gearContent());
    if (!R) return;
    World w = gearWorld();
    Character* c = adult(w);
    if (!c) return;
    c->worn.clear();
    c->gearPins.clear();
    c->inventory = {{"axe", 1}, {"satchel", 1}, {"stone", 9}};
    c->worn["shoulder:bag"] = "satchel";
    // the axe put in the bottom row of the hands, turned
    CHECK_EQ(w.applyNow(gearCmd("gear_pin", {{"id", c->id}, {"item", "axe"}, {"grid", "hands"}, {"x", 0}, {"y", 2}, {"turned", true}})).result,
             std::string("ok"));
    const GearLayout L = arrange(*R, *c);
    bool there = false;
    for (const auto& s : L.spots) there |= s.item == "axe" && s.grid == "hands" && s.y == 2 && s.turned;
    CHECK(there);
    // off the grid: refused
    CHECK_EQ(w.applyNow(gearCmd("gear_pin", {{"id", c->id}, {"item", "axe"}, {"grid", "hands"}, {"x", 3}, {"y", 1}})).result,
             std::string("rejected:no_room"));
    // the satchel off would leave the firewood without room: refused
    c->inventory.erase("stone");
    c->inventory["firewood"] = 6;
    CHECK(arrange(*R, *c).overflow.empty());
    CHECK_EQ(w.applyNow(gearCmd("take_off", {{"id", c->id}, {"item", "satchel"}})).result, std::string("rejected:no_room"));
}

TEST(altera_sim_gear_player_refuses_heavy_act) {
    World w = gearWorld();
    Character* c = adult(w);
    if (!c) return;
    const InteractionDef* d = w.content().interaction("gather_stones");
    const WorldObject* bank = nullptr;
    for (const auto& [id, o] : w.objects())
        if (o.type == "riverbank") bank = &o;
    if (!d || !bank) return;
    Command take = gearCmd("take_control", {{"id", c->id}, {"player", 1}});
    w.applyNow(take);
    c->worn.clear();
    c->inventory = {{"spear", 3}}; // AL-73: logs go on the shoulder now; long things fill the hands
    std::string why;
    c->ai.goal.source = "player"; // AL-24: the check is for the player's own orders
    CHECK(!canPerform(w, *c, *d, bank, &why, false, nullptr));
    CHECK(why.rfind("gear:", 0) == 0);
    c->inventory.clear();
    why.clear();
    canPerform(w, *c, *d, bank, &why, false, nullptr);
    CHECK(why.rfind("gear:", 0) != 0);
}
