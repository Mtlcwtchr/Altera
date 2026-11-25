#pragma once
// Named formulas of the content (LR-142, doc/design/DATA_DRIVEN.md §4):
// content/ai/formulas/*.json, any number of files, each
//
//   { "constants": { "persuade_cap": 0.95 },        // read as cfg.persuade_cap
//     "formulas": [
//       { "id": "persuade_bonus", "params": ["skill"], "defaults": {"skill": "eloquence"},
//         "expr": "attr.charm * 0.6 + skill.$skill * 0.4 + f.mood_edge",
//         "min": 0, "max": 1, "doc": "..." } ] }
//
// "expr" may also be a weighted mix, {"attr.charm": 0.5, "skill.eloquence": 0.3}: the
// weighted mean of those names (keys are names or f.<...> calls).
// A formula reads names with registered prefixes (sim/expr.hpp registerExprPrefix:
// attr., skill., trait., need., rep., t.<...> for the other side, cfg.<...>) and
// other formulas: `f.<id>` or `f.<id>(a, key=b)`. Parameters are substituted as
// text ($skill -> eloquence; an argument that is not a plain name or number goes in
// parentheses), each distinct call is compiled once. Unknown ids, unknown names,
// unbound parameters and cycles are load errors. Code asks the book by id; the
// numbers live in the data, so a balance change is a JSON edit.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sim/core.hpp"
#include "sim/expr.hpp"

namespace lr::sim {

struct FormulaDef {
    std::string id;
    std::string src;                          // the expression, with $param placeholders
    std::vector<std::string> params;          // in call order
    std::map<std::string, std::string> defaults;
    std::string doc;
    bool clampLo = false, clampHi = false;
    double lo = 0.0, hi = 0.0;
    std::string file;                         // where it came from (messages)
};

class FormulaBook {
public:
    FormulaBook() = default;
    FormulaBook(const FormulaBook& o) : defs_(o.defs_), constants_(o.constants_) {}
    FormulaBook& operator=(const FormulaBook& o) {
        if (this != &o) {
            defs_ = o.defs_;
            constants_ = o.constants_;
            compiled_.clear();
        }
        return *this;
    }
    FormulaBook(FormulaBook&&) = default;
    FormulaBook& operator=(FormulaBook&&) = default;

    // Optional extra check of a variable name at finalize (the Content knows its ids).
    using NameCheck = std::function<bool(const std::string& name, std::string* why)>;

    // Every *.json of `dir` through the data layers (sim/data_fs.hpp); a missing dir is an empty book.
    static FormulaBook load(const std::string& dir, std::vector<std::string>* errors);
    // One file's JSON ({"formulas": [...]} or a bare array); `file` names it in messages.
    void addJson(const Json& j, const std::string& file, std::vector<std::string>* errors);
    void add(FormulaDef d, std::vector<std::string>* errors);
    // Compiles every formula with its defaults and every call any of them makes, checks the
    // names (exprNameKnown, then `check`) and cycles. False when there were errors.
    bool finalize(std::vector<std::string>* errors, const NameCheck& check = {});

    bool has(const std::string& id) const { return defs_.count(id) > 0; }
    // "constants": {"name": number} of the formula files, read as cfg.<name>.
    const std::map<std::string, double>& constants() const { return constants_; }
    void setConstant(const std::string& name, double v) { constants_[name] = v; }
    const FormulaDef* def(const std::string& id) const;
    std::vector<std::string> ids() const;
    size_t size() const { return defs_.size(); }

    // The value of a formula by id ("persuade_bonus") or call ("f.persuade_bonus(skill=lore)"),
    // reading other names from `ctx`. An unknown formula gives `dflt`.
    double eval(const std::string& idOrCall, const ExprCtx& ctx, double dflt = 0.0) const;
    // Evaluates an expression compiled elsewhere (an act's chance in its own file), with
    // f.<...> names answered by this book.
    double evalExpr(const Expr& e, const ExprCtx& ctx, double dflt = 0.0) const;

    // A context that answers f.<...> from the book and everything else from `base`.
    struct Ctx : ExprCtx {
        const FormulaBook& book;
        const ExprCtx& base;
        mutable int depth = 0;
        Ctx(const FormulaBook& b, const ExprCtx& c) : book(b), base(c) {}
        double var(const std::string& name) const override;
        double sym(const std::string& fn, const std::string& arg) const override { return base.sym(fn, arg); }
        double rand01() const override { return base.rand01(); }
    };

private:
    struct Compiled {
        Expr expr;
        const FormulaDef* def = nullptr;
    };
    // Specialises "f.<id>(args)" / "f.<id>" / "<id>": the substituted source compiled.
    const Compiled* prepare(const std::string& call, std::string* error) const;
    double evalCompiled(const Compiled& c, const Ctx& ctx) const;

    std::map<std::string, FormulaDef> defs_;
    std::map<std::string, double> constants_;
    mutable std::map<std::string, std::unique_ptr<Compiled>> compiled_; // by canonical call
    mutable std::unique_ptr<std::mutex> mu_ = std::make_unique<std::mutex>();
};

// "f.x(a,k=b)" -> id "x", args {{"", "a"}, {"k", "b"}}; "f.x" / "x" -> id "x", no args.
bool parseFormulaCall(const std::string& call, std::string& id, std::vector<std::pair<std::string, std::string>>& args);
// The formula's source with its parameters bound (defaults fill the rest); false + why on a bad call.
bool bindFormula(const FormulaDef& d, const std::vector<std::pair<std::string, std::string>>& args, std::string& out,
                 std::string* why);

// A plain context over a map of names (tests, tools, the validator).
struct MapExprCtx : ExprCtx {
    std::map<std::string, double> vars;
    double roll = 0.5;
    double var(const std::string& name) const override {
        auto it = vars.find(name);
        return it == vars.end() ? 0.0 : it->second;
    }
    double sym(const std::string&, const std::string&) const override { return 0.0; }
    double rand01() const override { return roll; }
};

} // namespace lr::sim
