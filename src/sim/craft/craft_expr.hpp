#pragma once
// craft-expr: the small expression language of content/ai/craft/*.json
// (doc/design/CRAFT_EXPR.md). Formulas of functions, transforms, flaw chances
// and derive fields are compiled once at load and evaluated per item.
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
//   name    := [a-z_][a-z0-9_]* ( '.' [a-z0-9_]+ )*
// Truth is "!= 0"; comparisons and logic give 1 or 0. Numeric functions:
// min, max (any count), clamp(x, lo, hi), abs, floor, ceil, round. Symbolic
// functions take a bare name as their argument and are answered by the
// context: has(role), count(role), mass(role) / mass(), flaw(id), state(id),
// tag(t), eff(effect), role(r) (the piece itself has role r).

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace altera::sim::craft {

// What an expression can ask. Names are resolved by the evaluator's context
// (an item, a step); an unknown name reads 0.
struct ExprCtx {
    virtual ~ExprCtx() = default;
    virtual double var(const std::string& name) const = 0;
    // Symbolic call: fn in {has, count, mass, flaw, state, tag, eff}; arg may be empty (mass()).
    virtual double sym(const std::string& fn, const std::string& arg) const = 0;
};

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

} // namespace altera::sim::craft
