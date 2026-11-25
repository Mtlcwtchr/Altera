// AL-115 follow-ups: walking slows in mud, on ice and on steep ground; Earth~
// shapes the relief (sim/world_fields.hpp groundAdd); people put out a fire
// that comes near their village (sim/firefight.hpp).

#include <cstdio>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/world_fields.hpp"
#include "sim/world_reactions.hpp"
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

void dry(World& w) {
    w.fields().weather[village(w)].rainMm = -100.0;
    w.fields().ambientDay = -1;
}

void run(World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        w.step();
        fieldsTick(w);
    }
}

// The nearest land cell of a biome (with land round it) between `minM` and `maxM` from the village.
Vec2 cellOfBiome(const World& w, const std::string& biome, double minM, double maxM = 1e9) {
    const MapGrid& m = w.map();
    const Vec2 c = w.settlements().at(village(w)).center;
    Vec2 best{};
    double bd = 1e18;
    for (int j = 2; j < m.h - 2; ++j)
        for (int i = 2; i < m.w - 2; ++i) {
            bool ok = true;
            for (int dj = -1; dj <= 1 && ok; ++dj)
                for (int di = -1; di <= 1 && ok; ++di) {
                    const int k = (j + dj) * m.w + (i + di);
                    ok = (biome.empty() || m.biomeNames[m.biome[k]] == biome) && !m.water[k];
                }
            const Vec2 p{(i + 0.5) * m.cellM, (j + 0.5) * m.cellM};
            const double d = dist(p, c);
            if (ok && d >= minM && d <= maxM && d < bd) {
                bd = d;
                best = p;
            }
        }
    return best;
}

int burningNear(const World& w, Vec2 at, double r) {
    int n = 0;
    const int cn = fieldsConfig(w).chunk;
    for (const auto& [k, c] : w.fields().chunks)
        for (int idx = 0; idx < cn * cn; ++idx)
            if (c.fire[idx] > 0 && dist({c.cx * cn + idx % cn + 0.5, c.cy * cn + idx / cn + 0.5}, at) <= r) ++n;
    return n;
}

// cells the fire reached (burning or burnt out)
int scorchedNear(const World& w, Vec2 at, double r) {
    int n = 0;
    const int cn = fieldsConfig(w).chunk;
    for (const auto& [k, c] : w.fields().chunks)
        for (int idx = 0; idx < cn * cn; ++idx)
            if ((c.fire[idx] > 0 || c.burnt[idx]) && dist({c.cx * cn + idx % cn + 0.5, c.cy * cn + idx / cn + 0.5}, at) <= r) ++n;
    return n;
}

int chronicled(const World& w, const std::string& code) {
    int n = 0;
    for (const auto& e : w.chronicleLog()) n += e.code == code;
    return n;
}

} // namespace

TEST(altera_sim_relief_mud_and_ice_slow_the_step) {
    World w = bare();
    dry(w);
    const Vec2 m = cellOfBiome(w, "meadow", 700.0);
    const Vec2 far{m.x + 40.0, m.y};
    CHECK(std::abs(stepMoveFactor(w, m, far, 10.0) - 1.0) < 1e-9);   // untouched ground: full speed
    // a soaked strip across the way: mud
    fieldAdd(w, Field::moisture, {m.x + 5.0, m.y}, 3.0, 200.0);
    CHECK_EQ(cellStateAt(w, {m.x + 5.0, m.y}), std::string("mud"));
    const double mud = stepMoveFactor(w, m, far, 10.0);
    std::printf("  through mud: %.2f of the pace\n", mud);
    CHECK(mud < 0.85 && mud > 0.4);
    // frozen, the same strip is ice: slippery, but faster than mud
    World cold = w;
    fieldAdd(cold, Field::heat, {m.x + 5.0, m.y}, 3.0, -60.0);
    CHECK_EQ(cellStateAt(cold, {m.x + 5.0, m.y}), std::string("ice"));
    const double ice = stepMoveFactor(cold, m, far, 10.0);
    CHECK(ice < 1.0 && ice > mud);
    // a long walk barely notices a puddle on the way
    CHECK(stepMoveFactor(w, m, {m.x + 800.0, m.y}, 800.0) > 0.95);
}

TEST(altera_sim_relief_earth_heaves_a_mound_and_sinks_a_pit) {
    World w = bare();
    dry(w);
    const Vec2 m = cellOfBiome(w, "meadow", 700.0);
    const int moist0 = fieldByte(w, Field::moisture, m);
    // Earth~ through the magic channels: a mound
    const Json earthTilde = {{"raise_ground", "power"}};
    CHECK(applyChannels(w, m, 6.0, earthTilde, 4.0).empty());
    CHECK(groundAt(w, m) > 1.5);
    CHECK(slopeAt(w, {m.x + 4.0, m.y}) > 0.1);
    CHECK(moveFactorAt(w, {m.x + 4.0, m.y}) < 1.0);
    CHECK(fieldByte(w, Field::moisture, m) < moist0);   // raised land drains
    // a pit two metres deep a little way off fills with water: mud, nothing burns in it
    const Vec2 p{m.x + 30.0, m.y};
    groundAdd(w, p, 3.0, -2.0, true);
    CHECK(groundAt(w, p) < -1.5);
    CHECK(fieldByte(w, Field::moisture, p) >= 215);
    CHECK(!burnable(w, p));
    CHECK(moveFactorAt(w, p) < 0.6);
    // the relief stays: a week on, after the chunks have gone quiet, and through a save
    run(w, int(kTicksPerDay) * 7);
    CHECK(groundAt(w, m) > 1.5);
    CHECK(fieldByte(w, Field::moisture, p) >= 215);   // the pit still holds water while its chunk sleeps
    WorldFields copy;
    copy.fromJson(w.fields().toJson());
    CHECK(copy.chunks.size() == w.fields().chunks.size());
    bool same = true;
    for (const auto& [k, c] : w.fields().chunks) same = same && copy.chunks.at(k).ground == c.ground;
    CHECK(same);
}

TEST(altera_sim_relief_moving_ground_cracks_what_stands_on_it) {
    World w = bare();
    const Vec2 m = cellOfBiome(w, "meadow", 700.0);
    const double cell = w.content().build.cellM;
    const int gx = int(m.x / cell), gy = int(m.y / cell);
    BuildPiece f;
    f.id = w.newId();
    f.kind = "foundation_stone";
    f.x = gx, f.y = gy;
    f.built = true;
    w.pieces()[f.id] = f;
    BuildPiece wl = f;
    wl.id = w.newId();
    wl.kind = "wall_log";
    w.pieces()[wl.id] = wl;
    refreshPieces(w);
    const Vec2 at = pieceCentre(w.content().build, *w.content().build.find("foundation_stone"), f);
    // a small heave cracks it, a great one brings it down (and the wall it held)
    groundAdd(w, at, 4.0, 0.5);
    CHECK(w.pieces().count(f.id) == 1);
    CHECK(w.fields().pieceIntegrity.count(f.id) == 1);
    groundAdd(w, at, 4.0, 3.0);
    CHECK(w.pieces().count(f.id) == 0);
    CHECK(w.pieces().count(wl.id) == 0);
}

TEST(altera_sim_firefight_the_village_puts_out_a_fire_near_it) {
    World w = bare();
    installAi(w);
    dry(w);
    // a patch of grass and scrub catches 150-300 m from the houses
    Vec2 at{};
    for (const char* b : {"forest", "meadow", ""}) {
        at = cellOfBiome(w, b, 150.0, fieldsConfig(w).firefight.alarmM - 20.0);
        if (at.x > 0 && burnable(w, at)) break;
    }
    CHECK(burnable(w, at));
    World alone = w;   // the same fire with nobody fighting it (the fields only)
    std::printf("  fire in %s, fuel %d\n", w.map().biomeAt(at).c_str(), fuelAt(w, at));
    for (int k = -2; k <= 2; ++k) {
        ignite(w, {at.x + 3.0 * k, at.y}, 255);
        ignite(alone, {at.x + 3.0 * k, at.y}, 255);
    }
    int crew = 0;
    for (int t = 0; t < 36; ++t) {
        w.step();
        run(alone, 1);
        crew = std::max<int>(crew, static_cast<int>(w.fields().firefighters.size()));
        if (t % 4 == 0) std::printf("  t%d crew %zu burning %d/%d scorched %d/%d\n", t, w.fields().firefighters.size(), burningNear(w, at, 200.0),
                    burningNear(alone, at, 200.0), scorchedNear(w, at, 200.0), scorchedNear(alone, at, 200.0));
    }
    CHECK(chronicled(w, "chronicle.fire_alarm") == 1);
    CHECK(crew > 0 && crew <= fieldsConfig(w).firefight.crew);
    CHECK(scorchedNear(w, at, 200.0) < scorchedNear(alone, at, 200.0));
    // once it is out the people go back to their lives
    for (int t = 0; t < int(kTicksPerDay) && !w.fields().fireAlarm.empty(); ++t) w.step();
    CHECK(w.fields().fireAlarm.empty());
    CHECK(w.fields().firefighters.empty());
    CHECK(chronicled(w, "chronicle.fire_out") == 1);
}
