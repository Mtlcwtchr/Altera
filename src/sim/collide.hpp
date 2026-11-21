#pragma once
// Walking among things: what blocks a walker on the ground (houses, walls,
// posts, fires, ground too steep or wet) and how a walker of some radius moves
// against it. The game walks the player's person with this (WASD slides along
// a wall; a click on the ground finds a way round the houses) and pushes the
// others out of the houses they would cut through between two ticks.
//
// Pure geometry in metres on the ground plane, standard library only, so it
// builds with the headless sim and its tests (tests/test_altera_sim_collide.cpp).

#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace altera::sim::collide {

// `top`: how high the thing stands above the ground, metres (only lines of
// sight look at it, see Obstacles::sightHit).
struct Circle {
    double x = 0, y = 0, r = 0, top = 2.0;
};
// A box turned by yaw (radians, its local x axis at yaw from the world's x).
struct Box {
    double x = 0, y = 0, hx = 0, hy = 0, yaw = 0, top = 2.0;
};
// A wall: the segment a-b, `half` thick to each side.
struct Wall {
    double ax = 0, ay = 0, bx = 0, by = 0, half = 0.15, top = 2.0;
};

using Point = std::pair<double, double>;

class Obstacles {
public:
    void clear();
    void add(const Circle& c);
    void add(const Box& b);
    void add(const Wall& w);
    // Ground the walker may stand on (dry, not too steep); unset: everywhere.
    std::function<bool(double, double)> walkable;

    std::size_t size() const { return circles_.size() + boxes_.size() + walls_.size(); }
    // True when a disc of radius r at (x, y) overlaps a thing or stands on
    // ground it may not.
    bool blocked(double x, double y, double r) const;
    // Moves (x, y) by (dx, dy): what it runs into pushes it back along the
    // surface, so it slides along a wall instead of stopping dead. Never ends
    // inside a thing or on ground it may not stand on (it stays put then).
    // True when it moved at all.
    bool slide(double& x, double& y, double dx, double dy, double r) const;
    // Pushes a disc out of the things it overlaps (ground is not looked at):
    // the nearest free place. True when it moved.
    bool pushOut(double& x, double& y, double r) const;
    // A way from one point to another round what blocks, on a grid of `cell`
    // metres inside the box of both ends grown by `margin`; out: the corners
    // of the way (the start left out, the goal last). False when there is none
    // within `maxNodes` cells looked at (out then holds nothing).
    bool path(double fx, double fy, double tx, double ty, double r, std::vector<Point>& out, double cell = 0.5,
              double margin = 12.0, int maxNodes = 60000) const;
    // True when a disc can go straight from one point to the other (sampled every r/2).
    bool clearLine(double fx, double fy, double tx, double ty, double r) const;
    // A line of sight from (fx, fy, fz) to (tx, ty, tz), a ball of radius r
    // along it (a camera from what it looks at to where it would stand): the
    // fraction of the way, 0..1, it gets before a thing standing higher than the
    // line there (its `top` over `ground`) is in the way. Things the line starts
    // inside are passed until it is out of them; the ground is not looked at.
    // 1 when nothing is in the way.
    double sightHit(double fx, double fy, double fz, double tx, double ty, double tz, double r, double ground) const;

private:
    // The deepest overlap of a disc with one thing: the way out (unit) and how far.
    bool deepest(double x, double y, double r, double& nx, double& ny, double& depth) const;
    template <class F>
    void near(double x, double y, double r, F&& f) const;
    void index(int kind, int i, double minX, double minY, double maxX, double maxY);

    std::vector<Circle> circles_;
    std::vector<Box> boxes_;
    std::vector<Wall> walls_;
    static constexpr double kBucket = 8.0;
    std::unordered_map<long long, std::vector<std::pair<int, int>>> grid_;   // bucket -> (kind, index)
};

} // namespace altera::sim::collide
