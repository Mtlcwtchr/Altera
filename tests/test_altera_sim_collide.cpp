// Walking among things (sim/collide.hpp): discs, turned boxes and walls block
// a walker, a move slides along what it hits, a way is found round a house,
// ground one may not stand on stops the walker. And the game's Sims mode:
// `set_free_will {now}` hands an idle player's person to its own life at once.

#include <cmath>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/collide.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;
using namespace altera::sim::collide;

TEST(altera_sim_collide_blocks) {
    Obstacles o;
    o.add(Circle{0, 0, 1});
    o.add(Box{10, 0, 2, 1, 0.5});
    o.add(Wall{20, -5, 20, 5, 0.15});
    CHECK(o.blocked(0.5, 0, 0.3));
    CHECK(!o.blocked(1.5, 0, 0.3));
    CHECK(o.blocked(10, 0, 0.3));   // inside the box
    // the box is turned: a point 2.2 m along its long axis is just outside it
    CHECK(!o.blocked(10 + std::cos(0.5) * 2.5, std::sin(0.5) * 2.5, 0.2));
    CHECK(o.blocked(10 + std::cos(0.5) * 1.9, std::sin(0.5) * 1.9, 0.2));
    CHECK(o.blocked(20.2, 0, 0.3));
    CHECK(!o.blocked(20.6, 0, 0.3));
    CHECK(!o.blocked(20, 5.6, 0.3));   // past the wall's end
}

// AL-29: the camera's line of sight stops before a trunk, passes over what is
// lower than it, and leaves the thing it starts inside.
TEST(altera_sim_collide_sight_hit) {
    Obstacles o;
    o.add(Circle{5, 0, 0.3, 14.0});           // a tree halfway
    o.add(Box{0, 20, 2, 2, 0, 1.0});           // a low pile
    CHECK(std::fabs(o.sightHit(0, 0, 1.2, 10, 0, 6, 0.5, 0) - 1.0) > 0.4);
    const double t = o.sightHit(0, 0, 1.2, 10, 0, 6, 0.5, 0);
    CHECK(t > 0.3 && t < 0.45);                // just before the trunk (4.2 m of 10)
    CHECK(o.sightHit(0, 10, 1.2, 10, 10, 6, 0.5, 0) == 1.0);   // nothing on that line
    CHECK(o.sightHit(0, 16, 3.0, 0, 26, 8, 0.5, 0) == 1.0);    // over the pile
    CHECK(o.sightHit(5.2, 0, 1.2, 15, 0, 6, 0.5, 0) == 1.0);   // starts at the trunk, looks away
}

TEST(altera_sim_collide_slides_along_wall) {
    Obstacles o;
    o.add(Wall{0, -10, 0, 10, 0.15});
    double x = -1, y = 0;
    // walking into the wall at 45 degrees: it slides along it and never crosses
    for (int i = 0; i < 200; ++i) o.slide(x, y, 0.03, 0.03, 0.3);
    CHECK(x < -0.44);
    CHECK(y > 4.0);
    // straight into it: it stops at the wall
    double a = -1, b = 0;
    for (int i = 0; i < 100; ++i) o.slide(a, b, 0.05, 0, 0.3);
    CHECK(a < -0.44 && a > -0.5);
    // a big step at once does not jump the thin wall
    double c = -0.6, d = 0;
    o.slide(c, d, 3.0, 0, 0.3);
    CHECK(c < 0);
}

TEST(altera_sim_collide_ground) {
    Obstacles o;
    o.walkable = [](double x, double) { return x < 5.0; };   // water from x = 5
    double x = 4.5, y = 0;
    for (int i = 0; i < 50; ++i) o.slide(x, y, 0.1, 0, 0.3);
    CHECK(x < 5.0);
    CHECK(x > 4.8);
    // standing off the ground already (put there by the sim): it may walk out
    double p = 6, q = 0;
    CHECK(o.slide(p, q, -0.1, 0, 0.3));
}

TEST(altera_sim_collide_push_out) {
    Obstacles o;
    o.add(Box{0, 0, 4, 3, 0});
    double x = 3.5, y = 0.5;   // inside, near the +x face
    CHECK(o.pushOut(x, y, 0.3));
    CHECK(!o.blocked(x, y, 0.3));
    CHECK(x > 4.2 && std::fabs(y - 0.5) < 1e-6);
}

TEST(altera_sim_collide_path_round_house) {
    Obstacles o;
    o.add(Box{0, 0, 4, 3, 0.3});
    std::vector<Point> way;
    CHECK(o.path(-10, 0, 10, 0, 0.3, way));
    CHECK(!way.empty());
    if (way.empty()) return;
    CHECK(std::fabs(way.back().first - 10) < 1e-9 && std::fabs(way.back().second) < 1e-9);
    // every leg of the way is clear
    double px = -10, py = 0;
    for (const auto& [wx, wy] : way) {
        CHECK(o.clearLine(px, py, wx, wy, 0.3));
        px = wx, py = wy;
    }
    CHECK(way.size() >= 2);   // it turns round the house, not through it
    // straight when nothing is in the way
    CHECK(o.path(-10, 10, 10, 10, 0.3, way));
    CHECK_EQ(way.size(), std::size_t(1));
}

TEST(altera_sim_collide_path_goal_inside) {
    Obstacles o;
    o.add(Box{0, 0, 2, 2, 0});
    std::vector<Point> way;
    // the goal clicked on the house: the nearest free spot by it
    CHECK(o.path(-10, 0, 0, 0, 0.3, way));
    CHECK(!way.empty());
    if (!way.empty()) CHECK(!o.blocked(way.back().first, way.back().second, 0.3));
    // walled in on all sides: no way
    Obstacles box;
    box.add(Wall{-3, -3, 3, -3, 0.2});
    box.add(Wall{3, -3, 3, 3, 0.2});
    box.add(Wall{3, 3, -3, 3, 0.2});
    box.add(Wall{-3, 3, -3, -3, 0.2});
    CHECK(!box.path(0, 0, 20, 0, 0.3, way, 0.5, 6.0, 20000));
    CHECK(way.empty());
}

namespace {

const Content& mvp() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/ai");
    return c;
}

Command playerCmd(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = 1;
    return c;
}

} // namespace

TEST(altera_sim_free_will_now) {
    // The game's Sims mode counts idle seconds itself; `now` lets free will in
    // at once instead of after playerIdleResumeTicks.
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/tests/data/altera_mvp/test_world.json");
    World w = makeTestWorld(mvp(), cfg, 0);
    installAi(w);
    w.run(kTicksPerDay / 2);
    EntityId A = kNone;
    for (const auto& [id, c] : w.characters())
        if (c.body.stage() == "adult") {
            A = id;
            break;
        }
    CHECK(A != kNone);
    if (A == kNone) return;
    for (int i = 0; i < 64 && !w.character(A)->ai.goal.valid(); ++i) w.run(1);
    CHECK_EQ(w.applyNow(playerCmd("take_control", {{"id", A}})).result, std::string("ok"));
    CHECK(w.character(A)->ai.suspended);
    w.run(2);
    CHECK(w.character(A)->ai.suspended);   // still waiting for the player
    w.applyNow(playerCmd("set_free_will", {{"id", A}, {"on", true}, {"now", true}}));
    w.run(2);
    CHECK(!w.character(A)->ai.suspended);   // its own life again
    CHECK(w.character(A)->controller.isPlayer());
}
