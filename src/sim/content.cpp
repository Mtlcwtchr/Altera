#include "sim/content.hpp"

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
        warnUnknown(j, {"id", "tags", "nutrition", "stack", "warmth", "tool_wear", "spoil_days"}, "items", W);
        ItemDef d;
        d.id = j.at("id").get<std::string>();
        d.tags = strings(j, "tags");
        d.nutrition = opt(j, "nutrition", 0.0);
        d.warmth = opt(j, "warmth", 0.0);
        d.toolWear = opt(j, "tool_wear", 0.0);
        d.spoilDays = opt(j, "spoil_days", 0.0);
        if (!c.items.emplace(d.id, d).second) err.add("items: duplicate id " + d.id);
    }

    for (const auto& j : objects.value("objects", Json::array())) {
        warnUnknown(j, {"id", "stock", "max_stock", "regrow", "regrow_every", "biomes", "per_km2", "owned",
                        "settlement", "per_family", "per_person", "warmth", "shelter", "per_settlement", "season"},
                    "objects", W);
        ObjectDef d;
        d.id = j.at("id").get<std::string>();
        d.stock = opt(j, "stock", std::map<std::string, int>{});
        d.maxStock = opt(j, "max_stock", d.stock);
        d.regrow = opt(j, "regrow", std::map<std::string, int>{});
        d.regrowEvery = opt<Tick>(j, "regrow_every", 0);
        d.biomes = strings(j, "biomes");
        d.perKm2 = opt(j, "per_km2", 0.0);
        d.owned = opt(j, "owned", false);
        d.settlement = opt(j, "settlement", false);
        d.perFamily = opt(j, "per_family", 0);
        d.perPerson = opt(j, "per_person", 0);
        d.warmth = opt(j, "warmth", false);
        d.shelter = opt(j, "shelter", false);
        d.perSettlement = opt(j, "per_settlement", 0);
        d.seasons = strings(j, "season");
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
                        "on_fail", "risk", "combat", "terms", "legal_if", "legal_unless", "consent", "mvp", "intents", "range_m"},
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
                        "territory_m", "starve_days"},
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
            if (j["attack"].contains("damage")) {
                s.attackLo = j["attack"]["damage"].at(0).get<double>();
                s.attackHi = j["attack"]["damage"].at(1).get<double>();
            }
        }
        s.hungerPerDay = opt(j, "hunger_per_day", 0.0);
        s.breedEveryDays = opt(j, "breed_every_days", 0);
        s.territoryM = opt(j, "territory_m", 0.0);
        s.starveDays = opt(j, "starve_days", 0);
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

Content Content::load(const std::string& dir) {
    auto f = [&](const char* name) {
        std::ifstream probe(dir + "/" + name);
        return probe ? readJson(dir + "/" + name) : Json::object();
    };
    Content c = fromJson(f("items.json"), f("objects.json"), f("needs.json"), f("traits.json"), f("goals.json"),
                         f("htn.json"), f("interactions.json"), f("fsm.json"), f("config.json"), f("dialogue.json"),
                         f("knowledge.json"), f("species.json"));
    ValidationReport v = validate(c);
    for (auto& w : v.warnings) c.warnings.push_back(w);
    if (!v.ok()) {
        std::ostringstream os;
        os << "content validation failed (" << dir << "):";
        for (const auto& e : v.errors) os << "\n  " << e;
        throw std::runtime_error(os.str());
    }
    return c;
}

} // namespace altera::sim
