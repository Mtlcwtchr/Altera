#include "sim/act_check.hpp"

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"
#include "sim/char_expr.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

const bool kFxPrefix = [] {
    registerExprPrefix("fx.");
    return true;
}();

std::string fmt(double v) {
    std::string s = std::to_string(v);
    return s;
}

// A factor's source: an expression, or {"name": weight} = the weighted mean.
std::string factorSource(const Json& v, std::string* why) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return fmt(v.get<double>());
    if (!v.is_object() || v.empty()) {
        *why = "a factor is an expression or {name: weight}";
        return {};
    }
    std::string num;
    double sum = 0.0;
    for (const auto& [k, w] : v.items()) {
        if (!w.is_number()) {
            *why = "weight of " + k + " is not a number";
            return {};
        }
        const double x = w.get<double>();
        sum += std::abs(x);
        num += (num.empty() ? "" : " + ") + std::string("(") + fmt(x) + ")*" + k;
    }
    if (sum <= 0.0) {
        *why = "weights sum to 0";
        return {};
    }
    return "(" + num + ") / " + fmt(sum);
}

Expr compileIn(const std::string& src, const std::string& where, std::vector<std::string>& errors) {
    if (src.empty()) return {};
    std::string err;
    Expr e = Expr::compile(src, &err);
    if (!err.empty()) errors.push_back(where + ": " + err);
    return e;
}

double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

struct CheckCtx : CharExprCtx {
    const std::vector<std::pair<std::string, double>>* fx = nullptr;
    CheckCtx(const Character& a, const Character* t) : CharExprCtx(a, t) {
        extra = [this](const std::string& n, double& v) {
            if (n.rfind("fx.", 0) != 0 || !fx) return false;
            for (const auto& [k, x] : *fx)
                if (n.compare(3, std::string::npos, k) == 0) return v = x, true;
            return v = 0.0, true;
        };
    }
};

double spreadOf(const World& w, const Character& a, const Character* t, const ActCheck& k,
                const std::vector<std::pair<std::string, double>>& fx) {
    CheckCtx x(a, t);
    x.fx = &fx;
    return k.spread.empty() ? 0.15 : std::max(0.0, w.content().formulas.evalExpr(k.spread, x, 0.15));
}

double needOf(const World& w, const Character& a, const Character& t, const ActCheck& k,
              const std::vector<std::pair<std::string, double>>& fx) {
    CheckCtx x(a, &t);
    x.fx = &fx;
    return k.need.empty() ? 0.5 : w.content().formulas.evalExpr(k.need, x, 0.5);
}

const ActOutcome* pick(const ActCheck& k, double margin) {
    for (const auto& o : k.outcomes)
        if (!o.isElse && margin >= o.over) return &o;
    for (const auto& o : k.outcomes)
        if (o.isElse) return &o;
    return nullptr;
}

} // namespace

ActCheck parseActCheck(const Json& j, const std::string& where, std::vector<std::string>& errors) {
    ActCheck k;
    k.set = true;
    for (const auto& [key, v] : j.items())
        if (key != "factors" && key != "quality" && key != "spread" && key != "need" && key != "outcomes" &&
            key != "audience" && key[0] != '_')
            errors.push_back(where + ": unknown key '" + key + "'");
    const Json fx = j.value("factors", Json::object());
    std::string meanSrc;
    for (const auto& [name, v] : fx.items()) {
        std::string why;
        const std::string src = factorSource(v, &why);
        if (!why.empty()) {
            errors.push_back(where + " factors." + name + ": " + why);
            continue;
        }
        k.factors.emplace_back(name, compileIn(src, where + " factors." + name, errors));
        meanSrc += (meanSrc.empty() ? "" : " + ") + std::string("fx.") + name;
    }
    const std::string q = j.value("quality", std::string());
    if (!q.empty()) k.quality = compileIn(q, where + " quality", errors);
    else if (!k.factors.empty())
        k.quality = compileIn("(" + meanSrc + ") / " + std::to_string(k.factors.size()), where + " quality", errors);
    else errors.push_back(where + ": no factors and no quality");
    if (j.contains("spread")) {
        if (j["spread"].is_number()) k.spread = Expr::constant(j["spread"].get<double>());
        else k.spread = compileIn(j.value("spread", std::string()), where + " spread", errors);
    }
    if (j.contains("need")) {
        if (j["need"].is_number()) k.need = Expr::constant(j["need"].get<double>());
        else k.need = compileIn(j.value("need", std::string()), where + " need", errors);
    }
    if (j.contains("audience")) {
        const Json& a = j["audience"];
        k.audienceRadius = a.value("radius_m", 8.0);
        k.audienceMax = a.value("max", k.audienceMax);
        if (k.audienceRadius <= 0.0) errors.push_back(where + " audience: radius_m must be > 0");
    }
    int elses = 0;
    for (const auto& oj : j.value("outcomes", Json::array())) {
        ActOutcome o;
        o.id = oj.value("id", std::string());
        o.isElse = oj.value("else", false);
        o.over = oj.value("over", 0.0);
        if (o.id.empty()) errors.push_back(where + ": an outcome without id");
        if (o.isElse) ++elses;
        else if (!oj.contains("over")) errors.push_back(where + " outcome " + o.id + ": needs over or else");
        for (const auto& e : oj.value("eff", Json::array())) {
            try {
                o.eff.push_back(parseEffect(e.get<std::string>()));
            } catch (const std::exception& ex) {
                errors.push_back(where + " outcome " + o.id + ": " + ex.what());
            }
        }
        k.outcomes.push_back(std::move(o));
    }
    if (k.outcomes.empty()) errors.push_back(where + ": no outcomes");
    if (elses > 1) errors.push_back(where + ": more than one else outcome");
    for (size_t i = 1; i < k.outcomes.size(); ++i)
        if (!k.outcomes[i].isElse && !k.outcomes[i - 1].isElse && k.outcomes[i].over > k.outcomes[i - 1].over)
            errors.push_back(where + ": outcomes must go from the highest over down");
    return k;
}

ActCheckRoll actCheckQuality(const World& w, const Character& actor, const InteractionDef& d, const Character* target) {
    ActCheckRoll r;
    const ActCheck& k = d.check;
    if (!k.set) return r;
    const FormulaBook& fb = w.content().formulas;
    CheckCtx x(actor, target);
    x.fx = &r.factors;
    for (const auto& [name, e] : k.factors) r.factors.emplace_back(name, fb.evalExpr(e, x, 0.0));
    r.quality = k.quality.empty() ? 0.0 : fb.evalExpr(k.quality, x, 0.0);
    return r;
}

std::vector<EntityId> actAudience(const World& w, const Character& actor, const InteractionDef& d) {
    std::vector<std::pair<double, EntityId>> near;
    const double r2 = d.check.audienceRadius * d.check.audienceRadius;
    for (const auto& [id, c] : w.characters()) {
        if (id == actor.id || !c.body.alive || c.isAnimal() || c.body.downUntil > w.now()) continue;
        const double dx = c.pos.x - actor.pos.x, dy = c.pos.y - actor.pos.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= r2) near.emplace_back(d2, id);
    }
    std::sort(near.begin(), near.end());
    std::vector<EntityId> out;
    for (const auto& [d2, id] : near) {
        if (static_cast<int>(out.size()) >= d.check.audienceMax) break;
        out.push_back(id);
    }
    return out;
}

bool resolveActCheck(World& w, Character& actor, const InteractionDef& d, WorldObject* target, Character* targetChar,
                     const std::vector<EntityId>* witnesses, ActCheckRoll* out) {
    const ActCheck& k = d.check;
    if (!k.set) return true;
    std::vector<EntityId> targets;
    if (k.audienceRadius > 0.0) targets = actAudience(w, actor, d);
    else if (targetChar) targets.push_back(targetChar->id);
    ActCheckRoll r = actCheckQuality(w, actor, d, targetChar);
    for (EntityId tid : targets) {
        Character* t = w.character(tid);
        if (!t) continue;
        const double sd = spreadOf(w, actor, t, k, r.factors);
        Rng rng = w.rng(actor.id, "check:" + d.id + ":" + std::to_string(tid));
        // Box-Muller: one normal draw per target
        const double u1 = std::max(1e-12, rng.uniform()), u2 = rng.uniform();
        const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        const double margin = r.quality + sd * z - needOf(w, actor, *t, k, r.factors);
        if (margin >= 0.0) ++r.succeeded;
        const ActOutcome* o = pick(k, margin);
        if (!o) continue;
        r.outcomes.emplace_back(tid, o->id);
        applyEffectList(w, actor, o->eff, target, t, witnesses);
    }
    Json counts = Json::object();
    for (const auto& [tid, oid] : r.outcomes) counts[oid] = counts.value(oid, 0) + 1;
    Json fxj = Json::object();
    for (const auto& [n, v] : r.factors) fxj[n] = std::round(v * 1000.0) / 1000.0;
    trace(w, actor, 'F', "act.check",
          {{"action", d.id}, {"quality", std::round(r.quality * 1000.0) / 1000.0}, {"factors", fxj}, {"outcomes", counts}});
    const bool ok = targets.empty() || r.succeeded > 0;
    if (out) *out = std::move(r);
    return ok;
}

std::vector<std::pair<std::string, double>> actCheckOdds(const World& w, const Character& actor, const InteractionDef& d,
                                                         const Character& target) {
    std::vector<std::pair<std::string, double>> out;
    const ActCheck& k = d.check;
    if (!k.set) return out;
    const ActCheckRoll r = actCheckQuality(w, actor, d, &target);
    const double sd = std::max(1e-9, spreadOf(w, actor, &target, k, r.factors));
    const double mean = r.quality - needOf(w, actor, target, k, r.factors);
    double above = 0.0; // P(margin >= the previous threshold)
    for (const auto& o : k.outcomes) {
        if (o.isElse) continue;
        const double p = 1.0 - normalCdf((o.over - mean) / sd);
        out.emplace_back(o.id, std::max(0.0, p - above));
        above = std::max(above, p);
    }
    for (const auto& o : k.outcomes)
        if (o.isElse) out.emplace_back(o.id, std::max(0.0, 1.0 - above));
    return out;
}

void validateActChecks(const Content& c, ValidationReport& rep) {
    for (const auto& [id, d] : c.interactions) {
        if (!d.check.set) continue;
        std::vector<std::string> fxNames;
        for (const auto& [n, e] : d.check.factors) fxNames.push_back(n);
        auto checkExpr = [&](const Expr& e, const std::string& what) {
            for (const auto& n : e.names()) {
                if (n.rfind("fx.", 0) == 0) {
                    if (std::find(fxNames.begin(), fxNames.end(), n.substr(3)) == fxNames.end())
                        rep.errors.push_back("interaction " + id + " check " + what + ": unknown factor '" + n + "'");
                    continue;
                }
                if (n.rfind("f.", 0) == 0) {
                    std::string fid;
                    std::vector<std::pair<std::string, std::string>> args;
                    if (!parseFormulaCall(n, fid, args) || !c.formulas.has(fid))
                        rep.errors.push_back("interaction " + id + " check " + what + ": unknown formula '" + n + "'");
                    continue;
                }
                std::string why;
                if (!exprNameKnown(n, &why)) {
                    rep.errors.push_back("interaction " + id + " check " + what + ": " + why);
                    continue;
                }
                if (!charExprNameKnown(c, n, &why) && !why.empty())
                    rep.errors.push_back("interaction " + id + " check " + what + ": " + why);
            }
        };
        for (const auto& [n, e] : d.check.factors) checkExpr(e, "factors." + n);
        checkExpr(d.check.quality, "quality");
        checkExpr(d.check.spread, "spread");
        checkExpr(d.check.need, "need");
    }
}

} // namespace lr::sim
