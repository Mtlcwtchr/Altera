// AL-87: worth as a value component. Any def (material, shape, process, form,
// flaw) and any tag may carry "worth"; a made thing's worth is the product of
// the components it really holds and multiplies its trade value. Skill (q) is
// separate: a crude gold comb is still worth more than a fine wooden one.

#include <cmath>
#include <string>
#include <vector>

#include <iostream>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/content.hpp"
#include "sim/craft/craft.hpp"
#include "sim/craft/craft_chain.hpp"
#include "sim/trade.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
using namespace lr::sim::craft;

namespace {

const Content& worthContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

Actor worthActor(double skill) {
    Actor a;
    a.id = 11;
    for (const char* s : {"carpentry", "toolmaking", "smithing", "cooking"}) a.skills[s] = skill;
    for (const auto& [id, m] : worthContent().craft.materials) a.knowsMaterial[id] = skill;
    return a;
}

Workplace worthShop() {
    Workplace wp;
    for (const auto& [id, s] : worthContent().craft.stations) wp.stations.push_back(id);
    Tool t;
    t.tags = {"cutting", "saw", "striking", "whetstone", "grit"};
    t.hardness = 0.85;
    t.q = 0.8;
    wp.tools.push_back(t);
    return wp;
}

// The starter design of `form` with every mat:<from> swapped for mat:<to>.
Item makeFrom(const std::string& form, const std::string& from, const std::string& to, double skill) {
    const CraftData& d = worthContent().craft;
    const DesignDef* ds = d.design(form);
    CHECK(ds != nullptr);
    if (!ds) return {};
    Recipe r = recipeFromDesign(d, *ds, 1.0, true);
    for (auto& st : r.steps)
        for (auto& [slot, ins] : st.step.slots)
            for (auto& in : ins)
                if (in.ref == "mat:" + from) in.ref = "mat:" + to;
    std::vector<Item> pool;
    ChainOpts o;
    o.rawFromNothing = true;
    o.rawQ = 0.6;
    o.stopOnRisk = false;
    ChainRun run = runChain(d, r.steps, pool, worthActor(skill), worthShop(), 5, 1000, o);
    if (!run.done) std::cerr << "    " << to << ": stop " << run.stop << " " << run.stopArg << " at " << run.stopStep << "\n";
    CHECK(run.done);
    return run.out;
}

// A metal comb: lump -> bar -> plate -> comb (hammered and cut from sheet).
Item hammeredComb(const std::string& metal, double skill) {
    const CraftData& d = worthContent().craft;
    std::vector<RecipeStep> steps;
    auto add = [&](const std::string& shape, const std::string& in) {
        RecipeStep r;
        r.known = true;
        r.step.process = "hammer";
        r.step.shape = shape;
        r.step.slots["work"].push_back({in, 1});
        steps.push_back(r);
    };
    add("bar", "mat:" + metal);
    add("plate", "step:1");
    add("comb_raw", "step:2");
    std::vector<Item> pool;
    ChainOpts o;
    o.rawFromNothing = true;
    o.rawQ = 0.6;
    o.stopOnRisk = false;
    ChainRun run = runChain(d, steps, pool, worthActor(skill), worthShop(), 5, 1000, o);
    if (!run.done) std::cerr << "    " << metal << ": stop " << run.stop << " " << run.stopArg << " at " << run.stopStep << "\n";
    CHECK(run.done);
    return run.out;
}

} // namespace

TEST(lr_craft_worth_from_what_it_is_made_of) {
    const CraftData& d = worthContent().craft;
    CHECK(d.material("gold") && d.material("silver") && d.material("horn") && d.material("antler"));
    const Item bone = makeFrom("bone_comb", "bone", "bone", 0.6);
    const Item horn = makeFrom("bone_comb", "bone", "horn", 0.6);
    const Item gold = hammeredComb("gold", 0.6);
    CHECK_EQ(bone.form, std::string("bone_comb"));
    CHECK_EQ(gold.form, std::string("bone_comb")); // the archetype is the comb, whatever it is made of
    const double wb = itemWorth(d, bone), wh = itemWorth(d, horn), wg = itemWorth(d, gold);
    CHECK(wh > wb);
    CHECK(wg > wh * 4);
    // skill is a separate axis: a crude gold comb still outweighs a masterly bone one
    const Item crudeGold = hammeredComb("gold", 0.15);
    const Item fineBone = makeFrom("bone_comb", "bone", "bone", 0.95);
    CHECK(itemWorth(d, crudeGold) > itemWorth(d, fineBone));
}

TEST(lr_craft_worth_components_flaws_tags_parts) {
    const CraftData& d = worthContent().craft;
    Item comb = makeFrom("bone_comb", "bone", "horn", 0.6);
    const double base = itemWorth(d, comb);
    // a flaw is a component too, and it lowers the worth
    Item cracked = comb;
    cracked.flaws.push_back({"crack", true});
    CHECK(itemWorth(d, cracked) < base);
    // a tag carries worth wherever it shows (crafting_config worth.tags)
    Item engraved = comb;
    engraved.tags.insert("engraved");
    CHECK(itemWorth(d, engraved) > base * 1.2);
    // an assembly: a silver fitting on a wooden handle lifts the whole beyond the mass average
    Item handle;
    handle.material = "oak_wood";
    handle.shape = "handle";
    handle.len = 2;
    Item fitting;
    fitting.material = "silver";
    fitting.shape = "lump";
    Item whole;
    whole.parts.push_back({"handle", {handle}});
    whole.parts.push_back({"binder", {fitting}});
    Item plain = whole;
    plain.parts[1].item[0].material = "oak_wood";
    CHECK(itemWorth(d, whole) > itemWorth(d, plain) * 1.5);
    // a made thing with no worth anywhere is ordinary
    Item bare;
    CHECK(std::fabs(itemWorth(d, bare) - 1.0) < 1e-9);
}

TEST(lr_sim_trade_worth_follows_material) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(worthContent(), cfg, 7);
    installAi(w);
    std::vector<Character*> people;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && people.size() < 2) people.push_back(&c);
    CHECK(people.size() == 2);
    if (people.size() < 2) return;
    Character& judge = *people[0];
    Character& holder = *people[1];
    holder.inventory["bone_comb"] = 1;
    holder.craft.items.push_back(makeFrom("bone_comb", "bone", "bone", 0.6));
    const double boneV = worthTo(w, judge, "bone_comb", holder);
    holder.craft.items.clear();
    holder.craft.items.push_back(hammeredComb("gold", 0.6));
    const double goldV = worthTo(w, judge, "bone_comb", holder);
    CHECK(goldV > boneV * 4);
    // plain items take worth from their tags: silver is precious
    CHECK(itemWorthMult(worthContent(), holder, "silver") > 1.0);
}
