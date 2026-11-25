// AL-117 (LivelyRealms §7-§8): aspects in materials, alchemy processes (dissolve,
// distil, calcine), potions and their instability, runes (inscribe, bind), charge,
// artifacts (deeds -> resonance -> worth), and the sim side (drinking, commands).

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/alchemy.hpp"
#include "sim/content.hpp"
#include "sim/craft/craft_chain.hpp"
#include "sim/craft/craft_magic.hpp"
#include "sim/craft/craft_state.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
using namespace lr::sim::craft;

namespace {

const Content& alcContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}
const CraftData& D() { return alcContent().craft; }

Workplace shop() {
    Workplace wp;
    for (const auto& [id, s] : D().stations) wp.stations.push_back(id);
    Tool t;
    t.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
    t.hardness = 0.85;
    t.q = 0.7;
    wp.tools.push_back(t);
    return wp;
}

Actor adept(double skill = 0.8) {
    Actor a;
    a.id = 11;
    for (const char* s : {"cooking", "smithing", "carpentry", "tanning", "pottery", "brewing", "herbalism", "alchemy",
                          "runecraft", "toolmaking", "building", "firemaking"})
        a.skills[s] = skill;
    for (const auto& [id, m] : D().materials) a.knowsMaterial[id] = skill;
    return a;
}

ChainRun runDesign(const std::string& id, std::uint64_t seed, double skill = 0.8) {
    const DesignDef* ds = D().design(id);
    CHECK(ds != nullptr);
    std::vector<Item> pool;
    ChainOpts o;
    o.rawFromNothing = true;
    o.rawQ = 0.6;
    o.stopOnRisk = false;
    return runChain(D(), recipeFromDesign(D(), *ds, 1.0, true).steps, pool, adept(skill), shop(), seed, 1000, o);
}

// the first good run of a design over a few seeds (a step may fail by chance)
ChainRun goodRun(const std::string& id, const std::string& form) {
    for (std::uint64_t s = 1; s < 40; ++s) {
        ChainRun r = runDesign(id, s);
        if (r.done && r.out.form == form) return r;
    }
    return ChainRun{};
}

// a step of one process over the given inputs, retried over seeds until it gives a thing
StepResult step(const std::string& proc, std::map<std::string, std::vector<Item>> slots, const std::string& mode = "",
                const std::string& shape = "") {
    StepInput in{proc, std::move(slots), {shape, 0, mode, -1}};
    StepResult r;
    for (std::uint64_t s = 1; s < 60; ++s) {
        r = runStep(D(), in, adept(), shop(), 9, s, 0);
        if (r.hasOut) return r;
        if (!r.reason.empty()) return r;
    }
    return r;
}

Item silverPlate() {
    Item it = rawItem(D(), "silver", 0.7);
    recognize(D(), it);
    return it;
}

} // namespace

TEST(lr_alchemy_aspects_and_carriers_load) {
    const CraftData& d = D();
    CHECK(!d.aspects.empty());
    const AspectInfo* fire = d.aspect("fire");
    CHECK(fire && std::find(fire->opposes.begin(), fire->opposes.end(), "water") != fire->opposes.end());
    CHECK(fire && !fire->body.empty());
    std::string a;
    char v = '+';
    CHECK(parseAspectEffect("aspect.fire.minus", a, v) && a == "fire" && v == '-');
    CHECK(parseAspectEffect("aspect.life", a, v) && a == "life" && v == '+');
    CHECK(!parseAspectEffect("pain_relief", a, v));
    // silver conducts and holds; iron hardly; yarrow carries life
    CHECK(d.material("silver")->p.at("conductivity") > 0.7);
    CHECK(d.material("wrought_iron")->p.at("conductivity") < d.magic.runeMinConductivity);
    CHECK(aspectTotal(rawItem(d, "yarrow")) > 0.1);
    // every aspect a material names is an aspect of content/magic
    for (const auto& [id, m] : d.materials)
        for (const auto& e : m.effects)
            if (parseAspectEffect(e.id, a, v)) CHECK(d.aspect(a) != nullptr);
}

TEST(lr_alchemy_potions_concentrate) {
    const CraftData& d = D();
    Item yarrow = rawItem(d, "yarrow", 0.6);
    StepResult dis = step("dissolve", {{"work", {yarrow}}, {"liquid", {rawItem(d, "water")}}}, "", "liquid");
    CHECK(dis.hasOut);
    CHECK(dis.out.shape == "liquid");
    const double afterDissolve = dis.out.effects["aspect.life"];
    CHECK(afterDissolve > yarrow.effects["aspect.life"]);
    StepResult dst = step("distill", {{"work", {dis.out}}});
    CHECK(dst.hasOut);
    CHECK(dst.out.effects["aspect.life"] > afterDissolve);
    CHECK(dst.out.form == "healing_potion");
    // the starter designs make what they promise
    for (const auto& [id, form] : std::vector<std::pair<std::string, std::string>>{
             {"healing_potion", "healing_potion"}, {"sleeping_draught", "sleeping_draught"},
             {"fire_draught", "fire_draught"}, {"crucible", "crucible"}, {"alembic", "alembic"}}) {
        ChainRun r = goodRun(id, form);
        if (!r.done) std::cerr << "    " << id << ": not made\n";
        CHECK(r.done && r.out.form == form);
    }
    // distilled ale is spirit (a refine of alcohol)
    CHECK(d.material("alcohol")->refine.size() >= 1);
}

TEST(lr_alchemy_opposed_aspects_are_unstable) {
    const CraftData& d = D();
    Item calm = rawItem(d, "yarrow");
    CHECK(instability(d, calm) == 0.0);
    Item mixed;
    mixed.shape = "liquid";
    mixed.effects = {{"aspect.fire", 0.5}, {"aspect.water", 0.4}};
    CHECK(instability(d, mixed) > 0.6);
    Item along; // fire+ with water- (drying) goes along
    along.effects = {{"aspect.fire", 0.5}, {"aspect.water.minus", 0.4}};
    CHECK(instability(d, along) == 0.0);
    Item self; // fire+ against fire-
    self.effects = {{"aspect.fire", 0.5}, {"aspect.fire.minus", 0.5}};
    CHECK(instability(d, self) > 0.9);
    // distilling a strongly unstable brew blows up now and then
    Item brew = rawItem(d, "water");
    brew.effects = {{"aspect.fire", 0.6}, {"aspect.water", 0.6}};
    int blasts = 0;
    for (std::uint64_t s = 1; s <= 40; ++s) {
        StepInput in{"distill", {{"work", {brew}}}, {"", 0, "", -1}};
        StepResult r = runStep(d, in, adept(0.5), shop(), 3, s, 0);
        for (const auto& k : r.risks)
            if (k == "blast") ++blasts;
    }
    CHECK(blasts > 0);
    // the volatile flask design gives a flask now and then (its own blasts stop it otherwise)
    ChainRun f = goodRun("volatile_flask", "volatile_flask");
    CHECK(f.done && f.out.form == "volatile_flask");
}

TEST(lr_alchemy_runes_inscribe_bind_charge) {
    const CraftData& d = D();
    // iron does not take a rune
    Item iron = rawItem(d, "wrought_iron");
    StepResult no = step("inscribe", {{"work", {iron}}});
    CHECK(!no.hasOut && no.reason == "carrier_cannot_hold");
    // silver does
    Item silver = silverPlate();
    const double plainWorth = itemWorth(d, silver);
    StepResult ins = step("inscribe", {{"work", {silver}}});
    CHECK(ins.hasOut && ins.out.inscribed);
    CHECK(ins.out.rune.capacity > 0.5);
    // bind without aspects is refused; with a healing potion it takes life
    StepResult bare = step("bind", {{"work", {ins.out}}, {"reagent", {rawItem(d, "sand")}}});
    CHECK(!bare.hasOut && bare.reason == "no_aspect");
    ChainRun pot = goodRun("healing_potion", "healing_potion");
    CHECK(pot.done);
    StepResult b = step("bind", {{"work", {ins.out}}, {"reagent", {pot.out}}}, "on_touch");
    CHECK(b.hasOut);
    const Item& charm = b.out;
    CHECK(charm.rune.bound());
    CHECK(charm.rune.aspects.count("life") == 1);
    CHECK(charm.rune.trigger == "on_touch");
    CHECK(charm.form == silver.form); // the rune does not change what the thing is
    CHECK(std::find(charm.derivedTags.begin(), charm.derivedTags.end(), "enchanted") != charm.derivedTags.end());
    CHECK(charm.derived.count("rune.life") == 1);
    CHECK(itemWorth(d, charm) > plainWorth);
    // power is bounded by the carrier
    CHECK(runePower(d, charm) <= charm.rune.capacity * d.magic.bindCapShare + 1e-9);
    // charge: four uses, then rest
    Item c = charm;
    int uses = 0;
    while (!spendRune(d, c).empty() && uses < 10) ++uses;
    CHECK(uses == static_cast<int>(std::round(1.0 / d.magic.chargePerUse)));
    rechargeRune(c, c.rune.capacity);
    CHECK(!spendRune(d, c).empty());
    // saved and loaded intact
    Json j = charm;
    Item back = j.get<Item>();
    CHECK(back.inscribed && back.rune.aspects == charm.rune.aspects && back.rune.trigger == "on_touch");
    // a scroll: parchment, rune, a potion bound in
    ChainRun sc = goodRun("scroll", "scroll");
    CHECK(sc.done && sc.out.form == "scroll" && sc.out.rune.bound());
}

TEST(lr_alchemy_artifact_resonance) {
    const CraftData& d = D();
    Item blade = silverPlate();
    const double w0 = itemWorth(d, blade);
    double r = 0.0;
    for (int i = 0; i < 12; ++i) r = noteDeed(d, blade, "slew");
    CHECK(std::fabs(r - (1.0 - std::exp(-1.0))) < 1e-9);
    CHECK(blade.history.size() == 12);
    CHECK(itemWorth(d, blade) > w0 * 2.0);
    Json j = blade;
    CHECK(j.get<Item>().deeds == 12);
}

TEST(lr_alchemy_drinking_in_the_sim) {
    const Content& ct = alcContent();
    Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(ct, cfg);
    installAi(w);
    Character* c = nullptr;
    for (auto& [id, x] : w.characters())
        if (!x.isAnimal() && x.body.alive && x.body.ageDays >= 16 * 360) {
            c = &x;
            break;
        }
    CHECK(c != nullptr);
    if (!c) return;
    ChainRun pot = goodRun("healing_potion", "healing_potion");
    CHECK(pot.done);
    c->body.health = 0.5;
    c->craft.items.push_back(pot.out);
    Command cmd;
    cmd.source = "player:1";
    cmd.type = "alchemy_drink";
    cmd.args = {{"id", c->id}, {"item", static_cast<int>(c->craft.items.size()) - 1}};
    Command r = w.applyNow(cmd);
    CHECK(r.result.rfind("ok", 0) == 0);
    CHECK(r.result.find("life+") != std::string::npos);
    CHECK(c->body.health > 0.5);
    CHECK(c->craft.items.empty() || c->craft.items.back().form != "healing_potion");
    // a stone is no potion
    c->craft.items.push_back(rawItem(ct.craft, "granite"));
    cmd.args["item"] = static_cast<int>(c->craft.items.size()) - 1;
    CHECK(w.applyNow(cmd).result == "rejected:not_a_potion");
    // a rune of life on touch heals its holder and spends charge
    Item silver = silverPlate();
    StepResult ins = step("inscribe", {{"work", {silver}}});
    StepResult b = step("bind", {{"work", {ins.out}}, {"reagent", {pot.out}}}, "on_touch");
    CHECK(b.hasOut);
    c->craft.items.push_back(b.out);
    c->body.health = 0.5;
    Command use;
    use.source = "player:1";
    use.type = "alchemy_use_rune";
    use.args = {{"id", c->id}, {"item", static_cast<int>(c->craft.items.size()) - 1}};
    CHECK(w.applyNow(use).result.rfind("ok", 0) == 0);
    CHECK(c->body.health > 0.5);
    CHECK(c->craft.items.back().rune.charge < c->craft.items.back().rune.capacity);
}

TEST(altera_alchemy_runes_fire_through_magic) {
    const Content& ct = alcContent();
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(ct, cfg);
    installAi(w);
    std::vector<Character*> people;
    for (auto& [id, x] : w.characters())
        if (!x.isAnimal() && x.body.alive && x.body.ageDays >= 16 * 360) people.push_back(&x);
    CHECK(people.size() >= 2);
    if (people.size() < 2) return;
    Character& holder = *people[0];
    Character& other = *people[1];
    // a rune of life on silver, fired at another through the magic core, heals them
    ChainRun pot = goodRun("healing_potion", "healing_potion");
    CHECK(pot.done);
    StepResult ins = step("inscribe", {{"work", {silverPlate()}}});
    StepResult b = step("bind", {{"work", {ins.out}}, {"reagent", {pot.out}}}, "on_touch");
    CHECK(b.hasOut);
    holder.craft.items.push_back(b.out);
    other.body.health = 0.4;
    Command use;
    use.source = "player:1";
    use.type = "alchemy_use_rune";
    use.args = {{"id", holder.id}, {"item", static_cast<int>(holder.craft.items.size()) - 1}, {"target", other.id}};
    CHECK(w.applyNow(use).result.rfind("ok", 0) == 0);
    CHECK(other.body.health > 0.4);
    // bond- on the holder drains the rune, bond+ feeds it back
    const int idx = static_cast<int>(holder.craft.items.size()) - 1;
    const double before = holder.craft.items[static_cast<size_t>(idx)].rune.charge;
    applyAspectsTo(w, holder, {altera::sim::AspectDose{"bond", '-', 1.0}}, holder.id);
    const double drained = holder.craft.items[static_cast<size_t>(idx)].rune.charge;
    CHECK(drained < before);
    applyAspectsTo(w, holder, {altera::sim::AspectDose{"bond", '+', 1.0}}, holder.id);
    CHECK(holder.craft.items[static_cast<size_t>(idx)].rune.charge > drained);
}
