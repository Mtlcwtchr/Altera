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
    if (s == "mid") return parseHex("#c88a5c");
    if (s == "dark") return parseHex("#8a5a3a");
    return parseHex("#eab48a");
}

std::array<Rgba, N * N> pawn(const PawnSpec& spec, char d, int bob, bool child) {
    Canvas c;
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
    }
    if (d == 'E') brx *= 0.82, hcx += 0.7;
    hcy -= bob;
    bcy -= bob;
    const Mask body = ell(bcx, bcy, brx, bry);
    const bool cloak = !spec.cloak.empty();
    const Rgba cloakCol = cloak ? parseHex(spec.cloak) : 0;
    if (cloak && (d == 'S' || d == 'E')) c.layer(ell(bcx - (d == 'E' ? 0.6 : 0), bcy - 0.3, brx + 1.2, bry + 0.6), cloakCol);
    c.layer(body, sk);
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
    // hands
    const int hy = int(std::trunc(bcy + 1));
    if (d == 'S' || d == 'N') {
        for (int hx : {int(std::trunc(bcx - brx)) - 1, int(std::trunc(bcx + brx))}) c.layer(one(hx, hy), sk);
    } else c.layer(one(int(std::trunc(bcx + brx)) - 1, hy), sk);
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
    // head
    const Mask head = ell(hcx, hcy, d != 'E' ? hr : hr * 0.95, hr);
    const std::string hair = spec.hair.first.empty() ? "short" : spec.hair.first;
    const Rgba hc = parseHex(spec.hair.second.empty() ? "#5a3a20" : spec.hair.second);
    if (hair == "long" && d != 'N')
        c.layer(filter(ell(hcx - (d == 'E' ? 0.5 : 0), hcy + 1.4, hr + 0.9, hr + 1.6),
                       [&](int x, int y) { return y >= hcy - 1 && (d == 'E' || std::abs(x + 0.5 - hcx) >= hr - 1.2); }),
                hc);
    c.layer(head, sk);
    const double top = std::trunc(hcy - hr);
    const int faceY = int(std::trunc(hcy + 0.3));
    if (d == 'S') {
        c.px(int(std::trunc(hcx)) - 2, faceY, OUT);
        c.px(int(std::trunc(hcx)) + 1, faceY, OUT);
    } else if (d == 'E') c.px(int(std::trunc(hcx)) + 2, faceY, OUT);
    if (spec.beard && d != 'N')
        c.layer(filter(head, [&](int x, int y) { return y >= hcy + 1.2 && (d == 'S' || x + 0.5 >= hcx - 0.5); }), hc, nullptr, false);
    if (hair == "short" || hair == "long" || hair == "bun" || hair == "braids") {
        Mask hm;
        if (d == 'S') hm = filter(head, [&](int, int y) { return y + 0.5 <= hcy - 1.1; });
        else if (d == 'E')
            hm = filter(head, [&](int x, int y) { return y + 0.5 <= hcy - 1.1 || (x + 0.5 <= hcx - 1.2 && y + 0.5 <= hcy + 2); });
        else hm = filter(head, [&](int, int y) { return y + 0.5 <= hcy + 2.6; }) | (hair != "short" ? head : Mask());
        if (hair == "long" && d == 'N') hm |= ell(hcx, hcy + 1.4, hr + 0.9, hr + 1.6);
        if (hair == "bun") hm |= ell(hcx, top - 0.2, 1.8, 1.6);
        if (hair == "braids" && d != 'E') hm |= ell(hcx - hr - 0.6, hcy + 1.5, 1.2, 1.6) | ell(hcx + hr + 0.6, hcy + 1.5, 1.2, 1.6);
        c.layer(hm, hc);
    }
    const std::string& hat = spec.hat.first;
    const Rgba hcol = hat.empty() ? 0 : parseHex(spec.hat.second);
    if (hat == "hood") {
        Mask m = ell(hcx, hcy, hr + 1, hr + 0.9) | ell(hcx, hcy + 3.5, hr + 1.2, 2.2);
        if (d == 'S') m &= ~filter(head, [&](int x, int y) { return y + 0.5 >= hcy - 1.3 && std::abs(x + 0.5 - hcx) <= 2.8; });
        if (d == 'E') m &= ~filter(head, [&](int x, int y) { return y + 0.5 >= hcy - 1.3 && x + 0.5 >= hcx + 0.2; });
        c.layer(m, hcol);
    }
    if (hat == "helmet") {
        c.layer(filter(ell(hcx, hcy, hr + 0.6, hr + 0.6), [&](int, int y) { return y + 0.5 <= hcy - 0.6; }), hcol);
        if (d == 'S') c.layer(one(int(std::trunc(hcx)), int(std::trunc(hcy))), hcol, nullptr, false);
    }
    if (hat == "straw") {
        c.layer(ell(hcx, top + 2, hr + 2.6, 1.8), hcol, [](int x, int y, Rgba cc) { return (x + y) % 3 == 0 ? mulRgb(cc, 0.85) : Rgba(0); });
        c.layer(ell(hcx, top + 0.6, hr - 1.2, 1.7), hcol);
    }
    if (hat == "kerchief") c.layer(filter(ell(hcx, hcy, hr + 0.4, hr + 0.4), [&](int, int y) { return y + 0.5 <= hcy - 0.8 || d == 'N'; }), hcol);
    if (hat == "cap") c.layer(filter(ell(hcx, hcy - 0.6, hr + 0.3, hr), [&](int, int y) { return y + 0.5 <= hcy - 1.4; }), hcol);
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
    return k + "|" + cloak + "|" + hat.first + hat.second + "|" + tool;
}

Pixels pawnPixels(const PawnSpec& spec, char side, int bob) {
    const auto g = pawn(spec, side == 'W' ? 'E' : side, bob, spec.child);
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
