#pragma once
// The condition/effect mini-language of AI_DATA_SCHEMA.md §1.
//   condition: [!][t.]subject:key[op value]     op: >= <= > < == !=
//   effect:    [t.]subject:key(+|-|=)value  or  [t.]subject:key   (flag effects)
// `t.` addresses the target character instead of the actor. A key starting
// with `$` is a parameter of a parametric interaction (`params`), bound before
// evaluation by bindCondition / bindEffect.

#include <map>
#include <string>

#include "sim/core.hpp"

namespace altera::sim {

struct Condition {
    enum class Op { Truthy, Ge, Le, Gt, Lt, Eq, Ne };
    bool negate = false;
    bool onTarget = false; // t.<subject>: the target character
    std::string subject;   // without the t. prefix
    std::string key;
    Op op = Op::Truthy;
    double value = 0.0;
    std::string text;

    bool isParam() const { return !key.empty() && key[0] == '$'; }
    std::string param() const { return isParam() ? key.substr(1) : std::string(); }
    bool test(double v) const {
        bool r = false;
        switch (op) {
        case Op::Truthy: r = v > 0.0; break;
        case Op::Ge: r = v >= value - 1e-9; break;
        case Op::Le: r = v <= value + 1e-9; break;
        case Op::Gt: r = v > value; break;
        case Op::Lt: r = v < value; break;
        case Op::Eq: r = std::abs(v - value) < 1e-9; break;
        case Op::Ne: r = std::abs(v - value) >= 1e-9; break;
        }
        return negate ? !r : r;
    }
};

struct Effect {
    bool onTarget = false;    // t.<subject>: the target character
    bool onWitnesses = false; // w.<subject>: every witness (memory | moodlet | rel toward the actor)
    std::string subject;   // without the t. prefix
    std::string key;
    char op = 0; // '+', '-', '=' or 0 for a flag effect (moodlet/memory/spawn)
    double value = 0.0;
    std::string text;
    double signedDelta() const { return op == '-' ? -value : value; }
    bool isParam() const { return !key.empty() && key[0] == '$'; }
    std::string param() const { return isParam() ? key.substr(1) : std::string(); }
};

using ParamBinding = std::map<std::string, std::string>; // param name (no $) -> value id

// Throw std::runtime_error naming the text on a syntax error.
Condition parseCondition(const std::string& text);
Effect parseEffect(const std::string& text);

// Substitute `$name` keys from `b`; an unbound parameter throws.
Condition bindCondition(const Condition& k, const ParamBinding& b);
Effect bindEffect(const Effect& e, const ParamBinding& b);

} // namespace altera::sim
