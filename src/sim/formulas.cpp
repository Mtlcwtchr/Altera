#include "sim/formulas.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <set>

#include "sim/data_fs.hpp"

namespace lr::sim {

namespace {

bool plainToken(const std::string& s) {
    if (s.empty()) return false;
    for (char ch : s)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.')) return false;
    return true;
}

bool identChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// Names with a ".$" or a leading "$" left after binding: an unbound parameter.
bool hasDollar(const std::string& s) { return s.find('$') != std::string::npos; }

// The prefixes every sim context answers; modules add their own (registerExprPrefix).
const bool kBuiltinPrefixes = [] {
    registerExprPrefix("cfg.");   // a constant of a config (checked by the content's NameCheck)
    registerExprPrefix("arg.");   // a value the calling code passes in by name
    return true;
}();

} // namespace

bool parseFormulaCall(const std::string& call, std::string& id, std::vector<std::pair<std::string, std::string>>& args) {
    args.clear();
    std::string s = call;
    if (s.rfind("f.", 0) == 0) s = s.substr(2);
    const auto open = s.find('(');
    if (open == std::string::npos) {
        id = s;
        return !id.empty();
    }
    if (s.back() != ')') return false;
    id = s.substr(0, open);
    const std::string inner = s.substr(open + 1, s.size() - open - 2);
    std::string cur;
    int depth = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        size_t eq = std::string::npos;
        int d = 0;
        for (size_t k = 0; k < cur.size(); ++k) {
            if (cur[k] == '(') ++d;
            else if (cur[k] == ')') --d;
            else if (d == 0 && cur[k] == '=' && (k + 1 >= cur.size() || cur[k + 1] != '=') && (k == 0 || (cur[k - 1] != '<' && cur[k - 1] != '>' && cur[k - 1] != '!' && cur[k - 1] != '='))) {
                eq = k;
                break;
            }
        }
        if (eq != std::string::npos && plainToken(cur.substr(0, eq))) args.emplace_back(cur.substr(0, eq), cur.substr(eq + 1));
        else args.emplace_back("", cur);
        cur.clear();
    };
    for (char ch : inner) {
        if (ch == '(') ++depth;
        if (ch == ')') --depth;
        if (ch == ',' && depth == 0) flush();
        else cur += ch;
    }
    flush();
    return !id.empty();
}

bool bindFormula(const FormulaDef& d, const std::vector<std::pair<std::string, std::string>>& args, std::string& out,
                 std::string* why) {
    std::map<std::string, std::string> bound;
    size_t pos = 0;
    for (const auto& [k, v] : args) {
        if (k.empty()) {
            if (pos >= d.params.size()) {
                if (why) *why = "too many arguments for f." + d.id;
                return false;
            }
            bound[d.params[pos++]] = v;
        } else {
            if (std::find(d.params.begin(), d.params.end(), k) == d.params.end()) {
                if (why) *why = "f." + d.id + " has no parameter '" + k + "'";
                return false;
            }
            bound[k] = v;
        }
    }
    for (const auto& p : d.params)
        if (!bound.count(p)) {
            auto it = d.defaults.find(p);
            if (it == d.defaults.end()) {
                if (why) *why = "f." + d.id + ": no value for parameter '" + p + "'";
                return false;
            }
            bound[p] = it->second;
        }
    out.clear();
    const std::string& s = d.src;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '$') {
            size_t j = i + 1;
            while (j < s.size() && identChar(s[j])) ++j;
            const std::string name = s.substr(i + 1, j - i - 1);
            auto it = bound.find(name);
            if (it == bound.end()) {
                if (why) *why = "f." + d.id + ": '$" + name + "' is not a parameter";
                return false;
            }
            // inside a name (skill.$skill) the value must be a plain id; standing alone it may be an expression
            const bool inName = i > 0 && s[i - 1] == '.';
            const std::string& v = it->second;
            if (inName && !plainToken(v)) {
                if (why) *why = "f." + d.id + ": '" + v + "' is not an id (for $" + name + ")";
                return false;
            }
            out += (inName || plainToken(v)) ? v : "(" + v + ")";
            i = j;
        } else
            out += s[i++];
    }
    return true;
}

FormulaBook FormulaBook::load(const std::string& dir, std::vector<std::string>* errors) {
    FormulaBook b;
    if (!data::isDir(dir)) return b;
    for (const auto& name : data::listFiles(dir, ".json")) {
        const std::string path = dir + "/" + name;
        try {
            b.addJson(data::readJson(path), "formulas/" + name, errors);
        } catch (const std::exception& e) {
            if (errors) errors->push_back(e.what());
        }
    }
    return b;
}

void FormulaBook::addJson(const Json& j, const std::string& file, std::vector<std::string>* errors) {
    const Json consts = j.is_object() ? j.value("constants", Json::object()) : Json::object(); // a local: items() of a temporary dangles
    if (consts.is_object())
        for (const auto& [k, v] : consts.items()) {
            if (v.is_number()) constants_[k] = v.get<double>();
            else if (v.is_boolean()) constants_[k] = v.get<bool>() ? 1.0 : 0.0;
            else if (errors) errors->push_back(file + ": constant '" + k + "' is not a number");
        }
    const Json list = j.is_array() ? j : j.value("formulas", Json::array());
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        FormulaDef d;
        d.id = e.value("id", std::string());
        d.file = file;
        const auto& ex = e.contains("expr") ? e["expr"] : Json();
        if (ex.is_number()) d.src = ex.dump();
        else if (ex.is_string()) d.src = ex.get<std::string>();
        else if (ex.is_object()) { // a weighted mix {"attr.charm": 0.5, "skill.eloquence": 0.3}: the weighted mean
            std::string sum;
            double total = 0.0;
            for (const auto& [k, w] : ex.items()) {
                if (!w.is_number()) {
                    if (errors) errors->push_back(file + ": f." + d.id + ": weight of '" + k + "' is not a number");
                    continue;
                }
                const double wv = w.get<double>();
                total += wv;
                sum += (sum.empty() ? "" : " + ") + k + " * " + w.dump();
            }
            if (!sum.empty()) d.src = total > 1e-12 ? "(" + sum + ") / " + Json(total).dump() : sum;
        }
        d.doc = e.value("doc", std::string());
        for (const auto& p : e.value("params", Json::array()))
            if (p.is_string()) d.params.push_back(p.get<std::string>());
        const Json defs = e.value("defaults", Json::object());
        for (const auto& [k, v] : defs.items()) d.defaults[k] = v.is_string() ? v.get<std::string>() : v.dump();
        if (e.contains("min") && e["min"].is_number()) d.clampLo = true, d.lo = e["min"].get<double>();
        if (e.contains("max") && e["max"].is_number()) d.clampHi = true, d.hi = e["max"].get<double>();
        add(std::move(d), errors);
    }
}

void FormulaBook::add(FormulaDef d, std::vector<std::string>* errors) {
    if (d.id.empty() || !plainToken(d.id) || d.id.find('.') != std::string::npos) {
        if (errors) errors->push_back(d.file + ": a formula needs a plain id (got '" + d.id + "')");
        return;
    }
    if (d.src.empty()) {
        if (errors) errors->push_back(d.file + ": f." + d.id + " has no expr");
        return;
    }
    // the same id again (a mod layer or a later file) replaces it
    std::lock_guard<std::mutex> lk(*mu_);
    compiled_.clear();
    defs_[d.id] = std::move(d);
}

const FormulaDef* FormulaBook::def(const std::string& id) const {
    auto it = defs_.find(id);
    return it == defs_.end() ? nullptr : &it->second;
}

std::vector<std::string> FormulaBook::ids() const {
    std::vector<std::string> out;
    for (const auto& [id, d] : defs_) out.push_back(id);
    return out;
}

const FormulaBook::Compiled* FormulaBook::prepare(const std::string& call, std::string* error) const {
    std::string id;
    std::vector<std::pair<std::string, std::string>> args;
    if (!parseFormulaCall(call, id, args)) {
        if (error) *error = "bad formula call '" + call + "'";
        return nullptr;
    }
    std::string key = "f." + id;
    if (!args.empty()) {
        key += "(";
        for (size_t k = 0; k < args.size(); ++k) key += (k ? "," : "") + (args[k].first.empty() ? "" : args[k].first + "=") + args[k].second;
        key += ")";
    }
    std::lock_guard<std::mutex> lk(*mu_);
    if (auto it = compiled_.find(key); it != compiled_.end()) return it->second.get();
    auto dit = defs_.find(id);
    if (dit == defs_.end()) {
        if (error) *error = "unknown formula f." + id;
        return nullptr;
    }
    std::string src, why;
    if (!bindFormula(dit->second, args, src, &why)) {
        if (error) *error = why;
        return nullptr;
    }
    auto c = std::make_unique<Compiled>();
    c->def = &dit->second;
    std::string err;
    c->expr = Expr::compile(src, &err);
    if (!err.empty()) {
        if (error) *error = dit->second.file + ": f." + id + ": " + err;
        return nullptr;
    }
    const Compiled* out = c.get();
    compiled_[key] = std::move(c);
    return out;
}

bool FormulaBook::finalize(std::vector<std::string>* errors, const NameCheck& check) {
    bool ok = true;
    auto fail = [&](const std::string& e) {
        ok = false;
        if (errors) errors->push_back(e);
    };
    // calls reachable from every formula (with its defaults), depth-first with a cycle guard
    std::map<std::string, int> state; // 1 = on the stack, 2 = done
    std::function<void(const std::string&, const std::string&)> visit = [&](const std::string& call, const std::string& from) {
        std::string err;
        const Compiled* c = prepare(call, &err);
        if (!c) {
            fail((from.empty() ? "" : from + ": ") + err);
            return;
        }
        const std::string key = call.rfind("f.", 0) == 0 ? call : "f." + call;
        int& s = state[key];
        if (s == 2) return;
        if (s == 1) {
            fail(c->def->file + ": formula cycle through " + key);
            return;
        }
        s = 1;
        for (const auto& n : c->expr.names()) {
            if (n.rfind("f.", 0) == 0) {
                visit(n, c->def->file + ": f." + c->def->id);
                continue;
            }
            if (hasDollar(n)) {
                fail(c->def->file + ": f." + c->def->id + ": unbound parameter in '" + n + "'");
                continue;
            }
            std::string why;
            if (n.rfind("cfg.", 0) == 0) {
                if (constants_.count(n.substr(4))) continue;
                if (!check || !check(n, &why)) {
                    fail(c->def->file + ": f." + c->def->id + ": no constant '" + n.substr(4) + "'");
                    continue;
                }
                continue;
            }
            if (check && check(n, &why)) continue;
            if (!why.empty()) {
                fail(c->def->file + ": f." + c->def->id + ": " + why);
                continue;
            }
            if (!exprNameKnown(n, &why)) fail(c->def->file + ": f." + c->def->id + ": " + why);
        }
        s = 2;
    };
    for (const auto& [id, d] : defs_) {
        // the template itself must parse: bind every parameter to a dummy id
        std::vector<std::pair<std::string, std::string>> dummy;
        for (const auto& p : d.params) dummy.emplace_back(p, d.defaults.count(p) ? d.defaults.at(p) : "x");
        std::string src, why, err;
        if (!bindFormula(d, dummy, src, &why)) {
            fail(d.file + ": " + why);
            continue;
        }
        Expr::compile(src, &err);
        if (!err.empty()) {
            fail(d.file + ": f." + id + ": " + err);
            continue;
        }
        bool allDefaults = true;
        for (const auto& p : d.params)
            if (!d.defaults.count(p)) allDefaults = false;
        if (allDefaults) visit("f." + id, "");
    }
    return ok;
}

double FormulaBook::evalCompiled(const Compiled& c, const Ctx& ctx) const {
    double v = c.expr.eval(ctx, 0.0);
    if (c.def->clampLo) v = std::max(v, c.def->lo);
    if (c.def->clampHi) v = std::min(v, c.def->hi);
    return v;
}

double FormulaBook::Ctx::var(const std::string& name) const {
    if (name.rfind("cfg.", 0) == 0) {
        auto it = book.constants_.find(name.substr(4));
        if (it != book.constants_.end()) return it->second;
    }
    if (name.rfind("f.", 0) != 0) return base.var(name);
    const Compiled* c = book.prepare(name, nullptr);
    if (!c || depth > 64) return 0.0;
    ++depth;
    const double v = book.evalCompiled(*c, *this);
    --depth;
    return v;
}

double FormulaBook::eval(const std::string& idOrCall, const ExprCtx& ctx, double dflt) const {
    const Compiled* c = prepare(idOrCall, nullptr);
    if (!c) return dflt;
    Ctx fc(*this, ctx);
    return evalCompiled(*c, fc);
}

double FormulaBook::evalExpr(const Expr& e, const ExprCtx& ctx, double dflt) const {
    if (e.empty()) return dflt;
    Ctx fc(*this, ctx);
    return e.eval(fc, dflt);
}

} // namespace lr::sim
