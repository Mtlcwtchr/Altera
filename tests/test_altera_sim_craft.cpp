// Crafting without recipes (gdd crafting design v0.2): craft-expr, the
// content of content/ai/craft, the resolver (steps, assembly, recognition,
// flaws, salvage) and the examples of §12.

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/craft/craft.hpp"
#include "sim/content.hpp"
#include "sim/worldgen.hpp"
#include "sim/craft/craft_chain.hpp"

using namespace altera::sim;
using namespace altera::sim::craft;

namespace {

const CraftData& data() {
    static const CraftData d = CraftData::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai/craft");
    return d;
}

struct MapCtx : ExprCtx {
    std::map<std::string, double> v;
    double var(const std::string& n) const override {
        auto it = v.find(n);
        return it == v.end() ? 0.0 : it->second;
    }
    double sym(const std::string& fn, const std::string& a) const override {
        return fn == "has" && a == "handle" ? 1.0 : 0.0;
    }
};

double ev(const std::string& src, const MapCtx& c) {
    std::string err;
    Expr e = Expr::compile(src, &err);
    CHECK(err.empty());
    return e.eval(c);
}

} // namespace

TEST(altera_craft_expr) {
    MapCtx c;
    c.v["x"] = 0.4;
    c.v["blade.hardness"] = 0.5;
    c.v["q"] = 0.5;
    CHECK(std::fabs(ev("x + 0.08*q", c) - 0.44) < 1e-9);
    CHECK(std::fabs(ev("0.6+0.8*blade.hardness*q", c) - 0.8) < 1e-9);
    CHECK(ev("x >= 0.3 and not (q < 0.2)", c) == 1.0);
    CHECK(ev("x > 1 ? 5 : (q >= 0.5 ? 2 : 3)", c) == 2.0);
    CHECK(ev("clamp(max(1, 2, 3) - min(0.5, x), 0, 2)", c) == 2.0);
    CHECK(ev("has(handle) + has(shaft)", c) == 1.0);
    CHECK(ev("floor(2.7) + ceil(0.2) + abs(-1)", c) == 4.0);
    std::string err;
    Expr bad = Expr::compile("x + (", &err);
    CHECK(!err.empty());
    CHECK(bad.empty());
    Constraint k;
    CHECK(Constraint::parse("2..3", k, nullptr));
    CHECK(k.ok(2.5) && !k.ok(3.5));
    CHECK(std::fabs(k.closeness(3.1, 0.25) - 0.6) < 1e-6);
    CHECK(Constraint::parse(">=0.5", k, nullptr) && k.ok(0.5) && !k.ok(0.4));
    CHECK(Constraint::parse("1", k, nullptr) && k.ok(1) && !k.ok(2));
}

TEST(altera_craft_content_loads) {
    const CraftData& d = data();
    CHECK(d.loaded);
    for (const auto& e : d.errors) std::cerr << "    craft error: " << e << "\n";
    CHECK(d.errors.empty());
    CHECK(d.materials.size() >= 50);
    CHECK(d.processes.size() >= 17);
    CHECK(d.forms.size() >= 45);
}

namespace {

Workplace fullShop() {
    Workplace wp;
    for (const auto& [id, s] : data().stations) wp.stations.push_back(id);
    wp.stations.push_back("bellows_stand"); // unknown ids are ignored
    Tool t;
    t.tags = {"cutting", "saw", "striking", "tongs", "whetstone", "grit", "needle", "bellows"};
    t.hardness = 0.85;
    t.q = 0.7;
    wp.tools.push_back(t);
    return wp;
}

Actor actorWith(double skill, std::uint64_t id = 7) {
    Actor a;
    a.id = id;
    for (const char* s : {"cooking", "smithing", "carpentry", "knapping", "spinning", "weaving", "sewing", "tanning",
                          "pottery", "brewing", "herbalism", "toolmaking", "fiber_working", "building", "firemaking"})
        a.skills[s] = skill;
    for (const auto& [id2, m] : data().materials) a.knowsMaterial[id2] = skill;
    return a;
}

std::vector<RecipeStep> designSteps(const std::string& id) {
    const DesignDef* ds = data().design(id);
    CHECK(ds != nullptr);
    return ds ? recipeFromDesign(data(), *ds, 1.0, true).steps : std::vector<RecipeStep>{};
}

ChainRun runDesign(const std::string& id, const Actor& a, std::uint64_t seed) {
    std::vector<Item> pool;
    ChainOpts o;
    o.rawFromNothing = true;
    o.rawQ = 0.6;
    o.stopOnRisk = false;
    return runChain(data(), designSteps(id), pool, a, fullShop(), seed, 1000, o);
}

std::string describe(const Item& it) {
    std::string s = "form=" + it.form + " shape=" + it.shape + " mat=" + it.material + " q=" + std::to_string(it.q);
    for (const auto& [k, v] : it.fn)
        if (v != 0.0) s += " " + k + "=" + std::to_string(v).substr(0, 5);
    for (const auto& f : it.flaws) s += " flaw:" + f.id;
    return s;
}

} // namespace

TEST(altera_craft_every_archetype_reachable) {
    // every archetype has a starter design, and a master following it makes it
    const CraftData& d = data();
    Actor master = actorWith(0.95);
    int bad = 0;
    for (const auto& f : d.forms) {
        const DesignDef* ds = d.design(f.id);
        if (!ds) {
            std::cerr << "    no design for " << f.id << "\n";
            ++bad;
            continue;
        }
        int made = 0;
        std::string last;
        for (std::uint64_t seed = 1; seed <= 5; ++seed) {
            ChainRun r = runDesign(f.id, master, seed);
            if (r.done && r.out.form == f.id) ++made;
            else if (r.done) last = describe(r.out);
            else last = "stop " + r.stop + " " + r.stopArg + " at " + std::to_string(r.stopStep) +
                        (r.results.empty() ? "" : std::string(" outcome ") + outcomeName(r.results.back().outcome));
        }
        if (made < 3) {
            std::cerr << "    " << f.id << ": " << made << "/5; " << last << "\n";
            ++bad;
        }
    }
    CHECK(bad == 0);
}

TEST(altera_craft_every_design_makes_its_form) {
    // AL-89: the other designs of an archetype (tallow candle beside the wax one) make it too
    const CraftData& d = data();
    Actor master = actorWith(0.95);
    int bad = 0;
    for (const auto& ds : d.designs) {
        if (ds.id == ds.form) continue; // the starter design: every_archetype_reachable
        int made = 0;
        std::string last;
        for (std::uint64_t seed = 1; seed <= 5; ++seed) {
            ChainRun r = runDesign(ds.id, master, seed);
            if (r.done && r.out.form == ds.form) ++made;
            else if (r.done) last = describe(r.out);
            else last = "stop " + r.stop + " " + r.stopArg + " at " + std::to_string(r.stopStep);
        }
        if (made < 3) {
            std::cerr << "    " << ds.id << " -> " << ds.form << ": " << made << "/5; " << last << "\n";
            ++bad;
        }
    }
    CHECK(bad == 0);
}

TEST(altera_craft_deterministic) {
    // the same world seed, actor, tick and step number give the same result
    Actor a = actorWith(0.5);
    for (const char* id : {"sword", "ale", "rope", "clay_pot"}) {
        ChainRun r1 = runDesign(id, a, 42), r2 = runDesign(id, a, 42);
        CHECK(r1.done == r2.done && r1.stop == r2.stop && r1.time == r2.time);
        CHECK(r1.results.size() == r2.results.size());
        for (size_t i = 0; i < r1.results.size() && i < r2.results.size(); ++i) {
            CHECK(r1.results[i].outcome == r2.results[i].outcome);
            CHECK(r1.results[i].qStep == r2.results[i].qStep);
            CHECK(r1.results[i].newFlaws == r2.results[i].newFlaws);
        }
        if (r1.done && r2.done) CHECK(r1.out.form == r2.out.form && r1.out.q == r2.out.q);
    }
    // a different seed changes the rolls
    int differ = 0;
    for (std::uint64_t s = 1; s <= 8; ++s)
        if (runDesign("sword", a, s).results.front().qStep != runDesign("sword", a, s + 100).results.front().qStep)
            ++differ;
    CHECK(differ >= 6);
}

namespace {

struct Tally {
    int runs = 0, done = 0, named = 0, improvised = 0, flawed = 0;
    double q = 0;
};

bool anyFlaw(const Item& it) {
    if (!it.flaws.empty()) return true;
    for (const auto& p : it.parts)
        if (anyFlaw(p.item.front())) return true;
    return false;
}

Tally tally(const std::string& design, double skill, int n) {
    Tally t;
    const DesignDef* ds = data().design(design);
    Actor a = actorWith(skill, 11);
    for (int i = 0; i < n; ++i) {
        ChainRun r = runDesign(design, a, 1000 + static_cast<std::uint64_t>(i));
        ++t.runs;
        if (!r.done) continue;
        ++t.done;
        if (r.out.form == ds->form) ++t.named;
        else if (r.out.form.rfind("improvised_", 0) == 0) ++t.improvised;
        if (anyFlaw(r.out)) ++t.flawed;
        t.q += r.out.q;
    }
    if (t.done) t.q /= t.done;
    return t;
}

} // namespace

TEST(altera_craft_novice_flaws_master_quality) {
    // §12.1: a novice gets scraps, flaws and "something cutting"; a master a sword
    Tally nov = tally("sword", 0.1, 40), mid = tally("sword", 0.5, 40), mas = tally("sword", 0.9, 40);
    CHECK(mas.named > nov.named);
    CHECK(mas.named >= 30);
    CHECK(nov.done - nov.named + (nov.runs - nov.done) >= 25); // ruined or not a sword
    CHECK(mas.q > mid.q);
    if (nov.done && mid.done) CHECK(mid.q > nov.q);
    if (nov.done) CHECK(static_cast<double>(nov.flawed) / nov.done > static_cast<double>(mas.flawed) / std::max(1, mas.done));
    for (const char* id : {"clay_pot", "ale", "rope", "knife", "bread"}) {
        Tally n = tally(id, 0.1, 30), m = tally(id, 0.9, 30);
        CHECK(m.named >= n.named);
        CHECK(m.named >= 24);
        if (n.done && m.done) CHECK(m.q > n.q);
    }
}

TEST(altera_craft_examples_s12) {
    const CraftData& d = data();
    Workplace wp = fullShop();
    Actor a = actorWith(0.7);
    // 2. an unfired pot is not a pot; a wet one in the fire bursts into shards
    {
        StepInput mould{"mould", {{"work", {rawItem(d, "clay")}}}, {"vessel_raw", 2, "", -1}};
        StepResult m = runStep(d, mould, a, wp, 5, 1, 0);
        CHECK(m.hasOut);
        CHECK(m.out.form != "clay_pot");
        StepInput fire{"heat", {{"work", {m.out}}}, {"", 0, "fire", 2}};
        int burst = 0;
        for (int i = 0; i < 10; ++i) {
            StepResult f = runStep(d, fire, a, wp, 5, 10 + static_cast<std::uint64_t>(i), 1);
            if (!f.hasOut && !f.salvage.empty() && f.salvage.front().material == "shards") ++burst;
        }
        CHECK(burst >= 8);
    }
    // 4. mixing herbs: hemlock in water is a poison; willow bark boiled is a remedy
    {
        ChainRun p = runDesign("poison", a, 3);
        CHECK(p.done && p.out.form == "poison");
        CHECK(p.out.fn["safe"] < 0.5);
        ChainRun h = runDesign("healing_draught", a, 3);
        CHECK(h.done && h.out.form == "healing_draught");
        CHECK(h.out.derived["heal"] > 0.05);
    }
    // 5. rope from bark: soaked, twisted three times
    {
        ChainRun r = runDesign("rope", a, 9);
        CHECK(r.done && r.out.form == "rope");
        CHECK(r.out.fn["bind"] >= 0.6);
    }
    // 3. beer: without yeast the wort sours instead of making alcohol
    {
        ChainRun ale = runDesign("ale", a, 4);
        CHECK(ale.done && ale.out.tags.count("alcohol"));
        std::vector<RecipeStep> st = designSteps("ale");
        st.back().step.slots.erase("agent");
        std::vector<Item> pool;
        ChainOpts o;
        o.rawFromNothing = true;
        o.stopOnRisk = false;
        ChainRun sour = runChain(d, st, pool, a, wp, 4, 1000, o);
        CHECK(sour.done && !sour.out.tags.count("alcohol"));
        CHECK(sour.out.tags.count("vinegar"));
    }
    // 1. the sword: the derive numbers come out for the combat core
    {
        ChainRun s = runDesign("sword", actorWith(0.95), 2);
        CHECK(s.done && s.out.kind == "weapon");
        CHECK(s.out.derivedStr["weapon_id"] == "sword");
        CHECK(s.out.derived["damage_mult"] > 0.9 && s.out.derived["durability"] > 40);
    }
}

TEST(altera_craft_never_crashes) {
    // random inputs into every process: a step either refuses with a reason or
    // gives a result / salvage; nothing throws, nothing is NaN
    const CraftData& d = data();
    Workplace wp = fullShop();
    Actor a = actorWith(0.5);
    std::vector<std::string> mats;
    for (const auto& [id, m] : d.materials) mats.push_back(id);
    std::vector<std::string> shapes;
    for (const auto& [id, s] : d.shapes) shapes.push_back(id);
    Roll rng(99, 1, 2, 3);
    auto pick = [&](size_t n) { return static_cast<size_t>(rng.next() * static_cast<double>(n)) % n; };
    std::vector<Item> made;
    int started = 0, refused = 0;
    for (int i = 0; i < 4000; ++i) {
        const std::string& pid = d.processOrder[pick(d.processOrder.size())];
        const ProcessDef& p = *d.process(pid);
        StepInput in;
        in.process = pid;
        for (const auto& sl : p.slots) {
            int n = sl.min + static_cast<int>(pick(static_cast<size_t>(sl.max - sl.min + 1)));
            for (int k = 0; k < n; ++k) {
                if (!made.empty() && rng.next() < 0.35) in.slots[sl.name].push_back(made[pick(made.size())]);
                else in.slots[sl.name].push_back(rawItem(d, mats[pick(mats.size())], rng.next()));
            }
        }
        if (rng.next() < 0.6) in.target.shape = shapes[pick(shapes.size())];
        if (rng.next() < 0.5 && !p.modes.empty()) in.target.mode = p.modes[pick(p.modes.size())];
        in.target.heat = static_cast<int>(pick(5)) - 1;
        StepResult r = runStep(d, in, a, wp, 7, static_cast<std::uint64_t>(i), i);
        if (r.outcome == Outcome::NotStarted) {
            ++refused;
            CHECK(!r.reason.empty());
            continue;
        }
        ++started;
        if (r.hasOut) {
            CHECK(std::isfinite(r.out.q) && r.out.q >= 0 && r.out.q <= 1);
            for (const auto& [k, v] : r.out.fn) CHECK(std::isfinite(v));
            if (made.size() < 300) made.push_back(r.out);
            else made[pick(made.size())] = r.out;
        } else {
            CHECK(r.outcome == Outcome::Ruined || r.outcome == Outcome::Catastrophe);
        }
    }
    CHECK(started > 400);
    CHECK(refused > 400);
}

#include "sim/craft/craft_validate.hpp"

namespace {

Json readJ(const std::string& name) {
    return loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai/craft/" + name);
}

} // namespace

TEST(altera_craft_validator) {
    // the real content is clean; what is left are warnings about raw materials
    // the world does not gather yet
    const Content& c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    CHECK(c.craft.loaded);
    int craftWarnings = 0;
    for (const auto& w : c.warnings)
        if (w.rfind("craft:", 0) == 0) {
            ++craftWarnings;
            if (w.find("no source in the world") == std::string::npos && w.find("no process makes it") == std::string::npos)
                std::cerr << "    " << w << "\n";
            CHECK(w.find("no source in the world") != std::string::npos || w.find("no process makes it") != std::string::npos);
        }
    CHECK(craftWarnings < 30);
    // broken data is caught
    Json mats = readJ("materials.json"), procs = readJ("processes.json"), forms = readJ("forms.json"),
         designs = readJ("designs.json");
    procs["processes"][0]["flaws"][0]["id"] = "no_such_flaw";
    procs["processes"][1]["transform"]["purity"] = "x + bogus_name";
    forms["forms"][0]["needs"]["shape"] = Json::array({"no_such_shape"});
    designs["designs"][0]["steps"][0]["process"] = "no_such_process";
    mats["materials"][0]["shape"] = "nowhere";
    Content broken = c;
    broken.craft = CraftData::fromJson(mats, readJ("shapes.json"), procs, readJ("functions.json"), forms,
                                       readJ("flaws.json"), readJ("reacts.json"), readJ("stations.json"),
                                       readJ("crafting_config.json"), designs);
    ValidationReport r;
    validateCraft(broken, r);
    auto has = [&](const char* s) {
        for (const auto& e : r.errors)
            if (e.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(has("no_such_flaw"));
    CHECK(has("bogus_name"));
    CHECK(has("no_such_shape"));
    CHECK(has("no_such_process"));
    CHECK(has("nowhere"));
}

// --- K7: recipe book, fragments, teaching, fast craft limits ---------------

TEST(altera_craft_recipe_book_fragments) {
    const CraftData& d = data();
    RecipeBook book;
    Actor a = actorWith(0.8);
    ChainRun first = runDesign("dagger", a, 21);
    CHECK(first.done && first.out.form == "dagger");
    Recipe& r = learnFromItem(d, book, first.out, 1, a.id);
    CHECK(r.steps.size() == 7);
    CHECK(r.steps.back().known);
    CHECK(r.knownCount() < static_cast<int>(r.steps.size())); // fragmentary at first
    CHECK(!r.master(d.cfg));
    CHECK(r.nameKey == "form.dagger.name");
    const std::string id = r.id;
    int lastKnown = r.knownCount();
    for (int i = 0; i < 8; ++i) {
        ChainRun again = runDesign("dagger", a, 100 + static_cast<std::uint64_t>(i));
        if (!again.done) continue;
        Recipe& rr = learnFromItem(d, book, again.out, 1, a.id);
        CHECK(rr.id == id); // the same chain is the same record
        CHECK(rr.knownCount() >= lastKnown);
        lastKnown = rr.knownCount();
    }
    CHECK(book.recipes.size() == 1);
    const Recipe* m = book.find(id);
    CHECK(m && m->master(d.cfg));
    CHECK(m && m->knownCount() == static_cast<int>(m->steps.size()));
    // teaching: the pupil gets a copy capped by the teacher's skill, and must practise
    Recipe pupil = teachRecipe(d, *m, 0.6, 0.2);
    CHECK(pupil.mastery <= 0.6 * d.cfg.teachShare + 1e-9);
    CHECK(pupil.successes == 0 && !pupil.master(d.cfg));
    CHECK(pupil.knownCount() < static_cast<int>(pupil.steps.size()));
}

TEST(altera_craft_fast_craft_stops_with_a_reason) {
    const CraftData& d = data();
    Workplace wp = fullShop();
    Actor a = actorWith(0.8);
    std::vector<RecipeStep> steps = designSteps("cord");
    // no flax in the store: stops before the first stage, nothing used
    std::vector<Item> pool{rawItem(d, "clay")};
    ChainRun r = runChain(d, steps, pool, a, wp, 1, 10);
    CHECK(!r.done && r.stop == "missing" && r.stopArg == "mat:flax_fiber");
    CHECK(pool.size() == 1);
    // with flax it makes the cord and uses the flax
    pool.push_back(rawItem(d, "flax_fiber"));
    r = runChain(d, steps, pool, a, wp, 1, 10);
    CHECK(r.done && r.out.form == "cord");
    CHECK(pool.size() == 1 && pool.front().material == "clay");
    // a stored intermediate is taken instead of making it again
    std::vector<Item> flax{rawItem(d, "flax_fiber")};
    ChainRun strand = runChain(d, designSteps("thread"), flax, a, wp, 2, 10);
    CHECK(strand.done);
    std::vector<Item> store{strand.out};
    r = runChain(d, steps, store, a, wp, 3, 10);
    CHECK(r.done && r.out.form == "cord");
    CHECK(r.ran.size() == 1 && r.skipped.size() == 1);
    // no station: stops with the reason of the step
    Workplace bare;
    bare.tools = wp.tools;
    std::vector<Item> fibre{rawItem(d, "flax_fiber")};
    ChainRun c = runChain(d, designSteps("cloth"), fibre, a, bare, 4, 10);
    CHECK(!c.done && c.stop == "cannot_start" && c.stopArg.rfind("no_station", 0) == 0);
    CHECK(c.leftovers.size() == 1 && c.leftovers.front().shape == "strand"); // the strand made so far stays
    CHECK(fibre.size() == 1 && fibre.front().shape == "strand");
    // more than 12 stages: refused before it starts
    std::vector<RecipeStep> longer = designSteps("sword");
    std::vector<RecipeStep> chain = longer;
    for (int i = 0; i < 4; ++i) {
        RecipeStep extra;
        extra.step.process = "grind";
        extra.step.slots["work"] = {{"step:" + std::to_string(chain.size()), 1}};
        chain.push_back(extra);
    }
    std::vector<Item> none;
    ChainOpts o;
    o.rawFromNothing = true;
    ChainRun tl = runChain(d, chain, none, a, wp, 5, 10, o);
    CHECK(!tl.done && tl.stop == "too_long");
    CHECK(tl.results.empty());
}

TEST(altera_craft_breakthrough_chance) {
    const CraftData& d = data();
    Actor a = actorWith(0.9);
    double expert = breakthroughChance(d, a, 0.9, 1.0), novice = breakthroughChance(d, a, 0.05, 1.0);
    CHECK(expert > novice);
    CHECK(expert <= d.cfg.brCap + 1e-12);
    CHECK(breakthroughChance(d, a, 0.9, 0.0) < expert);
    a.traits = {"curious"};
    CHECK(breakthroughChance(d, a, 0.9, 0.5) > breakthroughChance(d, actorWith(0.9), 0.9, 0.5));
}

TEST(altera_craft_preview_blurs_for_novices) {
    const CraftData& d = data();
    Workplace wp = fullShop();
    StepInput in{"twist", {{"work", {rawItem(d, "flax_fiber")}}}, {"strand", 0, "", -1}};
    Preview nov = previewStep(d, in, actorWith(0.05), wp), mas = previewStep(d, in, actorWith(0.9), wp);
    CHECK(nov.canStart && mas.canStart);
    CHECK(nov.qSpread > mas.qSpread);
    CHECK(nov.pRuin >= mas.pRuin);
    CHECK(mas.expectedQ > nov.expectedQ);
    CHECK(nov.likelyForm.empty());
    CHECK(mas.likelyForm == "thread");
    StepInput bad{"hammer", {{"work", {rawItem(d, "flax_fiber")}}}, {}};
    Preview no = previewStep(d, bad, actorWith(0.9), wp);
    CHECK(!no.canStart && !no.reason.empty());
}

// --- K3: crafting in the life-sim -------------------------------------------

#include "sim/craft_sim.hpp"

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

Character* anAdult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.ageDays >= 16 * 360) return &c;
    return nullptr;
}

} // namespace

TEST(altera_craft_in_the_sim) {
    const Content& ct = realContent();
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(ct, cfg);
    Character* c = anAdult(w);
    CHECK(c != nullptr);
    if (!c) return;
    c->inventory["fiber_raw"] = 3;
    c->skills["spinning"] = 0.6;
    // one step by hand: raw fibre -> a strand (thread)
    CraftCommand cmd;
    cmd.process = "twist";
    cmd.slots["work"] = {"inv:fiber_raw"};
    cmd.target.shape = "strand";
    CraftOutcome o = craftAttempt(w, *c, cmd);
    CHECK(o.started);
    CHECK(c->inventory["fiber_raw"] == 2);
    CHECK(c->skills["spinning"] > 0.6);
    CHECK(c->craft.knowsMaterial["flax_fiber"] > 0.0);
    if (o.made >= 0) {
        CHECK(c->craft.items[static_cast<size_t>(o.made)].form == "thread");
        CHECK(c->inventory["thread"] >= 1); // counted as the sim's item
        CHECK(!o.recipe.empty());           // the book remembers how
    }
    // a missing input is refused with a reason and nothing is used
    CraftCommand none = cmd;
    none.slots["work"] = {"inv:log"};
    CraftOutcome n = craftAttempt(w, *c, none);
    CHECK(!n.started && !n.reason.empty());
    // fast craft from a mastered starter design
    const std::string rid = giveDesign(w, *c, "cord");
    CHECK(!rid.empty());
    ChainPlan plan = fastCraftPlan(w, *c, rid);
    CHECK(plan.ok && plan.stages >= 1 && plan.time > 0);
    const int fibre = c->inventory["fiber_raw"];
    CraftOutcome f = fastCraft(w, *c, rid, 1);
    CHECK(f.started);
    CHECK(c->inventory["fiber_raw"] < fibre || c->inventory["thread"] >= 0);
    if (f.reason == "done") CHECK(c->inventory["cord"] >= 1);
    // fast craft of a fragmentary record is refused
    Recipe partial = c->craft.book.recipes.front();
    partial.id = "r_partial";
    partial.mastery = 0.2;
    partial.successes = 1;
    c->craft.book.recipes.push_back(partial);
    CHECK(fastCraft(w, *c, "r_partial").reason == "not_master");
    // the crafting state survives a save
    Json j = *c;
    Character back = j.get<Character>();
    CHECK(back.craft.items.size() == c->craft.items.size());
    CHECK(back.craft.book.recipes.size() == c->craft.book.recipes.size());
    CHECK(back.craft.knowsMaterial == c->craft.knowsMaterial);
    if (!back.craft.items.empty()) CHECK(back.craft.items.front().trace.size() == c->craft.items.front().trace.size());
}

TEST(altera_craft_designs_become_goap_actions) {
    // a starter design whose inputs, tools and places the world has is an action
    const Content& ct = realContent();
    int n = 0;
    for (const auto& [id, d] : ct.interactions)
        if (id.rfind("craft:", 0) == 0 && d.baseId.empty()) { // <id>@foreign: the player's order at another's hearth
            ++n;
            CHECK(d.craft == "design:" + id.substr(6));
            CHECK(d.goap && !d.eff.empty());
        }
    CHECK(n >= 2);
    CHECK(ct.interactions.count("craft:cord"));
}

// --- K4: made weapons and armour in combat, repair ---------------------------

#include "sim/combat_bridge.hpp"

TEST(altera_craft_made_gear_in_combat) {
    const Content& ct = realContent();
    const CombatBridge* b = combatBridge(ct);
    CHECK(b != nullptr);
    if (!b) return;
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(ct, cfg);
    Character* c = anAdult(w);
    if (!c) return;
    c->inventory.clear();
    c->craft.items.clear();
    altera::combat::FighterSpec bare = fighterOf(*b, *c, "fighting");
    // a master's sword and a poor one
    ChainRun good = runDesign("sword", actorWith(0.95), 2), poor = runDesign("sword", actorWith(0.45), 3);
    CHECK(good.done && good.out.kind == "weapon");
    c->craft.items.push_back(good.out);
    altera::combat::FighterSpec armed = fighterOf(*b, *c, "fighting");
    CHECK(armed.weapon == "sword" && armed.weapon != bare.weapon);
    CHECK(std::fabs(armed.damageMult - good.out.derived["damage_mult"]) < 1e-9);
    CHECK(armed.durabilityMult > 0.1);
    if (poor.done && poor.out.kind == "weapon") {
        Character p = *c;
        p.craft.items = {poor.out};
        CHECK(fighterOf(*b, p, "fighting").damageMult < armed.damageMult);
    }
    // mail and a gambeson are worn with their quality; loose rings leave gaps
    ChainRun mail = runDesign("mail_shirt", actorWith(0.7), 4), pad = runDesign("gambeson", actorWith(0.7), 5);
    CHECK(mail.done && mail.out.derivedStr["layer"] == "mail");
    c->craft.items.push_back(mail.out);
    if (pad.done) c->craft.items.push_back(pad.out);
    // AL-109: made armour counts on the body only, not in the bag
    c->inventory["mail_shirt"] = 1;
    const altera::combat::FighterSpec bagged = fighterOf(*b, *c, "fighting");
    CHECK(std::find(bagged.armor.begin(), bagged.armor.end(), "mail") == bagged.armor.end());
    c->worn["torso:armour"] = "mail_shirt";
    altera::combat::FighterSpec worn = fighterOf(*b, *c, "fighting");
    CHECK(std::find(worn.armor.begin(), worn.armor.end(), "mail") != worn.armor.end());
    CHECK(worn.armorEff.size() == worn.armor.size());
    // the core plays the numbers: the same fight with the made sword hits harder
    altera::combat::Engine e(*b->data, 1);
    altera::combat::Fighter f1 = e.make(armed, 1, 0), f0 = e.make(bare, 1, 0);
    CHECK(f1.damageMult == armed.damageMult && f0.damageMult == 1.0);
    // a fight wears the weapon; a smith mends it
    wearMadeWeapon(*c, 0.5);
    CHECK(c->craft.items.front().wear >= 0.5 - 1e-9);
    c->skills["smithing"] = 0.8;
    c->craft.items.front().form = "sword";
    // repair needs a striking tool: give the smith a hammer made by a master
    ChainRun hammer = runDesign("hammer_tool", actorWith(0.9), 6);
    CHECK(hammer.done && hammer.out.form == "hammer_tool");
    c->craft.items.push_back(hammer.out);
    double before = c->craft.items.front().wear;
    CraftOutcome rep = repairMade(w, *c, 0, "straighten");
    CHECK(rep.started);
    bool lower = false;
    for (const auto& it : c->craft.items)
        if (it.kind == "weapon" && it.wear < before) lower = true;
    if (rep.made >= 0) CHECK(lower);
}

// --- K5 sim side: player commands and names -----------------------------------

#include "sim/ai.hpp"

TEST(altera_craft_commands_and_names) {
    const Content& ct = realContent();
    Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(ct, cfg);
    installAi(w);
    Character* c = anAdult(w);
    if (!c) return;
    c->inventory["fiber_raw"] = 2;
    Command cmd;
    cmd.source = "player:1";
    cmd.type = "craft_attempt";
    cmd.args = {{"id", c->id}, {"process", "twist"}, {"slots", {{"work", {"inv:fiber_raw"}}}}, {"shape", "strand"}};
    Command r = w.applyNow(cmd);
    CHECK(r.result.rfind("ok:", 0) == 0);
    CHECK(!c->craft.lastOutcome.empty());
    Command bad = cmd;
    bad.args["process"] = "no_such";
    CHECK(w.applyNow(bad).result.rfind("rejected:unknown_process", 0) == 0);
    // names come from loc: an archetype with its material, an intermediate, plain food
    const Loc loc = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    ChainRun sword = runDesign("sword", actorWith(0.95), 2);
    CHECK(sword.done);
    const std::string ru = craftItemName(loc, "ru", data(), sword.out), en = craftItemName(loc, "en", data(), sword.out);
    CHECK(ru.find("\xd0\xbc\xd0\xb5\xd1\x87") != std::string::npos && ru.find("\xd1\x81\xd1\x82\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd") != std::string::npos);
    CHECK(en == "steel sword");
    ChainRun bread = runDesign("bread", actorWith(0.9), 3);
    if (bread.done && bread.out.form == "bread") CHECK(craftItemName(loc, "en", data(), bread.out) == "bread");
    Item bar = rawItem(data(), "wrought_iron");
    recognize(data(), bar);
    CHECK(craftItemName(loc, "en", data(), bar) == "iron bar");
    CHECK(craftItemName(loc, "ru", data(), bar).find("\xd0\xb1\xd1\x80\xd1\x83\xd1\x81\xd0\xbe\xd0\xba") != std::string::npos);
}
