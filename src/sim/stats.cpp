#include "sim/stats.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

#include "sim/char_expr.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/species.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

namespace fs = std::filesystem;

std::string aiDir(const Content& c) { return c.dir; }

GrowthCurve readCurve(const Json& j, GrowthCurve g) {
    if (!j.is_object()) return g;
    g.softStart = j.value("soft_start", g.softStart);
    g.step = std::max(1e-6, j.value("step", g.step));
    g.perStepMult = std::clamp(j.value("per_step_mult", g.perStepMult), 1e-6, 1.0);
    return g;
}

Expr compileOr(const Json& j, const std::string& where, std::vector<std::string>& errors) {
    if (!j.is_string() || j.get<std::string>().empty()) return {};
    std::string err;
    Expr e = Expr::compile(j.get<std::string>(), &err);
    if (!err.empty()) errors.push_back(where + ": " + err);
    return e;
}

std::unique_ptr<StatRules> loadRules(const std::string& dir) {
    const fs::path p = fs::path(dir) / "stats" / "stats.json";
    if (!data::exists(p)) return nullptr;
    auto r = std::make_unique<StatRules>();
    Json j;
    try {
        j = data::readJson(p.string());
    } catch (const std::exception& e) {
        r->errors.push_back(std::string("stats.json: ") + e.what());
        return r;
    }
    r->version = j.value("version", r->version);
    for (const auto& g : j.value("groups", Json::array()))
        if (g.is_string()) r->groups.push_back(g.get<std::string>());
    for (const auto& s : j.value("stats", Json::array())) {
        StatDef d;
        d.id = s.value("id", std::string());
        d.group = s.value("group", std::string());
        d.nameKey = s.value("name_key", "attr." + d.id);
        d.descKey = s.value("desc_key", "attr." + d.id + ".desc");
        if (d.id.empty()) {
            r->errors.push_back("stats.json: a stat without id");
            continue;
        }
        if (r->has(d.id)) r->errors.push_back("stats.json: duplicate stat '" + d.id + "'");
        if (!r->groups.empty() && std::find(r->groups.begin(), r->groups.end(), d.group) == r->groups.end())
            r->errors.push_back("stats.json: stat " + d.id + " in unknown group '" + d.group + "'");
        r->stats.push_back(d);
    }
    const Json gr = j.value("growth", Json::object());
    r->statGrowth = readCurve(gr.value("stats", Json::object()), r->statGrowth);
    r->skillGrowth = readCurve(gr.value("skills", Json::object()), r->skillGrowth);
    const Json dc = j.value("skill_decay", Json::object());
    r->skillDecay.perYear = dc.value("per_year", r->skillDecay.perYear);
    r->skillDecay.floor = dc.value("floor", r->skillDecay.floor);
    r->skillDecay.overSoftMult = dc.value("over_soft_mult", r->skillDecay.overSoftMult);
    r->skillDecay.graceDays = dc.value("grace_days", r->skillDecay.graceDays);
    const Json tl = j.value("talents", Json::object());
    r->talents.sd = tl.value("sd", r->talents.sd);
    r->talents.lo = tl.value("lo", r->talents.lo);
    r->talents.hi = tl.value("hi", r->talents.hi);
    const Json fill = j.value("fill", Json::object());
    for (const auto& [k, v] : fill.items()) {
        if (k.empty() || k[0] == '_') continue;
        if (k == "default") r->fillDefault = compileOr(v, "stats.json fill.default", r->errors);
        else r->fill[k] = compileOr(v, "stats.json fill." + k, r->errors);
    }
    const Json cl = j.value("clamp", Json::object());
    r->clampLo = cl.value("lo", r->clampLo);
    r->startHi = cl.value("start_hi", r->startHi);
    for (const auto& pt : j.value("age_curve", Json::object()).value("points", Json::array())) {
        StatRules::AgePoint a;
        a.age = pt.value("age", 0.0);
        const Json tmp1 = pt.value("mult", Json::object());
        for (const auto& [k, v] : tmp1.items())
            if (v.is_number()) a.mult[k] = v.get<double>();
        r->ageCurve.push_back(a);
    }
    std::sort(r->ageCurve.begin(), r->ageCurve.end(), [](const auto& a, const auto& b) { return a.age < b.age; });
    r->refreshTicks = std::max(1, j.value("refresh_ticks", r->refreshTicks));
    const Json tmp2 = j.value("rename_skills", Json::object());
    for (const auto& [k, v] : tmp2.items())
        if (v.is_string() && !k.empty() && k[0] != '_') r->renameSkills[k] = v.get<std::string>();

    const fs::path sp = fs::path(dir) / "stats" / "states.json";
    if (data::exists(sp)) {
        try {
            const Json s = data::readJson(sp.string());
            for (const auto& m : s.value("mods", Json::array())) {
                StatStateMod d;
                d.id = m.value("id", std::string());
                const std::string where = "states.json " + d.id;
                d.when = compileOr(m.value("if", Json()), where + " if", r->errors);
                d.scale = compileOr(m.value("scale", Json()), where + " scale", r->errors);
                const Json tmp3 = m.value("add", Json::object());
                for (const auto& [k, v] : tmp3.items())
                    if (v.is_number()) d.add[k] = v.get<double>();
                const Json tmp4 = m.value("mult", Json::object());
                for (const auto& [k, v] : tmp4.items())
                    if (v.is_number()) d.mult[k] = v.get<double>();
                r->mods.push_back(std::move(d));
            }
        } catch (const std::exception& e) {
            r->errors.push_back(std::string("states.json: ") + e.what());
        }
    }
    return r;
}

std::uint64_t mixHash(std::uint64_t h, const std::string& s) {
    for (char ch : s) h = (h ^ std::uint8_t(ch)) * 1099511628211ull;
    return h;
}

double hashUnit(EntityId id, const std::string& salt) {
    std::uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) h = (h ^ std::uint8_t(std::uint64_t(id) >> (8 * i))) * 1099511628211ull;
    h = mixHash(h, salt);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (double(h >> 11) + 0.5) / double(1ull << 53);
}

// The context of stats formulas: a person's names plus attr_base., moodlet., body.
struct StatCtx : CharExprCtx {
    mutable Rng rng;
    StatCtx(const Character& c, std::uint64_t seed) : CharExprCtx(c), rng(seed) {
        roll = [this] { return rng.uniform(); };
        extra = [&c](const std::string& n, double& v) {
            if (n.rfind("attr_base.", 0) == 0) {
                auto it = c.attrs.find(n.substr(10));
                v = it == c.attrs.end() ? 0.5 : it->second;
                return true;
            }
            if (n.rfind("moodlet.", 0) == 0) {
                const std::string id = n.substr(8);
                v = std::any_of(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == id; }) ? 1.0
                                                                                                                       : 0.0;
                return true;
            }
            if (n == "body.health") return v = c.body.health, true;
            if (n == "body.fatigue") return v = c.body.fatigue, true;
            return false;
        };
    }
};

const bool kStatPrefixes = [] {
    registerExprPrefix("attr_base.");
    registerExprPrefix("moodlet.");
    registerExprPrefix("body.");
    return true;
}();

double kindShift(const Content& c, const Character& ch, const std::string& stat) {
    if (c.peoples.races.empty()) return 0.0;
    const RaceDef& rd = raceOf(c, ch);
    auto it = rd.statMods.find(stat);
    return it == rd.statMods.end() ? 0.0 : it->second;
}

double traitShift(const Content& c, const Character& ch, const std::string& stat) {
    double s = 0.0;
    for (const auto& t : ch.traits) {
        auto it = c.expansion.traitExt.find(t);
        if (it == c.expansion.traitExt.end()) continue;
        const Json& am = it->second.contains("attr_mod") ? it->second["attr_mod"] : Json();
        if (am.is_object() && am.contains(stat) && am[stat].is_number()) s += am[stat].get<double>();
    }
    return s;
}

std::string skillCategory(const Content& c, const std::string& skill) {
    auto it = c.expansion.knowledgeExt.find(skill);
    if (it != c.expansion.knowledgeExt.end() && it->second.contains("category") && it->second["category"].is_string())
        return it->second["category"].get<std::string>();
    return skill;
}

double years(const Character& c) {
    const double y = static_cast<double>(c.body.ageDays) / kDaysPerYear;
    if (c.body.adultYears == 14 && c.body.elderYears == 60) return y;
    if (y <= c.body.adultYears) return 14.0 * y / std::max(1, c.body.adultYears);
    return 14.0 + (y - c.body.adultYears) * 46.0 / std::max(1, c.body.elderYears - c.body.adultYears);
}

} // namespace

double GrowthCurve::divisions(double v) const { return v <= softStart ? 0.0 : (v - softStart) / step; }
double GrowthCurve::mult(double v) const { return std::pow(perStepMult, divisions(v)); }

bool StatRules::has(const std::string& id) const {
    return std::any_of(stats.begin(), stats.end(), [&](const StatDef& d) { return d.id == id; });
}
std::vector<std::string> StatRules::ids() const {
    std::vector<std::string> out;
    for (const auto& d : stats) out.push_back(d.id);
    return out;
}

const StatRules* statRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<StatRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(aiDir(c));
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, loadRules(aiDir(c))).first;
    return it->second.get();
}

std::vector<std::string> statIdsOf(const std::string& dir) {
    const fs::path p = fs::path(dir) / "stats" / "stats.json";
    std::vector<std::string> out;
    if (!data::exists(p)) return out;
    try {
        for (const auto& s : data::readJson(p.string()).value("stats", Json::array()))
            if (s.contains("id") && s["id"].is_string()) out.push_back(s["id"].get<std::string>());
    } catch (...) {
    }
    return out;
}

double statAgeMult(const StatRules& r, const std::string& stat, double y) {
    if (r.ageCurve.empty()) return 1.0;
    auto at = [&](const StatRules::AgePoint& p) {
        auto it = p.mult.find(stat);
        return it == p.mult.end() ? 1.0 : it->second;
    };
    if (y <= r.ageCurve.front().age) return at(r.ageCurve.front());
    for (size_t i = 1; i < r.ageCurve.size(); ++i)
        if (y <= r.ageCurve[i].age) {
            const auto& a = r.ageCurve[i - 1];
            const auto& b = r.ageCurve[i];
            const double t = (y - a.age) / std::max(1e-9, b.age - a.age);
            return at(a) + (at(b) - at(a)) * t;
        }
    return at(r.ageCurve.back());
}

void fillMissingStats(const Content& c, std::uint64_t seed, Character& ch) {
    const StatRules* r = statRules(c);
    if (!r || ch.isAnimal()) return;
    for (const auto& d : r->stats) {
        if (ch.attrs.count(d.id)) continue;
        StatCtx x(ch, seed ^ (std::uint64_t(ch.id) * 0x9e3779b97f4a7c15ull) ^ mixHash(1469598103934665603ull, d.id));
        auto f = r->fill.find(d.id);
        const Expr& e = f != r->fill.end() && !f->second.empty() ? f->second : r->fillDefault;
        const double v = e.empty() ? 0.5 : c.formulas.evalExpr(e, x, 0.5);
        ch.attrs[d.id] = std::clamp(v, r->clampLo, r->startHi);
    }
}

void refreshStats(const Content& c, Character& ch) {
    const StatRules* r = statRules(c);
    if (!r || ch.isAnimal()) return;
    const double y = years(ch);
    std::map<std::string, double> add, mult;
    StatCtx x(ch, 0);
    for (const auto& m : r->mods) {
        if (!m.when.empty() && c.formulas.evalExpr(m.when, x, 0.0) == 0.0) continue;
        const double s = m.scale.empty() ? 1.0 : std::clamp(c.formulas.evalExpr(m.scale, x, 1.0), 0.0, 1.0);
        for (const auto& [k, v] : m.add) add[k] += v * s;
        for (const auto& [k, v] : m.mult) {
            double& mm = mult.emplace(k, 1.0).first->second;
            mm *= 1.0 + (v - 1.0) * s;
        }
    }
    for (const auto& d : r->stats) {
        auto b = ch.attrs.find(d.id);
        const double base = b == ch.attrs.end() ? 0.5 : b->second;
        double v = (base + kindShift(c, ch, d.id) + traitShift(c, ch, d.id)) * statAgeMult(*r, d.id, y);
        if (auto a = add.find(d.id); a != add.end()) v += a->second;
        if (auto m = mult.find(d.id); m != mult.end()) v *= m->second;
        ch.attrsNow[d.id] = std::max(0.0, v);
    }
}

double skillTalentOf(const Content& c, EntityId id, const std::string& skill) {
    const StatRules* r = statRules(c);
    if (!r) return 1.0;
    // log-normal around 1: the person's knack for the skill's category
    const double u1 = hashUnit(id, "talent:" + skillCategory(c, skill));
    const double u2 = hashUnit(id, "talent2:" + skillCategory(c, skill));
    const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    return std::clamp(std::exp(r->talents.sd * z), r->talents.lo, r->talents.hi);
}

double practiceSkill(const World& w, Character& c, const std::string& skill, double gain) {
    double& v = c.skills[skill];
    const StatRules* r = statRules(w.content());
    if (!r) {
        v = std::clamp(v + gain, 0.0, 1.0);
        return v;
    }
    if (gain > 0.0) {
        GrowthCurve g = r->skillGrowth;
        g.softStart *= raceCapMult(w.content(), c, skill); // the kind's knack lifts where it slows
        gain *= g.mult(v) * skillTalentOf(w.content(), c.id, skill) * raceLearnMult(w.content(), c, skill);
        c.skillDay[skill] = c.body.ageDays;
    }
    v = std::max(0.0, v + gain);
    return v;
}

double trainStat(const World& w, Character& c, const std::string& stat, double gain) {
    const StatRules* r = statRules(w.content());
    double& v = c.attrs[stat];
    if (r && gain > 0.0) gain *= r->statGrowth.mult(v + kindShift(w.content(), c, stat));
    v = std::max(r ? r->clampLo : 0.0, v + gain);
    return v;
}

void statsTick(World& w) {
    const Content& ct = w.content();
    const StatRules* r = statRules(ct);
    if (!r) return;
    const Tick t = w.now();
    const bool refresh = t % r->refreshTicks == 0;
    const bool daily = t % kTicksPerDay == 0 && t > 0;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal()) continue;
        if (c.attrs.size() < r->stats.size()) { // a new person or an old save
            for (const auto& [from, to] : r->renameSkills) {
                auto it = c.skills.find(from);
                if (it == c.skills.end()) continue;
                double& v = c.skills[to];
                v = std::max(v, it->second);
                c.skills.erase(from);
            }
            fillMissingStats(ct, w.seed(), c);
        }
        if (refresh || c.attrsNow.empty()) refreshStats(ct, c);
        if (!daily) continue;
        const auto& d = r->skillDecay;
        for (auto& [sk, v] : c.skills) {
            if (v <= d.floor) continue;
            auto last = c.skillDay.find(sk);
            if (last != c.skillDay.end() && c.body.ageDays - last->second < d.graceDays) continue;
            const double over = r->skillGrowth.divisions(v);
            v -= d.perYear / kDaysPerYear * (v - d.floor) * std::pow(d.overSoftMult, over);
        }
    }
}

void validateStats(const Content& c, ValidationReport& rep) {
    const StatRules* r = statRules(c);
    if (!r) return;
    for (const auto& e : r->errors) rep.errors.push_back("stats: " + e);
    auto known = [&](const std::string& id) { return r->has(id); };
    for (const auto& [k, e] : r->fill)
        if (!known(k)) rep.errors.push_back("stats: fill names unknown stat '" + k + "'");
    for (const auto& a : r->ageCurve)
        for (const auto& [k, v] : a.mult)
            if (!known(k)) rep.errors.push_back("stats: age_curve names unknown stat '" + k + "'");
    for (const auto& m : r->mods) {
        for (const auto* mp : {&m.add, &m.mult})
            for (const auto& [k, v] : *mp)
                if (!known(k)) rep.errors.push_back("stats: states " + m.id + " names unknown stat '" + k + "'");
        for (const Expr* e : {&m.when, &m.scale})
            for (const auto& n : e->names()) {
                std::string why;
                if (n.rfind("attr_base.", 0) == 0 || n.rfind("attr.", 0) == 0) {
                    const std::string s = n.substr(n.find('.') + 1);
                    if (!known(s)) rep.errors.push_back("stats: states " + m.id + " reads unknown stat '" + s + "'");
                } else if (n.rfind("moodlet.", 0) == 0) {
                    if (!c.moodlets.count(n.substr(8)))
                        rep.errors.push_back("stats: states " + m.id + " reads unknown moodlet '" + n.substr(8) + "'");
                } else if (n == "body.health" || n == "body.fatigue") {
                } else if (!exprNameKnown(n, &why) || (!charExprNameKnown(c, n, &why) && !why.empty())) {
                    rep.errors.push_back("stats: states " + m.id + ": " + (why.empty() ? "unknown name " + n : why));
                }
            }
    }
}

} // namespace lr::sim
