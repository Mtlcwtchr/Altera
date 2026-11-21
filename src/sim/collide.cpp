#include "sim/collide.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>

namespace altera::sim::collide {

namespace {

long long bucketKey(long long bx, long long by) { return (bx << 32) ^ (by & 0xffffffffll); }

// Overlap of a disc (x, y, r) with a disc at (cx, cy) of radius cr: the way out and how deep.
bool discOverlap(double x, double y, double r, double cx, double cy, double cr, double& nx, double& ny, double& depth) {
    const double dx = x - cx, dy = y - cy, d = std::hypot(dx, dy);
    const double dep = cr + r - d;
    if (dep <= 0) return false;
    if (d > 1e-9) {
        nx = dx / d;
        ny = dy / d;
    } else {
        nx = 1;
        ny = 0;
    }
    depth = dep;
    return true;
}

bool boxOverlap(double x, double y, double r, const Box& b, double& nx, double& ny, double& depth) {
    const double c = std::cos(b.yaw), s = std::sin(b.yaw);
    const double wx = x - b.x, wy = y - b.y;
    const double lx = c * wx + s * wy, ly = -s * wx + c * wy;   // into the box's frame
    double ox, oy, dep;
    if (std::fabs(lx) <= b.hx && std::fabs(ly) <= b.hy) {
        // inside: out through the nearest face
        const double fx = b.hx - std::fabs(lx), fy = b.hy - std::fabs(ly);
        if (fx < fy) {
            ox = lx >= 0 ? 1 : -1;
            oy = 0;
            dep = fx + r;
        } else {
            ox = 0;
            oy = ly >= 0 ? 1 : -1;
            dep = fy + r;
        }
    } else {
        const double qx = std::clamp(lx, -b.hx, b.hx), qy = std::clamp(ly, -b.hy, b.hy);
        const double ex = lx - qx, ey = ly - qy, d = std::hypot(ex, ey);
        if (d >= r) return false;
        ox = ex / d;
        oy = ey / d;
        dep = r - d;
    }
    nx = c * ox - s * oy;   // back to the world
    ny = s * ox + c * oy;
    depth = dep;
    return true;
}

bool wallOverlap(double x, double y, double r, const Wall& w, double& nx, double& ny, double& depth) {
    const double vx = w.bx - w.ax, vy = w.by - w.ay, l2 = vx * vx + vy * vy;
    double t = l2 > 1e-12 ? ((x - w.ax) * vx + (y - w.ay) * vy) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double px = w.ax + vx * t, py = w.ay + vy * t;
    if (!discOverlap(x, y, r, px, py, w.half, nx, ny, depth)) return false;
    if (std::hypot(x - px, y - py) < 1e-9 && l2 > 1e-12) {   // on the line: out to the side
        const double l = std::sqrt(l2);
        nx = -vy / l;
        ny = vx / l;
    }
    return true;
}

} // namespace

void Obstacles::clear() {
    circles_.clear();
    boxes_.clear();
    walls_.clear();
    grid_.clear();
}

void Obstacles::index(int kind, int i, double minX, double minY, double maxX, double maxY) {
    const long long x0 = (long long)std::floor(minX / kBucket), x1 = (long long)std::floor(maxX / kBucket);
    const long long y0 = (long long)std::floor(minY / kBucket), y1 = (long long)std::floor(maxY / kBucket);
    for (long long bx = x0; bx <= x1; ++bx)
        for (long long by = y0; by <= y1; ++by) grid_[bucketKey(bx, by)].push_back({kind, i});
}

void Obstacles::add(const Circle& c) {
    circles_.push_back(c);
    index(0, int(circles_.size()) - 1, c.x - c.r, c.y - c.r, c.x + c.r, c.y + c.r);
}

void Obstacles::add(const Box& b) {
    boxes_.push_back(b);
    const double c = std::fabs(std::cos(b.yaw)), s = std::fabs(std::sin(b.yaw));
    const double ex = c * b.hx + s * b.hy, ey = s * b.hx + c * b.hy;
    index(1, int(boxes_.size()) - 1, b.x - ex, b.y - ey, b.x + ex, b.y + ey);
}

void Obstacles::add(const Wall& w) {
    walls_.push_back(w);
    index(2, int(walls_.size()) - 1, std::min(w.ax, w.bx) - w.half, std::min(w.ay, w.by) - w.half,
          std::max(w.ax, w.bx) + w.half, std::max(w.ay, w.by) + w.half);
}

template <class F>
void Obstacles::near(double x, double y, double r, F&& f) const {
    if (grid_.empty()) return;
    const long long x0 = (long long)std::floor((x - r) / kBucket), x1 = (long long)std::floor((x + r) / kBucket);
    const long long y0 = (long long)std::floor((y - r) / kBucket), y1 = (long long)std::floor((y + r) / kBucket);
    std::pair<int, int> seen[64];
    int nSeen = 0;
    for (long long bx = x0; bx <= x1; ++bx)
        for (long long by = y0; by <= y1; ++by) {
            const auto it = grid_.find(bucketKey(bx, by));
            if (it == grid_.end()) continue;
            for (const auto& e : it->second) {
                bool dup = false;
                for (int k = 0; k < nSeen && !dup; ++k) dup = seen[k] == e;
                if (dup) continue;
                if (nSeen < 64) seen[nSeen++] = e;
                f(e.first, e.second);
            }
        }
}

bool Obstacles::deepest(double x, double y, double r, double& nx, double& ny, double& depth) const {
    depth = 0;
    bool any = false;
    near(x, y, r, [&](int kind, int i) {
        double ox = 0, oy = 0, d = 0;
        bool hit = false;
        if (kind == 0) hit = discOverlap(x, y, r, circles_[i].x, circles_[i].y, circles_[i].r, ox, oy, d);
        else if (kind == 1) hit = boxOverlap(x, y, r, boxes_[i], ox, oy, d);
        else hit = wallOverlap(x, y, r, walls_[i], ox, oy, d);
        if (hit && d > depth) {
            depth = d;
            nx = ox;
            ny = oy;
            any = true;
        }
    });
    return any && depth > 1e-7;
}

bool Obstacles::blocked(double x, double y, double r) const {
    if (walkable && !walkable(x, y)) return true;
    double nx, ny, d;
    return deepest(x, y, r, nx, ny, d);
}

bool Obstacles::pushOut(double& x, double& y, double r) const {
    bool moved = false;
    for (int it = 0; it < 10; ++it) {
        double nx, ny, d;
        if (!deepest(x, y, r, nx, ny, d)) break;
        x += nx * (d + 1e-4);
        y += ny * (d + 1e-4);
        moved = true;
    }
    return moved;
}

bool Obstacles::slide(double& x, double& y, double dx, double dy, double r) const {
    const double len = std::hypot(dx, dy);
    if (len < 1e-12) return false;
    // In steps no longer than half the radius, so a thin wall is never jumped.
    const int steps = std::max(1, int(std::ceil(len / std::max(0.05, r * 0.5))));
    const double sx = dx / steps, sy = dy / steps, sl = len / steps;
    // Off the ground one may stand on already (put there by the sim): any way out is let be.
    const bool groundCheck = walkable && walkable(x, y);
    bool movedAny = false;
    for (int k = 0; k < steps; ++k) {
        const double tries[3][2] = {{sx, sy}, {sx, 0}, {0, sy}};
        bool moved = false;
        for (const auto& t : tries) {
            if (std::fabs(t[0]) + std::fabs(t[1]) < 1e-12) continue;
            double cx = x + t[0], cy = y + t[1];
            pushOut(cx, cy, r);
            const double step = std::hypot(cx - x, cy - y);
            if (step < 1e-6 || step > sl * 1.5 + 0.02) continue;   // stuck, or thrown through a thing
            double nx, ny, d;
            if (deepest(cx, cy, r, nx, ny, d)) continue;
            if (groundCheck && !walkable(cx, cy)) continue;
            x = cx;
            y = cy;
            moved = true;
            break;
        }
        if (!moved) break;
        movedAny = true;
    }
    return movedAny;
}

bool Obstacles::clearLine(double fx, double fy, double tx, double ty, double r) const {
    const double len = std::hypot(tx - fx, ty - fy);
    const int n = std::max(1, int(std::ceil(len / std::max(0.1, r * 0.5))));
    for (int k = 1; k <= n; ++k) {
        const double t = double(k) / n;
        if (blocked(fx + (tx - fx) * t, fy + (ty - fy) * t, r)) return false;
    }
    return true;
}

double Obstacles::sightHit(double fx, double fy, double fz, double tx, double ty, double tz, double r,
                           double ground) const {
    const double len = std::hypot(tx - fx, ty - fy);
    const int n = std::max(1, int(std::ceil(len / std::max(0.05, r * 0.5))));
    bool leaving = true;   // still inside what the line starts in
    for (int k = 0; k <= n; ++k) {
        const double t = double(k) / n;
        const double x = fx + (tx - fx) * t, y = fy + (ty - fy) * t, z = fz + (tz - fz) * t;
        bool hit = false;
        near(x, y, r, [&](int kind, int i) {
            if (hit) return;
            double ox = 0, oy = 0, d = 0;
            if (kind == 0) hit = z - r < ground + circles_[i].top && discOverlap(x, y, r, circles_[i].x, circles_[i].y, circles_[i].r, ox, oy, d);
            else if (kind == 1) hit = z - r < ground + boxes_[i].top && boxOverlap(x, y, r, boxes_[i], ox, oy, d);
            else hit = z - r < ground + walls_[i].top && wallOverlap(x, y, r, walls_[i], ox, oy, d);
            hit = hit && d > 1e-7;
        });
        if (!hit) leaving = false;
        else if (!leaving) return double(k - 1) / n;
    }
    return 1.0;
}

bool Obstacles::path(double fx, double fy, double tx, double ty, double r, std::vector<Point>& out, double cell,
                     double margin, int maxNodes) const {
    out.clear();
    if (clearLine(fx, fy, tx, ty, r) && !blocked(tx, ty, r)) {
        out.push_back({tx, ty});
        return true;
    }
    const double minX = std::min(fx, tx) - margin, minY = std::min(fy, ty) - margin;
    const double spanX = std::fabs(tx - fx) + 2 * margin, spanY = std::fabs(ty - fy) + 2 * margin;
    // A grid of at most ~1.5M cells: a long way walks on a coarser one.
    cell = std::max(cell, std::sqrt(spanX * spanY / 1.5e6));
    const int W = int(std::ceil(spanX / cell)) + 1, H = int(std::ceil(spanY / cell)) + 1;
    auto cx = [&](int i) { return minX + (i + 0.5) * cell; };
    auto cy = [&](int j) { return minY + (j + 0.5) * cell; };
    std::vector<std::int8_t> freeC(std::size_t(W) * H, -1);   // -1 unknown, 0 blocked, 1 free
    auto isFree = [&](int i, int j) {
        if (i < 0 || j < 0 || i >= W || j >= H) return false;
        std::int8_t& f = freeC[std::size_t(j) * W + i];
        if (f < 0) f = blocked(cx(i), cy(j), r) ? 0 : 1;
        return f == 1;
    };
    const int si = std::clamp(int((fx - minX) / cell), 0, W - 1), sj = std::clamp(int((fy - minY) / cell), 0, H - 1);
    int gi = std::clamp(int((tx - minX) / cell), 0, W - 1), gj = std::clamp(int((ty - minY) / cell), 0, H - 1);
    bool goalExact = !blocked(tx, ty, r);
    if (!isFree(gi, gj)) {
        // the goal stands in a thing: the nearest free cell round it
        int best = -1;
        double bd = 1e300;
        const int R = int(std::ceil(6.0 / cell));
        for (int dj = -R; dj <= R; ++dj)
            for (int di = -R; di <= R; ++di)
                if (isFree(gi + di, gj + dj)) {
                    const double d = std::hypot(cx(gi + di) - tx, cy(gj + dj) - ty);
                    if (d < bd) bd = d, best = (gj + dj) * W + (gi + di);
                }
        if (best < 0) return false;
        gi = best % W;
        gj = best / W;
        goalExact = false;
    }
    const int start = sj * W + si, goal = gj * W + gi;
    std::vector<float> g(std::size_t(W) * H, 1e30f);
    std::vector<int> from(std::size_t(W) * H, -1);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    auto h = [&](int i, int j) {
        const double ax = std::fabs(i - gi), ay = std::fabs(j - gj);
        return float((std::max(ax, ay) + 0.41421356 * std::min(ax, ay)) * cell);
    };
    g[std::size_t(start)] = 0;
    open.push({h(si, sj), start});
    int looked = 0;
    bool found = false;
    while (!open.empty() && looked < maxNodes) {
        const auto [f, n] = open.top();
        open.pop();
        const int i = n % W, j = n / W;
        if (f - h(i, j) > g[std::size_t(n)] + 1e-3f) continue;   // stale
        ++looked;
        if (n == goal) {
            found = true;
            break;
        }
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                if (!di && !dj) continue;
                const int ni = i + di, nj = j + dj;
                if (!isFree(ni, nj)) continue;
                if (di && dj && (!isFree(i + di, j) || !isFree(i, j + dj))) continue;   // no corner cutting
                const float ng = g[std::size_t(n)] + float((di && dj ? 1.41421356 : 1.0) * cell);
                const int m = nj * W + ni;
                if (ng < g[std::size_t(m)]) {
                    g[std::size_t(m)] = ng;
                    from[std::size_t(m)] = n;
                    open.push({ng + h(ni, nj), m});
                }
            }
    }
    if (!found) return false;
    std::vector<Point> cells;
    for (int n = goal; n != start && n >= 0; n = from[std::size_t(n)]) cells.push_back({cx(n % W), cy(n / W)});
    std::reverse(cells.begin(), cells.end());
    if (goalExact) {
        if (cells.empty()) cells.push_back({tx, ty});
        else cells.back() = {tx, ty};
    }
    // String pulling: from each corner, the farthest point still seen straight.
    Point at{fx, fy};
    std::size_t k = 0;
    while (k < cells.size()) {
        std::size_t far = k;
        for (std::size_t m = cells.size(); m-- > k + 1;)
            if (clearLine(at.first, at.second, cells[m].first, cells[m].second, r)) {
                far = m;
                break;
            }
        out.push_back(cells[far]);
        at = cells[far];
        k = far + 1;
    }
    return !out.empty();
}

} // namespace altera::sim::collide
