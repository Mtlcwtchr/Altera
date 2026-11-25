#pragma once
// craft-expr is the general expression language now (sim/expr.hpp, LR-142); the craft
// code keeps its names through these aliases.

#include "sim/expr.hpp"

namespace lr::sim::craft {

using ::lr::sim::Constraint;
using ::lr::sim::Expr;
using ::lr::sim::ExprCtx;
using ::lr::sim::ExprNode;
using ::lr::sim::isSymbolicFn;

} // namespace lr::sim::craft
