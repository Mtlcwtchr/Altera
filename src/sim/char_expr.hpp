#pragma once
// The expression context of a person (LR-142): what a named formula
// (sim/formulas.hpp) reads about the one who acts and the other side.
//
//   attr.<id>   the attribute 0..1 (expansion attributes.json)
//   skill.<id>  the skill level 0..1 (knowledge ids)
//   trait.<id>  1 when the person has the trait, else 0
//   need.<id>   the need 0..1 (1 = satisfied)
//   rep.<axis>  reputation -1..1
//   rel.<axis>  the person's relation to the other side (rel axes)
//   age         age in years
//   t.<any of the above>   the same of the other side (0 without one)
//   arg.<name>  a number the calling code passes in
//
// Modules with richer models (stats, derived values) register more prefixes and
// answer them by subclassing or by `extra`.

#include <functional>
#include <map>
#include <string>

#include "sim/expr.hpp"

namespace lr::sim {

struct Character;
struct Content;

struct CharExprCtx : ExprCtx {
    const Character& self;
    const Character* other = nullptr;
    std::map<std::string, double> args;                     // arg.<name>
    std::function<double()> roll;                           // rand01; empty = 0.5
    std::function<bool(const std::string&, double&)> extra; // names no prefix here answers
    explicit CharExprCtx(const Character& c, const Character* t = nullptr) : self(c), other(t) {}
    double var(const std::string& name) const override;
    double sym(const std::string&, const std::string&) const override { return 0.0; }
    double rand01() const override { return roll ? roll() : 0.5; }
};

// The name check of these prefixes against the content's ids (FormulaBook::finalize).
bool charExprNameKnown(const Content& c, const std::string& name, std::string* why);

} // namespace lr::sim
