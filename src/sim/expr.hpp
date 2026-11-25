#pragma once
// expr (born as craft-expr): the small expression language of the content
// (doc/design/CRAFT_EXPR.md, doc/design/DATA_DRIVEN.md). Craft formulas, the named
// formulas of content/ai/formulas/*.json (sim/formulas.hpp) and any other number
// the data decides are compiled once at load and evaluated per use.
//
//   expr    := or ( '?' expr ':' expr )?
//   or      := and ( ('or' | '||') and )*
//   and     := not ( ('and' | '&&') not )*
//   not     := ('not' | '!') not | cmp
//   cmp     := add ( ('<' | '<=' | '>' | '>=' | '==' | '!=') add )?
//   add     := mul ( ('+' | '-') mul )*
//   mul     := unary ( ('*' | '/') unary )*
//   unary   := '-' unary | primary
//   primary := number | name | name '(' args ')' | '(' expr ')'
//            | 'f.' id '(' [ (key '=')? text { ',' ... } ] ')'   a named formula with arguments
//   name    := [a-z_][a-z0-9_]* ( '.' [a-z0-9_]+ )*
// Truth is "!= 0"; comparisons and logic give 1 or 0. Numeric functions:
// min, max (any count), clamp(x, lo, hi), abs, floor, ceil, round, pow(b, e),
// sqrt, exp, log, lerp(a, b, t), sign, step(edge, x); chance: noise(mean, sd) (a normal
// draw from the context's rand01), p_over(mean, sd, t) (the chance such a draw is above t,
// for the statistical LOD). Symbolic
// functions take a bare name as their argument and are answered by the
// context: has(role), count(role), mass(role) / mass(), flaw(id), state(id),
// tag(t), eff(effect), role(r) (the piece itself has role r).

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace lr::sim {

// What an expression can ask. Names are resolved by the evaluator's context
// (an item, a step); an unknown name reads 0.
struct ExprCtx {
    virtual ~ExprCtx() = default;
    virtual double var(const std::string& name) const = 0;
    // Symbolic call: fn in {has, count, mass, flaw, state, tag, eff}; arg may be empty (mass()).
    virtual double sym(const std::string& fn, const std::string& arg) const = 0;
    // A draw in [0, 1) for functions that roll (registered by modules); the sim's
    // contexts answer it from the world's seeded stream. The default is the middle,
    // so a roll in a context without chance gives its expectation-ish value.
    virtual double rand01() const { return 0.5; }
};

// A function a module adds to the language (registerExprFn), e.g. roll_d20(),
// p_d20(bonus, dc). It must be registered before the content that uses it compiles.
struct ExprFn {
    using Eval = std::function<double(const std::vector<double>& args, const ExprCtx& ctx)>;
    int minArgs = 0, maxArgs = 0;
    Eval eval;
};
void registerExprFn(const std::string& name, int minArgs, int maxArgs, ExprFn::Eval eval);
bool isExprFn(const std::string& name);

// Which variable names the data may read: every name in a named formula
// (sim/formulas.hpp) must start with a registered prefix ("attr.", "skill.",
// "cfg.", ...) whose check, if given, accepts the rest (the id); else the
// formula fails validation. At run time an unknown name still reads 0.
using ExprPrefixCheck = std::function<bool(const std::string& rest, std::string* why)>;
void registerExprPrefix(const std::string& prefix, ExprPrefixCheck known = {});
bool exprNameKnown(const std::string& name, std::string* why);

struct ExprNode;

class Expr {
public:
    Expr() = default;
    // Empty source compiles to "no expression" (eval gives `dflt`).
    static Expr compile(const std::string& src, std::string* error);
    static Expr constant(double v);
    bool empty() const { return !root_; }
    double eval(const ExprCtx& ctx, double dflt = 0.0) const;
    const std::string& source() const { return src_; }
    // Plain variable names read (with dots) and symbolic calls as "fn(arg)".
    const std::set<std::string>& names() const { return names_; }
    const std::set<std::string>& symbols() const { return syms_; }

private:
    std::shared_ptr<const ExprNode> root_;
    std::string src_;
    std::set<std::string> names_;
    std::set<std::string> syms_;
};

bool isSymbolicFn(const std::string& fn);

// A condition on a number written as a short string: ">=0.5", "<0.3",
// "2..3" (inclusive), "1" (exactly, within 1e-6), "==1", "!=0".
struct Constraint {
    double lo = -1e300, hi = 1e300;
    bool notEqual = false;
    double neValue = 0.0;
    static bool parse(const std::string& s, Constraint& out, std::string* error);
    bool ok(double x) const;
    // 1 inside; falls linearly to 0 over `falloff` outside.
    double closeness(double x, double falloff) const;
};

} // namespace lr::sim
