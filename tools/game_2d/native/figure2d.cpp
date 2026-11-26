// LR-165: the 48x64 figures from content/sprites_2d/figures2d.json. A line-by-line port of
// tools/game_2d/web/figures.js: keep the two pixel-equal (the same float steps, floor(v+0.5)
// for rounding), tools/game_2d/figure_sheet.js --dump and figure2d_check compare them.
#include "native/figure2d.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace game2d {

namespace {

using Json = nlohmann::json;

Json& store() {
    static Json j = Json::object();
    return j;
}
const Json& D() { return store(); }
const Json& sub(const Json& j, const char* k) {
    static const Json none = Json::object();
    if (!j.is_object()) return none;
    const auto it = j.find(k);
    return it == j.end() ? none : *it;
}
double dnum(const Json& j, const char* k, double def) {
    const Json& v = sub(j, k);
    return v.is_number() ? v.get<double>() : def;
}

// --- variables: a name -> slot registry; a figure's numbers are one flat table ----------
std::unordered_map<std::string, int>& registry() {
    static std::unordered_map<std::string, int> r;
    return r;
}
int slot(const std::string& name) {
    auto& r = registry();
    const auto it = r.find(name);
    if (it != r.end()) return it->second;
    const int k = int(r.size());
    r.emplace(name, k);
    return k;
}
struct Vars {
    std::vector<double> v;
    std::vector<char> set;
    double get(int k) const { return k < int(v.size()) && set[std::size_t(k)] ? v[std::size_t(k)] : 0.0; }
    void put(int k, double x) {
        if (k >= int(v.size())) v.resize(std::size_t(k) + 16, 0.0), set.resize(std::size_t(k) + 16, 0);
        v[std::size_t(k)] = x;
        set[std::size_t(k)] = 1;
    }
    void put(const std::string& n, double x) { put(slot(n), x); }
};
int kX() { static const int k = slot("x"); return k; }
int kY() { static const int k = slot("y"); return k; }

// --- expressions (figures.js compile) ----------------------------------------------------
struct Expr {
    enum Op { Num, Var, Neg, Not, Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Abs, Min, Max, Trunc, Floor, Sel } op = Num;
    double num = 0;
    int var = 0;
    std::vector<Expr> k;
    double eval(const Vars& V) const {
        switch (op) {
        case Num: return num;
        case Var: return V.get(var);
        case Neg: return -k[0].eval(V);
        case Not: return k[0].eval(V) == 0 ? 1 : 0;
        case Add: return k[0].eval(V) + k[1].eval(V);
        case Sub: return k[0].eval(V) - k[1].eval(V);
        case Mul: return k[0].eval(V) * k[1].eval(V);
        case Div: { const double a = k[0].eval(V), b = k[1].eval(V); return b == 0 ? 0 : a / b; }
        case Mod: {
            const double a = k[0].eval(V), bb = k[1].eval(V);
            const long long b = (long long)std::trunc(bb);
            if (!b) return 0;
            const long long r = (long long)std::trunc(a) % b;
            return double(r < 0 ? r + std::llabs(b) : r);
        }
        case Lt: return k[0].eval(V) < k[1].eval(V) ? 1 : 0;
        case Le: return k[0].eval(V) <= k[1].eval(V) ? 1 : 0;
        case Gt: return k[0].eval(V) > k[1].eval(V) ? 1 : 0;
        case Ge: return k[0].eval(V) >= k[1].eval(V) ? 1 : 0;
        case Eq: return k[0].eval(V) == k[1].eval(V) ? 1 : 0;
        case Ne: return k[0].eval(V) != k[1].eval(V) ? 1 : 0;
        case And: { const double a = k[0].eval(V), b = k[1].eval(V); return (a != 0 && b != 0) ? 1 : 0; }
        case Or: { const double a = k[0].eval(V), b = k[1].eval(V); return (a != 0 || b != 0) ? 1 : 0; }
        case Abs: return std::abs(k[0].eval(V));
        case Min: return std::min(k[0].eval(V), k[1].eval(V));
        case Max: return std::max(k[0].eval(V), k[1].eval(V));
        case Trunc: return std::trunc(k[0].eval(V));
        case Floor: return std::floor(k[0].eval(V));
        case Sel: { const double c = k[0].eval(V), a = k[1].eval(V), b = k[2].eval(V); return c != 0 ? a : b; }
        }
        return 0;
    }
};

struct Parser {
    const std::string& t;
    std::size_t i = 0;
    bool bad = false;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && t[i] == ' ') ++i; }
    bool eat(const char* tok) {
        ws();
        const std::size_t n = std::strlen(tok);
        if (t.compare(i, n, tok) == 0) { i += n; return true; }
        return false;
    }
    static Expr bin(Expr::Op op, Expr a, Expr b) { Expr e; e.op = op; e.k.push_back(std::move(a)); e.k.push_back(std::move(b)); return e; }
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
        if (eat("-")) { Expr e; e.op = Expr::Neg; e.k.push_back(unE()); return e; }
        if (eat("!")) { Expr e; e.op = Expr::Not; e.k.push_back(unE()); return e; }
        return prim();
    }
    static bool numCh(char c) { return std::isdigit((unsigned char)c) || c == '.'; }
    static bool idCh(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
    Expr prim() {
        ws();
        Expr e;
        if (eat("(")) { e = orE(); if (!eat(")")) bad = true; return e; }
        std::size_t j = i;
        if (i < t.size() && numCh(t[i])) {
            while (j < t.size() && numCh(t[j])) ++j;
            e.num = std::atof(t.substr(i, j - i).c_str());
            i = j;
            return e;
        }
        while (j < t.size() && idCh(t[j])) ++j;
        const std::string id = t.substr(i, j - i);
        i = j;
        if (id.empty()) { bad = true; return e; }
        static const std::map<std::string, std::pair<Expr::Op, int>> fns = {
            {"abs", {Expr::Abs, 1}}, {"min", {Expr::Min, 2}}, {"max", {Expr::Max, 2}}, {"trunc", {Expr::Trunc, 1}},
            {"floor", {Expr::Floor, 1}}, {"sel", {Expr::Sel, 3}}};
        if (const auto f = fns.find(id); f != fns.end()) {
            e.op = f->second.first;
            if (!eat("(")) bad = true;
            for (int a = 0; a < f->second.second; ++a) {
                if (a && !eat(",")) bad = true;
                e.k.push_back(orE());
            }
            if (!eat(")")) bad = true;
            return e;
        }
        e.op = Expr::Var;
        e.var = slot(id);
        return e;
    }
};

std::unordered_map<std::string, std::unique_ptr<Expr>>& exprCache() {
    static std::unordered_map<std::string, std::unique_ptr<Expr>> c;
    return c;
}
const Expr& expr(const std::string& text) {
    auto& c = exprCache();
    const auto it = c.find(text);
    if (it != c.end()) return *it->second;
    Parser p(text);
    auto e = std::make_unique<Expr>(p.orE());
    p.ws();
    if (p.bad || p.i != text.size()) {
        std::fprintf(stderr, "figures2d.json: bad expression '%s'\n", text.c_str());
        *e = Expr();
    }
    return *c.emplace(text, std::move(e)).first->second;
}
double num(const Json& j, const Vars& V) {
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return expr(j.get<std::string>()).eval(V);
    return 0;
}

// --- colours --------------------------------------------------------------------------------
using Col = std::array<int, 3>;
int clampc(double v) { return int(std::max(0.0, std::min(255.0, std::floor(v + 0.5)))); }
int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
Col hexc(const std::string& h) {
    // parseInt(h.slice(1, 7), 16): the leading hex digits; none: 0
    long n = 0;
    bool any = false;
    for (std::size_t i = 1; i < h.size() && i < 7; ++i) {
        const int d = hexv(h[i]);
        if (d < 0) break;
        n = n * 16 + d;
        any = true;
    }
    if (!any) n = 0;
    return {int(n >> 16 & 255), int(n >> 8 & 255), int(n & 255)};
}
Col sh(const Col& c, double f) {
    Col o;
    for (int i = 0; i < 3; ++i) o[std::size_t(i)] = clampc(f < 1 ? c[std::size_t(i)] * f : c[std::size_t(i)] + (255 - c[std::size_t(i)]) * (f - 1));
    return o;
}
Col warm(const Col& c) { return {clampc(c[0] * 1.04), clampc(c[1]), clampc(c[2] * 0.94)}; }
Col cool(const Col& c) { return {clampc(c[0] * 0.95), clampc(c[1] * 0.97), clampc(c[2] * 1.07)}; }
Rgba rgba(const Col& c) { return 0xff000000u | std::uint32_t(c[0]) << 16 | std::uint32_t(c[1]) << 8 | std::uint32_t(c[2]); }

using Colours = std::unordered_map<std::string, Col>;
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::vector<std::string> args(const std::string& s) {
    std::vector<std::string> out;
    int d = 0;
    std::size_t st = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == '(') ++d;
        else if (ch == ')') --d;
        else if (ch == ',' && !d) { out.push_back(trim(s.substr(st, i - st))); st = i + 1; }
    }
    out.push_back(trim(s.substr(st)));
    return out;
}
Json numJson(const std::string& s) {   // a colour's number part: a number or an expression
    char* end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end && *end == 0 && !s.empty()) return d;
    return s;
}
Col colour(const std::string& in, const Colours& C, const Vars& V) {
    const std::string s = trim(in);
    if (!s.empty() && s[0] == '#') return hexc(s);
    auto ends = [&](const char* pre) { return s.rfind(pre, 0) == 0 && s.back() == ')'; };
    if (ends("mix(")) {
        const auto a = args(s.substr(4, s.size() - 5));
        if (a.size() >= 3) {
            const Col x = colour(a[0], C, V), y = colour(a[1], C, V);
            const double k = num(numJson(a[2]), V);
            Col o;
            for (int i = 0; i < 3; ++i) o[std::size_t(i)] = clampc(x[std::size_t(i)] * (1 - k) + y[std::size_t(i)] * k);
            return o;
        }
    }
    if (ends("tint(")) {
        const auto a = args(s.substr(5, s.size() - 6));
        if (a.size() >= 4) {
            const Col x = colour(a[0], C, V);
            return {clampc(x[0] * num(numJson(a[1]), V)), clampc(x[1] * num(numJson(a[2]), V)), clampc(x[2] * num(numJson(a[3]), V))};
        }
    }
    int d = 0;
    long star = -1;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++d;
        else if (s[i] == ')') --d;
        else if (s[i] == '*' && !d) star = long(i);
    }
    if (star > 0) return sh(colour(s.substr(0, std::size_t(star)), C, V), num(numJson(trim(s.substr(std::size_t(star) + 1))), V));
    if (const auto it = C.find(s); it != C.end()) return it->second;
    return {255, 0, 255};
}
Col colourJ(const Json& j, const Colours& C, const Vars& V) {
    if (j.is_string()) return colour(j.get<std::string>(), C, V);
    if (j.is_number()) return colour(std::to_string(j.get<double>()), C, V);
    return {255, 0, 255};
}

// --- shapes ------------------------------------------------------------------------------------
struct Box { double b[4] = {0, 0, 0, 0}; bool inf = false; };
Box uni(const Box& a, const Box& c) {
    Box o;
    if (a.inf || c.inf) { o.inf = true; return o; }
    o.b[0] = std::min(a.b[0], c.b[0]), o.b[1] = std::min(a.b[1], c.b[1]), o.b[2] = std::max(a.b[2], c.b[2]), o.b[3] = std::max(a.b[3], c.b[3]);
    return o;
}
struct Shape {
    enum K { None, Ell, Cap, Poly, Rect, Or, And, Sub, Mir, Both, Where } k = None;
    double a[8] = {};
    std::vector<std::array<double, 2>> pts;
    std::vector<Shape> kids;
    const Expr* where = nullptr;
    double m2 = 48;
    Box box;
    bool test(double x, double y, Vars& V) const {
        switch (k) {
        case None: return false;
        case Ell: { const double p = (x - a[0]) / a[2], q = (y - a[1]) / a[3]; return p * p + q * q <= 1; }
        case Cap: {
            const double x0 = a[0], y0 = a[1], x1 = a[2], y1 = a[3], r0 = a[4], r1 = a[5];
            const double dx = x1 - x0, dy = y1 - y0, L = dx * dx + dy * dy;
            double t = L != 0 ? ((x - x0) * dx + (y - y0) * dy) / L : 0;
            t = std::max(0.0, std::min(1.0, t));
            const double px = x0 + dx * t, py = y0 + dy * t, r = r0 + (r1 - r0) * t;
            return (x - px) * (x - px) + (y - py) * (y - py) <= r * r;
        }
        case Poly: {
            bool c = false;
            const std::size_t n = pts.size();
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const double xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
                if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c;
            }
            return c;
        }
        case Rect: return x >= a[0] && x <= a[2] && y >= a[1] && y <= a[3];
        case Or: for (const auto& s : kids) if (s.test(x, y, V)) return true; return false;
        case And: for (const auto& s : kids) if (!s.test(x, y, V)) return false; return true;
        case Sub: return kids[0].test(x, y, V) && !kids[1].test(x, y, V);
        case Mir: return kids[0].test(m2 - x, y, V);
        case Both: return kids[0].test(x, y, V) || kids[0].test(m2 - x, y, V);
        case Where: V.put(kX(), x); V.put(kY(), y); return where->eval(V) != 0;
        }
        return false;
    }
};
Shape polyShape(std::vector<std::array<double, 2>> pts) {
    Shape s;
    s.k = Shape::Poly;
    s.box.b[0] = 1e9, s.box.b[1] = 1e9, s.box.b[2] = -1e9, s.box.b[3] = -1e9;
    for (const auto& p : pts)
        s.box.b[0] = std::min(s.box.b[0], p[0]), s.box.b[1] = std::min(s.box.b[1], p[1]), s.box.b[2] = std::max(s.box.b[2], p[0]), s.box.b[3] = std::max(s.box.b[3], p[1]);
    if (pts.empty()) s.box = Box{};
    s.pts = std::move(pts);
    return s;
}
Shape nothing() { Shape s; return s; }
double mirrorX() { return dnum(D(), "mirror_x", 24); }

Shape shape(const Json& j, const Vars& V) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '@') return shape(sub(sub(D(), "shapes"), s.c_str() + 1), V);
        return nothing();
    }
    if (j.is_array()) return shape(Json{{"or", j}}, V);
    if (!j.is_object()) return nothing();
    auto nums = [&](const Json& a, int n, double* out) {
        for (int i = 0; i < n; ++i) out[i] = a.is_array() && int(a.size()) > i ? num(a[std::size_t(i)], V) : 0;
    };
    Shape s;
    if (j.contains("ell")) {
        s.k = Shape::Ell;
        nums(j["ell"], 4, s.a);
        s.box.b[0] = s.a[0] - s.a[2], s.box.b[1] = s.a[1] - s.a[3], s.box.b[2] = s.a[0] + s.a[2], s.box.b[3] = s.a[1] + s.a[3];
        return s;
    }
    if (j.contains("cap")) {
        s.k = Shape::Cap;
        nums(j["cap"], 6, s.a);
        const double rm = std::max(s.a[4], s.a[5]);
        s.box.b[0] = std::min(s.a[0], s.a[2]) - rm, s.box.b[1] = std::min(s.a[1], s.a[3]) - rm;
        s.box.b[2] = std::max(s.a[0], s.a[2]) + rm, s.box.b[3] = std::max(s.a[1], s.a[3]) + rm;
        return s;
    }
    if (j.contains("poly")) {
        std::vector<std::array<double, 2>> pts;
        for (const auto& p : j["poly"]) pts.push_back({num(p[0], V), num(p[1], V)});
        return polyShape(std::move(pts));
    }
    if (j.contains("rect")) {
        s.k = Shape::Rect;
        nums(j["rect"], 4, s.a);
        s.box.b[0] = s.a[0], s.box.b[1] = s.a[1], s.box.b[2] = s.a[2], s.box.b[3] = s.a[3];
        return s;
    }
    if (j.contains("torso")) {
        const double yb = num(j["torso"][0], V), flare = num(j["torso"][1], V);
        const Json& T = sub(D(), "torso");
        const Json& R = sub(T, V.get(slot("fem")) != 0 ? "f" : "m");
        std::vector<std::array<double, 2>> pts;
        if (R.is_array() && !R.empty()) {
            for (const auto& p : R)
                if (p[1].get<double>() <= yb) pts.push_back({p[0].get<double>(), p[1].get<double>()});
        } else {
            for (const auto& p : {std::array<double, 2>{34, 22}, std::array<double, 2>{32, 46}})
                if (p[1] <= yb) pts.push_back(p);
        }
        const std::array<double, 2> last = pts.empty() ? std::array<double, 2>{32, 22} : pts.back();
        const double hem = dnum(T, "hem_y", 46);
        if (yb > hem) pts.push_back({last[0] + flare, yb});
        else pts.push_back({last[0] - 0.8, yb});
        const double mx = mirrorX();
        const std::size_t n = pts.size();
        for (std::size_t i = n; i-- > 0;) pts.push_back({2 * mx - pts[i][0], pts[i][1]});
        return polyShape(std::move(pts));
    }
    if (j.contains("or")) {
        s.k = Shape::Or;
        for (const auto& k : j["or"]) s.kids.push_back(shape(k, V));
        Box b = s.kids.empty() ? Box{} : s.kids[0].box;
        for (const auto& k : s.kids) b = uni(b, k.box);
        s.box = b;
        return s;
    }
    if (j.contains("and")) {
        s.k = Shape::And;
        for (const auto& k : j["and"]) s.kids.push_back(shape(k, V));
        Box b;
        b.inf = true;
        for (const auto& k : s.kids) {
            if (k.box.inf) continue;
            if (b.inf) { b = k.box; continue; }
            b.b[0] = std::max(b.b[0], k.box.b[0]), b.b[1] = std::max(b.b[1], k.box.b[1]);
            b.b[2] = std::min(b.b[2], k.box.b[2]), b.b[3] = std::min(b.b[3], k.box.b[3]);
        }
        s.box = b;
        return s;
    }
    if (j.contains("sub")) {
        s.k = Shape::Sub;
        s.kids.push_back(shape(j["sub"][0], V));
        s.kids.push_back(shape(j["sub"][1], V));
        s.box = s.kids[0].box;
        return s;
    }
    if (j.contains("mir") || j.contains("both")) {
        const bool mir = j.contains("mir");
        s.k = mir ? Shape::Mir : Shape::Both;
        s.kids.push_back(shape(mir ? j["mir"] : j["both"], V));
        s.m2 = 2 * mirrorX();
        const Box& a = s.kids[0].box;
        Box mb;
        if (a.inf) mb.inf = true;
        else mb.b[0] = s.m2 - a.b[2], mb.b[1] = a.b[1], mb.b[2] = s.m2 - a.b[0], mb.b[3] = a.b[3];
        s.box = mir ? mb : uni(a, mb);
        return s;
    }
    if (j.contains("use")) {   // a named shape with some numbers set
        Vars W = V;
        for (const auto& [k, e] : sub(j, "with").items()) W.put(k, num(e, V));
        return shape(sub(sub(D(), "shapes"), j["use"].get<std::string>().c_str()), W);
    }
    if (j.contains("where")) {
        s.k = Shape::Where;
        const Json& w = j["where"];
        s.where = &expr(w.is_string() ? w.get<std::string>() : w.dump());
        s.box.inf = true;
        return s;
    }
    return nothing();
}

// --- the scene ------------------------------------------------------------------------------
struct Scene {
    int w = 0, h = 0;
    double ox = 0, oy = 0, dy = 0, sx = 1, sy = 1, hx = 1, G = 66, HY = 22, mx = 24, f22 = 22;
    std::vector<Rgba> px;
    void inv(double lx, double ly, double& x, double& y) const {
        if (ly >= f22) { x = mx + (lx - mx) / sx; y = G - (G - ly) / sy; return; }
        x = mx + (lx - mx) / hx;
        y = ly - (f22 - HY);
    }
    void fwd(double x, double y, double& X, double& Y) const {
        if (y >= HY) { X = mx + (x - mx) * sx; Y = G - (G - y) * sy; return; }
        X = mx + (x - mx) * hx;
        Y = y + (f22 - HY);
    }
};
struct Mode { bool soft = false, flat = false, rim1 = false; };
Rgba toPx(const Col& c) { return rgba(c); }
Col ofPx(Rgba c) { return {int(c >> 16 & 255), int(c >> 8 & 255), int(c & 255)}; }

void paint(Scene& sc, const Shape& S, const Col& base, const Mode& mode, const Json* pat, const Colours& C, Vars& V) {
    const int w = sc.w, h = sc.h;
    const double oy = sc.oy + sc.dy;
    int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (!S.box.inf) {
        const double m = sc.mx;
        const double xs[4] = {m + (S.box.b[0] - m) * sc.sx, m + (S.box.b[2] - m) * sc.sx, m + (S.box.b[0] - m) * sc.hx, m + (S.box.b[2] - m) * sc.hx};
        double X, ya, yb;
        sc.fwd(0, S.box.b[1], X, ya);
        sc.fwd(0, S.box.b[3], X, yb);
        const double mn = std::min(std::min(xs[0], xs[1]), std::min(xs[2], xs[3])), mxx = std::max(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
        x0 = std::max(0, int(std::floor(mn + sc.ox)) - 2);
        x1 = std::min(w - 1, int(std::floor(mxx + sc.ox)) + 2);
        y0 = std::max(0, int(std::floor(ya + oy)) - 2);
        y1 = std::min(h - 1, int(std::floor(yb + oy)) + 2);
        if (x0 > x1 || y0 > y1) return;
    }
    std::vector<unsigned char> m(std::size_t(w * h), 0);
    bool any = false;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double px, py;
            sc.inv(x + 0.5 - sc.ox, y + 0.5 - oy, px, py);
            if (S.test(px, py, V)) { m[std::size_t(y * w + x)] = 1; any = true; }
        }
    if (!any) return;
    auto at = [&](int x, int y) { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : m[std::size_t(y * w + x)]; };
    const Json& sd = sub(D(), "shade");
    auto sdv = [&](const char* k, double def) { const double v = dnum(sd, k, 0); return v != 0 ? v : def; };
    const Col Lc = warm(sh(base, sdv("light", 1.32))), Sc = cool(sh(base, sdv("shadow", 0.76))), Dc = cool(sh(base, sdv("dark", 0.5)));
    const int rim = mode.rim1 ? 1 : 2;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            if (!m[std::size_t(y * w + x)]) continue;
            Col c = base;
            if (!at(x - 1, y - 1) || (rim > 1 && !at(x - 2, y - 2) && at(x + 2, y + 2))) c = Lc;
            if (!mode.flat) {
                if (!at(x + 2, y + 2)) c = Sc;
                if (!at(x + 1, y + 1) || (!at(x, y + 1) && !mode.soft)) c = Dc;
            }
            if (pat && pat->is_array()) {
                double px, py;
                sc.inv(x + 0.5 - sc.ox, y + 0.5 - oy, px, py);
                V.put(kX(), px);
                V.put(kY(), py);
                for (const auto& p : *pat)
                    if (num(sub(p, "if"), V) != 0) { c = colourJ(sub(p, "c"), C, V); break; }
            }
            sc.px[std::size_t(y * w + x)] = toPx(c);
        }
}
void dot(Scene& sc, double x, double y, const Col& col, double w, double h) {
    double fx, fy;
    sc.fwd(x, y, fx, fy);
    const int X0 = int(std::floor(fx + sc.ox + 0.5)), Y0 = int(std::floor(fy + sc.oy + sc.dy + 0.5));
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const int X = X0 + i, Y = Y0 + j;
            if (X >= 0 && Y >= 0 && X < sc.w && Y < sc.h) sc.px[std::size_t(Y * sc.w + X)] = toPx(col);
        }
}
std::vector<Rgba> finish(const Scene& sc) {
    const int w = sc.w, h = sc.h;
    std::vector<Rgba> out(std::size_t(w * h), 0);
    const double k = [&] { const double v = dnum(sub(D(), "shade"), "outline", 0); return v != 0 ? v : 0.3; }();
    auto get = [&](int x, int y) -> Rgba { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : sc.px[std::size_t(y * w + x)]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Rgba c = get(x, y);
            if (!c) {
                Rgba n = get(x, y - 1);
                if (!n) n = get(x - 1, y);
                if (!n) n = get(x + 1, y);
                if (!n) n = get(x, y + 1);
                if (n) c = toPx(sh(ofPx(n), k));
            }
            out[std::size_t(y * w + x)] = c;
        }
    return out;
}

bool onSide(const Json& o, char side) {
    const Json& s = sub(o, "sides");
    if (!s.is_string()) return true;
    return s.get<std::string>().find(side) != std::string::npos;
}
void drawOps(Scene& sc, const Json& ops, const Colours& C, Vars& V, char side) {
    if (!ops.is_array()) return;
    for (const auto& o : ops) {
        if (!o.is_object() || !onSide(o, side)) continue;
        if (o.contains("when") && num(o["when"], V) == 0) continue;
        if (o.contains("ops")) {
            Vars W = V;
            Colours K = C;
            for (const auto& [k, e] : sub(o, "with").items()) W.put(k, num(e, V));
            for (const auto& [k, e] : sub(o, "cwith").items()) K[k] = colourJ(e, C, V);
            drawOps(sc, sub(sub(D(), "ops"), o["ops"].get<std::string>().c_str()), K, W, side);
            continue;
        }
        const Json& cj = sub(o, "c");
        const Col col = cj.is_string() && !cj.get<std::string>().empty() ? colour(cj.get<std::string>(), C, V) : colour("skin", C, V);
        if (o.contains("dot")) {
            const Json& d = o["dot"];
            auto opt = [&](std::size_t i) { if (d.size() <= i) return 1.0; const double v = num(d[i], V); return d[i].is_number() && d[i].get<double>() == 0 ? 1.0 : v; };
            dot(sc, num(d[0], V), num(d[1], V), col, opt(2), opt(3));
            continue;
        }
        if (!o.contains("s")) continue;
        Mode mode;
        std::istringstream in(o.value("m", std::string()));
        for (std::string wd; in >> wd;) mode.soft |= wd == "soft", mode.flat |= wd == "flat", mode.rim1 |= wd == "rim1";
        const Shape S = shape(o["s"], V);
        paint(sc, S, col, mode, o.contains("pat") ? &o["pat"] : nullptr, C, V);
    }
}

const Json* pick(const Json& list, long long k) {
    if (!list.is_array() || list.empty()) return nullptr;
    const long long n = (long long)list.size();
    return &list[std::size_t(((k % n) + n) % n)];
}
std::string pickStr(const Json& list, long long k) {
    const Json* p = pick(list, k);
    return p && p->is_string() ? p->get<std::string>() : std::string();
}
std::string aliasOf(const char* table, const std::string& s) {
    const Json& a = sub(D(), table);
    const Json& v = sub(a, s.c_str());
    return v.is_string() ? v.get<std::string>() : s;
}

} // namespace

void setFigureData(const nlohmann::json& d) {
    store() = d.is_object() && d.contains("peoples") ? d : nlohmann::json::object();
    exprCache().clear();
}
const nlohmann::json& figureData() { return D(); }
bool figuresOn() { return D().contains("peoples") && D().value("enabled", true); }

std::string FigureSpec::key() const {
    std::string k = people + '|' + culture + '|' + outfit + '|' + std::to_string(v) + '|' + hair + '|' + beard + '|' + tool + '|' + skin + '|' + hairColour + '|' + dye + '|';
    k += char('0' + fem), k += char('0' + child), k += char('0' + hide), k += char('0' + kit);
    return k;
}

Figure drawFigure(const FigureSpec& spec, char sideIn, const PawnPose& pose) {
    Figure out;
    if (!D().contains("peoples")) return out;
    const Json& peoples = D()["peoples"];
    const Json* Pp = peoples.contains(spec.people) ? &peoples[spec.people] : nullptr;
    if (!Pp) {
        const std::string def = D().value("default_people", std::string());
        Pp = peoples.contains(def) ? &peoples[def] : &peoples.begin().value();
    }
    const Json& P = *Pp;
    const int v = spec.v;
    const std::string psex = P.value("sex", std::string());
    const bool fem = psex == "f" ? true : psex == "m" ? false : spec.fem, child = spec.child;
    const Json& outfits = sub(D(), "outfits");
    const Json* Op = !spec.outfit.empty() && outfits.contains(spec.outfit) ? &outfits[spec.outfit] : nullptr;
    std::string hair = spec.hair;
    if (hair.empty()) hair = pickStr(sub(P, fem ? "hair_f" : "hair_m"), v);
    if (hair.empty()) hair = fem ? "long" : "short";
    hair = aliasOf("hair_alias", hair);
    std::string beard = "none";
    if (!fem && !child) {
        beard = spec.beard;
        if (beard.empty()) beard = pickStr(sub(P, "beards"), (long long)v * 7 + 3);
        if (beard.empty()) beard = "none";
    }
    beard = aliasOf("beard_alias", beard);
    const char side = sideIn == 'W' ? 'E' : sideIn;
    const bool cen = P.value("cen", false), adult = !child;

    Vars V;
    for (const char* src : {"numbers"})
        for (const auto& [k, e] : sub(D(), src).items()) if (e.is_number()) V.put(k, e.get<double>());
    for (const auto& [k, e] : sub(P, "flags").items()) if (e.is_number()) V.put(k, e.get<double>());
    if (Op) for (const auto& [k, e] : sub(*Op, "flags").items()) if (e.is_number()) V.put(k, e.get<double>());
    const Json& tools = sub(D(), "tools");
    V.put("v", v), V.put("fem", fem), V.put("cen", cen), V.put("child", child), V.put("adult", adult), V.put("hide", spec.hide), V.put("kit", spec.kit);
    V.put("S", side == 'S'), V.put("E", side == 'E'), V.put("N", side == 'N');
    V.put("step", pose.step), V.put("bob", pose.bob), V.put("swing", pose.swing), V.put("dip", pose.dip), V.put("blink", pose.blink);
    V.put("hastool", !spec.tool.empty() && tools.contains(spec.tool));
    V.put("dressed", Op != nullptr), V.put("covered", Op && Op->value("covered", false)), V.put("helm", Op && Op->value("helm", false));
    V.put("x", 0), V.put("y", 0);

    Colours C;
    for (const auto& [k, c] : sub(D(), "colours").items()) if (c.is_string()) C[k] = hexc(c.get<std::string>());
    auto fromList = [&](const Json& list, long long k, const char* def) { const std::string c = pickStr(list, k); return hexc(c.empty() ? def : c); };
    C["skin"] = !spec.skin.empty() ? hexc(spec.skin) : fromList(sub(P, "skins"), v, "#d09a70");
    C["hair"] = !spec.hairColour.empty() ? hexc(spec.hairColour) : fromList(sub(P, "hairs"), v + 1, "#3a2418");
    C["iris"] = fromList(sub(P, "irises"), v, "#4a3a2a");
    if (P.contains("fur")) C["fur"] = fromList(P["fur"], v, "#6b4a2f");
    if (const Json* cc = pick(sub(P, "coats"), v); cc && cc->is_array() && cc->size() >= 2) C["coat"] = hexc((*cc)[0].get<std::string>()), C["coat_d"] = hexc((*cc)[1].get<std::string>());
    auto colourTable = [&](const Json& t) {
        for (const auto& [k, c] : t.items()) {
            if (c.is_array()) C[k] = fromList(c, v, "#ff00ff");
            else if (c.is_string()) C[k] = hexc(c.get<std::string>());
        }
    };
    colourTable(sub(P, "colours"));
    if (Op) colourTable(sub(*Op, "colours"));
    const Json& cultures = sub(D(), "cultures");
    if (!spec.culture.empty() && cultures.contains(spec.culture)) colourTable(sub(cultures[spec.culture], "colours"));
    if (!spec.dye.empty() && Op && sub(*Op, "dye").is_string() && !(*Op)["dye"].get<std::string>().empty()) C[(*Op)["dye"].get<std::string>()] = hexc(spec.dye);

    bool haveChest = false;
    Col chest{};
    if (Op && Op->contains("chest")) {
        const Json& ch = (*Op)["chest"];
        if (ch.is_string()) haveChest = true, chest = colour(ch.get<std::string>(), C, V);
        else if (ch.is_array())
            for (const auto& e : ch)
                if (!e.contains("when") || num(e["when"], V) != 0) {
                    haveChest = true;
                    chest = colourJ(sub(e, "c"), C, V);
                    break;
                }
    }
    V.put("haschest", haveChest);
    const bool bare = !haveChest || chest == C["skin"];
    V.put("bare", bare);
    const bool modest = bare && (spec.hide || child);
    V.put("modest", modest);
    if (modest) {
        const std::string mc = D().value(fem ? "modest_f" : "modest_m", std::string("#e6dcc4"));
        chest = colour(mc, C, V);
        haveChest = true;
        V.put("bare", 0), V.put("haschest", 1);
    }
    C["chest"] = haveChest ? chest : C["skin"];
    for (const auto& kv : sub(D(), "vars"))   // derived numbers, in order ([name, expr] pairs)
        if (kv.is_array() && kv.size() == 2 && kv[0].is_string()) V.put(kv[0].get<std::string>(), num(kv[1], V));

    // the build
    double tsx = 1, tsy = 1;
    const Json& tj = fem && P.contains("t_f") ? P["t_f"] : sub(P, "t");
    tsx = dnum(tj, "sx", 1), tsy = dnum(tj, "sy", 1);
    if (child) {
        const Json& ck = sub(D(), "child");
        tsx *= dnum(ck, "sx", 0.8), tsy *= dnum(ck, "sy", 0.62);
    }
    double thx = (tsx + 1) / 2;
    if (side == 'E') {
        const Json& e = sub(sub(D(), "views"), cen ? "E_cen" : "E");
        auto f = [&](const char* k) { const double x = dnum(e, k, 0); return x != 0 ? x : 1.0; };
        tsx *= f("sx"), thx *= f("hx");
    }
    const Json& cv = sub(D(), cen ? "canvas_cen" : "canvas");
    Scene sc;
    sc.w = int(dnum(cv, "w", 96)), sc.h = int(dnum(cv, "h", 96)), sc.ox = dnum(cv, "ox", 24), sc.oy = dnum(cv, "oy", 24);
    sc.sx = tsx, sc.sy = tsy, sc.hx = thx;
    sc.G = dnum(D(), "ground", 66), sc.HY = dnum(D(), "neck", 22), sc.mx = mirrorX();
    sc.f22 = sc.G - (sc.G - sc.HY) * tsy;
    sc.px.assign(std::size_t(sc.w * sc.h), 0);

    static const Json empty = Json::object();
    const Json* srcs[6] = {&sub(D(), "base"), &sub(P, "layers"), Op ? &sub(*Op, "layers") : &empty, &sub(sub(D(), "hairs"), hair.c_str()),
                           &sub(sub(D(), "beards"), beard.c_str()), spec.tool.empty() ? &empty : &sub(tools, spec.tool.c_str())};
    for (const auto& L : sub(D(), "order")) {
        const std::string name = L.is_string() ? L.get<std::string>() : L.value("l", std::string());
        sc.dy = -double(pose.bob) + (L.is_object() && L.contains("dy") ? num(L["dy"], V) : 0.0);
        for (const Json* s : srcs) drawOps(sc, sub(*s, name.c_str()), C, V, side);
    }
    const auto px = finish(sc);
    out.w = sc.w, out.h = sc.h;
    out.px.assign(px.size(), 0);
    for (int y = 0; y < sc.h; ++y)
        for (int x = 0; x < sc.w; ++x) out.px[std::size_t(y * sc.w + x)] = px[std::size_t(y * sc.w + (sideIn == 'W' ? sc.w - 1 - x : x))];
    const Json& an = sub(cv, "anchor");
    out.ax = an.is_array() && an.size() >= 2 ? an[0].get<int>() : sc.w / 2;
    out.ay = an.is_array() && an.size() >= 2 ? an[1].get<int>() : sc.h - 4;
    if (sideIn == 'W') out.ax = sc.w - 1 - out.ax;
    out.scale = dnum(P, "scale", 1);
    return out;
}

Pixels figurePixels(const FigureSpec& spec, char side, const PawnPose& pose) {
    const Figure f = drawFigure(spec, side, pose);
    Pixels p;
    const int n = std::max(f.w, f.h);
    p.size = n;
    p.px.assign(std::size_t(n * n), 0);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) p.px[std::size_t(y * n + x)] = f.px[std::size_t(y * f.w + x)];
    return p;
}

} // namespace game2d
