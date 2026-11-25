#include "native/pixels2d.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <functional>

namespace game2d {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Rgba pack(int r, int g, int b) {
    auto cl = [](int v) { return std::uint32_t(std::clamp(v, 0, 255)); };
    return 0xff000000u | cl(r) << 16 | cl(g) << 8 | cl(b);
}
int red(Rgba c) { return int(c >> 16 & 0xff); }
int green(Rgba c) { return int(c >> 8 & 0xff); }
int blue(Rgba c) { return int(c & 0xff); }

Rgba mixRgb(Rgba a, Rgba b, double k) {
    return pack(int(red(a) * (1 - k) + red(b) * k), int(green(a) * (1 - k) + green(b) * k),
                int(blue(a) * (1 - k) + blue(b) * k));
}

// --- the pawn generator (pawns.js) -------------------------------------------
constexpr int N = kPawn;
using Mask = std::bitset<N * N>;
int key(int x, int y) { return y * N + x; }
bool inside(int x, int y) { return x >= 0 && y >= 0 && x < N && y < N; }
const Rgba OUT = 0xff1a1410u;
const Rgba WHITE = 0xffffffffu;

Mask ell(double cx, double cy, double rx, double ry) {
    Mask s;
    for (int x = 0; x < N; ++x)
        for (int y = 0; y < N; ++y) {
            const double a = (x + 0.5 - cx) / rx, b = (y + 0.5 - cy) / ry;
            if (a * a + b * b <= 1) s.set(std::size_t(key(x, y)));
        }
    return s;
}
Mask filter(const Mask& s, const std::function<bool(int, int)>& f) {
    Mask o;
    for (int k = 0; k < N * N; ++k)
        if (s.test(std::size_t(k)) && f(k % N, k / N)) o.set(std::size_t(k));
    return o;
}
Mask one(int x, int y) {
    Mask m;
    if (inside(x, y)) m.set(std::size_t(key(x, y)));
    return m;
}
Mask pts(std::initializer_list<std::pair<int, int>> list) {
    Mask m;
    for (auto [x, y] : list)
        if (inside(x, y)) m.set(std::size_t(key(x, y)));
    return m;
}

struct Canvas {
    std::array<Rgba, N * N> g{};   // 0: empty
    void layer(const Mask& m, Rgba col, const std::function<Rgba(int, int, Rgba)>& pattern = nullptr,
               bool outline = true) {
        auto has = [&](int x, int y) { return inside(x, y) && m.test(std::size_t(key(x, y))); };
        if (outline) {
            Mask ring;
            for (int k = 0; k < N * N; ++k) {
                if (!m.test(std::size_t(k))) continue;
                const int x = k % N, y = k / N;
                const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& dd : d) {
                    const int qx = x + dd[0], qy = y + dd[1];
                    if (!inside(qx, qy)) continue;
                    if (!m.test(std::size_t(key(qx, qy)))) ring.set(std::size_t(key(qx, qy)));
                }
            }
            for (int q = 0; q < N * N; ++q) {
                if (!ring.test(std::size_t(q))) continue;
                const Rgba u = g[std::size_t(q)];
                g[std::size_t(q)] = (u == 0 || u == OUT) ? OUT : mulRgb(u, 0.55);
            }
        }
        for (int k = 0; k < N * N; ++k) {
            if (!m.test(std::size_t(k))) continue;
            const int x = k % N, y = k / N;
            Rgba c = col;
            if (pattern) {
                const Rgba p = pattern(x, y, col);
                if (p) c = p;
            }
            if (x + 1 >= N || y + 1 >= N || !has(x + 1, y + 1)) c = mulRgb(c, 0.78);
            else if (x == 0 || y == 0 || !has(x - 1, y - 1)) c = mixRgb(c, WHITE, 0.18);
            g[std::size_t(k)] = c;
        }
    }
    void px(int x, int y, Rgba c) {
        if (inside(x, y)) g[std::size_t(key(x, y))] = c;
    }
};

Rgba skinOf(const std::string& s) {
    if (!s.empty() && s[0] == '#') return parseHex(s); // AL-114: a kind's own skin (orc green, troll grey)
    if (s == "mid") return parseHex("#c88a5c");
    if (s == "dark") return parseHex("#8a5a3a");
    return parseHex("#eab48a");
}

std::array<Rgba, N * N> pawn(const PawnSpec& spec, char d, const PawnPose& pose, bool child) {
    Canvas c;
    const int bob = pose.bob;
    const Rgba sk = skinOf(spec.skin);
    double hcx, hcy, hr, bcx, bcy, brx, bry;
    if (child) {
        hcx = 10, hcy = 9.5, hr = 3.7, bcx = 10, bcy = 15.6, brx = 3.8, bry = 3.9;
    } else {
        hcx = 10, hcy = 6.6, hr = 4.3, bcx = 10, bcy = 14;
        brx = 5, bry = 5.2;
        if (spec.body == "thin") brx = 4.2, bry = 5.2;
        else if (spec.body == "fat") brx = 6.4, bry = 5.4;
        else if (spec.body == "fem") brx = 4.6, bry = 5.2;
        // LR-139: the kind's build: a dwarf squat and low, an elf slim and tall, an ogre hunched
        const std::string& b = spec.build;
        if (b == "stocky") brx *= 1.2, bry *= 0.8, bcy += 1.2, hcy += 2.4;
        else if (b == "slim") brx *= 0.86, bry *= 1.06, hcy -= 0.4;
        else if (b == "broad") brx *= 1.14, hcy += 0.3;
        else if (b == "hulking") brx *= 1.24, bry *= 1.02, hcy += 1.4;
        else if (b == "wiry") brx *= 0.84, bry *= 0.9, bcy += 0.5, hcy += 0.6;
    }
    // LR-139: the shape of the head
    double hrx = hr, hry = hr;
    if (spec.head == "wide") hrx = hr * 1.14, hry = hr * 0.92;
    else if (spec.head == "long") hrx = hr * 0.9, hry = hr * 1.08;
    else if (spec.head == "small") hrx = hry = hr * 0.8;
    else if (spec.head == "big") hrx = hry = hr * 1.12;
    if (d == 'E') brx *= 0.82, hcx += 0.7;
    if (d == 'E' && spec.build == "hulking") hcx += 0.8;   // the head thrust forward
    hcy -= bob;
    bcy -= bob;
    hcy += pose.dip;
    const Mask body = ell(bcx, bcy, brx, bry);
    // LR-139: long hair falls behind the shoulders (drawn before the body, so the body hides it below them)
    {
        const std::string hk = spec.hair.first;
        const Rgba hcB = parseHex(spec.hair.second.empty() ? "#5a3a20" : spec.hair.second);
        if (hk == "long" && d != 'N') c.layer(ell(hcx - (d == 'E' ? 1.0 : 0), hcy + 2.4, (d == 'E' ? hrx : hrx + 1.1), hry + 2.6), hcB);
        if (hk == "ponytail" && d == 'E') c.layer(pts({{int(std::trunc(hcx - hrx)), int(std::trunc(hcy))}, {int(std::trunc(hcx - hrx)) - 1, int(std::trunc(hcy)) + 1},
                                                        {int(std::trunc(hcx - hrx)) - 1, int(std::trunc(hcy)) + 2}, {int(std::trunc(hcx - hrx)), int(std::trunc(hcy)) + 3}}), hcB);
    }
    const bool cloak = !spec.cloak.empty();
    const Rgba cloakCol = cloak ? parseHex(spec.cloak) : 0;
    if (cloak && (d == 'S' || d == 'E')) c.layer(ell(bcx - (d == 'E' ? 0.6 : 0), bcy - 0.3, brx + 1.2, bry + 0.6), cloakCol);
    if (spec.skinPat == "warts") c.layer(body, sk, [](int x, int y, Rgba cc) { return (x * 7 + y * 3) % 11 == 0 ? mulRgb(cc, 0.78) : Rgba(0); });
    else c.layer(body, sk);
    // LR-139: feet under the body, bare or shod; walking they step in turn
    {
        Rgba fc = mulRgb(sk, 0.8);
        for (const auto& [kind, colHex] : spec.wear)
            if (kind == "boots") fc = parseHex(colHex);
        const int fy = std::min(N - 1, int(std::trunc(bcy + bry - 0.2)));
        const int bx = int(std::trunc(bcx));
        const int st = pose.step;
        if (d == 'E') {
            if (st == 1) c.layer(pts({{bx - 2, fy}, {bx + 2, fy}}), fc, nullptr, false);
            else if (st == 3) c.layer(pts({{bx, fy}}), fc, nullptr, false);
            else c.layer(pts({{bx - 1, fy}, {bx + 1, fy}}), fc, nullptr, false);
        } else {
            if (st != 1) c.layer(one(bx - 2, fy), fc, nullptr, false);
            if (st != 3) c.layer(one(bx + 1, fy), fc, nullptr, false);
        }
    }
    auto robeMask = [&] { return body | ell(bcx, bcy + 1.5, brx + 0.8, bry - 0.4); };
    for (const auto& [kind, colHex] : spec.wear) {
        const Rgba col = parseHex(colHex);
        if (kind == "shirt") c.layer(body, col);
        if (kind == "pants") c.layer(filter(body, [&](int, int y) { return y >= bcy + 2.2; }), col);
        if (kind == "robe") c.layer(robeMask(), col);
        if (kind == "mail") c.layer(body, col, [](int x, int y, Rgba cc) { return (x + y) % 2 ? mulRgb(cc, 0.8) : Rgba(0); });
        if (kind == "belt") {
            const int t = int(std::trunc(bcy));
            c.layer(filter(robeMask(), [&](int, int y) { return t <= y && y <= t; }), col, nullptr, false);
        }
        if (kind == "apron" && d != 'N') {
            c.layer(filter(body, [&](int x, int y) {
                        const bool fx = d == 'S' ? std::abs(x + 0.5 - bcx) <= 2.6 : x + 0.5 >= bcx + brx - 2.4;
                        return fx && y >= bcy - 2;
                    }),
                    col);
        }
        if (kind == "tabard" && d != 'N') {
            c.layer(filter(body, [&](int x, int) {
                        return d == 'S' ? std::abs(x + 0.5 - bcx) <= 2.2 : x + 0.5 >= bcx + brx - 2;
                    }),
                    col);
        }
        // AL-109: boots at the bottom of the body, a satchel on the hip, a pack on the back
        if (kind == "boots") c.layer(filter(body, [&](int, int y) { return y + 0.5 >= bcy + bry - 1.6; }), col, nullptr, false);
        const int bx = int(std::trunc(bcx)), by = int(std::trunc(bcy));
        if (kind == "bag") {
            if (d == 'S') {
                c.layer(pts({{bx - 2, by - 4}, {bx - 1, by - 3}, {bx, by - 2}, {bx + 1, by - 1}}), mulRgb(col, 0.7), nullptr, false);
                c.layer(pts({{bx + 2, by}, {bx + 3, by}, {bx + 2, by + 1}, {bx + 3, by + 1}}), col);
            } else if (d == 'E') c.layer(pts({{bx - 1, by}, {bx, by}, {bx - 1, by + 1}, {bx, by + 1}}), col);
            else c.layer(pts({{bx - 3, by}, {bx - 2, by}, {bx - 3, by + 1}, {bx - 2, by + 1}}), col);
        }
        if (kind == "pack") {
            if (d == 'N') {
                Mask m;
                for (int y = by - 3; y <= by + 1; ++y)
                    for (int x = bx - 2; x <= bx + 1; ++x) m |= one(x, y);
                c.layer(m, col);
            } else if (d == 'E') {
                Mask m;
                for (int y = by - 3; y <= by + 1; ++y) m |= one(int(std::trunc(bcx - brx)), y) | one(int(std::trunc(bcx - brx)) + 1, y);
                c.layer(m, col);
            } else c.layer(pts({{bx - 2, by - 4}, {bx - 2, by - 3}, {bx + 1, by - 4}, {bx + 1, by - 3}}), mulRgb(col, 0.7), nullptr, false);
        }
    }
    if (cloak && d == 'N') c.layer(ell(bcx, bcy - 0.3, brx + 1.2, bry + 0.6), cloakCol);
    if (cloak && d == 'S') {
        const Rgba gold = parseHex("#e8c040");
        c.px(int(std::trunc(bcx)) - 1, int(std::trunc(bcy - bry)) + 1, gold);
        c.px(int(std::trunc(bcx)), int(std::trunc(bcy - bry)) + 1, gold);
    }
    // hands (LR-139: they swing in turn walking; at work the tool hand goes up and down)
    const int hy = int(std::trunc(bcy + 1)) - 2 * pose.swing;
    const int sw = pose.step == 1 ? 1 : pose.step == 3 ? -1 : 0;
    if (d == 'S' || d == 'N') {
        c.layer(one(int(std::trunc(bcx - brx)) - 1, int(std::trunc(bcy + 1)) + sw), sk);
        c.layer(one(int(std::trunc(bcx + brx)), hy - sw), sk);
    } else c.layer(one(int(std::trunc(bcx + brx)) - 1 + sw, hy), sk);
    // tool
    const std::string& tool = spec.tool;
    int tx = int(std::trunc(bcx + brx)) + (d != 'E' ? 1 : 0);
    const int ty = hy;
    if (d == 'N') tx = int(std::trunc(bcx - brx)) - 2;
    const Rgba wood = parseHex("#7a4a28"), steel = parseHex("#c8ccd0");
    if (tool == "spear") {
        Mask m;
        for (int y = 0; y < ty + 4; ++y) m |= one(tx, y);
        c.layer(m, wood);
        c.layer(pts({{tx, 0}, {tx, 1}}), steel, nullptr, false);
    }
    if (tool == "hammer") {
        c.layer(pts({{tx, ty - 1}, {tx, ty}, {tx, ty + 1}}), wood);
        c.layer(pts({{tx - 1, ty - 3}, {tx, ty - 3}, {tx + 1, ty - 3}, {tx - 1, ty - 2}, {tx, ty - 2}, {tx + 1, ty - 2}}),
                parseHex("#8a8e94"));
    }
    if (tool == "staff") {
        Mask m;
        for (int y = int(std::trunc(hcy)) - 2; y < N - 1; ++y) m |= one(tx, y);
        c.layer(m, parseHex("#6a4a28"));
        c.layer(one(tx, int(std::trunc(hcy)) - 3), parseHex("#c8a848"));
    }
    if (tool == "bread")
        c.layer(pts({{tx - 1, ty - 1}, {tx, ty - 1}, {tx + 1, ty - 1}, {tx - 1, ty}, {tx, ty}, {tx + 1, ty}}), parseHex("#d89848"));
    if (tool == "sickle") {
        c.layer(pts({{tx, ty}, {tx, ty + 1}}), wood);
        c.layer(pts({{tx, ty - 1}, {tx + 1, ty - 2}, {tx + 1, ty - 3}, {tx, ty - 4}}), steel);
    }
    // AL-109: the tool of the act in hand
    auto stick = [&](int y0, int y1, Rgba col) {
        Mask m;
        for (int y = y0; y <= y1; ++y) m |= one(tx, y);
        c.layer(m, col);
    };
    const Rgba iron = parseHex("#8a8e94"), straw = parseHex("#c8a050");
    if (tool == "axe") {
        stick(ty - 2, ty + 1, wood);
        c.layer(pts({{tx + 1, ty - 3}, {tx + 1, ty - 2}, {tx + 1, ty - 1}, {tx + 2, ty - 2}}), steel);
    }
    if (tool == "knife") {
        c.layer(one(tx, ty), wood);
        c.layer(pts({{tx, ty - 1}, {tx, ty - 2}}), steel, nullptr, false);
    }
    if (tool == "sword") {
        stick(ty - 7, ty - 1, steel);
        c.layer(pts({{tx - 1, ty}, {tx, ty}, {tx + 1, ty}}), parseHex("#b89040"), nullptr, false);
        c.layer(one(tx, ty + 1), wood, nullptr, false);
    }
    if (tool == "bow") {
        c.layer(pts({{tx, ty - 4}, {tx + 1, ty - 3}, {tx + 1, ty - 2}, {tx + 1, ty - 1}, {tx + 1, ty}, {tx + 1, ty + 1}, {tx, ty + 2}}), wood);
        Mask m;
        for (int y = ty - 3; y <= ty + 1; ++y) m |= one(tx, y);
        c.layer(m, parseHex("#e8e0c8"), nullptr, false);
    }
    if (tool == "stick") stick(ty - 3, ty + 3, wood);
    if (tool == "pick") {
        stick(ty - 3, ty + 1, wood);
        c.layer(pts({{tx - 1, ty - 3}, {tx, ty - 4}, {tx + 1, ty - 3}}), iron);
    }
    if (tool == "spade") {
        stick(ty - 4, ty + 1, wood);
        c.layer(pts({{tx - 1, ty + 2}, {tx, ty + 2}, {tx + 1, ty + 2}, {tx - 1, ty + 3}, {tx, ty + 3}, {tx + 1, ty + 3}}), iron);
    }
    if (tool == "fork") {
        stick(ty - 4, ty + 3, wood);
        c.layer(pts({{tx - 1, ty - 5}, {tx + 1, ty - 5}, {tx - 1, ty - 6}, {tx + 1, ty - 6}, {tx, ty - 5}}), iron, nullptr, false);
    }
    if (tool == "rod") {
        c.layer(pts({{tx, ty + 1}, {tx, ty}, {tx, ty - 1}, {tx, ty - 2}, {tx + 1, ty - 3}, {tx + 1, ty - 4}, {tx + 1, ty - 5},
                     {tx + 2, ty - 6}, {tx + 2, ty - 7}}),
                wood, nullptr, false);
        c.layer(pts({{tx + 3, ty - 6}, {tx + 3, ty - 5}, {tx + 3, ty - 4}, {tx + 3, ty - 3}}), parseHex("#d8e0e8"), nullptr, false);
    }
    if (tool == "needle") c.layer(pts({{tx, ty - 1}, {tx, ty - 2}}), steel, nullptr, false);
    if (tool == "spindle") {
        stick(ty - 2, ty + 1, wood);
        c.layer(pts({{tx - 1, ty + 1}, {tx + 1, ty + 1}}), parseHex("#a06a38"), nullptr, false);
        c.layer(one(tx, ty - 3), parseHex("#e8e0d0"), nullptr, false);
    }
    if (tool == "saw") {
        c.layer(one(tx, ty), wood);
        c.layer(pts({{tx, ty - 1}, {tx, ty - 2}, {tx, ty - 3}, {tx, ty - 4}, {tx + 1, ty - 2}, {tx + 1, ty - 4}}), steel);
    }
    if (tool == "torch") {
        stick(ty - 1, ty + 1, wood);
        c.layer(one(tx, ty - 2), parseHex("#e87828"), nullptr, false);
        c.layer(one(tx, ty - 3), parseHex("#f8d848"), nullptr, false);
    }
    if (tool == "basket") c.layer(pts({{tx - 1, ty}, {tx, ty}, {tx + 1, ty}, {tx - 1, ty + 1}, {tx, ty + 1}, {tx + 1, ty + 1}}), straw,
                                  [](int x, int y, Rgba cc) { return (x + y) % 2 ? mulRgb(cc, 0.8) : Rgba(0); });
    if (tool == "bucket") {
        c.layer(pts({{tx - 1, ty}, {tx, ty}, {tx + 1, ty}, {tx - 1, ty + 1}, {tx, ty + 1}, {tx + 1, ty + 1}}), wood);
        c.layer(pts({{tx - 1, ty}, {tx, ty}, {tx + 1, ty}}), iron, nullptr, false);
    }
    // LR-139: the kind's ears, under the head so its outline shades them (and again over the hair)
    Mask ears;
    {
        const int ey = int(std::trunc(hcy));
        const int hl = int(std::trunc(hcx - hrx)), hrr = int(std::trunc(hcx + hrx));
        Mask ear;
        const std::string& e = spec.ears;
        if (d == 'S' || d == 'N') {
            if (e == "pointed") ear = pts({{hl - 1, ey}, {hl - 1, ey - 1}, {hl - 2, ey - 2}, {hl - 3, ey - 3}, {hrr, ey}, {hrr, ey - 1}, {hrr + 1, ey - 2}, {hrr + 2, ey - 3}});
            if (e == "half") ear = pts({{hl - 1, ey}, {hl - 1, ey - 1}, {hrr, ey}, {hrr, ey - 1}});
            if (e == "long") ear = pts({{hl - 1, ey}, {hl - 2, ey}, {hl - 3, ey - 1}, {hrr, ey}, {hrr + 1, ey}, {hrr + 2, ey - 1}});
            if (e == "big") ear = pts({{hl - 1, ey}, {hl - 1, ey + 1}, {hrr, ey}, {hrr, ey + 1}});
        } else {
            const int bx = int(std::trunc(hcx - 0.6));
            if (e == "pointed") ear = pts({{bx - 1, ey - 1}, {bx - 2, ey - 2}, {bx - 3, ey - 3}});
            if (e == "half") ear = pts({{bx - 1, ey - 1}, {bx - 2, ey - 2}});
            if (e == "long") ear = pts({{bx - 1, ey}, {bx - 2, ey - 1}, {bx - 3, ey - 1}, {bx - 4, ey - 2}});
            if (e == "big") ear = pts({{bx - 1, ey}, {bx - 1, ey + 1}, {bx - 2, ey}});
            if (int(hrx + 0.5) >= 4 && (e == "pointed" || e == "half")) ear |= one(bx - 4 + (e == "half"), ey - 3 + (e == "half"));
            ear &= ~ell(hcx, hcy, hrx * 0.95, hry);
        }
        if (ear.any()) c.layer(ear, sk);
        ears = ear;
    }
    // head
    const Mask head = ell(hcx, hcy, d != 'E' ? hrx : hrx * 0.95, hry);
    const std::string hair = spec.hair.first.empty() ? "short" : spec.hair.first;
    const Rgba hc = parseHex(spec.hair.second.empty() ? "#5a3a20" : spec.hair.second);
    if (spec.skinPat == "warts") c.layer(head, sk, [](int x, int y, Rgba cc) { return (x * 5 + y * 7) % 9 == 0 ? mulRgb(cc, 0.78) : Rgba(0); });
    else c.layer(head, sk);
    const double top = std::trunc(hcy - hry);
    const int faceY = int(std::trunc(hcy + 0.3));
    const int fx = int(std::trunc(hcx));
    // LR-139: the face of the kind: eyes (their colour, a blink), a heavy brow, a nose, tusks
    const Rgba eye = pose.blink ? mulRgb(sk, 0.6) : spec.eyes.empty() ? OUT : parseHex(spec.eyes);
    const Rgba dark = mulRgb(sk, 0.62), ivory = parseHex("#f0e8d0");
    if (d == 'S') {
        c.px(fx - 2, faceY, eye);
        c.px(fx + 1, faceY, eye);
        if (spec.brow == "heavy") c.layer(pts({{fx - 2, faceY - 1}, {fx - 1, faceY - 1}, {fx, faceY - 1}, {fx + 1, faceY - 1}}), dark, nullptr, false);
        if (spec.nose == "big" || spec.nose == "long") c.layer(pts({{fx - 1, faceY + 1}, {fx, faceY + 1}}), mulRgb(sk, 0.8), nullptr, false);
        if (spec.nose == "long") c.px(fx, faceY + 2, mulRgb(sk, 0.7));
        if (spec.tusks == "yes") c.layer(pts({{fx - 2, faceY + 2}, {fx + 1, faceY + 2}, {fx - 2, faceY + 1}, {fx + 1, faceY + 1}}), ivory, nullptr, false);
        else if (spec.tusks == "small") c.layer(pts({{fx - 2, faceY + 2}, {fx + 1, faceY + 2}}), ivory, nullptr, false);
    } else if (d == 'E') {
        c.px(fx + 2, faceY, eye);
        if (spec.brow == "heavy") c.layer(pts({{fx + 1, faceY - 1}, {fx + 2, faceY - 1}, {fx + 3, faceY - 1}}), dark, nullptr, false);
        const int nx = int(std::trunc(hcx + hrx * 0.95));
        if (spec.nose == "big") c.layer(one(nx, faceY + 1), sk);
        if (spec.nose == "long") c.layer(pts({{nx, faceY + 1}, {nx + 1, faceY + 1}, {nx + 1, faceY + 2}}), sk);
        if (!spec.tusks.empty()) c.layer(pts({{fx + 2, faceY + 2}, {fx + 2, faceY + (spec.tusks == "yes" ? 1 : 2)}}), ivory, nullptr, false);
    }
    if (spec.beard && d != 'N') {
        // LR-139: the beard's style
        const std::string bs = !spec.beardStyle.empty() ? spec.beardStyle : spec.longBeard ? "long" : "full";
        const Mask chin = filter(head, [&](int x, int y) { return y >= hcy + 1.2 && (d == 'S' || x + 0.5 >= hcx - 0.5); });
        const Mask lip = d == 'S' ? pts({{fx - 2, faceY + 1}, {fx - 1, faceY + 1}, {fx, faceY + 1}, {fx + 1, faceY + 1}})
                                  : pts({{fx + 1, faceY + 1}, {fx + 2, faceY + 1}});
        if (bs == "stubble") c.layer(chin, mixRgb(sk, hc, 0.45), [&](int x, int y, Rgba cc) { return (x + y) % 2 ? mixRgb(sk, hc, 0.25) : cc; }, false);
        else if (bs == "moustache") c.layer(lip, hc, nullptr, false);
        else if (bs == "goatee") {
            c.layer(lip, hc, nullptr, false);
            c.layer(d == 'S' ? pts({{fx - 1, faceY + 2}, {fx, faceY + 2}, {fx - 1, faceY + 3}, {fx, faceY + 3}}) : pts({{fx + 1, faceY + 2}, {fx + 1, faceY + 3}}), hc, nullptr, false);
        } else {
            Mask bm = chin;
            const bool lng = bs == "long" || bs == "braided";
            if (lng) bm |= ell(hcx + (d == 'E' ? 1.2 : 0), hcy + hry + 1.2, d == 'E' ? 1.8 : 2.6, 2.6);
            if (bs == "braided") bm |= ell(hcx + (d == 'E' ? 1.2 : 0), hcy + hry + 3.6, 1.0, 1.4);
            c.layer(bm, hc, bs == "braided" ? [](int, int y, Rgba cc) { return y % 2 ? mulRgb(cc, 0.75) : Rgba(0); } : std::function<Rgba(int, int, Rgba)>(nullptr), lng);
        }
        if (spec.tusks == "yes" && d == 'S') c.layer(pts({{fx - 2, faceY + 1}, {fx + 1, faceY + 1}}), ivory, nullptr, false);
    }
    if (hair == "tuft") c.layer(pts({{fx - 1, int(top)}, {fx, int(top) - 1}, {fx, int(top)}, {fx + 1, int(top)}}), hc);
    // LR-139: more cuts: cropped (a short top), mohawk (a ridge), curly (a mop), topknot, ponytail
    if (hair == "cropped") {
        const Mask m = d == 'N' ? filter(head, [&](int, int y) { return y + 0.5 <= hcy + 1.2; }) : filter(head, [&](int, int y) { return y + 0.5 <= hcy - 2.0; });
        c.layer(m, mixRgb(hc, sk, 0.25), nullptr, false);
    }
    if (hair == "mohawk") {
        Mask m = d == 'E' ? filter(ell(hcx - 0.4, hcy - 0.6, hrx + 0.4, hry + 0.6), [&](int, int y) { return y + 0.5 <= top + 2.0; })
                          : filter(ell(hcx, hcy - 0.6, hrx, hry + 0.9), [&](int x, int y) { return std::abs(x + 0.5 - hcx) <= 1.0 && (d == 'N' || y + 0.5 <= top + 1.5); });
        c.layer(m, hc);
    }
    if (hair == "curly") {
        const Mask m = d == 'N' ? ell(hcx, hcy - 0.3, hrx + 0.7, hry + 0.6)
                                : filter(ell(hcx - (d == 'E' ? 0.6 : 0), hcy - 0.5, hrx + 0.7, hry + 0.5),
                                         [&](int x, int y) { return y + 0.5 <= hcy - 0.9 || (d == 'E' ? x + 0.5 <= hcx - 1.0 : std::abs(x + 0.5 - hcx) >= hrx - 0.4) && y + 0.5 <= hcy + 1.0; });
        c.layer(m, hc, [](int x, int y, Rgba cc) { return (x + 2 * y) % 3 == 0 ? mulRgb(cc, 0.78) : Rgba(0); });
    }
    if (hair == "short" || hair == "long" || hair == "bun" || hair == "braids" || hair == "topknot" || hair == "ponytail") {
        Mask hm;
        if (d == 'S') hm = filter(head, [&](int, int y) { return y + 0.5 <= hcy - 1.1; });
        else if (d == 'E')
            hm = filter(head, [&](int x, int y) { return y + 0.5 <= hcy - 1.1 || (x + 0.5 <= hcx - 1.2 && y + 0.5 <= hcy + 2); });
        else hm = filter(head, [&](int, int y) { return y + 0.5 <= hcy + 2.6; }) | (hair != "short" ? head : Mask());
        if (hair == "long" && d == 'N') hm |= ell(hcx, hcy + 1.4, hrx + 0.9, hry + 1.6);
        // long hair frames the face down the cheeks
        if (hair == "long" && d == 'S') hm |= filter(head, [&](int x, int y) { return std::abs(x + 0.5 - hcx) >= hrx - 1.1 && y + 0.5 <= hcy + 2.2; });
        if (hair == "long" && d == 'E') hm |= filter(head, [&](int x, int y) { return x + 0.5 <= hcx + 0.4 && y + 0.5 <= hcy + 2.6; });
        if (hair == "bun") hm |= ell(hcx, top - 0.2, 1.8, 1.6);
        if (hair == "topknot") hm |= ell(hcx - (d == 'E' ? 0.6 : 0), top - 0.9, 1.1, 1.3);
        if (hair == "ponytail" && d == 'N') hm |= pts({{fx, int(std::trunc(hcy + hry)) - 1}, {fx, int(std::trunc(hcy + hry))}, {fx - 1, int(std::trunc(hcy + hry)) + 1}, {fx, int(std::trunc(hcy + hry)) + 2}});
        c.layer(hm, hc);
        // braids hang over the shoulders (LR-139: plaited, not muffs)
        if (hair == "braids") {
            const int y0 = int(std::trunc(hcy)) + 1;
            auto plait = [&](int x) {
                Mask m;
                for (int y = y0; y <= y0 + 4; ++y) m |= one(x, y);
                c.layer(m, hc, [](int, int y, Rgba cc) { return y % 2 ? mulRgb(cc, 0.72) : Rgba(0); });
            };
            if (d != 'E') {
                plait(int(std::trunc(hcx - hrx + 0.2)));
                plait(int(std::trunc(hcx + hrx - 1.2)));
            } else plait(int(std::trunc(hcx - hrx * 0.6)));
        }
    }
    if (ears.any() && hair != "none") c.layer(ears & ~head, sk, nullptr, false);   // ears through the hair
    const std::string& hat = spec.hat.first;
    const Rgba hcol = hat.empty() ? 0 : parseHex(spec.hat.second);
    if (hat == "hood") {
        Mask m = ell(hcx, hcy, hrx + 1, hry + 0.9) | ell(hcx, hcy + 3.5, hrx + 1.2, 2.2);
        if (d == 'S') m &= ~filter(head, [&](int x, int y) { return y + 0.5 >= hcy - 1.3 && std::abs(x + 0.5 - hcx) <= 2.8; });
        if (d == 'E') m &= ~filter(head, [&](int x, int y) { return y + 0.5 >= hcy - 1.3 && x + 0.5 >= hcx + 0.2; });
        c.layer(m, hcol);
    }
    if (hat == "helmet") {
        c.layer(filter(ell(hcx, hcy, hrx + 0.6, hry + 0.6), [&](int, int y) { return y + 0.5 <= hcy - 0.6; }), hcol);
        if (d == 'S') c.layer(one(int(std::trunc(hcx)), int(std::trunc(hcy))), hcol, nullptr, false);
    }
    if (hat == "straw") {
        c.layer(ell(hcx, top + 2, hrx + 2.6, 1.8), hcol, [](int x, int y, Rgba cc) { return (x + y) % 3 == 0 ? mulRgb(cc, 0.85) : Rgba(0); });
        c.layer(ell(hcx, top + 0.6, hrx - 1.2, 1.7), hcol);
    }
    if (hat == "kerchief") c.layer(filter(ell(hcx, hcy, hrx + 0.4, hry + 0.4), [&](int, int y) { return y + 0.5 <= hcy - 0.8 || d == 'N'; }), hcol);
    if (hat == "cap") c.layer(filter(ell(hcx, hcy - 0.6, hrx + 0.3, hry), [&](int, int y) { return y + 0.5 <= hcy - 1.4; }), hcol);
    return c.g;
}

} // namespace

Rgba parseHex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return 0xffff00ffu;
    int v[3];
    for (int i = 0; i < 3; ++i) {
        const int a = hexDigit(hex[std::size_t(1 + i * 2)]), b = hexDigit(hex[std::size_t(2 + i * 2)]);
        if (a < 0 || b < 0) return 0xffff00ffu;
        v[i] = a * 16 + b;
    }
    return pack(v[0], v[1], v[2]);
}

Rgba mulRgb(Rgba c, double k) { return pack(int(red(c) * k), int(green(c) * k), int(blue(c) * k)); }

void SpriteSet::load(const nlohmann::json& j) {
    pal_.clear();
    grids_.clear();
    const nlohmann::json pal = j.value("pal", nlohmann::json::object()), grids = j.value("sprites", nlohmann::json::object());
    for (const auto& [k, v] : pal.items())
        if (!k.empty() && v.is_string()) pal_[k[0]] = parseHex(v.get<std::string>());
    for (const auto& [k, v] : grids.items()) {
        std::vector<std::string> rows;
        if (v.is_array())
            for (const auto& r : v) rows.push_back(r.is_string() ? r.get<std::string>() : std::string());
        grids_[k] = std::move(rows);
    }
}

void SpriteSet::merge(const nlohmann::json& j) {
    const nlohmann::json grids = j.value("sprites", nlohmann::json::object());
    for (const auto& [k, v] : grids.items()) {
        std::vector<std::string> rows;
        if (v.is_array())
            for (const auto& r : v) rows.push_back(r.is_string() ? r.get<std::string>() : std::string());
        grids_[k] = std::move(rows);
    }
}

Rgba SpriteSet::colour(char letter) const {
    const auto it = pal_.find(letter);
    return it == pal_.end() ? 0xffff00ffu : it->second;
}

Pixels SpriteSet::get(const std::string& name, const std::map<char, char>& recolour) const {
    Pixels out;
    const auto it = grids_.find(name);
    if (it == grids_.end()) return out;
    out.size = 16;
    out.px.assign(256, 0);
    for (int y = 0; y < 16; ++y) {
        const std::string row = y < int(it->second.size()) ? it->second[std::size_t(y)] : std::string();
        for (int x = 0; x < 16 && x < int(row.size()); ++x) {
            char ch = row[std::size_t(x)];
            if (ch == '.' || ch == ' ') continue;
            if (const auto r = recolour.find(ch); r != recolour.end()) ch = r->second;
            out.px[std::size_t(y * 16 + x)] = colour(ch);
        }
    }
    return out;
}

std::string PawnSpec::key() const {
    std::string k = body + "|" + skin + "|" + (child ? "c" : "a") + "|" + hair.first + hair.second + "|" + (beard ? "b" : "") + "|";
    for (const auto& [a, b] : wear) k += a + b + ",";
    return k + "|" + cloak + "|" + hat.first + hat.second + "|" + tool + "|" + ears + head + "," + nose + brow + eyes + "," + tusks +
           build + skinPat + (longBeard ? "L" : "") + beardStyle + (naked ? "N" : "");
}

Pixels pawnPixels(const PawnSpec& spec, char side, int bob) {
    PawnPose pose;
    pose.bob = bob;
    return pawnPixels(spec, side, pose);
}

std::string PawnPose::key() const {
    return std::string(1, char('0' + bob)) + char('a' + step + 1) + char('0' + swing) + char('0' + dip) + (blink ? 'b' : '-');
}

PawnPose pawnPose(bool moving, double walkedM, bool working, double ms, long long id) {
    PawnPose p;
    const double phase = double((idHash(double(id < 0 ? 0 : id), 7) % 1000)) / 1000.0;
    if (moving) {
        p.step = int(std::floor(walkedM * 3.0)) & 3;   // a stride of two steps in 1.33 m
        p.bob = p.step & 1;
        return p;
    }
    const double s = ms / 1000.0 + phase * 4.0;
    if (working) {
        const int f = int(std::floor(s / 0.3)) & 1;   // a blow every 0.6 s
        p.swing = f;
        p.bob = f;
        return p;
    }
    p.dip = std::fmod(s, 2.6) < 0.9 ? 1 : 0;           // a slow breath
    p.blink = std::fmod(s + phase * 3.0, 3.7) < 0.14;  // and a blink now and then
    return p;
}

Pixels pawnPixels(const PawnSpec& spec, char side, const PawnPose& pose) {
    const auto g = pawn(spec, side == 'W' ? 'E' : side, pose, spec.child);
    Pixels out;
    out.size = N;
    out.px.assign(std::size_t(N * N), 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) out.px[std::size_t(y * N + x)] = g[std::size_t(key(side == 'W' ? N - 1 - x : x, y))];
    return out;
}

Pixels beastPixels(const std::string& sp, char face) {
    static const std::map<std::string, std::string> kColour = {
        {"hare", "#a88868"}, {"deer", "#8a5a30"},  {"boar", "#4a3a30"}, {"wolf", "#7a7a78"},
        {"hens", "#e8e0d0"}, {"goats", "#c8c0b0"}, {"pig", "#e0a090"},  {"fish_shoal", "#6aa8e0"}};
    const bool big = sp == "deer" || sp == "boar" || sp == "wolf" || sp == "pig" || sp == "goats";
    const auto it = kColour.find(sp);
    const Rgba col = parseHex(it == kColour.end() ? "#8a6a48" : it->second);
    const double rx = big ? 5 : 3.5, ry = big ? 3.2 : 2.5, cx = 8, cy = 10;
    Pixels out;
    out.size = 16;
    out.px.assign(256, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const double a = (x + 0.5 - cx) / rx, b = (y + 0.5 - cy) / ry, d = a * a + b * b;
            const double ah = (x + 0.5 - (cx + (face == 'W' ? -rx : rx))) / 1.8, bh = (y + 0.5 - (cy - 1.5)) / 1.6;
            const bool headIn = ah * ah + bh * bh <= 1;
            if (d <= 1 || headIn) out.px[std::size_t(y * 16 + x)] = d > 0.6 && !headIn ? mulRgb(col, 0.75) : col;
            else if (d <= 1.45 || ah * ah + bh * bh <= 1.6) out.px[std::size_t(y * 16 + x)] = OUT;
        }
    return out;
}

std::uint32_t idHash(double n, int salt) {
    const std::uint64_t v = std::uint64_t(n) * 2654435761ull + std::uint64_t(salt) * 40503ull;
    std::uint32_t h = std::uint32_t(v);
    h ^= h >> 15;
    h = h * 2246822519u;
    h ^= h >> 13;
    return h;
}

} // namespace game2d
