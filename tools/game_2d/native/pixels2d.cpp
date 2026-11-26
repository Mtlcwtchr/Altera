#include "native/pixels2d.hpp"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
[[maybe_unused]] Mask pts(std::initializer_list<std::pair<int, int>> list) {
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

// --- LR-139: the parts of a pawn as data (content/sprites_2d/pawns2d.json) -------------
// Hair, beards, ears, noses, brows, tusks, eyes, hats, skin patterns, builds and head
// shapes are shapes and expressions in the JSON: the code only stamps them, so a new cut
// or a new kind's ears need no rebuild (pawns.js reads the same file).
using Json = nlohmann::json;

Json& partsStore() {
    static Json j = Json::object();
    return j;
}
const Json& parts() { return partsStore(); }
// j[k] by reference (a static empty object when absent): no temporaries to dangle
const Json& sub(const Json& j, const char* k) {
    static const Json none = Json::object();
    if (!j.is_object()) return none;
    const auto it = j.find(k);
    return it == j.end() ? none : *it;
}

// The variables an expression may name. x, y: the pixel; the rest: the pawn's frame.
enum Var { vX, vY, vHCX, vHCY, vHRX, vHRY, vTOP, vFX, vFY, vEY, vHL, vHRR, vEB, vNX, vBCX, vBCY, vBRX, vBRY, vBX, vBY, vSTEP, vSWING, vCHILD, vTX, vTY, vHY, vSW, vFOOTY, vCount };
const char* kVarNames[vCount] = {"x", "y", "hcx", "hcy", "hrx", "hry", "top", "fx", "fy", "ey", "hl", "hrr", "eb", "nx",
                                 "bcx", "bcy", "brx", "bry", "bx", "by", "step", "swing", "child", "tx", "ty", "hy", "sw", "footy"};

struct Vars {
    double v[vCount] = {};
    char side = 'S';
    void set(char d, double hcx, double hcy, double hrx, double hry, double bcx, double bcy, double brx, double bry, const PawnPose& pose, bool child) {
        side = d;
        v[vHCX] = hcx, v[vHCY] = hcy, v[vHRX] = hrx, v[vHRY] = hry;
        v[vTOP] = std::trunc(hcy - hry);
        v[vFX] = std::trunc(hcx), v[vFY] = std::trunc(hcy + 0.3), v[vEY] = std::trunc(hcy);
        v[vHL] = std::trunc(hcx - hrx), v[vHRR] = std::trunc(hcx + hrx), v[vEB] = std::trunc(hcx - 0.6);
        v[vNX] = std::trunc(hcx + hrx * 0.95);
        v[vBCX] = bcx, v[vBCY] = bcy, v[vBRX] = brx, v[vBRY] = bry, v[vBX] = std::trunc(bcx), v[vBY] = std::trunc(bcy);
        v[vSTEP] = pose.step, v[vSWING] = pose.swing, v[vCHILD] = child ? 1 : 0;
        // the hands: they swing in turn walking; at work the tool hand goes up and down; the tool sits by it
        v[vHY] = std::trunc(bcy + 1) - 2 * pose.swing;
        v[vSW] = pose.step == 1 ? 1 : pose.step == 3 ? -1 : 0;
        v[vTX] = d == 'N' ? std::trunc(bcx - brx) - 2 : std::trunc(bcx + brx) + (d != 'E' ? 1 : 0);
        v[vTY] = v[vHY];
        v[vFOOTY] = std::min(double(N - 1), std::trunc(bcy + bry - 0.2));
    }
};

// A tiny expression language: numbers, the variables, + - * / %, comparisons, && || !,
// abs min max trunc floor. Compiled once per text.
struct Expr {
    enum Op { Num, Var_, Neg, Not, Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Abs, Min, Max, Trunc, Floor } op = Num;
    double num = 0;
    int var = 0;
    std::vector<Expr> k;
    double eval(const double* v) const {
        switch (op) {
        case Num: return num;
        case Var_: return v[var];
        case Neg: return -k[0].eval(v);
        case Not: return k[0].eval(v) == 0 ? 1 : 0;
        case Add: return k[0].eval(v) + k[1].eval(v);
        case Sub: return k[0].eval(v) - k[1].eval(v);
        case Mul: return k[0].eval(v) * k[1].eval(v);
        case Div: { const double b = k[1].eval(v); return b == 0 ? 0 : k[0].eval(v) / b; }
        case Mod: { const long long b = (long long)k[1].eval(v); if (!b) return 0; long long a = (long long)k[0].eval(v) % b; return double(a < 0 ? a + std::llabs(b) : a); }
        case Lt: return k[0].eval(v) < k[1].eval(v);
        case Le: return k[0].eval(v) <= k[1].eval(v);
        case Gt: return k[0].eval(v) > k[1].eval(v);
        case Ge: return k[0].eval(v) >= k[1].eval(v);
        case Eq: return k[0].eval(v) == k[1].eval(v);
        case Ne: return k[0].eval(v) != k[1].eval(v);
        case And: return k[0].eval(v) != 0 && k[1].eval(v) != 0;
        case Or: return k[0].eval(v) != 0 || k[1].eval(v) != 0;
        case Abs: return std::abs(k[0].eval(v));
        case Min: return std::min(k[0].eval(v), k[1].eval(v));
        case Max: return std::max(k[0].eval(v), k[1].eval(v));
        case Trunc: return std::trunc(k[0].eval(v));
        case Floor: return std::floor(k[0].eval(v));
        }
        return 0;
    }
};

struct ExprParser {
    const std::string& t;
    std::size_t i = 0;
    bool bad = false;
    explicit ExprParser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && t[i] == ' ') ++i; }
    bool eat(const char* tok) {
        ws();
        const std::size_t n = std::strlen(tok);
        if (t.compare(i, n, tok) == 0) { i += n; return true; }
        return false;
    }
    static Expr bin(Expr::Op op, Expr a, Expr b) { Expr e; e.op = op; e.k = {std::move(a), std::move(b)}; return e; }
    Expr orE() { Expr a = andE(); while (eat("||")) a = bin(Expr::Or, std::move(a), andE()); return a; }
    Expr andE() { Expr a = eqE(); while (eat("&&")) a = bin(Expr::And, std::move(a), eqE()); return a; }
    Expr eqE() {
        Expr a = relE();
        for (;;) {
            if (eat("==")) a = bin(Expr::Eq, std::move(a), relE());
            else if (eat("!=")) a = bin(Expr::Ne, std::move(a), relE());
            else return a;
        }
    }
    Expr relE() {
        Expr a = addE();
        for (;;) {
            if (eat("<=")) a = bin(Expr::Le, std::move(a), addE());
            else if (eat(">=")) a = bin(Expr::Ge, std::move(a), addE());
            else if (eat("<")) a = bin(Expr::Lt, std::move(a), addE());
            else if (eat(">")) a = bin(Expr::Gt, std::move(a), addE());
            else return a;
        }
    }
    Expr addE() {
        Expr a = mulE();
        for (;;) {
            if (eat("+")) a = bin(Expr::Add, std::move(a), mulE());
            else if (eat("-")) a = bin(Expr::Sub, std::move(a), mulE());
            else return a;
        }
    }
    Expr mulE() {
        Expr a = unE();
        for (;;) {
            if (eat("*")) a = bin(Expr::Mul, std::move(a), unE());
            else if (eat("/")) a = bin(Expr::Div, std::move(a), unE());
            else if (eat("%")) a = bin(Expr::Mod, std::move(a), unE());
            else return a;
        }
    }
    Expr unE() {
        if (eat("-")) { Expr e; e.op = Expr::Neg; e.k = {unE()}; return e; }
        if (eat("!")) { Expr e; e.op = Expr::Not; e.k = {unE()}; return e; }
        return prim();
    }
    Expr prim() {
        ws();
        Expr e;
        if (eat("(")) { e = orE(); if (!eat(")")) bad = true; return e; }
        if (i < t.size() && (std::isdigit((unsigned char)t[i]) || t[i] == '.')) {
            std::size_t j = i;
            while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '.')) ++j;
            e.num = std::atof(t.substr(i, j - i).c_str());
            i = j;
            return e;
        }
        std::size_t j = i;
        while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) ++j;
        const std::string id = t.substr(i, j - i);
        i = j;
        static const std::map<std::string, std::pair<Expr::Op, int>> fns = {
            {"abs", {Expr::Abs, 1}}, {"min", {Expr::Min, 2}}, {"max", {Expr::Max, 2}}, {"trunc", {Expr::Trunc, 1}}, {"floor", {Expr::Floor, 1}}};
        if (const auto f = fns.find(id); f != fns.end()) {
            e.op = f->second.first;
            if (!eat("(")) { bad = true; return e; }
            for (int a = 0; a < f->second.second; ++a) {
                if (a && !eat(",")) bad = true;
                e.k.push_back(orE());
            }
            if (!eat(")")) bad = true;
            return e;
        }
        for (int k = 0; k < vCount; ++k)
            if (id == kVarNames[k]) { e.op = Expr::Var_; e.var = k; return e; }
        bad = true;
        return e;
    }
};

// A number or an expression text, compiled once (a bad text is 0 and says so on stderr once).
const Expr& compiled(const std::string& text) {
    static std::map<std::string, Expr> cache;
    const auto it = cache.find(text);
    if (it != cache.end()) return it->second;
    ExprParser p(text);
    Expr e = p.orE();
    p.ws();
    if (p.bad || p.i != text.size()) {
        std::fprintf(stderr, "pawns2d.json: bad expression '%s'\n", text.c_str());
        e = Expr();
    }
    return cache.emplace(text, std::move(e)).first->second;
}
double num(const Json& j, const Vars& V) {
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return compiled(j.get<std::string>()).eval(V.v);
    return 0;
}

// The colours a part may name: skin, hair, eye, hat, c (the op's own) and pawns2d.json colours.
struct Colours {
    std::map<std::string, Rgba> named;
};
Colours colours(const PawnSpec& spec, Rgba sk, bool blink) {
    Colours c;
    for (const auto& [k, v] : sub(parts(), "colours").items())
        if (v.is_string()) c.named[k] = parseHex(v.get<std::string>());
    c.named["skin"] = sk;
    c.named["hair"] = parseHex(spec.hair.second.empty() ? "#5a3a20" : spec.hair.second);
    const Rgba out = c.named.count("outline") ? c.named["outline"] : OUT;
    c.named["eye"] = blink ? mulRgb(sk, 0.6) : spec.eyes.empty() ? out : parseHex(spec.eyes);
    if (!spec.hat.first.empty()) c.named["hat"] = parseHex(spec.hat.second);
    return c;
}
// "name", "name*0.8", "mix(a,b,0.45)", "#rrggbb"
Rgba colourOf(const std::string& sp, const Colours& C, Rgba own) {
    std::string t;
    for (char ch : sp) if (ch != ' ') t += ch;
    if (t.empty()) return own;
    if (t[0] == '#') return parseHex(t);
    if (t.rfind("mix(", 0) == 0 && t.back() == ')') {
        const std::string in = t.substr(4, t.size() - 5);
        const auto a = in.find(','), b = in.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) return own;
        return mixRgb(colourOf(in.substr(0, a), C, own), colourOf(in.substr(a + 1, b - a - 1), C, own), std::atof(in.substr(b + 1).c_str()));
    }
    if (const auto star = t.find('*'); star != std::string::npos) return mulRgb(colourOf(t.substr(0, star), C, own), std::atof(t.substr(star + 1).c_str()));
    if (t == "c") return own;
    const auto it = C.named.find(t);
    return it == C.named.end() ? 0xffff00ffu : it->second;
}

bool onSide(const Json& j, char d) {
    const std::string sides = j.value("sides", std::string("SEN"));
    return sides.find(d) != std::string::npos;
}

// A shape: {ell: [cx, cy, rx, ry]} | {head: true} | {body: true} | {px: [[x, y]...]} | "@name"
// (pawns2d.json shapes), each with sides, when (the whole shape), where (each pixel), minus (a shape).
Mask shapeMask(const Json& sh, char d, Vars& V, const Mask& head, const Mask& body) {
    Mask m;
    if (sh.is_string()) {
        const std::string n = sh.get<std::string>();
        if (n.size() > 1 && n[0] == '@') {
            const Json& defs = sub(parts(), "shapes");
            const auto it = defs.find(n.substr(1));
            if (it != defs.end()) {
                if (it->is_array()) for (const auto& x : *it) m |= shapeMask(x, d, V, head, body);
                else m = shapeMask(*it, d, V, head, body);
            }
        }
        return m;
    }
    if (!sh.is_object() || !onSide(sh, d)) return m;
    if (sh.contains("when") && num(sh["when"], V) == 0) return m;
    if (sh.contains("ell") && sh["ell"].is_array() && sh["ell"].size() >= 4) {
        const Json& e = sh["ell"];
        m = ell(num(e[0], V), num(e[1], V), num(e[2], V), num(e[3], V));
    } else if (sh.value("head", false)) m = head;
    else if (sh.value("body", false)) m = body;
    else if (sh.contains("rect") && sh["rect"].is_array() && sh["rect"].size() >= 4) {   // [x0, y0, x1, y1], inclusive
        const Json& r = sh["rect"];
        const int x0 = int(std::floor(num(r[0], V))), y0 = int(std::floor(num(r[1], V))), x1 = int(std::floor(num(r[2], V))), y1 = int(std::floor(num(r[3], V)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) m |= one(x, y);
    } else if (sh.contains("col") && sh["col"].is_array() && sh["col"].size() >= 3) {   // [x, y0, y1]: a stick
        const Json& r = sh["col"];
        const int x = int(std::floor(num(r[0], V))), y0 = int(std::floor(num(r[1], V))), y1 = int(std::floor(num(r[2], V)));
        for (int y = y0; y <= y1; ++y) m |= one(x, y);
    } else if (sh.contains("px") && sh["px"].is_array())
        for (const auto& q : sh["px"])
            if (q.is_array() && q.size() >= 2) m |= one(int(std::floor(num(q[0], V))), int(std::floor(num(q[1], V))));
    if (sh.contains("where")) {
        const Expr& w = compiled(sh["where"].is_string() ? sh["where"].get<std::string>() : std::string("1"));
        m = filter(m, [&](int x, int y) { V.v[vX] = x, V.v[vY] = y; return w.eval(V.v) != 0; });
    }
    if (sh.contains("minus")) m &= ~shapeMask(sh["minus"], d, V, head, body);
    return m;
}

// Layers `m` in `col`, an optional pattern {if: expr, colour: spec} recolouring some pixels.
void layerPatterned(Canvas& c, const Mask& m, Rgba col, const Json& pattern, Vars& V, const Colours& C, bool outline = true) {
    if (!pattern.is_object() || !pattern.contains("if")) {
        c.layer(m, col, nullptr, outline);
        return;
    }
    const Expr& w = compiled(pattern["if"].get<std::string>());
    const std::string cs = pattern.value("colour", std::string("c*0.78"));
    c.layer(m, col, [&](int x, int y, Rgba cc) {
        V.v[vX] = x, V.v[vY] = y;
        return w.eval(V.v) != 0 ? colourOf(cs, C, cc) : Rgba(0);
    }, outline);
}

// An op: {shapes: [...] (or the shape's own fields), colour, outline (true), flat (set pixels, no
// shading), pattern, sides}. The shapes of one op are joined, then layered once.
void drawOps(Canvas& c, const Json& ops, char d, Vars& V, const Mask& head, const Mask& body, const Colours& C) {
    if (!ops.is_array()) return;
    for (const auto& op : ops) {
        if (!op.is_object() || !onSide(op, d)) continue;
        Mask m;
        if (op.contains("shapes") && op["shapes"].is_array()) {
            for (const auto& sh : op["shapes"]) m |= shapeMask(sh, d, V, head, body);
            if (op.contains("where")) {
                const Expr& w = compiled(op["where"].get<std::string>());
                m = filter(m, [&](int x, int y) { V.v[vX] = x, V.v[vY] = y; return w.eval(V.v) != 0; });
            }
        } else m = shapeMask(op, d, V, head, body);
        if (m.none()) continue;
        const Rgba col = colourOf(op.value("colour", std::string("skin")), C, 0);
        if (op.value("flat", false)) {
            for (int k = 0; k < N * N; ++k)
                if (m.test(std::size_t(k))) c.g[std::size_t(k)] = col;
            continue;
        }
        layerPatterned(c, m, col, op.contains("pattern") ? op["pattern"] : Json(), V, C, op.value("outline", true));
    }
}

const Json& skinPattern(const PawnSpec& spec, const char* where) {
    static const Json none;
    if (spec.skinPat.empty()) return none;
    const Json& sp = sub(parts(), "skin_patterns");
    const auto it = sp.find(spec.skinPat);
    if (it == sp.end() || !it->is_object() || !it->contains(where)) return none;
    return it->find(where).value();
}

// The ops of `group`'s entry `name` (a list), or of its `slot` when the entry is an object.
const Json& partOps(const char* group, const std::string& name, const char* slot = nullptr) {
    static const Json none = Json::array();
    if (name.empty()) return none;
    const Json& g = sub(parts(), group);
    const auto it = g.find(name);
    if (it == g.end()) return none;
    if (slot) {
        if (!it->is_object()) return none;
        const auto s = it->find(slot);
        return s == it->end() ? none : *s;
    }
    return *it;
}

Rgba skinOf(const std::string& s) {
    if (!s.empty() && s[0] == '#') return parseHex(s); // AL-114: a kind's own skin (orc green, troll grey)
    // LR-139: the named tones are data (pawns2d.json skins)
    const Json& sk = sub(parts(), "skins");
    if (sk.contains(s) && sk[s].is_string()) return parseHex(sk[s].get<std::string>());
    if (sk.contains("light") && sk["light"].is_string()) return parseHex(sk["light"].get<std::string>());
    return parseHex("#eab48a");
}

// The head and all on it, in the order of pawns2d.json's groups: ears (under the head), the
// head, face (eyes), brows, noses, beards, tusks, the front of the hair, the ears again over
// the hair, the hat.
void drawHead(Canvas& c, const PawnSpec& spec, char d, const PawnPose& pose, Vars& V, const Mask& body, Rgba sk) {
    const Colours C = colours(spec, sk, pose.blink);
    const double hcx = V.v[vHCX], hcy = V.v[vHCY], hrx = V.v[vHRX], hry = V.v[vHRY];
    const Mask head = ell(hcx, hcy, d != 'E' ? hrx : hrx * 0.95, hry);
    // the ears: under the head, so its outline shades them
    Mask ears;
    for (const auto& op : partOps("ears", spec.ears)) if (op.is_object() && onSide(op, d)) ears |= shapeMask(op, d, V, Mask(), body);
    if (ears.any()) c.layer(ears, sk);
    layerPatterned(c, head, sk, skinPattern(spec, "head"), V, C);
    drawOps(c, sub(parts(), "face"), d, V, head, body, C);
    drawOps(c, partOps("brows", spec.brow), d, V, head, body, C);
    drawOps(c, partOps("noses", spec.nose), d, V, head, body, C);
    if (spec.beard) {
        const std::string bs = !spec.beardStyle.empty() ? spec.beardStyle : spec.longBeard ? "long" : "full";
        drawOps(c, partOps("beards", bs), d, V, head, body, C);
    }
    drawOps(c, partOps("tusks", spec.tusks), d, V, head, body, C);
    const std::string hair = spec.hair.first.empty() ? "short" : spec.hair.first;
    drawOps(c, partOps("hair", hair, "front"), d, V, head, body, C);
    const Json& bald = sub(parts(), "bald");
    const bool hairless = std::find(bald.begin(), bald.end(), hair) != bald.end();
    if (ears.any() && !hairless) c.layer(ears & ~head, sk, nullptr, false);   // ears through the hair
    if (!spec.hat.first.empty()) drawOps(c, partOps("hats", spec.hat.first), d, V, head, body, C);
}

std::array<Rgba, N * N> pawn(const PawnSpec& spec, char d, const PawnPose& pose, bool child) {
    Canvas c;
    const int bob = pose.bob;
    const Rgba sk = skinOf(spec.skin);
    const Json& P = parts();
    // the frame of the pawn (pawns2d.json frame.adult|child, bodies, builds, heads)
    const Json fr = P.value("frame", Json::object()).value(child ? "child" : "adult", Json::object());
    double hcx = fr.value("hcx", 10.0), hcy = fr.value("hcy", child ? 9.5 : 6.6), hr = fr.value("hr", child ? 3.7 : 4.3);
    double bcx = fr.value("bcx", 10.0), bcy = fr.value("bcy", child ? 15.6 : 14.0), brx = fr.value("brx", child ? 3.8 : 5.0),
           bry = fr.value("bry", child ? 3.9 : 5.2);
    double hcxE = 0;
    if (!child) {
        const Json bodies = P.value("bodies", Json::object());
        if (bodies.contains(spec.body) && bodies[spec.body].is_array() && bodies[spec.body].size() >= 2)
            brx = bodies[spec.body][0].get<double>(), bry = bodies[spec.body][1].get<double>();
        // LR-139: the kind's build: a dwarf squat and low, an elf slim and tall, an ogre hunched
        const Json builds = P.value("builds", Json::object());
        if (!spec.build.empty() && builds.contains(spec.build)) {
            const Json& b = builds[spec.build];
            brx *= b.value("brx", 1.0), bry *= b.value("bry", 1.0);
            bcy += b.value("bcy", 0.0), hcy += b.value("hcy", 0.0);
            hcxE = b.value("hcx_e", 0.0);
        }
    }
    // LR-139: the shape of the head
    double hrx = hr, hry = hr;
    const Json heads = P.value("heads", Json::object());
    if (!spec.head.empty() && heads.contains(spec.head) && heads[spec.head].is_array() && heads[spec.head].size() >= 2)
        hrx = hr * heads[spec.head][0].get<double>(), hry = hr * heads[spec.head][1].get<double>();
    if (d == 'E') brx *= 0.82, hcx += 0.7 + hcxE;
    hcy -= bob;
    bcy -= bob;
    hcy += pose.dip;
    Vars V;
    V.set(d, hcx, hcy, hrx, hry, bcx, bcy, brx, bry, pose, child);
    const Mask body = ell(bcx, bcy, brx, bry);
    Colours C = colours(spec, sk, false);
    if (!spec.cloak.empty()) C.named["cloak"] = parseHex(spec.cloak);
    // LR-139: the back of the hair falls behind the shoulders (drawn before the body, which hides it below them)
    drawOps(c, partOps("hair", spec.hair.first, "back"), d, V, Mask(), body, C);
    if (!spec.cloak.empty()) drawOps(c, partOps("cloak", "back"), d, V, Mask(), body, C);
    layerPatterned(c, body, sk, skinPattern(spec, "body"), V, C);
    // feet (bare, or in the boots' colour), the clothes layer by layer, the cloak's front, the hands, the tool
    C.named["feet"] = colourOf(sub(parts(), "colours").value("feet", std::string("skin*0.8")), C, sk);
    for (const auto& [kind, colHex] : spec.wear)
        if (kind == "boots") C.named["feet"] = parseHex(colHex);
    drawOps(c, sub(parts(), "feet"), d, V, Mask(), body, C);
    for (const auto& [kind, colHex] : spec.wear) {
        C.named["wear"] = parseHex(colHex);
        drawOps(c, partOps("wear", kind), d, V, Mask(), body, C);
    }
    if (!spec.cloak.empty()) drawOps(c, partOps("cloak", "front"), d, V, Mask(), body, C);
    drawOps(c, sub(parts(), "hands"), d, V, Mask(), body, C);
    drawOps(c, partOps("tools", spec.tool), d, V, Mask(), body, C);
    drawHead(c, spec, d, pose, V, body, sk);
    return c.g;
}

} // namespace

void setPawnParts(const nlohmann::json& j) { partsStore() = j.is_object() ? j : nlohmann::json::object(); }
const nlohmann::json& pawnParts() { return parts(); }

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
    // 16x16 as a rule; LR-159: a bigger grid (a horse, a wagon: 32x32) keeps its size
    int n = 16;
    n = std::max(n, int(it->second.size()));
    for (const auto& row : it->second) n = std::max(n, int(row.size()));
    n = std::min(n, 64);
    out.size = n;
    out.px.assign(std::size_t(n * n), 0);
    for (int y = 0; y < n; ++y) {
        const std::string row = y < int(it->second.size()) ? it->second[std::size_t(y)] : std::string();
        for (int x = 0; x < n && x < int(row.size()); ++x) {
            char ch = row[std::size_t(x)];
            if (ch == '.' || ch == ' ') continue;
            if (const auto r = recolour.find(ch); r != recolour.end()) ch = r->second;
            out.px[std::size_t(y * n + x)] = colour(ch);
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
