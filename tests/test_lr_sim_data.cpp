// LR-142: data-driven content. The data layers (a mod over content/, "_merge" by id,
// directory unions, hot-reload stamps), the named formulas (params, constants, mixes,
// name checks, cycles, chance functions) and templates (extends / use / params).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "sim/char_expr.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/formulas.hpp"
#include "sim/templates.hpp"

using namespace lr::sim;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

fs::path scratch(const char* name) {
    const fs::path d = fs::temp_directory_path() / ("lr_data_" + std::string(name));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Unmounts when the test ends, so other tests read plain files.
struct Unmount {
    ~Unmount() { data::mount(""); }
};

} // namespace

TEST(lr_data_layers_replace_merge_and_list) {
    Unmount um;
    const fs::path d = scratch("layers");
    writeFile(d / "content/ai/a.json", R"J({"items":[{"id":"x","v":1},{"id":"y","v":2}],"k":{"a":1,"b":2}})J");
    writeFile(d / "content/ai/b.json", R"J({"keep":true})J");
    writeFile(d / "content/species/elf.json", R"J({"id":"elf"})J");
    writeFile(d / "mods/m1/ai/a.json",
              R"J({"_merge":true,"items":[{"id":"y","v":5},{"id":"z","v":9},{"id":"x","_remove":true}],"k":{"b":3}})J");
    writeFile(d / "mods/m2/ai/b.json", R"J({"replaced":true})J");
    writeFile(d / "mods/m2/species/orc.json", R"J({"id":"orc"})J");
    const auto mods = data::discoverMods({(d / "mods").string()});
    CHECK_EQ(mods.size(), size_t(2));
    data::mount((d / "content").string(), mods);

    const Json a = data::readJson((d / "content/ai/a.json").string());
    CHECK(!a.contains("_merge"));
    CHECK_EQ(a["items"].size(), size_t(2));
    CHECK_EQ(a["items"][0]["id"].get<std::string>(), std::string("y"));
    CHECK_EQ(a["items"][0]["v"].get<int>(), 5);
    CHECK_EQ(a["items"][1]["id"].get<std::string>(), std::string("z"));
    CHECK_EQ(a["k"]["a"].get<int>(), 1);
    CHECK_EQ(a["k"]["b"].get<int>(), 3);
    const Json b = data::readJson((d / "content/ai/b.json").string());
    CHECK(b.contains("replaced") && !b.contains("keep"));
    // a path with ".." inside the root maps onto the layers too
    const auto sp = data::listFiles((d / "content/ai/../species").string());
    CHECK_EQ(sp.size(), size_t(2));
    CHECK(data::exists((d / "content/species/orc.json").string()));
    // the drop-in stream reads the layered JSON
    data::InFile in(d / "content/ai/a.json");
    CHECK(bool(in));
    CHECK_EQ(Json::parse(in)["items"].size(), size_t(2));
    data::InFile none(d / "content/ai/missing.json");
    CHECK(!none);

    // hot reload: nothing changed, then an edit and a new file in a watched dir
    CHECK(data::changed().empty());
    writeFile(d / "mods/m1/ai/a.json", R"J({"_merge":true,"k":{"b":4}})J");
    writeFile(d / "mods/m1/species/goblin.json", R"J({"id":"goblin"})J");
    const auto ch = data::changed();
    CHECK(ch.size() >= 2);
    const auto g0 = data::generation();
    const std::string key0 = data::cacheKey("x");
    data::reload();
    CHECK_EQ(data::generation(), g0 + 1);
    CHECK(data::cacheKey("x") != key0);
    CHECK_EQ(data::readJson((d / "content/ai/a.json").string())["k"]["b"].get<int>(), 4);
    data::restoreGeneration(g0);
    CHECK_EQ(data::generation(), g0);
}

TEST(lr_data_load_order_and_content_root) {
    Unmount um;
    const fs::path d = scratch("order");
    writeFile(d / "mods/alpha/x.json", "{}");
    writeFile(d / "mods/beta/x.json", "{}");
    writeFile(d / "mods/_off/x.json", "{}");
    writeFile(d / "mods/load_order.json", R"J({"mods":["beta"],"off":["alpha"]})J");
    const auto mods = data::discoverMods({(d / "mods").string()});
    CHECK_EQ(mods.size(), size_t(1));
    CHECK(mods[0].find("beta") != std::string::npos);
    // content/ beside an exe wins over the baked source tree
    writeFile(d / "bin/data/content/ai/config.json", "{}");
    const std::string root = data::findContentRoot((d / "bin/game.exe").string(), "/nowhere/content");
    CHECK(root.find("bin/data/content") != std::string::npos);
    CHECK_EQ(data::findContentRoot("", "/nowhere/content"), std::string("/nowhere/content"));
}

// A mod folder dropped in while the game runs joins the layers on the next check.
TEST(lr_data_new_mod_joins_while_running) {
    Unmount um;
    const fs::path d = scratch("newmod");
    writeFile(d / "bin/content/ai/a.json", R"J({"v":1})J");
    fs::create_directories(d / "bin/mods");
    data::mountDefault((d / "bin/game.exe").string(), "/nowhere/content");
    CHECK(data::layers().empty());
    (void)data::readJson((d / "bin/content/ai/a.json").string());
    CHECK(data::changed().empty());
    writeFile(d / "bin/mods/late/ai/a.json", R"J({"_merge":true,"w":2})J");
    CHECK(!data::changed().empty());
    CHECK_EQ(data::layers().size(), size_t(1));
    CHECK_EQ(data::readJson((d / "bin/content/ai/a.json").string())["w"].get<int>(), 2);
}

TEST(lr_formulas_params_constants_mixes) {
    std::vector<std::string> err;
    FormulaBook b;
    b.addJson(Json::parse(R"J({
        "constants": {"cap": 0.9},
        "formulas": [
          {"id": "mood_edge", "expr": "need.fun * 0.2"},
          {"id": "persuade", "params": ["skill"], "defaults": {"skill": "eloquence"},
           "expr": "min(cfg.cap, attr.charm * 0.6 + skill.$skill * 0.4 + f.mood_edge)"},
          {"id": "persuade_lore", "expr": "f.persuade(skill=lore)"},
          {"id": "blend", "expr": {"attr.charm": 3, "skill.eloquence": 1}, "max": 0.5},
          {"id": "scaled", "params": ["k"], "expr": "attr.charm * $k"},
          {"id": "twice", "expr": "f.scaled(2) + f.scaled(k=1 + 1)"}
        ]})J"),
              "test.json", &err);
    CHECK(b.finalize(&err));
    for (const auto& e : err) std::cerr << e << "\n";
    CHECK(err.empty());
    MapExprCtx c;
    c.vars = {{"attr.charm", 0.5}, {"skill.eloquence", 1.0}, {"skill.lore", 0.0}, {"need.fun", 1.0}};
    CHECK(std::fabs(b.eval("persuade", c) - 0.9) < 1e-9);          // 0.3 + 0.4 + 0.2 capped at 0.9
    CHECK(std::fabs(b.eval("persuade_lore", c) - 0.5) < 1e-9);     // 0.3 + 0 + 0.2
    CHECK(std::fabs(b.eval("f.persuade(skill=lore)", c) - 0.5) < 1e-9);
    CHECK(std::fabs(b.eval("blend", c) - 0.5) < 1e-9);             // (1.5 + 1) / 4 = 0.625 -> max 0.5
    CHECK(std::fabs(b.eval("twice", c) - 2.0) < 1e-9);
    CHECK_EQ(b.eval("nope", c, -1.0), -1.0);
    // an expression compiled elsewhere reads the book's formulas
    std::string e2;
    const Expr x = Expr::compile("f.persuade_lore * 2", &e2);
    CHECK(e2.empty());
    CHECK(std::fabs(b.evalExpr(x, c) - 1.0) < 1e-9);
}

TEST(lr_formulas_errors_are_load_errors) {
    auto errorsOf = [](const std::string& src) {
        std::vector<std::string> err;
        FormulaBook b;
        b.addJson(Json::parse(src), "bad.json", &err);
        b.finalize(&err);
        return err;
    };
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","expr":"f.b"},{"id":"b","expr":"f.a + 1"}]})J").empty()); // cycle
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","expr":"f.missing"}]})J").empty());
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","expr":"bogus_name * 2"}]})J").empty());           // no prefix answers it
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","expr":"cfg.nothing"}]})J").empty());              // no such constant
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","params":["s"],"expr":"skill.$t"}]})J").empty());   // not a parameter
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","expr":"attr.x +"}]})J").empty());                 // syntax
    CHECK(!errorsOf(R"J({"formulas":[{"id":"a","params":["s"],"expr":"skill.$s"},{"id":"b","expr":"f.a(1+2)"}]})J").empty()); // an expression inside a name
    CHECK(errorsOf(R"J({"formulas":[{"id":"a","expr":"attr.x + t.skill.y + arg.n"}]})J").empty());
}

TEST(lr_formulas_chance_and_registered_functions) {
    registerExprFn("twice_of", 1, 1, [](const std::vector<double>& a, const ExprCtx&) { return a[0] * 2.0; });
    std::string err;
    MapExprCtx c;
    c.roll = 0.5;
    CHECK(std::fabs(Expr::compile("p_over(0.5, 0.1, 0.5)", &err).eval(c) - 0.5) < 1e-9);
    CHECK(Expr::compile("p_over(0.7, 0.1, 0.5)", &err).eval(c) > 0.97);
    CHECK_EQ(Expr::compile("p_over(0.4, 0, 0.5)", &err).eval(c), 0.0);
    const double n = Expr::compile("noise(1, 0.2)", &err).eval(c);
    CHECK(std::isfinite(n) && std::fabs(n - 1.0) < 1.0);
    CHECK(std::fabs(Expr::compile("sqrt(0.25) + pow(2, 3) + lerp(0, 10, 0.5)", &err).eval(c) - 13.5) < 1e-9);
    CHECK_EQ(Expr::compile("twice_of(4)", &err).eval(c), 8.0);
    CHECK(err.empty());
}

TEST(lr_formulas_person_context) {
    Character a, b;
    a.id = 1;
    b.id = 2;
    a.attrs["charisma"] = 0.8;
    a.skills["eloquence"] = 0.5;
    a.traits = {"brave"};
    b.attrs["charisma"] = 0.2;
    a.relations[2].trust = 0.6;
    CharExprCtx c(a, &b);
    c.args["dc"] = 0.4;
    std::string err;
    CHECK(std::fabs(Expr::compile("attr.charisma - t.attr.charisma + trait.brave + rel.trust + arg.dc", &err).eval(c) -
                    (0.8 - 0.2 + 1 + 0.6 + 0.4)) < 1e-9);
    CHECK(err.empty());
}

TEST(lr_templates_extends_use_params) {
    std::vector<std::string> err;
    TemplateBook book;
    book.add(Json::parse(R"J({"templates":[
        {"id":"blade","params":["metal"],"tags":["tool","blade"],"material":"$metal","worth":"$w","defaults":{"w":2}},
        {"id":"mix.sharp","edge":0.8}
      ]})J"),
             "t.json", &err);
    Json list = Json::parse(R"J([
        {"id":"sword","extends":"blade","use":["mix.sharp"],"args":{"metal":"iron"},"+tags":["weapon"]},
        {"id":"base_food","abstract":true,"nutrition":0.3,"tags":["food"]},
        {"id":"bread","extends":"base_food","nutrition":0.5},
        {"id":"plain","v":1}
      ])J");
    expandTemplates(list, book, "items", &err);
    for (const auto& e : err) std::cerr << e << "\n";
    CHECK(err.empty());
    CHECK_EQ(list.size(), size_t(3));
    const Json& sword = list[0];
    CHECK_EQ(sword["material"].get<std::string>(), std::string("iron"));
    CHECK_EQ(sword["worth"].get<int>(), 2);   // "$w" alone takes the number
    CHECK_EQ(sword["tags"].size(), size_t(3));
    CHECK_EQ(sword["edge"].get<double>(), 0.8);
    CHECK(!sword.contains("extends") && !sword.contains("args") && !sword.contains("params"));
    CHECK_EQ(list[1]["id"].get<std::string>(), std::string("bread"));
    CHECK_EQ(list[1]["nutrition"].get<double>(), 0.5);
    CHECK_EQ(list[1]["tags"][0].get<std::string>(), std::string("food"));

    std::vector<std::string> bad;
    Json cyc = Json::parse(R"J([{"id":"a","extends":"b"},{"id":"b","extends":"a"},{"id":"c","extends":"nope"}])J");
    expandTemplates(cyc, book, "items", &bad);
    CHECK(bad.size() >= 2);
}

// A mod beside the real content adds an item (by "_merge"), a template-made item and a formula,
// and the whole content loads with them, as the built game would.
TEST(lr_data_mod_over_real_content) {
    Unmount um;
    const fs::path d = scratch("realmod");
    const std::string root = std::string(LR_SIM_SOURCE_DIR) + "/content";
    writeFile(d / "testmod/ai/items.json", R"J({"_merge":true,"items":[
        {"id":"lr142_test_cake","tags":["food"],"nutrition":0.4},
        {"id":"lr142_test_cake_big","extends":"lr142_test_cake","nutrition":0.8}]})J");
    writeFile(d / "testmod/ai/formulas/test.json",
              R"J({"constants":{"k":0.5},"formulas":[{"id":"lr142_probe","expr":"cfg.k * 2"}]})J");
    data::mount(root, {(d / "testmod").string()});
    const Content c = Content::load(root + "/ai");
    CHECK(c.items.count("lr142_test_cake") == 1);
    CHECK(c.items.count("lr142_test_cake_big") == 1);
    CHECK(std::fabs(c.items.at("lr142_test_cake_big").nutrition - 0.8) < 1e-9);
    CHECK(c.items.count("bread") == 1 || c.items.size() > 10);   // the base items are all still there
    MapExprCtx ctx;
    CHECK(std::fabs(c.formulas.eval("lr142_probe", ctx) - 1.0) < 1e-9);

    // a formula naming a trait the content lacks fails the load
    writeFile(d / "testmod/ai/formulas/test.json", R"J({"formulas":[{"id":"bad","expr":"trait.no_such_trait_lr142"}]})J");
    bool threw = false;
    try {
        Content::load(root + "/ai");
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("no_such_trait_lr142") != std::string::npos;
    }
    CHECK(threw);
}
