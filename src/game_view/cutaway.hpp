#pragma once
// Walls and roofs out of the way of the top view, as in The Sims (AL-56).
//
// Three wall modes: up (the houses whole), cut away (the walls that face the
// camera down to a stub, the roofs and the storeys above the one looked at
// gone) and down (every wall a stub). In every mode what stands between the
// camera and a person or the mouse dissolves, and with the walls up the roof
// and the upper storeys of the house a person is inside of go.
//
// Nothing is faded with alpha: the statics shader (assets/shaders/altera_game/
// statics.hlsl) drops pixels by an ordered dither against `vis`, above `keep`
// metres over the piece's own bottom - the rough PSX look, and no sorting.
//
// Pure logic, the standard library only: tests/test_altera_cutaway.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace altera::game::cutaway {

enum class Mode : int { Up = 0, Cut = 1, Down = 2 };
// Home goes round as The Sims' wall button does: cut away -> down -> up -> cut away.
inline Mode nextMode(Mode m) { return m == Mode::Cut ? Mode::Down : m == Mode::Down ? Mode::Up : Mode::Cut; }

enum class Kind { Wall, Pillar, Floor, Roof, Whole };

struct Piece {
    std::uint64_t key = 0;     // stable over frames (the fade goes on from where it was)
    Kind kind = Kind::Wall;
    std::uint64_t family = 0;  // pieces of one family that touch make one house
    double x = 0, y = 0;       // centre on the ground plan, world metres
    double z0 = 0, z1 = 0;     // bottom and top, world metres
    double nx = 0, ny = 0;     // a wall's normal, either way (turned to point out of its house)
    double hx = 1, hy = 1;     // half extents: a wall along / across, a cell, a whole building
    double yaw = 0;            // a whole building's turn (the other kinds lie on the grid)
};

// What the view is about: the people looked after, the mouse on the ground.
// The first one sets the storey looked at (cut away, down).
struct Focus {
    double x = 0, y = 0, z = 0;   // feet / the ground, world metres
    bool person = true;           // a person is seen by the chest, the mouse by the ground
};

// keep: metres over the piece's bottom that stay whatever vis says.
struct Look {
    float vis = 1;
    float keep = 0;
};

constexpr float kKeepNone = -1000.0f;
constexpr float kWallStub = 0.7f;    // a cut wall, as The Sims leaves it
constexpr float kWholeStub = 1.1f;   // a baked building cut: its walls to the sill
constexpr double kAboveFeet = 1.5;   // a piece whose bottom is this far over the feet is "upstairs"
constexpr double kFadeRate = 4.0;    // vis per second (a quarter of a second for a full fade)

namespace detail {

// The segment a -> b against an axis-aligned box: true when it passes through.
inline bool segmentBox(const double a[3], const double b[3], const double lo[3], const double hi[3]) {
    double t0 = 0, t1 = 1;
    for (int k = 0; k < 3; ++k) {
        const double d = b[k] - a[k];
        if (std::fabs(d) < 1e-9) {
            if (a[k] < lo[k] || a[k] > hi[k]) return false;
            continue;
        }
        double u = (lo[k] - a[k]) / d, v = (hi[k] - a[k]) / d;
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return false;
    }
    return true;
}

} // namespace detail

// Does the piece stand between `from` (the focus as seen) and the camera?
inline bool occludes(const Piece& p, const double from[3], const double cam[3]) {
    constexpr double kNear = 0.05;   // the focus's own side: not in the way
    double a[3] = {from[0], from[1], from[2]};
    // start a little towards the camera, so what the person leans on is not "in the way"
    const double d[3] = {cam[0] - from[0], cam[1] - from[1], cam[2] - from[2]};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6) return false;
    for (int k = 0; k < 3; ++k) a[k] += d[k] / len * std::min(0.35, len * kNear);
    switch (p.kind) {
    case Kind::Wall: {
        const double den = d[0] * p.nx + d[1] * p.ny;
        if (std::fabs(den) < 1e-9) return false;
        const double t = ((p.x - a[0]) * p.nx + (p.y - a[1]) * p.ny) / den;
        if (t <= 0 || t >= 1) return false;
        const double hx = a[0] + d[0] * t - p.x, hy = a[1] + d[1] * t - p.y, hz = a[2] + d[2] * t;
        const double along = std::fabs(hx * -p.ny + hy * p.nx);
        return along <= p.hx + 0.25 && hz >= p.z0 - 0.1 && hz <= p.z1 + 0.1;
    }
    case Kind::Pillar:
    case Kind::Floor:
    case Kind::Roof: {
        const double r = p.kind == Kind::Pillar ? 0.45 : 0.0;
        const double lo[3] = {p.x - p.hx - r, p.y - p.hy - r, p.z0 - 0.1}, hi[3] = {p.x + p.hx + r, p.y + p.hy + r, p.z1 + 0.1};
        return detail::segmentBox(a, cam, lo, hi);
    }
    case Kind::Whole: {
        const double c = std::cos(-p.yaw), s = std::sin(-p.yaw);
        auto local = [&](const double w[3], double o[3]) {
            const double x = w[0] - p.x, y = w[1] - p.y;
            o[0] = x * c - y * s, o[1] = x * s + y * c, o[2] = w[2];
        };
        double la[3], lb[3];
        local(a, la);
        local(cam, lb);
        const double lo[3] = {-p.hx, -p.hy, p.z0 - 0.1}, hi[3] = {p.hx, p.hy, p.z1 + 0.1};
        return detail::segmentBox(la, lb, lo, hi);
    }
    }
    return false;
}

// Is the point (on the ground plan) under the piece's footprint?
inline bool under(const Piece& p, double x, double y, double margin) {
    if (p.kind == Kind::Whole) {
        const double c = std::cos(-p.yaw), s = std::sin(-p.yaw);
        const double lx = (x - p.x) * c - (y - p.y) * s, ly = (x - p.x) * s + (y - p.y) * c;
        return std::fabs(lx) <= p.hx + margin && std::fabs(ly) <= p.hy + margin;
    }
    return std::fabs(x - p.x) <= p.hx + margin && std::fabs(y - p.y) <= p.hy + margin;
}

class Cutaway {
public:
    // Every frame: the pieces in view, where the camera is, what the view is
    // about. Fills looks() in the pieces' order, eased from the last frame.
    void update(Mode mode, const std::vector<Piece>& pieces, const double cam[3], const std::vector<Focus>& foci,
                double dt) {
        houses(pieces);
        targets_.assign(pieces.size(), Look{});
        for (std::size_t i = 0; i < pieces.size(); ++i) targets_[i] = target(mode, pieces, i, cam, foci);
        ease(pieces, dt);
    }
    const std::vector<Look>& looks() const { return looks_; }
    const std::vector<Look>& targets() const { return targets_; }
    // The house (index) each piece of the last update fell in, and the wall normals turned outwards.
    const std::vector<int>& houseOf() const { return house_; }
    const std::vector<std::array<double, 2>>& outward() const { return out_; }

private:
    struct House {
        double cx = 0, cy = 0;
        int cells = 0;   // floors and roofs: the inside is known
    };
    std::vector<int> house_;
    std::vector<House> houseInfo_;
    std::vector<std::array<double, 2>> out_;   // outward on the ground plan (0, 0: unknown)
    std::vector<Look> targets_, looks_;
    struct Faded {
        Look look;
        std::uint64_t seen = 0;
    };
    std::unordered_map<std::uint64_t, Faded> faded_;
    std::uint64_t frame_ = 0;

    // Pieces of a family that touch (centres within 1.6 m: cell to edge, edge
    // to corner, storey over storey) make one house; its inside is the middle
    // of its floors and roofs.
    void houses(const std::vector<Piece>& ps) {
        const std::size_t n = ps.size();
        std::vector<int> up(n);
        for (std::size_t i = 0; i < n; ++i) up[i] = int(i);
        auto root = [&](int i) {
            while (up[std::size_t(i)] != i) i = up[std::size_t(i)] = up[std::size_t(up[std::size_t(i)])];
            return i;
        };
        constexpr double kCell = 2.0, kJoin = 1.6;
        std::unordered_map<std::int64_t, std::vector<int>> grid;
        auto cellKey = [](std::int64_t cx, std::int64_t cy) { return cx * 1000003 + cy; };
        for (std::size_t i = 0; i < n; ++i) {
            if (ps[i].kind == Kind::Whole) continue;
            grid[cellKey(std::int64_t(std::floor(ps[i].x / kCell)), std::int64_t(std::floor(ps[i].y / kCell)))].push_back(int(i));
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (ps[i].kind == Kind::Whole) continue;
            const auto cx = std::int64_t(std::floor(ps[i].x / kCell)), cy = std::int64_t(std::floor(ps[i].y / kCell));
            for (std::int64_t gx = cx - 1; gx <= cx + 1; ++gx)
                for (std::int64_t gy = cy - 1; gy <= cy + 1; ++gy) {
                    auto it = grid.find(cellKey(gx, gy));
                    if (it == grid.end()) continue;
                    for (int j : it->second) {
                        if (std::size_t(j) <= i || ps[std::size_t(j)].family != ps[i].family) continue;
                        if (std::hypot(ps[std::size_t(j)].x - ps[i].x, ps[std::size_t(j)].y - ps[i].y) > kJoin) continue;
                        const int a = root(int(i)), b = root(j);
                        if (a != b) up[std::size_t(std::max(a, b))] = std::min(a, b);
                    }
                }
        }
        house_.assign(n, -1);
        houseInfo_.clear();
        std::unordered_map<int, int> index;
        for (std::size_t i = 0; i < n; ++i) {
            const int r = root(int(i));
            auto [it, fresh] = index.emplace(r, int(houseInfo_.size()));
            if (fresh) houseInfo_.push_back({});
            house_[i] = it->second;
            const Piece& p = ps[i];
            if (p.kind == Kind::Floor || p.kind == Kind::Roof) {
                House& h = houseInfo_[std::size_t(it->second)];
                h.cx += p.x, h.cy += p.y, ++h.cells;
            }
        }
        for (House& h : houseInfo_)
            if (h.cells) h.cx /= h.cells, h.cy /= h.cells;
        out_.assign(n, {0.0, 0.0});
        for (std::size_t i = 0; i < n; ++i) {
            const Piece& p = ps[i];
            const House& h = houseInfo_[std::size_t(house_[i])];
            if (!h.cells) continue;
            const double ox = p.x - h.cx, oy = p.y - h.cy;
            if (p.kind == Kind::Wall) {
                const double s = ox * p.nx + oy * p.ny;
                if (std::fabs(s) > 0.2) out_[i] = {s > 0 ? p.nx : -p.nx, s > 0 ? p.ny : -p.ny};
            } else if (p.kind == Kind::Pillar) {
                const double l = std::hypot(ox, oy);
                if (l > 0.3) out_[i] = {ox / l, oy / l};
            }
        }
    }

    // Is a focus inside house h, under one of its floors or roofs?
    bool inside(const std::vector<Piece>& ps, int h, const Focus& f) const {
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (house_[i] != h) continue;
            const Piece& p = ps[i];
            if ((p.kind == Kind::Floor || p.kind == Kind::Roof) && p.z0 > f.z + 1.0 && under(p, f.x, f.y, 0.1)) return true;
        }
        return false;
    }

    static void seen(const Focus& f, double o[3]) {
        o[0] = f.x, o[1] = f.y, o[2] = f.z + (f.person ? 1.2 : 0.3);
    }

    Look target(Mode mode, const std::vector<Piece>& ps, std::size_t i, const double cam[3],
                const std::vector<Focus>& foci) const {
        const Piece& p = ps[i];
        const Look full{1, 0}, gone{0, kKeepNone};
        const Look cut{0, p.kind == Kind::Whole ? kWholeStub : p.kind == Kind::Wall || p.kind == Kind::Pillar ? kWallStub : kKeepNone};
        const bool standing = p.kind == Kind::Wall || p.kind == Kind::Pillar;
        // upstairs of the storey looked at
        if (mode != Mode::Up && !foci.empty() && p.kind != Kind::Whole && p.z0 > foci.front().z + kAboveFeet) return gone;
        if (mode == Mode::Up && p.kind != Kind::Whole)
            for (const Focus& f : foci)
                if (p.z0 > f.z + kAboveFeet && inside(ps, house_[i], f)) return gone;
        // in the way of what the view is about
        for (const Focus& f : foci) {
            double s[3];
            seen(f, s);
            if (occludes(p, s, cam)) return cut;
        }
        if (p.kind == Kind::Whole) {
            if (mode == Mode::Down) return cut;
            for (const Focus& f : foci)
                if (under(p, f.x, f.y, mode == Mode::Cut ? 1.0 : 0.0) && f.z < p.z1) return cut;
            return full;
        }
        if (mode == Mode::Down && standing) return cut;
        if (mode == Mode::Cut && standing) {
            const auto& o = out_[i];
            const double tx = cam[0] - p.x, ty = cam[1] - p.y, tl = std::hypot(tx, ty);
            if (tl > 1e-6 && (o[0] * tx + o[1] * ty) / tl > 0.1) return cut;
        }
        return full;
    }

    void ease(const std::vector<Piece>& ps, double dt) {
        ++frame_;
        looks_.resize(ps.size());
        const float step = float(std::clamp(dt, 0.0, 0.25) * kFadeRate);
        for (std::size_t i = 0; i < ps.size(); ++i) {
            const Look t = targets_[i];
            auto [it, fresh] = faded_.try_emplace(ps[i].key, Faded{t, frame_});
            Faded& f = it->second;
            if (!fresh) {
                // a cut back up keeps the stub it had while it comes back
                if (t.vis < 1) f.look.keep = t.keep;
                f.look.vis = f.look.vis < t.vis ? std::min(t.vis, f.look.vis + step) : std::max(t.vis, f.look.vis - step);
                // a frame with nothing faded away (all eased in): drop the stale keep for good
                if (f.look.vis >= 1) f.look.keep = t.keep;
            }
            f.seen = frame_;
            looks_[i] = f.look;
        }
        if (frame_ % 600 == 0)
            for (auto it = faded_.begin(); it != faded_.end();)
                it = frame_ - it->second.seen > 600 ? faded_.erase(it) : std::next(it);
    }
};

} // namespace altera::game::cutaway
