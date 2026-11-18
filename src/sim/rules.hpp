#pragma once
// The condition/effect mini-language of AI_DATA_SCHEMA.md §1.
//   condition: [!]subject:key[op value]     op: >= <= > < == !=
//   effect:    subject:key(+|-|=)value  or  subject:key   (moodlet/memory/spawn)

#include <string>

#include "sim/core.hpp"

namespace altera::sim {

struct Condition {
    enum class Op { Truthy, Ge, Le, Gt, Lt, Eq, Ne };
    bool negate = false;
    std::string subject;
    std::string key;
    Op op = Op::Truthy;
    double value = 0.0;
    std::string text;

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
    std::string subject;
    std::string key;
    char op = 0; // '+', '-', '=' or 0 for a flag effect (moodlet/memory/spawn)
    double value = 0.0;
    std::string text;
    double signedDelta() const { return op == '-' ? -value : value; }
};

// Throw std::runtime_error naming the text on a syntax error.
Condition parseCondition(const std::string& text);
Effect parseEffect(const std::string& text);

} // namespace altera::sim
