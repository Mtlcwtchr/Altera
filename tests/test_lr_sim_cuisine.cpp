// AL-74: the cuisine pack (content/ai/cuisine + the new forms and designs of
// content/ai/craft): every dish, drink and household thing is an item with
// category tags and a worth, the household cooks it at its own hearth through
// the generated craft:<design> acts, the AI picks it by HTN, and it is eaten
// or used.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/craft_sim.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& cuisine() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

// dishes cooked at the hearth (generated acts) and drinks
const std::vector<std::string> kDishes = {
    "honey_porridge", "milk_porridge", "mushroom_stew", "fish_soup", "garden_soup", "meat_pie", "fish_pie",
    "berry_tart", "apple_pie", "honey_cake", "nut_bread", "cheese_pie", "wake_loaf", "blood_sausage",
    "herb_roast", "roast_fish", "baked_apples", "baked_roots", "eggs_greens", "sweet_curd"};
const std::vector<std::string> kDrinks = {"herb_tea", "honey_milk", "berry_wine", "mulled_wine"};
// design -> item of the household things a homestead can make (wood, bone, clay)
const std::vector<std::pair<std::string, std::string>> kHomeThings = {
    {"spoon", "wooden_spoon"},  {"ladle", "ladle"},           {"trencher", "trencher"},
    {"wooden_cup", "wooden_cup"}, {"tankard", "tankard"},     {"spindle", "spindle"},
    {"spade", "wooden_spade"},  {"pitchfork", "pitchfork"},   {"carved_figure", "carved_toy"},
    {"broom", "broom"},         {"fishing_rod", "fishing_line"}, {"drum", "drum"},
    {"bone_flute", "bone_flute"}, {"bone_comb", "bone_comb"}, {"bone_dice", "bone_dice"},
    {"bone_charm", "bone_charm"}, {"clay_jug", "clay_jug"},   {"clay_candlestick", "clay_candlestick"},
    {"clay_pipe", "clay_pipe"}, {"clay_whistle", "clay_whistle"}, {"bowl", "bowl"}, {"clay_pot", "clay_pot"}};

// why a design does not run here: what its chain misses
void giveInputs(Character& c, const InteractionDef& d);
std::string whyNot(World& w, Character& c, const std::string& design) {
    giveInputs(c, w.content().interactions.at("craft:" + design));
    craft::ChainPlan p = designPlan(w, c, design);
    std::string s = p.ok ? "plan ok" : "plan:";
    for (const auto& m : p.missing) s += " missing " + m;
    for (const auto& t : p.needTools) s += " tool " + t;
    for (const auto& t : p.needStations) s += " station " + t;
    const std::string rid = giveDesign(w, c, design, 1.0);
    if (craft::Recipe* r = c.craft.book.find(rid)) {
        r->successes = 99;
        for (const auto& st : r->steps) s += " {" + st.step.process + " -> " + st.outForm + "/" + st.outShape + "/" + st.outMat + "}";
    }
    for (const auto& [it, n] : c.inventory) {
        auto def = w.content().items.find(it);
        if (def == w.content().items.end() || def->second.material.empty()) continue;
        craft::Item raw = craft::rawItem(w.content().craft, def->second.material, 0.5);
        craft::recognize(w.content().craft, raw);
        s += " inv:" + it + "=" + raw.form;
    }
    CraftOutcome o = fastCraft(w, c, rid, 1);
    s += "; run: " + o.reason + " " + o.reasonArg;
    for (const auto& r : o.chain.results) s += std::string(" [") + craft::outcomeName(r.outcome) + "]";
    if (o.chain.done) s += " out " + o.chain.out.form + " shape " + o.chain.out.shape;
    return s;
}

bool hasTag(const ItemDef& d, const char* t) { return std::find(d.tags.begin(), d.tags.end(), t) != d.tags.end(); }

int inv(const Character& c, const std::string& item) {
    auto it = c.inventory.find(item);
    return it == c.inventory.end() ? 0 : it->second;
}

Character* aCook(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.ageDays >= 16 * 360 && c.status.family != kNone) return &c;
    return nullptr;
}

// the person stands at the own hearth with a master's hands and nothing else on them
WorldObject* toHearth(World& w, Character& c) {
    WorldObject* h = w.object(w.families().at(c.status.family).hearth);
    if (!h) return nullptr;
    c.pos = h->pos;
    c.inventory.clear();
    c.craft = {};
    for (const char* s : {"cooking", "brewing", "carpentry", "pottery", "spinning", "sewing", "fiber_working", "toolmaking"})
        c.skills[s] = 0.95;
    return h;
}

// the inputs of a generated act (its has:<item>>=n preconditions) into the hands
void giveInputs(Character& c, const InteractionDef& d) {
    for (const auto& k : d.pre)
        if (k.subject == "has") c.inventory[k.key] = std::max(inv(c, k.key), static_cast<int>(k.value));
}

} // namespace

TEST(lr_cuisine_items_tagged_and_priced) {
    const Content& ct = cuisine();
    for (const auto& id : kDishes) {
        auto it = ct.items.find(id);
        CHECK(it != ct.items.end());
        if (it == ct.items.end()) continue;
        CHECK(hasTag(it->second, "food") && hasTag(it->second, "dish"));
        CHECK(it->second.nutrition > 0.0 && it->second.value > 0.0 && it->second.quality > 0.0);
        CHECK(it->second.spoilDays > 0.0);
        CHECK(ct.interactions.count("eat_" + id) && ct.interactions.count("store_" + id) && ct.interactions.count("take_" + id));
    }
    for (const auto& id : kDrinks) {
        auto it = ct.items.find(id);
        CHECK(it != ct.items.end());
        if (it == ct.items.end()) continue;
        CHECK(hasTag(it->second, "drink") && it->second.value > 0.0);
        CHECK(ct.interactions.count("drink_" + id));
    }
    for (const auto& [design, item] : kHomeThings) {
        auto it = ct.items.find(item);
        CHECK(it != ct.items.end());
        if (it != ct.items.end()) CHECK(it->second.value > 0.0 && !it->second.tags.empty());
    }
    // the base and expansion items carry the category tags too (annotated by the pack)
    CHECK(hasTag(ct.items.at("bread"), "baked") && ct.items.at("bread").value > 0.0);
    CHECK(hasTag(ct.items.at("mead"), "alcohol") && hasTag(ct.items.at("mead"), "luxury"));
    CHECK(hasTag(ct.items.at("meat"), "meat") && hasTag(ct.items.at("fish"), "fish"));
    CHECK(ct.items.at("carved_toy").value > 0.0);
    // sources of the new raw inputs: water at the well, leaven at the hearth, bones from butchering
    CHECK(ct.interactions.count("fetch_water") && ct.interactions.count("set_leaven"));
    bool bones = false;
    for (const auto& e : ct.interactions.at("butcher_carcass").eff) bones = bones || e.key == "bone";
    CHECK(bones);
    // nothing of the pack was silently dropped for a clash with the base or the expansion
    for (const auto& k : ct.expansion.kept) CHECK(k.rfind("cuisine:", 0) != 0 || k == "cuisine:item:fish");
}

TEST(lr_cuisine_dishes_are_hearth_acts) {
    // every dish and the hot drinks are made at the own hearth (one place, the house's
    // table a step away), by whoever knows cooking; berry wine ferments a day, so it is a brew act
    const Content& ct = cuisine();
    std::vector<std::string> acts = kDishes;
    acts.insert(acts.end(), {"honey_milk", "mulled_wine", "herbal_tea", "stew", "cheese"});
    for (const auto& id : acts) {
        auto it = ct.interactions.find("craft:" + id);
        CHECK(it != ct.interactions.end());
        if (it == ct.interactions.end()) {
            std::cerr << "    no act craft:" << id << "\n";
            continue;
        }
        CHECK_EQ(it->second.target, std::string("object:hearth"));
        bool own = false, cooking = false;
        for (const auto& k : it->second.pre) {
            own = own || (k.subject == "target_owner" && k.key == "family");
            cooking = cooking || (k.subject == "knows" && k.key == "cooking");
        }
        CHECK(own && cooking);
    }
    CHECK(!ct.interactions.count("craft:berry_wine"));
    CHECK(ct.interactions.count("brew_berry_wine"));
    for (const auto& [design, item] : kHomeThings) {
        auto it = ct.interactions.find("craft:" + design);
        CHECK(it != ct.interactions.end());
        if (it == ct.interactions.end()) std::cerr << "    no act craft:" << design << "\n";
    }
    // the metal ones need a forge: made in the smithy (AL-88), never at the homestead
    for (const char* id : {"cauldron", "sickle", "hoe"}) {
        auto it = ct.interactions.find(std::string("craft:") + id);
        CHECK(it != ct.interactions.end());
        if (it != ct.interactions.end()) CHECK(it->second.targetType != "homestead");
    }
}

TEST(lr_cuisine_cook_and_eat_every_dish) {
    // the generated act, run the way the FSM runs it, makes the dish from the inputs it
    // asks for; eating it feeds and empties the hands
    const Content& ct = cuisine();
    World w = makeTestWorld(ct, loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    Character* c = aCook(w);
    CHECK(c != nullptr);
    if (!c) return;
    std::vector<std::string> acts = kDishes;
    acts.insert(acts.end(), {"honey_milk", "mulled_wine", "herbal_tea"});
    int made = 0;
    for (const auto& id : acts) {
        const InteractionDef* d = &ct.interactions.at("craft:" + id);
        WorldObject* hearth = toHearth(w, *c);
        CHECK(hearth != nullptr);
        if (!hearth) return;
        const std::string item = id == "herbal_tea" ? "herb_tea" : id;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            giveInputs(*c, *d);
            craftAct(w, *c, *d, hearth, nullptr, Json::object());
            ok = inv(*c, item) >= 1;
            w.run(1);
        }
        if (!ok) {
            std::cerr << "    craft:" << id << " made nothing (" << whyNot(w, *c, id) << ")\n";
            continue;
        }
        ++made;
        const std::string eat = (ct.interactions.count("eat_" + item) ? "eat_" : "drink_") + item;
        c->needs["hunger"] = 0.2;
        c->needs["thirst"] = 0.2;
        const int before = inv(*c, item);
        applyEffects(w, *c, ct.interactions.at(eat), nullptr, nullptr);
        CHECK_EQ(inv(*c, item), before - 1);
        CHECK(c->need("hunger") > 0.2 || c->need("thirst") > 0.2);
    }
    CHECK_EQ(made, static_cast<int>(acts.size()));
}

TEST(lr_cuisine_things_made_and_used) {
    const Content& ct = cuisine();
    World w = makeTestWorld(ct, loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    Character* c = aCook(w);
    CHECK(c != nullptr);
    if (!c) return;
    int made = 0;
    for (const auto& [design, item] : kHomeThings) {
        const InteractionDef* d = &ct.interactions.at("craft:" + design);
        WorldObject* hearth = toHearth(w, *c);
        if (!hearth) return;
        WorldObject* place = hearth;
        if (d->targetType != "hearth")
            for (EntityId id : w.objectsOfType(d->targetType))
                if (WorldObject* o = w.object(id)) {
                    place = o;
                    c->pos = o->pos;
                    break;
                }
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            giveInputs(*c, *d);
            craftAct(w, *c, *d, place, nullptr, Json::object());
            ok = inv(*c, item) >= 1;
            w.run(1);
        }
        if (ok) ++made;
        else std::cerr << "    craft:" << design << " made nothing (" << whyNot(w, *c, design) << ")\n";
    }
    CHECK_EQ(made, static_cast<int>(kHomeThings.size()));
    // in use: a flute is played, a comb grooms, a pipe smokes dried herbs
    c->inventory = {{"bone_flute", 1}, {"bone_comb", 1}, {"clay_pipe", 1}, {"dried_herbs", 1}};
    c->needs["fun"] = 0.2;
    c->needs["hygiene"] = 0.2;
    applyEffects(w, *c, ct.interactions.at("play_flute"), nullptr, nullptr);
    CHECK(c->need("fun") > 0.2);
    applyEffects(w, *c, ct.interactions.at("comb_hair"), nullptr, nullptr);
    CHECK(c->need("hygiene") > 0.2);
    applyEffects(w, *c, ct.interactions.at("smoke_pipe"), nullptr, nullptr);
    CHECK_EQ(inv(*c, "dried_herbs"), 0);
    CHECK_EQ(inv(*c, "clay_pipe"), 1); // the pipe stays
}

TEST(lr_cuisine_ai_cooks_and_eats) {
    // a hungry cook with meat in hand and grain in the store makes blood sausage by
    // HTN (the cheapest way the larder allows), and the AI then goes through with it
    const Content& ct = cuisine();
    World w = makeTestWorld(ct, loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
    Character* c = aCook(w);
    CHECK(c != nullptr);
    if (!c) return;
    WorldObject* store = w.object(c->home);
    const EntityId hearth = w.families().at(c->status.family).hearth;
    CHECK(store != nullptr && hearth != kNone);
    if (!store || hearth == kNone) return;
    for (auto& [k, n] : store->stock)
        if (ct.items.count(k) && ct.items.at(k).nutrition > 0.0) n = 0;
    store->stock["grain"] = 2;
    store->stock["firewood"] = 6;
    c->inventory.clear();
    c->inventory["meat"] = 1;
    c->traits.clear();
    c->skills["cooking"] = 0.9;
    c->knownObjects.insert(hearth);
    c->knownObjects.insert(c->home);
    for (EntityId wl : w.objectsOfType("well")) c->knownObjects.erase(wl);
    c->needs["hunger"] = 0.25;
    Plan plan;
    std::string why;
    Json log = Json::array();
    CHECK(decompose(w, *c, "obtain_food", plan, why, log));
    const bool sausage = std::find(plan.methods.begin(), plan.methods.end(), "obtain_food/cook_blood_sausage") != plan.methods.end() ||
                         std::find(plan.methods.begin(), plan.methods.end(), "cook_blood_sausage") != plan.methods.end();
    if (!sausage)
        for (const auto& m : plan.methods) std::cerr << "    chose " << m << "\n";
    CHECK(sausage);
    // the AI runs it: the sausage is made at the hearth and eaten
    installAi(w);
    bool madeIt = false, ateIt = false;
    for (int t = 0; t < 200 && !ateIt; ++t) {
        w.run(1);
        if (inv(*c, "blood_sausage") > 0) madeIt = true;
        if (madeIt && inv(*c, "blood_sausage") == 0) ateIt = true;
    }
    CHECK(madeIt);
    CHECK(ateIt);
    CHECK_EQ(inv(*c, "meat"), 0);
}
