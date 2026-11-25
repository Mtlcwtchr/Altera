// AL-112: fields and kitchen gardens on the 2D map (src/client2d/farm2d):
// the plots of the planned village painted into cells, the stage of each
// strip / bed from the field's stock, the garden's crops, the tiles there are.
#include <set>
#include <string>

#include "client2d/farm2d.hpp"
#include "client2d/grid2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
namespace c2 = altera::client2d;

namespace {

const std::string kRoot = ALTERA_SIM_SOURCE_DIR;

const Json& cropsFile() {
    static const Json j = loadJsonFile(kRoot + "/content/sprites_2d/crops2d.json");
    return j;
}

const c2::CropRules& cropRules() {
    static const c2::CropRules r = c2::CropRules::fromJson(cropsFile().value("plots", Json::object()));
    return r;
}

World& cropWorld() {
    static const Content content = Content::load(kRoot + "/content/ai");
    static World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        cfg["village_plan"] = true;
        World v = makeTestWorld(content, cfg, 7);
        installAi(v);
        return v;
    }();
    return w;
}

const WorldObject* firstOf(const World& w, const std::string& type) {
    for (EntityId id : w.objectsOfType(type))
        if (const WorldObject* o = w.object(id)) return o;
    return nullptr;
}

} // namespace

TEST(altera_client2d_crops_rules) {
    const c2::CropRules& r = cropRules();
    CHECK(r.byType.count("field") == 1);
    CHECK(r.byType.count("garden") == 1);
    CHECK(r.byType.at("garden").crops.size() >= 6);
    // every tile the rules name is drawn
    const Json sprites = cropsFile().value("sprites", Json::object());
    int named = 0;
    for (const auto& [type, rule] : r.byType) {
        for (const auto& [stage, list] : rule.tiles)
            for (const auto& s : list) {
                CHECK(sprites.contains(s));
                ++named;
            }
        for (const auto& s : rule.path) CHECK(sprites.contains(s));
        for (const auto& [crop, stages] : rule.crops) {
            for (const char* st : {"young", "grown", "ripe"}) CHECK(stages.count(st) == 1);
            for (const auto& [stage, list] : stages)
                for (const auto& s : list) {
                CHECK(sprites.contains(s));
                ++named;
            }
        }
    }
    CHECK(named > 30);
    for (const auto& [name, grid] : sprites.items()) {
        CHECK_EQ(grid.size(), std::size_t(16));
        for (const auto& row : grid) CHECK_EQ(row.get<std::string>().size(), std::size_t(16));
    }
}

TEST(altera_client2d_crops_stages) {
    const World& w = cropWorld();
    const WorldObject* f = firstOf(w, "field");
    CHECK(f != nullptr);
    if (!f) return;
    const c2::PlotRule& rule = cropRules().byType.at("field");
    const CropDef& cd = w.content().objects.at("field").crop;
    WorldObject o = *f;
    auto stagesOf = [&](const WorldObject& x) {
        std::map<std::string, int> n;
        for (const auto& p : c2::plotParts(w, x, rule)) ++n[p.stage];
        return n;
    };
    o.stock["strips"] = 10;
    o.stock["ripe"] = 0;
    o.stock["ripe_age"] = 0;
    // half sown just now, a few tilled
    o.stock["sown"] = 5, o.stock["tilled"] = 2, o.stock["grown"] = 0;
    auto n = stagesOf(o);
    CHECK_EQ(n["sown"], 5);
    CHECK_EQ(n["tilled"], 2);
    CHECK_EQ(n["fallow"] + n["stubble"], 3);
    // growing: young, then heading
    o.stock["sown"] = 10, o.stock["tilled"] = 0, o.stock["grown"] = cd.growDays * 4 / 10;
    CHECK_EQ(stagesOf(o)["young"], 10);
    o.stock["grown"] = cd.growDays - 2;
    CHECK_EQ(stagesOf(o)["heading"], 10);
    // ripe: the strips still standing, the rest reaped to stubble; left too long it is over
    o.stock["sown"] = 0, o.stock["grown"] = 0;
    const double perStrip = cd.seedPerStrip * 0.5 * (cd.yieldLo + cd.yieldHi) / cd.grainPerSheaf;
    o.stock["ripe"] = int(perStrip * 4);
    n = stagesOf(o);
    CHECK(n["ripe"] >= 4 && n["ripe"] <= 5);
    CHECK_EQ(n["ripe"] + n["stubble"], 10);
    o.stock["ripe_age"] = cd.spoilAfter + 5;
    CHECK(stagesOf(o)["over"] >= 4);
    // the field has no garden crops, the garden has
    for (const auto& p : c2::plotParts(w, o, rule)) CHECK(p.crop.empty());
}

TEST(altera_client2d_crops_garden) {
    const World& w = cropWorld();
    const WorldObject* gd = firstOf(w, "garden");
    CHECK(gd != nullptr);
    if (!gd) return;
    const c2::PlotRule& rule = cropRules().byType.at("garden");
    WorldObject o = *gd;
    o.stock["strips"] = 12;
    const auto a = c2::plotParts(w, o, rule), b = c2::plotParts(w, o, rule);
    std::set<std::string> crops;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK_EQ(a[i].crop, b[i].crop);   // stable
        CHECK(rule.crops.count(a[i].crop) == 1);
        crops.insert(a[i].crop);
    }
    CHECK(crops.size() >= 2);   // beds of different crops
    CHECK_EQ(a[0].crop, a[1].crop);   // pick_pairs: two beds share
}

TEST(altera_client2d_crops_paint) {
    const World& w = cropWorld();
    // a window round each field and garden: its cells are painted with tiles there are
    const Json sprites = cropsFile().value("sprites", Json::object());
    int fieldsSeen = 0, gardensSeen = 0;
    for (const char* type : {"field", "garden"}) {
        int k = 0;
        for (EntityId id : w.objectsOfType(type)) {
            if (k++ >= 3) break;
            const WorldObject* o = w.object(id);
            c2::Grid2d g = c2::buildGrid(w, c2::GroundRules::fromJson(loadJsonFile(kRoot + "/content/sprites_2d/map2d.json")
                                                                           .value("ground", Json::object())),
                                         int(o->pos.x) - 40, int(o->pos.y) - 40, 80, 80);
            c2::paintCrops(w, cropRules(), g);
            bool mine = false;
            for (const auto& p : g.plots) mine = mine || p.id == id;
            if (!mine) continue;
            (std::string(type) == "field" ? fieldsSeen : gardensSeen)++;
            CHECK_EQ(g.crop.size(), std::size_t(g.w * g.h));
            int cells = 0;
            for (auto c : g.crop) {
                if (!c) continue;
                ++cells;
                CHECK(c <= g.cropTiles.size());
                if (c <= g.cropTiles.size()) CHECK(sprites.contains(g.cropTiles[c - 1].sprite));
            }
            CHECK(cells > 8);
            const Json j = c2::toJson(g);
            CHECK_EQ(j.value("crop", std::string()).size(), std::size_t(g.w * g.h));
            CHECK(j["plots"].size() >= 1);
        }
    }
    CHECK(fieldsSeen >= 1);
    CHECK(gardensSeen >= 1);
}
