// LR-156: the relief of the world (src/sim/relief) and its cliffs on the 2D map
// (src/client2d/relief2d): made by the terrain generator, deterministic, saved as
// its settings and made again on load, level ground round the village, cliff masks
// that agree with the levels, the tiles the rules name exist, climbing is slower.
#include <set>
#include <string>

#include "client2d/grid2d.hpp"
#include "client2d/relief2d.hpp"
#include "framework.hpp"
#include "sim/relief.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& reliefContent() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

Json reliefCfg() {
    Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    return cfg;
}

World& reliefWorld() {
    static World w = makeTestWorld(reliefContent(), reliefCfg(), 11);
    return w;
}

const Json& cliffsFile() {
    static const Json j = loadJsonFile(kRoot + "/content/sprites_2d/cliffs2d.json");
    return j;
}

const Settlement& firstSettlement(const World& w) { return w.settlements().begin()->second; }

} // namespace

TEST(lr_relief_made_and_varied) {
    const World& w = reliefWorld();
    const Relief* r = relief(w);
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(r->n >= 16);
    CHECK(!r->heights.empty());
    // the land has hills: levels from 0 up to several, nothing below the water
    int lo = 1 << 20, hi = -1;
    const double size = w.map().sizeM;
    for (double y = 50; y < size; y += 97)
        for (double x = 50; x < size; x += 97) {
            const double h = r->heightAt({x, y});
            CHECK(h >= 0.0);
            lo = std::min(lo, r->levelAt({x, y}));
            hi = std::max(hi, r->levelAt({x, y}));
        }
    CHECK(lo == 0);
    CHECK(hi >= 4);
    // the water is the low ground
    for (int i = 0; i < w.map().w * w.map().h; ++i)
        if (w.map().water[std::size_t(i)]) {
            const Vec2 p{(i % w.map().w + 0.5) * w.map().cellM, (i / w.map().w + 0.5) * w.map().cellM};
            CHECK(r->levelAt(p) <= 1);
            break;
        }
}

TEST(lr_relief_village_is_level) {
    const World& w = reliefWorld();
    const Relief* r = relief(w);
    if (!r) return;
    const Settlement& s = firstSettlement(w);
    const int at = r->levelAt(s.center);
    for (const auto& line : s.roads)
        for (const Vec2& p : line)
            if (dist(p, s.center) < 250) CHECK_EQ(r->levelAt(p), at);
    for (const auto& [oid, o] : w.objects())
        if (o.ownerFamily != kNone && dist(o.pos, s.center) < 250) CHECK_EQ(r->levelAt(o.pos), at);
}

TEST(lr_relief_deterministic_and_saved) {
    World a = makeTestWorld(reliefContent(), reliefCfg(), 11);
    const World& b = reliefWorld();
    CHECK(relief(a) && relief(b));
    if (!relief(a) || !relief(b)) return;
    CHECK(relief(a)->heights == relief(b)->heights);
    // a save carries the settings, not the heights; the load makes the same land
    const Json snap = a.toJson();
    CHECK(snap.at("map").contains("relief"));
    CHECK(!snap.at("map").at("relief").contains("heights"));
    World c(reliefContent(), 0);
    c.loadJson(snap);
    CHECK(relief(c) != nullptr);
    if (!relief(c)) return;
    for (double y = 300; y < a.map().sizeM; y += 1777)
        for (double x = 200; x < a.map().sizeM; x += 1531) CHECK(std::abs(reliefHeightAt(a, {x, y}) - reliefHeightAt(c, {x, y})) < 1e-9);
    // a world asked flat is flat
    Json flat = reliefCfg();
    flat["relief"] = false;
    World f = makeTestWorld(reliefContent(), flat, 11);
    CHECK(relief(f) == nullptr);
    CHECK_EQ(reliefLevelAt(f, {500, 500}), 0);
}

TEST(lr_relief_cliffs_match_levels) {
    const World& w = reliefWorld();
    const Relief* r = relief(w);
    if (!r) return;
    const c2::ReliefRules rules = c2::ReliefRules::fromJson(cliffsFile().value("rules", Json::object()));
    CHECK(rules.styles.size() >= 4);
    CHECK(rules.byBiome.count("forest") == 1);
    // find a hilly window: the steepest of a few
    int best = -1, bx = 0, by = 0;
    const int size = int(w.map().sizeM);
    for (int y = 64; y + 64 < size; y += 1024)
        for (int x = 64; x + 64 < size; x += 1024) {
            const int d = std::abs(r->levelAt({x + 0.5, y + 0.5}) - r->levelAt({x + 63.5, y + 63.5}));
            if (d > best) best = d, bx = x, by = y;
        }
    c2::Grid2d g = c2::buildGrid(w, c2::GroundRules::defaults(), bx, by, 64, 64);
    c2::paintRelief(w, rules, g);
    CHECK_EQ(g.level.size(), std::size_t(64 * 64));
    CHECK_EQ(g.cliff.size(), g.level.size());
    int cliffs = 0;
    static const int dx[8] = {0, 1, 0, -1, 1, 1, -1, -1}, dy[8] = {-1, 0, 1, 0, -1, 1, 1, -1};
    for (int y = 1; y < 63; ++y)
        for (int x = 1; x < 63; ++x) {
            const std::size_t i = std::size_t(y * 64 + x);
            if (g.at(bx + x, by + y) == c2::Ground::water) continue;
            for (int d = 0; d < 8; ++d) {
                const int nl = g.level[std::size_t((y + dy[d]) * 64 + x + dx[d])];
                CHECK_EQ(bool(g.cliff[i] & (1 << d)), nl > g.level[i]);
            }
            cliffs += g.cliff[i] != 0;
            CHECK((g.cliffStyle[i] & 0x7f) < rules.styles.size());
        }
    CHECK(cliffs > 0);
    const Json j = c2::toJson(g);
    CHECK(j.contains("cliff") && j["cliff"].get<std::string>().size() == 64 * 64 * 2);
    CHECK(j.contains("level") && j.contains("cliff_style") && j.contains("cliff_styles"));
}

TEST(lr_relief_tiles_exist) {
    const Json& f = cliffsFile();
    const Json sprites = f.value("sprites", Json::object());
    const Json rules = f.value("rules", Json::object());
    const int nv = rules.value("variants", 1);
    for (const auto& st : rules.at("styles")) {
        const std::string p = "cf_" + st.get<std::string>() + "_";
        for (const char* d : {"n", "s", "e", "w"}) {
            for (int v = 0; v < nv; ++v) CHECK(sprites.contains(p + d + "_" + std::to_string(v)));
            CHECK(sprites.contains(p + "ramp_" + d));
        }
        for (const char* c : {"c_ne", "c_nw", "c_se", "c_sw"}) CHECK(sprites.contains(p + c));
    }
    for (const auto& [k, rows] : sprites.items()) {
        CHECK_EQ(rows.size(), std::size_t(16));
        for (const auto& row : rows) CHECK_EQ(row.get<std::string>().size(), std::size_t(16));
    }
}

TEST(lr_relief_climbing_is_slower) {
    const World& w = reliefWorld();
    const Relief* r = relief(w);
    if (!r) return;
    // walk east from somewhere until a level changes within a step
    const int size = int(w.map().sizeM);
    for (int y = 200; y < size; y += 400)
        for (int x = 200; x + 2 < size; ++x) {
            const Vec2 a{x + 0.5, y + 0.5}, b{x + 1.5, y + 0.5};
            if (r->levelAt(a) != r->levelAt(b)) {
                CHECK(reliefMoveFactor(w, a, b, 1.0) < 0.5);
                CHECK(reliefMoveFactor(w, a, {a.x, a.y}, 1.0) == 1.0);
                return;
            }
        }
    CHECK(false);   // no step anywhere
}
