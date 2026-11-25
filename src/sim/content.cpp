#include "sim/content.hpp"
#include "sim/consequences.hpp"
#include "sim/craft_sim.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace altera::sim {

double Curve::eval(double x) const {
    double r = 0.0;
    if (type == "linear") r = m * x + b;
    else if (type == "inverse") r = 1.0 - x;
    else if (type == "quadratic") r = std::pow(clamp01(x), p);
    else if (type == "logistic") r = 1.0 / (1.0 + std::exp(-k * (x - x0)));
    else if (type == "inverse_logistic") r = 1.0 - 1.0 / (1.0 + std::exp(-k * (x - x0)));
    else if (type == "step") r = x >= x0 ? hi : lo;
    else if (type == "const") r = v;
    return clamp01(r);
}

const std::vector<std::string>& relAxes() {
    static const std::vector<std::string> a = {"affinity", "trust", "fear", "familiarity", "debt",
                                               "enmity", "respect", "attraction"};
    return a;
}

InteractionDef bindParams(const InteractionDef& d, const ParamBinding& b) {
    for (const auto& p : d.params) {
        auto it = b.find(p.name);
        if (it == b.end()) throw std::runtime_error("interaction " + d.id + ": parameter " + p.name + " not bound");
        if (!p.of.empty() && std::find(p.of.begin(), p.of.end(), it->second) == p.of.end())
            throw std::runtime_error("interaction " + d.id + ": " + it->second + " is not allowed for $" + p.name);
    }
    auto conds = [&](std::vector<Condition>& v) {
        for (auto& k : v) k = bindCondition(k, b);
    };
    auto effs = [&](std::vector<Effect>& v) {
        for (auto& e : v) e = bindEffect(e, b);
    };
    auto key = [&](const std::string& k) {
        if (k.empty() || k[0] != '$') return k;
        auto it = b.find(k.substr(1));
        if (it == b.end()) throw std::runtime_error("interaction " + d.id + ": unbound " + k);
        return it->second;
    };
    InteractionDef r = d;
    conds(r.pre);
    effs(r.eff);
    effs(r.onFail);
    if (r.hasUntil) r.until = bindCondition(r.until, b);
    for (auto& rk : r.risk) effs(rk.eff);
    for (auto& [o, v] : r.combat.outcomes) effs(v);
    effs(r.terms.onBreach);
    std::map<std::string, int> give, take;
    for (const auto& [k, n] : r.terms.give) give[key(k)] += n;
    for (const auto& [k, n] : r.terms.take) take[key(k)] += n;
    r.terms.give = give;
    r.terms.take = take;
    r.chance.skill = key(r.chance.skill);
    r.params.clear();
    return r;
}

const NeedDef* Content::need(const std::string& id) const {
    for (const auto& n : needs)
        if (n.id == id) return &n;
    return nullptr;
}
const GoalDef* Content::goal(const std::string& id) const {
    for (const auto& g : goals)
        if (g.id == id) return &g;
    return nullptr;
}
const InteractionDef* Content::interaction(const std::string& id) const {
    auto it = interactions.find(id);
    return it == interactions.end() ? nullptr : &it->second;
}
const HtnTask* Content::task(const std::string& id) const {
    auto it = tasks.find(id);
    return it == tasks.end() ? nullptr : &it->second;
}
bool Content::fsmInterruptible(const std::string& state) const {
    std::string s = state;
    for (int guard = 0; guard < 16; ++guard) {
        auto it = fsm.find(s);
        if (it == fsm.end()) return true;
        if (it->second.interruptible >= 0) return it->second.interruptible != 0;
        if (it->second.parent.empty()) return true;
        s = it->second.parent;
    }
    return true;
}
std::string Content::fsmRoot(const std::string& state) const {
    std::string s = state;
    for (int guard = 0; guard < 16; ++guard) {
        auto it = fsm.find(s);
        if (it == fsm.end() || it->second.parent.empty()) return s;
        s = it->second.parent;
    }
    return s;
}

namespace {

Json readJson(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    try {
        return Json::parse(f);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

template <class T> T opt(const Json& j, const char* key, T def) {
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? def : it->get<T>();
}

std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it != j.end() && it->is_array())
        for (const auto& s : *it) out.push_back(s.get<std::string>());
    return out;
}

struct Errors {
    std::vector<std::string> list;
    void add(std::string s) { list.push_back(std::move(s)); }
};

std::vector<Condition> conditions(const Json& j, const char* key, const std::string& where, Errors& err) {
    std::vector<Condition> out;
    for (const auto& s : strings(j, key)) {
        try {
            out.push_back(parseCondition(s));
        } catch (const std::exception& e) {
            err.add(where + ": " + e.what());
        }
    }
    return out;
}

std::vector<Effect> effects(const Json& j, const char* key, const std::string& where, Errors& err) {
    std::vector<Effect> out;
    for (const auto& s : strings(j, key)) {
        try {
            out.push_back(parseEffect(s));
        } catch (const std::exception& e) {
            err.add(where + ": " + e.what());
        }
    }
    return out;
}

void warnUnknown(const Json& j, std::initializer_list<const char*> known, const std::string& where,
                 std::vector<std::string>& warnings) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.key().empty() && it.key()[0] == '_') continue; // _doc, _part, ...: notes
        bool ok = false;
        for (const char* k : known)
            if (it.key() == k) ok = true;
        if (!ok) warnings.push_back(where + ": unknown field '" + it.key() + "'");
    }
}

Curve curveFrom(const Json& j) {
    Curve c;
    c.type = opt<std::string>(j, "type", "linear");
    c.m = opt(j, "m", 1.0);
    c.b = opt(j, "b", 0.0);
    c.k = opt(j, "k", 10.0);
    c.x0 = opt(j, "x0", 0.5);
    c.p = opt(j, "p", 2.0);
    c.lo = opt(j, "lo", 0.0);
    c.hi = opt(j, "hi", 1.0);
    c.v = opt(j, "v", 1.0);
    return c;
}

} // namespace

Content Content::fromJson(const Json& items, const Json& objects, const Json& needs, const Json& traits,
                          const Json& goals, const Json& htn, const Json& interactions, const Json& fsm,
                          const Json& config, const Json& dialogue, const Json& knowledge,
                          const Json& species) {
    Content c;
    Errors err;
    auto& W = c.warnings;

    for (const auto& j : items.value("items", Json::array())) {
        warnUnknown(j, {"id", "tags", "nutrition", "stack", "warmth", "tool_wear", "spoil_days", "material", "value", "quality"}, "items", W);
        ItemDef d;
        d.id = j.at("id").get<std::string>();
        d.tags = strings(j, "tags");
        d.nutrition = opt(j, "nutrition", 0.0);
        d.warmth = opt(j, "warmth", 0.0);
        d.toolWear = opt(j, "tool_wear", 0.0);
        d.spoilDays = opt(j, "spoil_days", 0.0);
        d.material = opt(j, "material", std::string());
        d.value = opt(j, "value", 0.0);
        d.quality = opt(j, "quality", 0.0);
        if (!c.items.emplace(d.id, d).second) err.add("items: duplicate id " + d.id);
    }

    for (const auto& j : objects.value("objects", Json::array())) {
        warnUnknown(j, {"id", "stock", "max_stock", "regrow", "regrow_every", "biomes", "per_km2", "owned",
                        "settlement", "per_family", "per_person", "warmth", "shelter", "per_settlement", "season", "placed_last", "crop", "station",
                        "own_seed", "stand_m"},
                    "objects", W);
        ObjectDef d;
        d.id = j.at("id").get<std::string>();
        d.stock = opt(j, "stock", std::map<std::string, int>{});
        d.maxStock = opt(j, "max_stock", d.stock);
        d.regrow = opt(j, "regrow", std::map<std::string, int>{});
        d.regrowEvery = opt<Tick>(j, "regrow_every", 0);
        d.biomes = strings(j, "biomes");
        d.perKm2 = opt(j, "per_km2", 0.0);
        d.standM = opt(j, "stand_m", 0.0);
        d.owned = opt(j, "owned", false);
        d.settlement = opt(j, "settlement", false);
        d.perFamily = opt(j, "per_family", 0);
        d.station = opt(j, "station", std::string());
        d.perPerson = opt(j, "per_person", 0);
        d.warmth = opt(j, "warmth", false);
        d.shelter = opt(j, "shelter", false);
        d.perSettlement = opt(j, "per_settlement", 0);
        d.ownSeed = opt(j, "own_seed", false);
        d.placedLast = opt(j, "placed_last", false);
        d.seasons = strings(j, "season");
        if (j.contains("crop")) {
            const Json& k = j.at("crop");
            warnUnknown(k, {"seed", "produce", "seed_per_strip", "grain_per_sheaf", "sow", "grow_days", "yield",
                            "drought_at", "waterlog_at", "drought_loss", "frost_c", "frost_loss", "wet_harvest_mm",
                            "weeds_every", "weed_loss", "spoil_after", "spoil_per_day", "staple", "seed_from",
                            "strips_per_person", "_doc"},
                        "objects.crop", W);
            d.hasCrop = true;
            CropDef& cd = d.crop;
            cd.seed = k.at("seed").get<std::string>();
            cd.produce = opt(k, "produce", cd.produce);
            cd.seedPerStrip = std::max(1, opt(k, "seed_per_strip", cd.seedPerStrip));
            cd.grainPerSheaf = std::max(1, opt(k, "grain_per_sheaf", cd.grainPerSheaf));
            if (k.contains("sow")) {
                cd.sowFrom = k.at("sow").at(0).get<int>();
                cd.sowTo = k.at("sow").at(1).get<int>();
            }
            cd.growDays = opt(k, "grow_days", cd.growDays);
            if (k.contains("yield")) {
                cd.yieldLo = k.at("yield").at(0).get<double>();
                cd.yieldHi = k.at("yield").at(1).get<double>();
            }
            cd.droughtAt = opt(k, "drought_at", cd.droughtAt);
            cd.waterlogAt = opt(k, "waterlog_at", cd.waterlogAt);
            cd.droughtLoss = opt(k, "drought_loss", cd.droughtLoss);
            cd.frostC = opt(k, "frost_c", cd.frostC);
            cd.frostLoss = opt(k, "frost_loss", cd.frostLoss);
            cd.wetHarvestMm = opt(k, "wet_harvest_mm", cd.wetHarvestMm);
            cd.weedsEvery = std::max(1, opt(k, "weeds_every", cd.weedsEvery));
            cd.weedLoss = opt(k, "weed_loss", cd.weedLoss);
            cd.spoilAfter = opt(k, "spoil_after", cd.spoilAfter);
            cd.spoilPerDay = opt(k, "spoil_per_day", cd.spoilPerDay);
            cd.staple = opt(k, "staple", cd.staple);
            cd.seedFrom = opt(k, "seed_from", cd.seedFrom);
            if (k.contains("strips_per_person")) {
                cd.stripsLo = k.at("strips_per_person").at(0).get<double>();
                cd.stripsHi = k.at("strips_per_person").at(1).get<double>();
            }
        }
        if (!c.objects.emplace(d.id, d).second) err.add("objects: duplicate id " + d.id);
    }

    for (const auto& j : needs.value("needs", Json::array())) {
        warnUnknown(j, {"id", "decay_per_day", "critical", "start", "night_mult", "biome_mult", "mvp", "season_mult",
                        "sheltered_mult", "clothing_mult"}, "needs", W);
        NeedDef d;
        d.id = j.at("id").get<std::string>();
        d.decayPerDay = opt(j, "decay_per_day", 0.5);
        d.critical = opt(j, "critical", 0.1);
        if (j.contains("start")) {
            d.startLo = j["start"].at(0).get<double>();
            d.startHi = j["start"].at(1).get<double>();
        }
        d.nightMult = opt(j, "night_mult", 1.0);
        d.biomeMult = opt(j, "biome_mult", std::map<std::string, double>{});
        d.mvp = opt(j, "mvp", true);
        d.seasonMult = opt(j, "season_mult", std::map<std::string, double>{});
        d.shelteredMult = opt(j, "sheltered_mult", 1.0);
        d.clothingMult = opt(j, "clothing_mult", std::map<std::string, double>{});
        if (c.need(d.id)) err.add("needs: duplicate id " + d.id);
        c.needs.push_back(d);
    }
    for (const auto& j : needs.value("moodlets", Json::array())) {
        MoodletDef d;
        d.id = j.at("id").get<std::string>();
        d.value = opt(j, "value", 0.0);
        d.ttl = opt<Tick>(j, "ttl", 72);
        c.moodlets[d.id] = d;
    }

    for (const auto& j : traits.value("traits", Json::array())) {
        warnUnknown(j, {"id", "goal_mult", "need_mult", "legal_penalty", "method_bias", "hysteresis_mult", "inherit",
                        "weight_in_pool"},
                    "traits",
                    W);
        TraitDef d;
        d.id = j.at("id").get<std::string>();
        d.goalMult = opt(j, "goal_mult", std::map<std::string, double>{});
        d.needMult = opt(j, "need_mult", std::map<std::string, double>{});
        d.legalPenalty = opt(j, "legal_penalty", std::map<std::string, double>{});
        d.methodBias = opt(j, "method_bias", std::map<std::string, double>{});
        d.hysteresisMult = opt(j, "hysteresis_mult", 1.0);
        d.inherit = opt(j, "inherit", 0.0);
        d.weightInPool = opt(j, "weight_in_pool", 1.0);
        if (!c.traits.emplace(d.id, d).second) err.add("traits: duplicate id " + d.id);
    }
    for (const auto& p : traits.value("exclusive", Json::array()))
        c.exclusiveTraits.emplace_back(p.at(0).get<std::string>(), p.at(1).get<std::string>());

    for (const auto& j : goals.value("goals", Json::array())) {
        warnUnknown(j, {"id", "task", "priority_class", "aggregate", "considerations", "base", "min_ticks",
                        "interrupt", "cooldown_on_fail"},
                    "goals", W);
        GoalDef d;
        d.id = j.at("id").get<std::string>();
        d.task = opt<std::string>(j, "task", "");
        d.priorityClass = opt<std::string>(j, "priority_class", "need");
        d.aggregate = opt<std::string>(j, "aggregate", "product");
        d.base = opt(j, "base", 1.0);
        d.minTicks = opt<Tick>(j, "min_ticks", 3);
        d.cooldownOnFail = opt<Tick>(j, "cooldown_on_fail", 12);
        if (j.contains("interrupt") && !j["interrupt"].is_null()) {
            try {
                d.interrupt = parseCondition(j["interrupt"].get<std::string>());
                d.hasInterrupt = true;
            } catch (const std::exception& e) {
                err.add("goal " + d.id + ": " + e.what());
            }
        }
        for (const auto& cj : j.value("considerations", Json::array())) {
            Consideration k;
            k.input = cj.at("input").get<std::string>();
            k.norm = opt(cj, "norm", 10.0);
            k.weight = opt(cj, "weight", 1.0);
            if (cj.contains("curve")) k.curve = curveFrom(cj["curve"]);
            d.considerations.push_back(k);
        }
        if (c.goal(d.id)) err.add("goals: duplicate id " + d.id);
        c.goals.push_back(d);
    }

    for (const auto& j : htn.value("tasks", Json::array())) {
        HtnTask t;
        t.id = j.at("id").get<std::string>();
        for (const auto& mj : j.value("methods", Json::array())) {
            warnUnknown(mj, {"id", "pre", "cost", "legal", "steps"}, "htn " + t.id, W);
            HtnMethod m;
            m.id = mj.at("id").get<std::string>();
            const std::string where = "htn " + t.id + "/" + m.id;
            m.pre = conditions(mj, "pre", where, err);
            m.cost = opt(mj, "cost", 1.0);
            m.legal = strings(mj, "legal");
            for (const auto& sj : mj.value("steps", Json::array())) {
                HtnStep s;
                if (sj.contains("do")) {
                    s.kind = HtnStep::Kind::Do;
                    s.ref = sj["do"].get<std::string>();
                    if (sj.contains("params") && sj["params"].is_object()) s.params = sj["params"];
                } else if (sj.contains("task")) {
                    s.kind = HtnStep::Kind::Task;
                    s.ref = sj["task"].get<std::string>();
                } else if (sj.contains("achieve")) {
                    s.kind = HtnStep::Kind::Achieve;
                    s.achieve = conditions(sj, "achieve", where, err);
                    s.allow = strings(sj, "allow");
                } else {
                    err.add(where + ": step without do/task/achieve");
                    continue;
                }
                m.steps.push_back(s);
            }
            t.methods.push_back(m);
        }
        if (!c.tasks.emplace(t.id, t).second) err.add("htn: duplicate task " + t.id);
    }

    for (const auto& j : interactions.value("interactions", Json::array())) {
        warnUnknown(j, {"id", "category", "target", "pre", "eff", "from_target", "duration", "cost", "fsm", "reserve",
                        "goap", "legal", "visibility", "social", "speech", "continuous", "until", "params", "chance",
                        "on_fail", "risk", "combat", "terms", "legal_if", "legal_unless", "consent", "mvp", "intents", "range_m", "craft"},
                    "interactions", W);
        InteractionDef d;
        d.id = j.at("id").get<std::string>();
        const std::string where = "interaction " + d.id;
        d.category = opt<std::string>(j, "category", "");
        d.target = opt<std::string>(j, "target", "self");
        if (d.targetsObject()) d.targetType = d.target.substr(7);
        if (d.targetsCreature()) d.targetType = d.target.substr(9);
        if (d.targetsCharacter()) d.targetType = "person";
        d.rangeM = opt(j, "range_m", -1.0);
        d.pre = conditions(j, "pre", where, err);
        d.eff = effects(j, "eff", where, err);
        if (j.contains("params") && j["params"].is_object())
            for (auto it = j["params"].begin(); it != j["params"].end(); ++it) {
                ParamDef p;
                p.name = it.key();
                if (it->is_string()) p.type = it->get<std::string>();
                else {
                    p.type = opt<std::string>(*it, "type", "");
                    p.of = strings(*it, "of");
                }
                d.params.push_back(p);
            }
        if (j.contains("chance") && !j["chance"].is_null()) {
            const Json& cj = j["chance"];
            d.chance.set = true;
            if (cj.is_number()) d.chance.base = cj.get<double>();
            else {
                warnUnknown(cj, {"base", "skill", "per_skill", "add", "min", "max"}, where + " chance", W);
                d.chance.base = opt(cj, "base", 1.0);
                d.chance.skill = opt<std::string>(cj, "skill", "");
                d.chance.perSkill = opt(cj, "per_skill", 0.0);
                for (const auto& aj : cj.value("add", Json::array())) {
                    ChanceInput in;
                    try {
                        in.input = parseCondition(aj.at("input").get<std::string>());
                    } catch (const std::exception& e) {
                        err.add(where + " chance.add: " + e.what());
                        continue;
                    }
                    in.k = opt(aj, "k", 0.0);
                    d.chance.add.push_back(in);
                }
                d.chance.min = opt(cj, "min", 0.0);
                d.chance.max = opt(cj, "max", 1.0);
            }
        }
        d.onFail = effects(j, "on_fail", where + " on_fail", err);
        for (const auto& rj : j.value("risk", Json::array())) {
            warnUnknown(rj, {"id", "p", "eff"}, where + " risk", W);
            RiskDef r;
            r.id = opt<std::string>(rj, "id", "");
            r.p = opt(rj, "p", 0.0);
            r.eff = effects(rj, "eff", where + " risk " + r.id, err);
            d.risk.push_back(r);
        }
        if (j.contains("combat") && j["combat"].is_object()) {
            const Json& cj = j["combat"];
            warnUnknown(cj, {"attack_skill", "defense_skill", "damage", "lethal", "outcomes"}, where + " combat", W);
            CombatDef& cb = d.combat;
            cb.set = true;
            cb.attackSkill = opt<std::string>(cj, "attack_skill", cb.attackSkill);
            cb.defenseSkill = opt<std::string>(cj, "defense_skill", cb.defenseSkill);
            if (cj.contains("damage")) {
                cb.damageLo = cj["damage"].at(0).get<double>();
                cb.damageHi = cj["damage"].at(1).get<double>();
            }
            cb.lethal = opt(cj, "lethal", false);
            if (cj.contains("outcomes") && cj["outcomes"].is_object())
                for (auto it = cj["outcomes"].begin(); it != cj["outcomes"].end(); ++it) {
                    const Json holder = {{"e", *it}};
                    cb.outcomes[it.key()] = effects(holder, "e", where + " combat." + it.key(), err);
                }
        }
        if (j.contains("terms") && j["terms"].is_object()) {
            const Json& tj = j["terms"];
            warnUnknown(tj, {"give", "take", "due", "commitment", "on_breach"}, where + " terms", W);
            TermsDef& t = d.terms;
            t.set = true;
            t.give = opt(tj, "give", std::map<std::string, int>{});
            t.take = opt(tj, "take", std::map<std::string, int>{});
            t.due = opt<Tick>(tj, "due", 0);
            t.commitment = opt<std::string>(tj, "commitment", "");
            t.onBreach = effects(tj, "on_breach", where + " terms", err);
        }
        d.fromTarget = opt(j, "from_target", false);
        d.duration = opt<Tick>(j, "duration", 1);
        d.cost = opt(j, "cost", 1.0);
        d.fsm = opt<std::string>(j, "fsm", "Interact");
        d.craft = opt<std::string>(j, "craft", "");
        d.reserve = opt(j, "reserve", false);
        d.goap = opt(j, "goap", d.target != "character" && d.params.empty());
        d.legal = strings(j, "legal");
        auto legalRules = [&](const char* key, std::vector<LegalRule>& out) {
            for (const auto& lj : j.value(key, Json::array())) {
                LegalRule r;
                r.tag = opt<std::string>(lj, "tag", "");
                r.mult = opt(lj, "mult", -1.0);
                for (const auto& alt : strings(lj, "if")) {
                    std::vector<Condition> any;
                    size_t from = 0;
                    while (from <= alt.size()) {
                        const size_t bar = alt.find('|', from);
                        const std::string part = alt.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
                        try {
                            any.push_back(parseCondition(part));
                        } catch (const std::exception& e) {
                            err.add(where + " " + key + ": " + e.what());
                        }
                        if (bar == std::string::npos) break;
                        from = bar + 1;
                    }
                    r.all.push_back(std::move(any));
                }
                if (r.tag.empty()) err.add(where + " " + key + ": rule without tag");
                out.push_back(std::move(r));
            }
        };
        legalRules("legal_if", d.legalIf);
        legalRules("legal_unless", d.legalUnless);
        d.consent = opt<std::string>(j, "consent", "");
        d.intents = strings(j, "intents");
        d.visibility = opt<std::string>(j, "visibility", "public");
        d.social = j.value("social", Json());
        d.speech = strings(j, "speech");
        d.continuous = opt(j, "continuous", false);
        if (j.contains("until") && !j["until"].is_null()) {
            try {
                d.until = parseCondition(j["until"].get<std::string>());
                d.hasUntil = true;
            } catch (const std::exception& e) {
                err.add(where + ": " + e.what());
            }
        }
        if (!c.interactions.emplace(d.id, d).second) err.add("interactions: duplicate id " + d.id);
    }

    for (const auto& j : fsm.value("states", Json::array())) {
        FsmStateDef d;
        d.id = j.at("id").get<std::string>();
        d.parent = opt<std::string>(j, "parent", "");
        if (j.contains("interruptible")) d.interruptible = j["interruptible"].get<bool>() ? 1 : 0;
        d.wakeOn = strings(j, "wake_on");
        if (!c.fsm.emplace(d.id, d).second) err.add("fsm: duplicate state " + d.id);
    }

    for (auto it = dialogue.begin(); it != dialogue.end(); ++it)
        if (it.key() != "speech_acts" && it.key() != "templates") c.dialogueRules[it.key()] = *it;
    for (const auto& j : knowledge.value("knowledge", Json::array())) {
        warnUnknown(j, {"id", "requires", "learn_ticks", "self_ticks", "start", "start_chance_adult",
                        "family_knowledge"}, "knowledge", W);
        KnowledgeDef k;
        k.id = j.at("id").get<std::string>();
        k.requiresKnowledge = strings(j, "requires");
        k.learnTicks = opt<Tick>(j, "learn_ticks", 0);
        k.selfTicks = opt<Tick>(j, "self_ticks", 0);
        k.start = opt<std::string>(j, "start", "");
        k.startChanceAdult = opt(j, "start_chance_adult", 0.0);
        k.familyKnowledge = opt(j, "family_knowledge", false);
        if (!c.knowledge.emplace(k.id, k).second) err.add("knowledge: duplicate id " + k.id);
    }
    for (const auto& j : species.value("species", Json::array())) {
        warnUnknown(j, {"id", "habitat", "biomes", "per_km2", "group", "max_group", "speed_m_per_tick", "notice_m",
                        "flee_m", "diet", "prey", "aggression", "attack", "hunger_per_day", "breed_every_days",
                        "territory_m", "starve_days", "domestic", "per_family", "per_family_chance",
                        "graze_seasons", "fodder", "fodder_per_head", "products", "breed_season", "brood", "cull",
                        "growth_per_year", "meat_days", "hunt_below", "leave_heads", "refuge_share", "immigrate_below", "_doc",
                        "sapience", "loot", "tags", "lair", "innate_spells", "hunt", "_source"},
                    "species", W);
        SpeciesDef s;
        s.id = j.at("id").get<std::string>();
        s.habitat = opt<std::string>(j, "habitat", "land");
        s.biomes = strings(j, "biomes");
        s.perKm2 = opt(j, "per_km2", 0.0);
        if (j.contains("group")) {
            s.groupLo = j["group"].at(0).get<int>();
            s.groupHi = j["group"].at(1).get<int>();
        }
        s.maxGroup = opt(j, "max_group", s.groupHi);
        s.speedMPerTick = opt(j, "speed_m_per_tick", 0.0);
        s.noticeM = opt(j, "notice_m", 0.0);
        s.fleeM = opt(j, "flee_m", 0.0);
        s.diet = opt<std::string>(j, "diet", "graze");
        s.prey = strings(j, "prey");
        s.aggression = opt(j, "aggression", 0.0);
        if (j.contains("attack") && j["attack"].is_object()) {
            s.attackChance = opt(j["attack"], "chance", 0.0);
            s.attackSatedDays = opt(j["attack"], "sated_days_after_kill", 0);
            if (j["attack"].contains("damage")) {
                s.attackLo = j["attack"]["damage"].at(0).get<double>();
                s.attackHi = j["attack"]["damage"].at(1).get<double>();
            }
        }
        s.hungerPerDay = opt(j, "hunger_per_day", 0.0);
        s.breedEveryDays = opt(j, "breed_every_days", 0);
        s.territoryM = opt(j, "territory_m", 0.0);
        s.starveDays = opt(j, "starve_days", 0);
        s.growthPerYear = opt(j, "growth_per_year", 0.0);
        s.meatDays = opt(j, "meat_days", 1.0);
        s.huntBelow = opt(j, "hunt_below", 0.7);
        s.leaveHeads = opt(j, "leave_heads", 0);
        s.immigrateBelow = opt(j, "immigrate_below", 0.0);
        s.refugeShare = opt(j, "refuge_share", 0.0);
        s.domestic = opt(j, "domestic", false);
        if (j.contains("per_family")) {
            s.perFamilyLo = j["per_family"].at(0).get<int>();
            s.perFamilyHi = j["per_family"].at(1).get<int>();
        }
        s.perFamilyChance = opt(j, "per_family_chance", 1.0);
        s.grazeSeasons = strings(j, "graze_seasons");
        s.fodder = opt<std::string>(j, "fodder", "");
        s.fodderPerHead = opt(j, "fodder_per_head", 0.0);
        for (const auto& pj : j.value("products", Json::array())) {
            SpeciesDef::Product pr;
            pr.item = pj.at("item").get<std::string>();
            pr.perHeadDay = opt(pj, "per_head_day", 0.0);
            if (pj.contains("days")) {
                pr.from = pj["days"].at(0).get<int>();
                pr.to = pj["days"].at(1).get<int>();
            }
            pr.capPerHead = opt(pj, "cap_per_head", pr.capPerHead);
            s.products.push_back(pr);
        }
        if (j.contains("breed_season")) {
            s.breedFrom = j["breed_season"].at(0).get<int>();
            s.breedTo = j["breed_season"].at(1).get<int>();
        }
        if (j.contains("brood")) {
            s.broodLo = j["brood"].at(0).get<int>();
            s.broodHi = j["brood"].at(1).get<int>();
        }
        if (j.contains("cull")) {
            const Json& cj = j["cull"];
            if (cj.contains("days")) {
                s.cullFrom = cj["days"].at(0).get<int>();
                s.cullTo = cj["days"].at(1).get<int>();
            }
            s.keep = opt(cj, "keep", 0);
        }
        s.sapience = opt(j, "sapience", s.sapience);
        if (j.contains("loot") && j["loot"].is_object()) s.loot = j["loot"].get<std::map<std::string, int>>();
        s.tags = strings(j, "tags");
        if (j.contains("innate_spells") && j["innate_spells"].is_array()) s.innateSpells = j["innate_spells"];
        if (!c.species.emplace(s.id, s).second) err.add("species: duplicate id " + s.id);
    }
    c.householdMust = strings(knowledge, "household_must");
    c.settlementMust = strings(knowledge, "settlement_must");
    for (const auto& sa : dialogue.value("speech_acts", Json::array()))
        c.speechActs.push_back(sa.is_string() ? sa.get<std::string>() : sa.at("id").get<std::string>());
    for (const auto& j : dialogue.value("templates", Json::array())) {
        warnUnknown(j, {"id", "speech", "pre", "eff", "variants", "replies"}, "dialogue", W);
        DialogueTemplate t;
        t.id = j.at("id").get<std::string>();
        const std::string where = "dialogue " + t.id;
        t.speech = opt<std::string>(j, "speech", "");
        t.pre = conditions(j, "pre", where, err);
        t.eff = effects(j, "eff", where, err);
        t.replies = strings(j, "replies");
        for (const auto& vj : j.value("variants", Json::array())) {
            DialogueVariant v;
            if (vj.is_string()) v.id = vj.get<std::string>();
            else {
                warnUnknown(vj, {"id", "pre", "weight"}, where + " variant", W);
                v.id = vj.at("id").get<std::string>();
                v.pre = conditions(vj, "pre", where + "/" + v.id, err);
                v.weight = opt(vj, "weight", 1.0);
            }
            t.variants.push_back(v);
        }
        if (!c.dialogue.emplace(t.id, t).second) err.add("dialogue: duplicate template " + t.id);
    }

    AiConfig& a = c.config;
    a.topK = opt(config, "top_k", a.topK);
    a.hysteresisBonus = opt(config, "hysteresis_bonus", a.hysteresisBonus);
    a.switchMargin = opt(config, "switch_margin", a.switchMargin);
    a.failPenalty = opt(config, "fail_penalty", a.failPenalty);
    a.htnMaxDepth = opt(config, "htn_max_depth", a.htnMaxDepth);
    a.methodMaxFails = opt(config, "method_max_fails", a.methodMaxFails);
    a.methodFailTicks = opt(config, "method_fail_ticks", a.methodFailTicks);
    a.goapMaxDepth = opt(config, "goap_max_depth", a.goapMaxDepth);
    a.goapMaxNodes = opt(config, "goap_max_nodes", a.goapMaxNodes);
    a.reserveTicks = opt(config, "reserve_ticks", a.reserveTicks);
    a.walkMPerTick = opt(config, "walk_m_per_tick", a.walkMPerTick);
    a.interactionRangeM = opt(config, "interaction_range_m", a.interactionRangeM);
    a.contactM = opt(config, "contact_m", a.contactM);
    a.contactSlackM = opt(config, "contact_slack_m", a.contactSlackM);
    a.rangedFromM = opt(config, "ranged_from_m", a.rangedFromM);
    a.legalBase = opt(config, "legal_base", a.legalBase);
    a.perceptionRadiusM = opt(config, "perception_radius_m", a.perceptionRadiusM);
    a.noticeChance = opt(config, "notice_chance", a.noticeChance);
    a.trackRadiusM = opt(config, "track_radius_m", a.trackRadiusM);
    a.traceSize = opt(config, "trace_size", a.traceSize);
    a.playerIdleResumeTicks = opt(config, "player_idle_resume_ticks", a.playerIdleResumeTicks);
    a.playerGoalHoldTicks = opt(config, "player_goal_hold_ticks", a.playerGoalHoldTicks);
    a.playerGoalHoldBonus = opt(config, "player_goal_hold_bonus", a.playerGoalHoldBonus);
    a.hintDecayTicks = opt(config, "hint_decay_ticks", a.hintDecayTicks);
    a.memoryCap = opt(config, "memory_cap", a.memoryCap);
    if (config.contains("skill_gain")) {
        a.skillGainMult = opt(config["skill_gain"], "mult", a.skillGainMult);
        a.skillGainCurve = opt(config["skill_gain"], "curve", a.skillGainCurve);
        a.skillMinAgeYears = opt(config["skill_gain"], "min_age_years", a.skillMinAgeYears);
        a.skillChildUntilYears = opt(config["skill_gain"], "child_until_years", a.skillChildUntilYears);
        a.skillChildMult = opt(config["skill_gain"], "child_mult", a.skillChildMult);
        a.skillTalentMin = opt(config["skill_gain"], "talent_min", a.skillTalentMin);
        a.skillTalentMax = opt(config["skill_gain"], "talent_max", a.skillTalentMax);
        a.skillOverTalentFalloff = opt(config["skill_gain"], "over_talent_falloff", a.skillOverTalentFalloff);
    }
    if (config.contains("modifier_bounds")) {
        a.modMaxAbsDelta = opt(config["modifier_bounds"], "max_abs_delta", a.modMaxAbsDelta);
        a.modMaxTtl = opt<Tick>(config["modifier_bounds"], "max_ttl", a.modMaxTtl);
    }
    a.memoryTypes = opt(config, "memory_types", std::map<std::string, double>{});
    if (config.contains("reputation_axes")) a.reputationAxes = strings(config, "reputation_axes");
    if (config.contains("status_pairs")) a.statusPairs = strings(config, "status_pairs");
    if (config.contains("rights")) a.rights = strings(config, "rights");
    if (config.contains("verdicts")) a.verdicts = strings(config, "verdicts");
    if (config.contains("household_roles")) a.householdRoles = strings(config, "household_roles");
    if (config.contains("death_causes")) a.deathCauses = strings(config, "death_causes");
    if (config.contains("commitments") && config["commitments"].is_object())
        for (auto it = config["commitments"].begin(); it != config["commitments"].end(); ++it) {
            CommitmentDef cm;
            cm.id = it.key();
            cm.due = opt<Tick>(*it, "due", cm.due);
            cm.onBreach = effects(*it, "on_breach", "config commitments." + cm.id, err);
            a.commitments[cm.id] = cm;
        }

    if (!err.list.empty()) {
        std::ostringstream os;
        os << "content errors:";
        for (const auto& e : err.list) os << "\n  " << e;
        throw std::runtime_error(os.str());
    }
    return c;
}

const SoilDef& ClimateDef::soil(const std::string& biome) const {
    static const SoilDef fallback;
    auto it = soils.find(biome);
    if (it != soils.end()) return it->second;
    it = soils.find("default");
    return it != soils.end() ? it->second : fallback;
}

ClimateDef parseClimate(const Json& j, std::vector<std::string>& W) {
    ClimateDef d;
    if (!j.is_object() || j.empty()) return d;
    d.loaded = true;
    static const char* kSeasonNames[4] = {"spring", "summer", "autumn", "winter"};
    const Json seasons = j.value("seasons", Json::object());
    for (int i = 0; i < 4; ++i) {
        if (!seasons.contains(kSeasonNames[i])) continue;
        const Json& s = seasons.at(kSeasonNames[i]);
        warnUnknown(s, {"temp_c", "temp_sd", "rain_mm", "wet_chance", "_doc"}, "climate.seasons", W);
        SeasonClimate& sc = d.seasons[i];
        sc.tempC = opt(s, "temp_c", sc.tempC);
        sc.tempSd = opt(s, "temp_sd", sc.tempSd);
        sc.rainMm = opt(s, "rain_mm", sc.rainMm);
        sc.wetChance = std::clamp(opt(s, "wet_chance", sc.wetChance), 0.01, 0.99);
    }
    d.biomeRainMult = opt(j, "biome_rain_mult", std::map<std::string, double>{});
    d.biomeTempAdd = opt(j, "biome_temp_add", std::map<std::string, double>{});
    if (j.contains("wind")) { // AL-115
        const Json& wj = j.at("wind");
        warnUnknown(wj, {"from_deg", "ms", "sd", "_doc"}, "climate.wind", W);
        d.windFromDeg = opt(wj, "from_deg", d.windFromDeg);
        d.windMs = opt(wj, "ms", d.windMs);
        d.windSd = opt(wj, "sd", d.windSd);
    }
    const Json soils = j.value("soils", Json::object()); // a named copy: items() of a temporary dangles
    for (const auto& [biome, s] : soils.items()) {
        if (biome.rfind('_', 0) == 0) continue;
        warnUnknown(s, {"fertility", "depth", "drainage", "capacity_mm", "_doc"}, "climate.soils", W);
        SoilDef sd;
        sd.fertility = opt(s, "fertility", sd.fertility);
        sd.depth = opt(s, "depth", sd.depth);
        sd.drainage = opt(s, "drainage", sd.drainage);
        sd.capacityMm = opt(s, "capacity_mm", sd.capacityMm);
        d.soils[biome] = sd;
    }
    return d;
}

CareDef parseCare(const Json& j, std::vector<std::string>& W) {
    CareDef d;
    if (!j.is_object()) return d;
    d.threshold = opt(j, "need_threshold", d.threshold);
    d.charityDays = opt(j, "charity_days", d.charityDays);
    d.charitySpare = opt(j, "charity_spare_days", d.charitySpare);
    d.circles = opt(j, "circles", std::map<std::string, double>{});
    for (const auto& x : j.value("dependents", Json::array())) {
        warnUnknown(x, {"id", "age_lt", "age_ge", "health_lt", "trait", "weight", "needs", "_doc"}, "care.dependents", W);
        DependentDef dd;
        dd.id = x.at("id").get<std::string>();
        dd.ageLt = opt(x, "age_lt", dd.ageLt);
        dd.ageGe = opt(x, "age_ge", dd.ageGe);
        dd.healthLt = opt(x, "health_lt", dd.healthLt);
        dd.trait = opt(x, "trait", std::string());
        dd.weight = opt(x, "weight", dd.weight);
        dd.needs = strings(x, "needs");
        d.dependents.push_back(dd);
    }
    return d;
}

BuildCatalog parseBuild(const Json& j) {
    BuildCatalog b;
    if (!j.is_object()) return b;
    b.cellM = j.value("cell_m", b.cellM);
    b.storeyM = j.value("storey_m", b.storeyM);
    b.minStability = j.value("min_stability", b.minStability);
    b.maxPerFamily = j.value("max_per_family", b.maxPerFamily);
    b.thingM = j.value("thing_m", b.thingM);
    const Json list = j.value("pieces", Json::array());
    for (const auto& p : list) {
        BuildPieceDef d;
        d.id = p.at("id").get<std::string>();
        d.slot = p.at("slot").get<std::string>();
        d.category = p.at("category").get<std::string>();
        d.material = p.value("material", std::string());
        d.mesh = p.value("mesh", d.id);
        if (p.contains("cost"))
            for (const auto& [k, v] : p["cost"].items()) d.cost[k] = v.get<int>();
        d.work = std::max(1, p.value("work", 1));
        d.carry = p.value("carry", d.carry);
        d.shelter = p.value("shelter", false);
        d.mirror = p.value("mirror", false);
        d.door = p.value("door", false);
        d.makes = p.value("makes", std::string());
        if (d.slot == "thing") d.sub = std::max(1, int(std::lround(b.cellM / std::max(0.1, b.thingM))));
        b.pieces.push_back(std::move(d));
    }
    return b;
}

RoomCatalog parseRooms(const Json& j) {
    RoomCatalog c;
    if (!j.is_object()) return c;
    const Json fb = j.value("fallback", Json::object());
    c.closedFallback = fb.value("closed", c.closedFallback);
    c.openFallback = fb.value("open", c.openFallback);
    c.roofCover = j.value("roof_cover", c.roofCover);
    std::set<std::string> types;
    for (const auto& r : j.value("rooms", Json::array())) {
        RoomRuleDef d;
        d.id = r.at("id").get<std::string>();
        d.priority = r.value("priority", 0);
        d.purposeRank = r.value("purpose_rank", d.priority);
        d.enclosure = r.value("enclosure", d.enclosure);
        for (const auto& n : r.value("needs", Json::array())) {
            RoomNeed need;
            need.any = n.value("any", std::vector<std::string>{});
            need.n = std::max(1, n.value("n", 1));
            for (const auto& t : need.any) types.insert(t);
            d.needs.push_back(std::move(need));
        }
        c.rules.push_back(std::move(d));
    }
    std::stable_sort(c.rules.begin(), c.rules.end(), [](const RoomRuleDef& a, const RoomRuleDef& b) { return a.priority > b.priority; });
    c.thingTypes.assign(types.begin(), types.end());
    return c;
}

Content Content::load(const std::string& dir) {
    auto f = [&](const char* name) {
        std::ifstream probe(dir + "/" + name);
        return probe ? readJson(dir + "/" + name) : Json::object();
    };
    Json items = f("items.json"), objects = f("objects.json"), needs = f("needs.json"), traits = f("traits.json"),
         htn = f("htn.json"), interactions = f("interactions.json"), config = f("config.json"),
         knowledge = f("knowledge.json");
    // AL-74: the cuisine pack (dishes, drinks, household things) joins before the expansion,
    // by the same rules: a new id is added, a shared one keeps what was there.
    ExpansionData cuisine =
        mergeExpansion(dir + "/cuisine", items, objects, needs, traits, htn, interactions, config, knowledge);
    // AL-86: the resources pack (world sources, raw things, the items archetypes count as),
    // by the same rules, after the cuisine and before the expansion.
    ExpansionData resources =
        mergeExpansion(dir + "/resources", items, objects, needs, traits, htn, interactions, config, knowledge);
    // AL-117: the alchemy pack (potions, scrolls, alembic and crucible, reagents, alchemy and runecraft)
    ExpansionData alchemy =
        mergeExpansion(dir + "/alchemy", items, objects, needs, traits, htn, interactions, config, knowledge);
    ExpansionData expansion =
        mergeExpansion(dir + "/expansion", items, objects, needs, traits, htn, interactions, config, knowledge);
    for (const auto& e : cuisine.errors) expansion.errors.push_back("cuisine: " + e);
    for (const auto& k : cuisine.kept) expansion.kept.push_back("cuisine:" + k);
    for (const auto& e : resources.errors) expansion.errors.push_back("resources: " + e);
    for (const auto& k : resources.kept) expansion.kept.push_back("resources:" + k);
    for (const auto& e : alchemy.errors) expansion.errors.push_back("alchemy: " + e);
    for (const auto& k : alchemy.kept) expansion.kept.push_back("alchemy:" + k);
    // A pack may annotate items it does not add (base and expansion ones): its category tags
    // join theirs, other fields (worth, quality, material) fill in where the item has none.
    for (const char* pack : {"cuisine", "resources"}) {
        const std::string path = dir + "/" + pack + "/items.json";
        std::ifstream probe(path);
        if (!probe) continue;
        const Json cu = readJson(path);
        for (const auto& a : cu.value("annotate", Json::array())) {
            const std::string id = a.value("id", std::string());
            bool found = false;
            for (auto& e : items["items"]) {
                if (e.value("id", std::string()) != id) continue;
                found = true;
                Json& tags = e["tags"];
                if (!tags.is_array()) tags = Json::array();
                for (const auto& t : a.value("add_tags", Json::array()))
                    if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
                for (const auto& [k, v] : a.items())
                    if (k != "id" && k != "add_tags" && k[0] != '_' && !e.contains(k)) e[k] = v;
            }
            if (!found) expansion.errors.push_back(std::string(pack) + ": annotate of unknown item '" + id + "'");
        }
    }
    // AL-114: monsters (content/creatures) join the animals, their loot items and hunts join the catalog.
    Json species = f("species.json");
    std::vector<std::string> creatureErrors = mergeCreatures(dir + "/../creatures", species, items, interactions);
    Content c = fromJson(items, objects, needs, traits, f("goals.json"), htn, interactions, f("fsm.json"), config,
                         f("dialogue.json"), knowledge, species);
    c.expansion = std::move(expansion);
    for (auto& e : creatureErrors) c.expansion.errors.push_back("creatures: " + e);
    c.peoples = loadSpecies(dir + "/../species"); // AL-114: sapient kinds (absent = everyone human)
    c.climate = parseClimate(f("climate.json"), c.warnings);
    c.care = parseCare(f("care.json"), c.warnings);
    c.roomsJson = f("rooms.json"); // AL-88: before the design acts (workshop rooms)
    c.craft = craft::CraftData::load(dir + "/craft");
    if (c.craft.loaded && c.craft.cfg.goapDesigns)
        for (auto& a : designInteractions(c)) c.interactions.emplace(a.id, a);
    c.build = parseBuild(f("build.json"));
    c.rooms = parseRooms(c.roomsJson);
    parseBackstories(f("backstories.json"), c);
    {
        const Json names = f("names.json");
        const Json given = names.value("given", Json::object());
        for (const auto& n : given.value("m", Json::array())) c.givenNamesM.push_back(n.get<std::string>());
        for (const auto& n : given.value("f", Json::array())) c.givenNamesF.push_back(n.get<std::string>());
        for (const auto& n : names.value("family", Json::array())) c.familyNames.push_back(n.get<std::string>());
    }
    c.dir = dir;
    ValidationReport v = validate(c);
    for (auto& w : v.warnings) c.warnings.push_back(w);
    if (!v.ok()) {
        std::ostringstream os;
        os << "content validation failed (" << dir << "):";
        for (const auto& e : v.errors) os << "\n  " << e;
        throw std::runtime_error(os.str());
    }
    addForeignVariants(c); // AL-32: acts on another household's things, built from the validated ones
    return c;
}

} // namespace altera::sim
