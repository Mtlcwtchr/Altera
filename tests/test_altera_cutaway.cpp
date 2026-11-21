// The top view's cutaway (src/game_view/cutaway.hpp, AL-56): which walls and
// roofs go in each wall mode, what dissolves in the way of a person, the fade.

#include <cmath>

#include "framework.hpp"
#include "game_view/cutaway.hpp"

using namespace altera::game::cutaway;

namespace {

// A 2 x 2 cell house (2 m cells, 3 m storey) on the ground at (0..4, 0..4):
// floors, walls round it (pieces 4..11), four corner pillars, a roof.
struct House {
    std::vector<Piece> ps;
    int southWall = -1, northWall = -1, eastWall = -1, roof = -1, floor = -1, pillarSW = -1, pillarNE = -1;
    explicit House(std::uint64_t family = 1, double ox = 0, std::uint64_t keyBase = 0) {
        std::uint64_t key = keyBase;
        auto add = [&](Kind k, double x, double y, double z0, double z1, double nx, double ny, double hx, double hy) {
            Piece p;
            p.key = ++key, p.kind = k, p.family = family, p.x = ox + x, p.y = y, p.z0 = z0, p.z1 = z1;
            p.nx = nx, p.ny = ny, p.hx = hx, p.hy = hy;
            ps.push_back(p);
            return int(ps.size()) - 1;
        };
        for (double x : {1.0, 3.0})
            for (double y : {1.0, 3.0}) {
                const int f = add(Kind::Floor, x, y, 0, 0.2, 0, 0, 1, 1);
                if (floor < 0) floor = f;
            }
        // walls: normals given the "wrong" way half the time (made outward by the house)
        for (double x : {1.0, 3.0}) {
            const int s = add(Kind::Wall, x, 0, 0, 3, 0, 1, 1, 0.1);
            const int n = add(Kind::Wall, x, 4, 0, 3, 0, 1, 1, 0.1);
            if (southWall < 0) southWall = s, northWall = n;
        }
        for (double y : {1.0, 3.0}) {
            add(Kind::Wall, 0, y, 0, 3, -1, 0, 1, 0.1);
            const int e = add(Kind::Wall, 4, y, 0, 3, -1, 0, 1, 0.1);
            if (eastWall < 0) eastWall = e;
        }
        pillarSW = add(Kind::Pillar, 0, 0, 0, 3, 0, 0, 0.2, 0.2);
        add(Kind::Pillar, 4, 0, 0, 3, 0, 0, 0.2, 0.2);
        add(Kind::Pillar, 0, 4, 0, 3, 0, 0, 0.2, 0.2);
        pillarNE = add(Kind::Pillar, 4, 4, 0, 3, 0, 0, 0.2, 0.2);
        for (double x : {1.0, 3.0})
            for (double y : {1.0, 3.0}) {
                const int r = add(Kind::Roof, x, y, 3, 5, 0, 0, 1, 1);
                if (roof < 0) roof = r;
            }
    }
};

const double kCam[3] = {2, -20, 15};   // south of the house, high up, looking north and down

bool isCut(const Look& l) { return l.vis == 0 && l.keep == kWallStub; }
bool isGone(const Look& l) { return l.vis == 0 && l.keep == kKeepNone; }
bool isFull(const Look& l) { return l.vis == 1; }

std::vector<Look> targets(Mode m, const std::vector<Piece>& ps, const std::vector<Focus>& foci) {
    Cutaway c;
    c.update(m, ps, kCam, foci, 0);
    return c.targets();
}

} // namespace

TEST(altera_cutaway_walls_point_out_of_the_house) {
    House h;
    Cutaway c;
    c.update(Mode::Up, h.ps, kCam, {}, 0);
    CHECK(c.houseOf()[std::size_t(h.southWall)] == c.houseOf()[std::size_t(h.roof)]);
    CHECK(std::fabs(c.outward()[std::size_t(h.southWall)][1] + 1) < 1e-9);   // south: -y
    CHECK(std::fabs(c.outward()[std::size_t(h.northWall)][1] - 1) < 1e-9);   // north: +y
    CHECK(std::fabs(c.outward()[std::size_t(h.eastWall)][0] - 1) < 1e-9);    // east: +x
}

TEST(altera_cutaway_cut_mode_drops_the_front_walls_and_the_roof) {
    House h;
    const auto t = targets(Mode::Cut, h.ps, {Focus{2, -30, 0, true}});
    CHECK(isCut(t[std::size_t(h.southWall)]));   // faces the camera
    CHECK(isFull(t[std::size_t(h.northWall)]));  // the back wall stands
    CHECK(isFull(t[std::size_t(h.eastWall)]));   // side on to the camera: stands
    CHECK(isCut(t[std::size_t(h.pillarSW)]));
    CHECK(isFull(t[std::size_t(h.pillarNE)]));
    CHECK(isGone(t[std::size_t(h.roof)]));       // upstairs of the storey looked at
    CHECK(isFull(t[std::size_t(h.floor)]));
}

TEST(altera_cutaway_down_mode_cuts_every_wall) {
    House h;
    const auto t = targets(Mode::Down, h.ps, {Focus{2, -30, 0, true}});
    CHECK(isCut(t[std::size_t(h.southWall)]));
    CHECK(isCut(t[std::size_t(h.northWall)]));
    CHECK(isCut(t[std::size_t(h.eastWall)]));
    CHECK(isGone(t[std::size_t(h.roof)]));
}

TEST(altera_cutaway_up_mode_keeps_the_house_whole) {
    House h;
    const auto t = targets(Mode::Up, h.ps, {Focus{2, -10, 0, true}});   // in front: nothing in the way
    for (const Look& l : t) CHECK(isFull(l));
    const auto none = targets(Mode::Up, h.ps, {});
    for (const Look& l : none) CHECK(isFull(l));
}

TEST(altera_cutaway_up_mode_opens_the_house_a_person_is_in) {
    House h;
    const auto t = targets(Mode::Up, h.ps, {Focus{2, 2, 0, true}});
    for (std::size_t i = 0; i < h.ps.size(); ++i)
        if (h.ps[i].kind == Kind::Roof) CHECK(isGone(t[i]));
    CHECK(isCut(t[std::size_t(h.southWall)]));   // between the person and the camera
    CHECK(isFull(t[std::size_t(h.northWall)]));
    CHECK(isFull(t[std::size_t(h.floor)]));
}

TEST(altera_cutaway_dissolves_what_hides_a_person_behind_a_house) {
    House h;
    const auto t = targets(Mode::Up, h.ps, {Focus{2, 7, 0, true}});
    CHECK(isCut(t[std::size_t(h.northWall)]));   // the back wall, between them
    CHECK(isFull(t[std::size_t(h.southWall)]));  // the line passes over the front one
    bool roofGone = false;
    for (std::size_t i = 0; i < h.ps.size(); ++i)
        if (h.ps[i].kind == Kind::Roof && isGone(t[i])) roofGone = true;
    CHECK(roofGone);
    // the mouse on the ground in front of the house hides behind nothing
    const auto m = targets(Mode::Up, h.ps, {Focus{2, -5, 0, false}});
    for (const Look& l : m) CHECK(isFull(l));
}

TEST(altera_cutaway_upper_storey_goes_in_cut_mode_only_above_the_focus) {
    House h;
    // a second storey: the roof pieces as its floor, walls on top
    Piece w = h.ps[std::size_t(h.northWall)];
    w.key = 999, w.z0 = 3.2, w.z1 = 6;
    h.ps.push_back(w);
    const auto down = targets(Mode::Cut, h.ps, {Focus{2, 2, 0, true}});
    CHECK(isGone(down.back()));
    const auto up = targets(Mode::Cut, h.ps, {Focus{2, 2, 3.2, true}});   // the person upstairs
    CHECK(isFull(up.back()));
}

TEST(altera_cutaway_families_make_separate_houses) {
    House a(1, 0, 0), b(2, 4.0, 100);   // two families wall to wall
    std::vector<Piece> ps = a.ps;
    ps.insert(ps.end(), b.ps.begin(), b.ps.end());
    Cutaway c;
    c.update(Mode::Up, ps, kCam, {Focus{2, 2, 0, true}}, 0);
    const std::size_t roofA = std::size_t(a.roof), roofB = a.ps.size() + std::size_t(b.roof);
    CHECK(c.houseOf()[roofA] != c.houseOf()[roofB]);
    CHECK(isGone(c.targets()[roofA]));
    CHECK(isFull(c.targets()[roofB + 3]));   // the far roof cell of the neighbours' house
}

TEST(altera_cutaway_whole_buildings_cut_to_the_sill) {
    Piece p;
    p.key = 1, p.kind = Kind::Whole, p.x = 10, p.y = 10, p.z0 = 0, p.z1 = 6, p.hx = 4, p.hy = 3, p.yaw = 0.5;
    const std::vector<Piece> ps{p};
    CHECK(isFull(targets(Mode::Up, ps, {Focus{30, 30, 0, true}})[0]));
    CHECK(isFull(targets(Mode::Cut, ps, {Focus{30, 30, 0, true}})[0]));
    const Look in = targets(Mode::Up, ps, {Focus{10, 10, 0, true}})[0];
    CHECK(in.vis == 0 && in.keep == kWholeStub);
    const Look down = targets(Mode::Down, ps, {Focus{30, 30, 0, true}})[0];
    CHECK(down.vis == 0 && down.keep == kWholeStub);
}

TEST(altera_cutaway_fades_over_a_quarter_second) {
    House h;
    Cutaway c;
    const std::vector<Focus> far{Focus{2, -30, 0, true}};
    c.update(Mode::Up, h.ps, kCam, far, 0.016);
    CHECK(isFull(c.looks()[std::size_t(h.southWall)]));
    c.update(Mode::Down, h.ps, kCam, far, 0.1);
    const Look mid = c.looks()[std::size_t(h.southWall)];
    CHECK(std::fabs(mid.vis - 0.6f) < 1e-4f && mid.keep == kWallStub);
    c.update(Mode::Down, h.ps, kCam, far, 0.2);
    CHECK(isCut(c.looks()[std::size_t(h.southWall)]));
    // back up: the stub stays while the wall comes back, then nothing is held
    c.update(Mode::Up, h.ps, kCam, far, 0.1);
    const Look back = c.looks()[std::size_t(h.southWall)];
    CHECK(std::fabs(back.vis - 0.4f) < 1e-4f && back.keep == kWallStub);
    c.update(Mode::Up, h.ps, kCam, far, 0.25);
    CHECK(isFull(c.looks()[std::size_t(h.southWall)]));
}
