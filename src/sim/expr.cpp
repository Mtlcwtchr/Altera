#include "sim/expr.hpp"

#include <cctype>
#include <cmath>
#include <map>
#include <cstdlib>
#include <stdexcept>

namespace lr::sim {

namespace {

enum class Op {
    Num, Var, Sym, Neg, Not, Add, Sub, Mul, Div, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Tern,
    Min, Max, Clamp, Abs, Floor, Ceil, Round, Pow, Sqrt, Exp, Log, Lerp, Sign, Step, Fn, Noise, POver
};

} // namespace

struct ExprNode {
    Op op = Op::Num;
    double num = 0.0;
    std::string name, arg; // Var: name; Sym: name(fn) + arg; Fn: name of a registered function
    const ExprFn* fn = nullptr;
    std::vector<std::shared_ptr<const ExprNode>> kids;
};

namespace {

using NodeP = std::shared_ptr<const ExprNode>;

const std::set<std::string>& symbolicFns() {
    static const std::set<std::string> s{"has", "count", "mass", "flaw", "state", "tag", "eff", "role"};
    return s;
}

std::map<std::string, ExprFn>& fnRegistry() {
    static std::map<std::string, ExprFn> r;
    return r;
}

// The raw text of a call argument: up to the next ',' or ')' at depth 0.
std::string trimmed(const std::string& t) {
    size_t a = 0, b = t.size();
    while (a < b && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(t[b - 1]))) --b;
    return t.substr(a, b - a);
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::set<std::string>* names;
    std::set<std::string>* syms;

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error(what + " at " + std::to_string(i) + " in '" + s + "'");
    }
    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool peek(const char* t) {
        ws();
        size_t n = std::char_traits<char>::length(t);
        if (s.compare(i, n, t) != 0) return false;
        // word operators must end at a word boundary
        if (std::isalpha(static_cast<unsigned char>(t[0])) && i + n < s.size() &&
            (std::isalnum(static_cast<unsigned char>(s[i + n])) || s[i + n] == '_'))
            return false;
        return true;
    }
    bool eat(const char* t) {
        if (!peek(t)) return false;
        i += std::char_traits<char>::length(t);
        return true;
    }
    static NodeP mk(Op op, std::vector<NodeP> kids) {
        auto n = std::make_shared<ExprNode>();
        n->op = op;
        n->kids = std::move(kids);
        return n;
    }

    NodeP expr() {
        NodeP c = orE();
        if (eat("?")) {
            NodeP a = expr();
            if (!eat(":")) fail("expected ':'");
            NodeP b = expr();
            return mk(Op::Tern, {c, a, b});
        }
        return c;
    }
    NodeP orE() {
        NodeP l = andE();
        while (eat("||") || eat("or")) l = mk(Op::Or, {l, andE()});
        return l;
    }
    NodeP andE() {
        NodeP l = notE();
        while (eat("&&") || eat("and")) l = mk(Op::And, {l, notE()});
        return l;
    }
    NodeP notE() {
        if (peek("!=")) fail("unexpected '!='");
        if (eat("not") || eat("!")) return mk(Op::Not, {notE()});
        return cmp();
    }
    NodeP cmp() {
        NodeP l = add();
        if (eat("<=")) return mk(Op::Le, {l, add()});
        if (eat(">=")) return mk(Op::Ge, {l, add()});
        if (eat("==")) return mk(Op::Eq, {l, add()});
        if (eat("!=")) return mk(Op::Ne, {l, add()});
        if (eat("<")) return mk(Op::Lt, {l, add()});
        if (eat(">")) return mk(Op::Gt, {l, add()});
        return l;
    }
    NodeP add() {
        NodeP l = mul();
        for (;;) {
            if (eat("+")) l = mk(Op::Add, {l, mul()});
            else if (peek("-")) { ++i; l = mk(Op::Sub, {l, mul()}); }
            else return l;
        }
    }
    NodeP mul() {
        NodeP l = unary();
        for (;;) {
            if (eat("*")) l = mk(Op::Mul, {l, unary()});
            else if (eat("/")) l = mk(Op::Div, {l, unary()});
            else return l;
        }
    }
    NodeP unary() {
        if (eat("-")) return mk(Op::Neg, {unary()});
        if (eat("+")) return unary();
        return primary();
    }
    std::string ident() {
        ws();
        size_t b = i;
        auto word = [&]() {
            if (i >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                fail("expected a name");
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
        };
        word();
        while (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                fail("expected a name after '.'");
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
        }
        return s.substr(b, i - b);
    }
    // f.<id>(a, k=b): a call of a named formula with arguments. The arguments are kept as
    // text and the call reads as one name, "f.<id>(a,k=b)", answered by the formula book
    // (sim/formulas.hpp), which specialises the formula once per distinct call.
    NodeP formulaCall(const std::string& id) {
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        for (;;) {
            if (i >= s.size()) fail("expected ')' after " + id + "(");
            const char ch = s[i++];
            if (depth == 0 && (ch == ',' || ch == ')')) {
                const std::string t = trimmed(cur);
                if (!t.empty() || ch == ',') {
                    if (t.empty()) fail("empty argument in " + id + "(");
                    parts.push_back(t);
                }
                cur.clear();
                if (ch == ')') break;
                continue;
            }
            if (ch == '(') ++depth;
            if (ch == ')') --depth;
            cur += ch;
        }
        std::string name = id + "(";
        for (size_t k = 0; k < parts.size(); ++k) {
            std::string a;
            for (char ch : parts[k])
                if (!std::isspace(static_cast<unsigned char>(ch))) a += ch;
            name += (k ? "," : "") + a;
        }
        name += ")";
        auto n = std::make_shared<ExprNode>();
        n->op = Op::Var;
        n->name = name;
        if (names) names->insert(name);
        return n;
    }
    NodeP primary() {
        ws();
        if (i >= s.size()) fail("unexpected end");
        char ch = s[i];
        if (ch == '(') {
            ++i;
            NodeP e = expr();
            if (!eat(")")) fail("expected ')'");
            return e;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
            const char* b = s.c_str() + i;
            char* e = nullptr;
            double v = std::strtod(b, &e);
            if (e == b) fail("bad number");
            i += static_cast<size_t>(e - b);
            auto n = std::make_shared<ExprNode>();
            n->op = Op::Num;
            n->num = v;
            return n;
        }
        std::string id = ident();
        if (!peek("(")) {
            auto n = std::make_shared<ExprNode>();
            if (id == "true" || id == "false") {
                n->op = Op::Num;
                n->num = id == "true" ? 1.0 : 0.0;
                return n;
            }
            n->op = Op::Var;
            n->name = id;
            if (names) names->insert(id);
            return n;
        }
        eat("(");
        if (id.rfind("f.", 0) == 0) return formulaCall(id);
        if (symbolicFns().count(id)) {
            std::string arg;
            if (!peek(")")) arg = ident();
            if (!eat(")")) fail("expected ')' after " + id + "(");
            auto n = std::make_shared<ExprNode>();
            n->op = Op::Sym;
            n->name = id;
            n->arg = arg;
            if (syms) syms->insert(id + "(" + arg + ")");
            return n;
        }
        std::vector<NodeP> args;
        if (!peek(")")) {
            args.push_back(expr());
            while (eat(",")) args.push_back(expr());
        }
        if (!eat(")")) fail("expected ')'");
        auto need = [&](size_t lo, size_t hi) {
            if (args.size() < lo || args.size() > hi) fail("wrong argument count for " + id);
        };
        Op op;
        if (id == "min") { need(1, 64); op = Op::Min; }
        else if (id == "max") { need(1, 64); op = Op::Max; }
        else if (id == "clamp") { need(3, 3); op = Op::Clamp; }
        else if (id == "abs") { need(1, 1); op = Op::Abs; }
        else if (id == "floor") { need(1, 1); op = Op::Floor; }
        else if (id == "ceil") { need(1, 1); op = Op::Ceil; }
        else if (id == "round") { need(1, 1); op = Op::Round; }
        else if (id == "pow") { need(2, 2); op = Op::Pow; }
        else if (id == "sqrt") { need(1, 1); op = Op::Sqrt; }
        else if (id == "exp") { need(1, 1); op = Op::Exp; }
        else if (id == "log") { need(1, 1); op = Op::Log; }
        else if (id == "lerp") { need(3, 3); op = Op::Lerp; }
        else if (id == "sign") { need(1, 1); op = Op::Sign; }
        else if (id == "step") { need(2, 2); op = Op::Step; }
        else if (id == "noise") { need(2, 2); op = Op::Noise; }
        else if (id == "p_over") { need(3, 3); op = Op::POver; }
        else if (auto it = fnRegistry().find(id); it != fnRegistry().end()) {
            need(static_cast<size_t>(it->second.minArgs), static_cast<size_t>(it->second.maxArgs));
            auto n = mk(Op::Fn, std::move(args));
            auto* m = const_cast<ExprNode*>(n.get());
            m->name = id;
            m->fn = &it->second;
            return n;
        }
        else fail("unknown function " + id);
        return mk(op, std::move(args));
    }
};

double evalNode(const ExprNode& n, const ExprCtx& c) {
    auto k = [&](size_t j) { return evalNode(*n.kids[j], c); };
    switch (n.op) {
    case Op::Num: return n.num;
    case Op::Var: return c.var(n.name);
    case Op::Sym: return c.sym(n.name, n.arg);
    case Op::Neg: return -k(0);
    case Op::Not: return k(0) != 0.0 ? 0.0 : 1.0;
    case Op::Add: return k(0) + k(1);
    case Op::Sub: return k(0) - k(1);
    case Op::Mul: return k(0) * k(1);
    case Op::Div: {
        double d = k(1);
        return std::fabs(d) < 1e-12 ? 0.0 : k(0) / d;
    }
    case Op::Lt: return k(0) < k(1) ? 1.0 : 0.0;
    case Op::Le: return k(0) <= k(1) ? 1.0 : 0.0;
    case Op::Gt: return k(0) > k(1) ? 1.0 : 0.0;
    case Op::Ge: return k(0) >= k(1) ? 1.0 : 0.0;
    case Op::Eq: return std::fabs(k(0) - k(1)) < 1e-9 ? 1.0 : 0.0;
    case Op::Ne: return std::fabs(k(0) - k(1)) < 1e-9 ? 0.0 : 1.0;
    case Op::And: return (k(0) != 0.0 && k(1) != 0.0) ? 1.0 : 0.0;
    case Op::Or: return (k(0) != 0.0 || k(1) != 0.0) ? 1.0 : 0.0;
    case Op::Tern: return k(0) != 0.0 ? k(1) : k(2);
    case Op::Min: {
        double v = k(0);
        for (size_t j = 1; j < n.kids.size(); ++j) v = std::min(v, k(j));
        return v;
    }
    case Op::Max: {
        double v = k(0);
        for (size_t j = 1; j < n.kids.size(); ++j) v = std::max(v, k(j));
        return v;
    }
    case Op::Clamp: return std::min(std::max(k(0), k(1)), k(2));
    case Op::Abs: return std::fabs(k(0));
    case Op::Floor: return std::floor(k(0));
    case Op::Ceil: return std::ceil(k(0) - 1e-9);
    case Op::Round: return std::round(k(0));
    case Op::Pow: {
        const double b = k(0), e = k(1);
        const double v = std::pow(b, e);
        return std::isfinite(v) ? v : 0.0;
    }
    case Op::Sqrt: return std::sqrt(std::max(0.0, k(0)));
    case Op::Exp: return std::exp(std::min(k(0), 700.0));
    case Op::Log: {
        const double x = k(0);
        return x > 0.0 ? std::log(x) : 0.0;
    }
    case Op::Lerp: return k(0) + (k(1) - k(0)) * k(2);
    case Op::Sign: {
        const double x = k(0);
        return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0;
    }
    case Op::Step: return k(1) >= k(0) ? 1.0 : 0.0; // step(edge, x)
    case Op::Noise: { // noise(mean, sd): a normal draw from the context's stream (Box-Muller)
        const double mean = k(0), sd = k(1);
        const double u1 = std::max(1e-12, c.rand01()), u2 = c.rand01();
        return mean + sd * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
    case Op::POver: { // p_over(mean, sd, t): the chance a normal draw is above t
        const double mean = k(0), sd = k(1), t = k(2);
        if (sd <= 1e-12) return mean > t ? 1.0 : 0.0;
        return 0.5 * std::erfc((t - mean) / (sd * std::sqrt(2.0)));
    }
    case Op::Fn: {
        std::vector<double> a(n.kids.size());
        for (size_t j = 0; j < n.kids.size(); ++j) a[j] = k(j);
        const double v = n.fn->eval(a, c);
        return std::isfinite(v) ? v : 0.0;
    }
    }
    return 0.0;
}

} // namespace

bool isSymbolicFn(const std::string& fn) { return symbolicFns().count(fn) > 0; }

void registerExprFn(const std::string& name, int minArgs, int maxArgs, ExprFn::Eval eval) {
    ExprFn& f = fnRegistry()[name];
    f.minArgs = minArgs;
    f.maxArgs = maxArgs;
    f.eval = std::move(eval);
}

bool isExprFn(const std::string& name) { return fnRegistry().count(name) > 0; }

namespace {
std::map<std::string, ExprPrefixCheck>& prefixRegistry() {
    static std::map<std::string, ExprPrefixCheck> r;
    return r;
}
} // namespace

void registerExprPrefix(const std::string& prefix, ExprPrefixCheck known) { prefixRegistry()[prefix] = std::move(known); }

bool exprNameKnown(const std::string& name, std::string* why) {
    for (const auto& [p, check] : prefixRegistry()) {
        if (name.compare(0, p.size(), p) != 0) continue;
        if (!check) return true;
        std::string w;
        if (check(name.substr(p.size()), &w)) return true;
        if (why) *why = w.empty() ? "unknown " + p + " id '" + name.substr(p.size()) + "'" : w;
        return false;
    }
    if (why) *why = "no data source answers '" + name + "' (registered prefixes: " + [] {
        std::string all;
        for (const auto& [p, c] : prefixRegistry()) all += (all.empty() ? "" : " ") + p;
        return all.empty() ? std::string("none") : all;
    }() + ")";
    return false;
}

Expr Expr::compile(const std::string& src, std::string* error) {
    Expr e;
    e.src_ = src;
    bool blank = true;
    for (char ch : src)
        if (!std::isspace(static_cast<unsigned char>(ch))) blank = false;
    if (blank) return e;
    try {
        Parser p{src, 0, &e.names_, &e.syms_};
        NodeP r = p.expr();
        p.ws();
        if (p.i != src.size()) p.fail("trailing input");
        e.root_ = r;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        e.root_.reset();
        e.names_.clear();
        e.syms_.clear();
    }
    return e;
}

Expr Expr::constant(double v) {
    Expr e;
    auto n = std::make_shared<ExprNode>();
    n->op = Op::Num;
    n->num = v;
    e.root_ = n;
    e.src_ = std::to_string(v);
    return e;
}

double Expr::eval(const ExprCtx& ctx, double dflt) const {
    if (!root_) return dflt;
    double v = evalNode(*root_, ctx);
    return std::isfinite(v) ? v : 0.0;
}

bool Constraint::parse(const std::string& s0, Constraint& out, std::string* error) {
    std::string s;
    for (char ch : s0)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += ch;
    out = Constraint{};
    auto num = [&](const std::string& t, double& v) {
        if (t.empty()) return false;
        char* e = nullptr;
        v = std::strtod(t.c_str(), &e);
        return e && *e == '\0';
    };
    double a = 0, b = 0;
    bool ok = false;
    if (s.rfind(">=", 0) == 0) { ok = num(s.substr(2), a); out.lo = a; }
    else if (s.rfind("<=", 0) == 0) { ok = num(s.substr(2), a); out.hi = a; }
    else if (s.rfind("==", 0) == 0) { ok = num(s.substr(2), a); out.lo = out.hi = a; }
    else if (s.rfind("!=", 0) == 0) { ok = num(s.substr(2), a); out.notEqual = true; out.neValue = a; }
    else if (s.rfind(">", 0) == 0) { ok = num(s.substr(1), a); out.lo = a + 1e-9; }
    else if (s.rfind("<", 0) == 0) { ok = num(s.substr(1), a); out.hi = a - 1e-9; }
    else if (size_t d = s.find(".."); d != std::string::npos) {
        ok = num(s.substr(0, d), a) && num(s.substr(d + 2), b) && a <= b;
        out.lo = a;
        out.hi = b;
    } else {
        ok = num(s, a);
        out.lo = out.hi = a;
    }
    if (!ok && error) *error = "bad constraint '" + s0 + "'";
    return ok;
}

bool Constraint::ok(double x) const {
    if (notEqual) return std::fabs(x - neValue) > 1e-6;
    return x >= lo - 1e-6 && x <= hi + 1e-6;
}

double Constraint::closeness(double x, double falloff) const {
    if (ok(x)) return 1.0;
    if (notEqual || falloff <= 0) return 0.0;
    double d = x < lo ? lo - x : x - hi;
    return std::max(0.0, 1.0 - d / falloff);
}

} // namespace lr::sim
