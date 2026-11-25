// AL-115: the fields of the world's cells (sim/world_fields.hpp): writing and easing
// back, fire that runs with the wind and dies in the rain, things that break and
// leave debris, the save, and the channels of the magic aspects.

#include <cstdio>
#include <fstream>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World bare() {
    return makeTestWorld(real(), loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json"));
}

EntityId village(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return id;
    return kNone;
}

// Only the fields run: the world's clock and fieldsTick.
void run(World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        w.step();
        fieldsTick(w);
    }
}

// A cell of the biome far enough from water and from the village.
bool findCell(const World& w, const std::string& biome, Vec2& out) {
    const MapGrid& m = w.map();
    Vec2 c{};
    if (const auto it = w.settlements().find(village(w)); it != w.settlements().end()) c = it->second.center;
    double best = 1e18;
    for (int j = 2; j < m.h - 2; ++j)
        for (int i = 2; i < m.w - 2; ++i) {
            bool ok = true;
            for (int dj = -1; dj <= 1 && ok; ++dj)
                for (int di = -1; di <= 1 && ok; ++di) {
                    const int k = (j + dj) * m.w + (i + di);
                    ok = m.biomeNames[m.biome[k]] == biome && !m.water[k];
                }
            if (!ok) continue;
            const Vec2 p{(i + 0.5) * m.cellM, (j + 0.5) * m.cellM};
            const double d = dist(p, c);
            if (d < 700.0) continue;
            if (d < best) {
                best = d;
                out = p;
            }
        }
    return best < 1e17;
}

void setRain(World& w, double mm) {
    w.fields().weather[village(w)].rainMm = mm;
    w.fields().ambientDay = -1;
}

int burningCells(const World& w) {
    int n = 0;
    for (const auto& [k, c] : w.fields().chunks)
        for (auto f : c.fire) n += f > 0;
    return n;
}

const Json& aspects() {
    static const Json j = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/magic/aspects.json");
    return j;
}
const Json& channelsOf(const std::string& aspect, const std::string& vec) {
    for (const auto& a : aspects().at("aspects"))
        if (a.at("id") == aspect) return a.at("world_channels").at(vec);
    static const Json none = Json::object();
    return none;
}

} // namespace

TEST(altera_sim_fields_config_loads) {
    World w = bare();
    const FieldsConfig& cfg = fieldsConfig(w);
    CHECK(cfg.materials.count("wood") && cfg.materials.count("stone") && cfg.materials.count("thatch"));
    CHECK(cfg.things.count("tree") && cfg.things.at("tree").keepsStockAsDebris);
    CHECK(cfg.materialOfObject("water") == nullptr);
    CHECK(cfg.materialOfPiece("rubble_stone") && cfg.materialOfPiece("rubble_stone")->flammable == 0.0);
    std::vector<std::string> warn;
    std::ifstream in(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/world/fields.json");
    FieldsConfig::fromJson(Json::parse(in), &warn);
    for (const auto& s : warn) std::printf("  %s\n", s.c_str());
    CHECK(warn.empty());
    // every debris type and item exists
    for (const auto& [id, m] : cfg.materials) {
        if (!m.debrisType.empty()) CHECK(real().objects.count(m.debrisType));
        for (const auto& [it, n] : m.debris) CHECK(real().items.count(it));
        for (const auto& [it, n] : m.burntDebris) CHECK(real().items.count(it));
    }
    for (const auto& [t, r] : cfg.things) CHECK(real().objects.count(t));
}

TEST(altera_sim_fields_channel_expressions) {
    CHECK(std::abs(channelValue("+2*power", 3.0) - 6.0) < 1e-9);
    CHECK(std::abs(channelValue("-0.3*power", 2.0) + 0.6) < 1e-9);
    CHECK(std::abs(channelValue("power", 1.5) - 1.5) < 1e-9);
    CHECK(std::abs(channelValue("2.5", 9.0) - 2.5) < 1e-9);
    CHECK(std::abs(channelValue(Json(4), 9.0) - 4.0) < 1e-9);
}

TEST(altera_sim_fields_rest_at_baseline_and_ease_back) {
    World w = bare();
    Vec2 p;
    CHECK(findCell(w, "meadow", p));
    const FieldsConfig& cfg = fieldsConfig(w);
    const DayWeather d = weatherToday(w, village(w));
    CHECK_EQ(fieldByte(w, Field::heat, p), cfg.heatOfC(d.tempC));
    run(w, 10);
    CHECK(w.fields().chunks.empty());   // nothing wrote: nothing stored
    setRain(w, -100.0);
    fieldAdd(w, Field::moisture, p, 3.0, 60.0);
    fieldAdd(w, Field::taint, p, 3.0, 80.0);
    const int m0 = fieldByte(w, Field::moisture, p);
    CHECK(m0 > fieldByte(w, Field::moisture, {p.x + 40, p.y}) + 40);
    CHECK(fieldByte(w, Field::taint, p) >= 70);
    CHECK(!w.fields().chunks.empty());
    run(w, int(kTicksPerDay));
    CHECK(fieldByte(w, Field::moisture, p) < m0 - 30);   // dries back
    CHECK(fieldByte(w, Field::taint, p) > 0);            // the poison lingers for days
    run(w, int(kTicksPerDay) * 45);
    CHECK_EQ(fieldByte(w, Field::taint, p), 0);
    CHECK(w.fields().chunks.empty());                    // all at rest again: dropped
}

TEST(altera_sim_fields_fire_runs_with_wind_and_burns_out) {
    World w = bare();
    Vec2 p;
    CHECK(findCell(w, "forest", p));
    setRain(w, -100.0);
    w.fields().weather[village(w)].wind = 4.0;
    run(w, 1);
    const double from = fieldsViewJson(w, nullptr, 0)["wind"]["from_deg"].get<double>();
    CHECK(ignite(w, p, 220));
    // the trees round it
    int trees0 = 0;
    for (const auto& [id, o] : w.objects()) trees0 += o.type == "tree" && dist(o.pos, p) < 60.0;
    run(w, 60);
    CHECK(burningCells(w) > 20);
    const double rad = (from + 180.0) * 3.14159265358979323846 / 180.0;
    const Vec2 down{std::sin(rad), -std::cos(rad)};
    double far = 0.0, back = 0.0;
    for (const auto& [k, c] : w.fields().chunks)
        for (int idx = 0; idx < int(c.fire.size()); ++idx) {
            if (!c.fire[idx] && !c.burnt[idx]) continue;
            const int n = fieldsConfig(w).chunk;
            const double x = c.cx * n + idx % n + 0.5 - p.x, y = c.cy * n + idx / n + 0.5 - p.y;
            const double along = x * down.x + y * down.y;
            far = std::max(far, along);
            back = std::max(back, -along);
        }
    std::printf("  wind from %.0f deg: fire ran %.0f m downwind, %.0f m upwind\n", from, far, back);
    CHECK(far > back * 2.0 + 3.0);
    CHECK(fieldsViewJson(w, &p, 200.0)["lights"].size() > 0);
    run(w, int(kTicksPerDay) * 2);
    int trees1 = 0, wreck = 0;
    for (const auto& [id, o] : w.objects()) {
        trees1 += o.type == "tree" && dist(o.pos, p) < 60.0;
        wreck += o.type == "wreckage" && dist(o.pos, p) < 200.0;
    }
    std::printf("  trees in 60 m: %d -> %d, wreckage piles %d, burning cells left %d\n", trees0, trees1, wreck, burningCells(w));
    CHECK(trees0 == 0 || trees1 < trees0);
    // the ash: a burnt cell is more fertile for a while, then the fuel grows back
    const int fertBurnt = fieldByte(w, Field::fertility, p);
    CHECK(fertBurnt > fieldsConfig(w).biomes.at("forest").fertility);
}

TEST(altera_sim_fields_rain_puts_fire_out) {
    World dry = bare();
    Vec2 p;
    CHECK(findCell(dry, "forest", p));
    World wet = dry;
    setRain(dry, -100.0);
    setRain(wet, 6.0);
    ignite(dry, p, 220);
    ignite(wet, p, 220);
    run(dry, 40);
    run(wet, 40);
    std::printf("  burning after 40 ticks: dry %d, rain %d\n", burningCells(dry), burningCells(wet));
    CHECK(burningCells(wet) * 4 < burningCells(dry) + 1);
}

TEST(altera_sim_fields_blows_break_things_and_leave_debris) {
    World w = bare();
    installAi(w);
    // a storage with goods in it: broken, its goods stay in the wreckage
    EntityId store = kNone;
    for (const auto& [id, o] : w.objects())
        if (o.type == "storage" && !o.stock.empty()) store = id;
    CHECK(store != kNone);
    const Vec2 at = w.object(store)->pos;
    int goods = 0;
    for (const auto& [k, n] : w.object(store)->stock) goods += n;
    CHECK(!damageObject(w, store, 30, "force"));
    CHECK(objectIntegrity(w, *w.object(store)) == objectIntegrityMax(w, *w.object(store)) - 30);
    Command c;
    c.source = "scenario";
    c.type = "damage";
    c.args = {{"x", at.x}, {"y", at.y}, {"r", 0.5}, {"amount", 500}};
    CHECK_EQ(w.applyNow(c).result, std::string("ok"));
    CHECK(w.object(store) == nullptr);
    int left = 0;
    for (const auto& [id, o] : w.objects())
        if (o.type == "wreckage" && dist(o.pos, at) < 1.5)
            for (const auto& [k, n] : o.stock) left += n;
    CHECK(left >= goods);
    // a stone hearth stands a blow the storage did not
    for (const auto& [id, o] : w.objects())
        if (o.type == "hearth") {
            CHECK(!damageObject(w, id, 150, "force"));
            break;
        }
}

TEST(altera_sim_fields_broken_wall_brings_down_what_it_held) {
    World w = bare();
    Vec2 m;
    CHECK(findCell(w, "meadow", m));
    const double cell = w.content().build.cellM;
    const int gx = int(m.x / cell), gy = int(m.y / cell);
    auto put = [&](const std::string& kind, int x, int rot) {
        BuildPiece p;
        p.id = w.newId();
        p.kind = kind;
        p.x = x, p.y = gy, p.rot = rot;
        p.built = true;
        w.pieces()[p.id] = p;
        return p.id;
    };
    const EntityId f1 = put("foundation_stone", gx, 0), f2 = put("foundation_stone", gx + 1, 0);
    const EntityId wall = put("wall_log", gx, 0), wall2 = put("wall_stone", gx + 1, 0);
    refreshPieces(w);
    // a stone foundation stands what breaks a log wall
    CHECK(!damagePiece(w, f1, 130, "force"));
    CHECK(damagePiece(w, wall, 500, "force"));
    CHECK(w.pieces().count(wall) == 0 && w.pieces().count(f1) == 1);
    // the foundation under the stone wall goes: the wall comes down with it
    CHECK(damagePiece(w, f2, 1000, "force"));
    CHECK(w.pieces().count(f2) == 0 && w.pieces().count(wall2) == 0);
    int rubble = 0;
    for (const auto& [id, o] : w.objects()) rubble += (o.type == "rubble" || o.type == "wreckage") && dist(o.pos, m) < 10.0;
    CHECK(rubble > 0);
    // a log wall catches fire from a burning cell next to it and burns down; stone does not burn
    const EntityId logWall = put("wall_log", gx, 1);
    refreshPieces(w);
    setRain(w, -100.0);
    const BuildPiece& lw = w.pieces().at(logWall);
    const Vec2 at = pieceCentre(w.content().build, *w.content().build.find(lw.kind), lw);
    ignite(w, at, 220);
    CHECK(w.fields().burningPieces.count(logWall) == 1);
    CHECK(w.fields().burningPieces.count(f1) == 0);
    run(w, int(kTicksPerDay));
    CHECK(w.pieces().count(logWall) == 0);
    CHECK(w.pieces().count(f1) == 1);
}

TEST(altera_sim_fields_saved_and_loaded) {
    World w = bare();
    Vec2 p;
    CHECK(findCell(w, "forest", p));
    setRain(w, -100.0);
    ignite(w, p, 220);
    fieldAdd(w, Field::aether, p, 4.0, 90.0);
    run(w, 20);
    const Json j = w.toJson();
    World back(real(), w.seed());
    back.loadJson(j);
    CHECK_EQ(back.stateHash(), w.stateHash());
    CHECK_EQ(fieldByte(back, Field::aether, p), fieldByte(w, Field::aether, p));
    CHECK_EQ(burningCells(back), burningCells(w));
    run(w, 10);
    run(back, 10);
    CHECK_EQ(back.stateHash(), w.stateHash());   // and goes on the same
}

TEST(altera_sim_fields_magic_channels_write_the_world) {
    World w = bare();
    Vec2 forest, meadow;
    CHECK(findCell(w, "forest", forest));
    CHECK(findCell(w, "meadow", meadow));
    setRain(w, -100.0);
    // [Fire+] power 3 in a dry wood lights it
    applyChannels(w, forest, 2.0, channelsOf("fire", "+"), 3.0);
    run(w, 5);
    CHECK(burningCells(w) > 0);
    // [Water+] soaks the meadow, [Water-] dries it
    const int m0 = fieldByte(w, Field::moisture, meadow);
    applyChannels(w, meadow, 3.0, channelsOf("water", "+"), 2.0);
    CHECK(fieldByte(w, Field::moisture, meadow) > m0 + 60);
    // [Poison+] taints, [Life+] cleans it
    applyChannels(w, meadow, 3.0, channelsOf("poison", "+"), 2.0);
    const int t0 = fieldByte(w, Field::taint, meadow);
    CHECK(t0 > 50);
    applyChannels(w, meadow, 3.0, channelsOf("life", "+"), 2.0);
    CHECK(fieldByte(w, Field::taint, meadow) < t0);
    // [Weather+] brings rain to the village today
    const double rain0 = w.fields().weather[village(w)].rainMm;
    applyChannels(w, meadow, 3.0, channelsOf("weather", "+"), 2.0);
    CHECK(w.fields().weather[village(w)].rainMm > rain0);
    // keys for other systems come back
    const auto other = applyChannels(w, meadow, 3.0, channelsOf("earth", "~"), 1.0);
    CHECK(other.size() == 1 && other[0] == "raise_ground");
    // [Fire~] draws heat from round the disc into it
    Vec2 hot{meadow.x + 300, meadow.y};
    fieldAdd(w, Field::heat, {hot.x + 5, hot.y}, 2.0, 40.0);
    const int h0 = fieldByte(w, Field::heat, hot);
    applyChannels(w, hot, 2.5, channelsOf("fire", "~"), 2.0);
    CHECK(fieldByte(w, Field::heat, hot) > h0);
}
