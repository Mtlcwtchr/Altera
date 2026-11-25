#include "sim/craft/craft_data.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace lr::sim::craft {

namespace {

std::vector<std::string> strs(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    const Json& v = j.at(key);
    if (v.is_string()) out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const auto& x : v)
            if (x.is_string()) out.push_back(x.get<std::string>());
    return out;
}

template <class T>
T opt(const Json& j, const char* key, T dflt) {
    if (!j.contains(key) || j.at(key).is_null()) return dflt;
    try {
        return j.at(key).get<T>();
    } catch (...) {
        return dflt;
    }
}

std::map<std::string, double> numMap(const Json& j, const char* key) {
    std::map<std::string, double> out;
    if (!j.contains(key) || !j.at(key).is_object()) return out;
    for (auto it = j.at(key).begin(); it != j.at(key).end(); ++it)
        if (it.value().is_number()) out[it.key()] = it.value().get<double>();
    return out;
}

struct Errs {
    std::vector<std::string>& v;
    void add(const std::string& s) { v.push_back(s); }
};

Expr expr(const std::string& src, const std::string& where, Errs& e) {
    std::string err;
    Expr x = Expr::compile(src, &err);
    if (!err.empty()) e.add(where + ": " + err);
    return x;
}

Expr exprJ(const Json& j, const char* key, const std::string& where, Errs& e) {
    if (!j.contains(key)) return {};
    const Json& v = j.at(key);
    if (v.is_number()) return Expr::constant(v.get<double>());
    if (v.is_string()) return expr(v.get<std::string>(), where + "." + key, e);
    e.add(where + "." + key + ": expected a craft-expr string or a number");
    return {};
}

// "worth": a value component any def may carry (AL-87); 1 = ordinary.
double worthField(const Json& j, const std::string& where, Errs& e) {
    if (!j.is_object() || !j.contains("worth")) return 1.0;
    const Json& v = j["worth"];
    if (!v.is_number() || v.get<double>() <= 0) {
        e.add(where + ".worth: expected a number > 0");
        return 1.0;
    }
    return v.get<double>();
}

void warnUnknown(const Json& j, std::initializer_list<const char*> known, const std::string& where,
                 std::vector<std::string>& w) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key().rfind("_", 0) == 0) continue;
        bool ok = false;
        for (const char* k : known)
            if (it.key() == k) ok = true;
        if (!ok) w.push_back(where + ": unknown field '" + it.key() + "'");
    }
}

} // namespace

const std::vector<std::string>& propNames() {
    static const std::vector<std::string> v{"hardness", "toughness", "density", "flex",    "edge",     "melt",
                                            "flammable", "porous",   "fiber",   "moisture", "purity",  "nutrition",
                                            "sugar",    "toxic",     "tacky",   "workable", "warmth",  "cover",
                                            "waterproof",
                                            // AL-117: how well the stuff carries aspects, how much power it holds
                                            "conductivity", "mana_capacity"};
    return v;
}

bool isProp(const std::string& n) {
    const auto& v = propNames();
    return std::find(v.begin(), v.end(), n) != v.end();
}

template <class M>
static auto findIn(const M& m, const std::string& id) -> const typename M::mapped_type* {
    auto it = m.find(id);
    return it == m.end() ? nullptr : &it->second;
}

const MaterialDef* CraftData::material(const std::string& id) const { return findIn(materials, id); }
const ShapeDef* CraftData::shape(const std::string& id) const { return findIn(shapes, id); }
const ProcessDef* CraftData::process(const std::string& id) const { return findIn(processes, id); }
const StationDef* CraftData::station(const std::string& id) const { return findIn(stations, id); }
const FlawDef* CraftData::flaw(const std::string& id) const { return findIn(flaws, id); }
const FormDef* CraftData::form(const std::string& id) const {
    for (const auto& f : forms)
        if (f.id == id) return &f;
    return nullptr;
}
const DesignDef* CraftData::design(const std::string& id) const {
    for (const auto& d : designs)
        if (d.id == id) return &d;
    return nullptr;
}
bool CraftData::isFunction(const std::string& id) const {
    for (const auto& f : functions)
        if (f.id == id) return true;
    return false;
}

CraftData CraftData::fromJson(const Json& materials, const Json& shapes, const Json& processes, const Json& functions,
                              const Json& forms, const Json& flaws, const Json& reacts, const Json& stations,
                              const Json& config, const Json& designs) {
    CraftData d;
    Errs E{d.errors};
    auto& W = d.warnings;

    // --- config
    {
        const Json& c = config;
        if (c.contains("outcome")) {
            const Json& o = c["outcome"];
            d.cfg.k = opt(o, "k", d.cfg.k);
            d.cfg.noise = opt(o, "noise", d.cfg.noise);
            if (o.contains("thresholds")) {
                const Json& t = o["thresholds"];
                d.cfg.tCatastrophe = opt(t, "catastrophe", d.cfg.tCatastrophe);
                d.cfg.tRuin = opt(t, "flawed", d.cfg.tRuin);
                d.cfg.tOk = opt(t, "ok", d.cfg.tOk);
                d.cfg.tGood = opt(t, "good", d.cfg.tGood);
                d.cfg.tMaster = opt(t, "masterwork", d.cfg.tMaster);
            }
        }
        if (c.contains("weights")) {
            const Json& w = c["weights"];
            d.cfg.wTool = opt(w, "tool", d.cfg.wTool);
            d.cfg.wStation = opt(w, "station", d.cfg.wStation);
            d.cfg.wKnow = opt(w, "material_knowledge", d.cfg.wKnow);
            d.cfg.wTrait = opt(w, "trait", d.cfg.wTrait);
            d.cfg.repeatBonus = opt(w, "repeat_bonus", d.cfg.repeatBonus);
        }
        if (c.contains("quality_tiers")) {
            const Json& q = c["quality_tiers"];
            d.cfg.tierCrude = opt(q, "crude", d.cfg.tierCrude);
            d.cfg.tierGood = opt(q, "good", d.cfg.tierGood);
            d.cfg.tierMaster = opt(q, "masterwork", d.cfg.tierMaster);
        }
        if (c.contains("form_match")) {
            const Json& f = c["form_match"];
            d.cfg.tau = opt(f, "tau", d.cfg.tau);
            d.cfg.falloff = opt(f, "falloff", d.cfg.falloff);
            d.cfg.improvisedMin = opt(f, "improvised_min", d.cfg.improvisedMin);
            d.cfg.effectPresent = opt(f, "effect_present", d.cfg.effectPresent);
        }
        if (c.contains("salvage")) {
            d.cfg.salvageMin = opt(c["salvage"], "min", d.cfg.salvageMin);
            d.cfg.salvageMax = opt(c["salvage"], "max", d.cfg.salvageMax);
        }
        if (c.contains("xp")) {
            const Json& x = c["xp"];
            d.cfg.xpRate = opt(x, "rate", d.cfg.xpRate);
            d.cfg.xpWindow = opt(x, "challenge_window", d.cfg.xpWindow);
            d.cfg.xpFailRatio = opt(x, "fail_ratio", d.cfg.xpFailRatio);
            d.cfg.knowGain = opt(x, "know_gain", d.cfg.knowGain);
        }
        d.cfg.novice = opt(c, "novice_skill", d.cfg.novice);
        d.cfg.goapDesigns = opt(c, "goap_designs", d.cfg.goapDesigns);
        d.cfg.toolQualityBare = opt(c, "bare_tool_quality", d.cfg.toolQualityBare);
        d.cfg.dryBelow = opt(c, "dry_below", d.cfg.dryBelow);
        if (c.contains("recipe_book")) {
            const Json& r = c["recipe_book"];
            d.cfg.masterThreshold = opt(r, "master_threshold", d.cfg.masterThreshold);
            d.cfg.masterSuccesses = opt(r, "master_successes", d.cfg.masterSuccesses);
            d.cfg.fastCraftMaxStages = opt(r, "fast_craft_max_stages", d.cfg.fastCraftMaxStages);
            d.cfg.fragmentsPerSuccess = opt(r, "fragments_per_success", d.cfg.fragmentsPerSuccess);
            d.cfg.masteryPerSuccess = opt(r, "mastery_per_success", d.cfg.masteryPerSuccess);
            d.cfg.teachShare = opt(r, "teach_share", d.cfg.teachShare);
        }
        if (c.contains("breakthrough")) {
            const Json& b = c["breakthrough"];
            d.cfg.brExpert = opt(b, "base_expert", d.cfg.brExpert);
            d.cfg.brNovice = opt(b, "base_novice", d.cfg.brNovice);
            d.cfg.brCap = opt(b, "cap", d.cfg.brCap);
            d.cfg.brCooldownDays = opt(b, "cooldown_days", d.cfg.brCooldownDays);
            d.cfg.brNoveltyBoost = opt(b, "novelty_boost", d.cfg.brNoveltyBoost);
            if (b.contains("on_hit")) d.cfg.brQBonus = opt(b["on_hit"], "q_bonus", d.cfg.brQBonus);
        }
        if (c.contains("limits")) {
            d.cfg.maxInputsPerStep = opt(c["limits"], "max_inputs_per_step", d.cfg.maxInputsPerStep);
            d.cfg.maxPartsPerItem = opt(c["limits"], "max_parts_per_item", d.cfg.maxPartsPerItem);
        }
        if (c.contains("traits") && c["traits"].is_object())
            for (auto it = c["traits"].begin(); it != c["traits"].end(); ++it)
                for (auto jt = it.value().begin(); jt != it.value().end(); ++jt)
                    if (jt.value().is_number()) d.cfg.traitMods[it.key()][jt.key()] = jt.value().get<double>();
        if (c.contains("worth")) {
            const Json& wj = c["worth"];
            d.cfg.worthAvg = opt(wj, "parts_avg", d.cfg.worthAvg);
            d.cfg.worthMax = opt(wj, "parts_finest", d.cfg.worthMax);
            d.cfg.worthMin = opt(wj, "min", d.cfg.worthMin);
            d.cfg.worthCap = opt(wj, "cap", d.cfg.worthCap);
            if (wj.contains("tags") && wj["tags"].is_object())
                for (auto it = wj["tags"].begin(); it != wj["tags"].end(); ++it) {
                    if (!it.value().is_number() || it.value().get<double>() <= 0)
                        E.add("crafting_config.worth.tags." + it.key() + ": worth must be a number > 0");
                    else d.cfg.tagWorth[it.key()] = it.value().get<double>();
                }
        }
        if (c.contains("magic")) { // AL-117
            const Json& m = c["magic"];
            auto& g = d.magic;
            g.runeMinConductivity = opt(m, "rune_min_conductivity", g.runeMinConductivity);
            g.aspectMax = opt(m, "aspect_max", g.aspectMax);
            g.unstableSafe = opt(m, "unstable_safe", g.unstableSafe);
            g.blastK = opt(m, "blast_k", g.blastK);
            if (m.contains("blast_effects")) g.blastEffects = strs(m, "blast_effects");
            g.bindCapShare = opt(m, "bind_cap_share", g.bindCapShare);
            g.runeWorth = opt(m, "rune_worth", g.runeWorth);
            g.resonanceWorth = opt(m, "resonance_worth", g.resonanceWorth);
            g.resonanceDeeds = std::max(1.0, opt(m, "resonance_deeds", g.resonanceDeeds));
            g.resonanceRune = opt(m, "resonance_rune", g.resonanceRune);
            if (m.contains("triggers")) g.triggers = strs(m, "triggers");
            g.defaultTrigger = m.value("default_trigger", g.defaultTrigger);
            g.chargePerUse = opt(m, "charge_per_use", g.chargePerUse);
        }
        if (c.contains("heat") && c["heat"].contains("levels"))
            for (auto it = c["heat"]["levels"].begin(); it != c["heat"]["levels"].end(); ++it)
                d.cfg.heatC[std::stoi(it.key())] = it.value().get<double>();
    }

    // --- shapes
    for (const auto& j : shapes.value("shapes", Json::array())) {
        ShapeDef s;
        s.id = j.value("id", "");
        warnUnknown(j, {"id", "role", "stage", "len", "thick", "v", "worth"}, "shapes." + s.id, W);
        s.worth = worthField(j, "shapes." + s.id, E);
        s.role = j.value("role", "");
        s.stage = j.value("stage", "part");
        if (j.contains("len") && j["len"].is_array() && j["len"].size() == 3) {
            s.lenMin = j["len"][0].get<int>();
            s.lenMax = j["len"][1].get<int>();
            s.lenDefault = j["len"][2].get<int>();
        }
        s.thick = j.value("thick", 1);
        s.v = numMap(j, "v");
        if (s.id.empty()) E.add("shapes: an entry without id");
        else if (!d.shapes.emplace(s.id, s).second) E.add("shapes: duplicate " + s.id);
    }

    // --- materials
    for (const auto& j : materials.value("materials", Json::array())) {
        MaterialDef m;
        m.id = j.value("id", "");
        const std::string where = "materials." + m.id;
        warnUnknown(j, {"id", "tags", "p", "states", "heat", "refine", "effects", "potency", "shape", "len", "thick",
                        "salvage", "item", "raw", "worth"},
                    where, W);
        m.worth = worthField(j, where, E);
        m.tags = strs(j, "tags");
        m.p = numMap(j, "p");
        for (const auto& [k, v] : m.p)
            if (!isProp(k)) E.add(where + ".p: unknown property " + k);
        m.potency = opt(j, "potency", 1.0);
        m.shape = j.value("shape", "lump");
        m.len = j.value("len", 1);
        m.thick = j.value("thick", 1);
        m.salvage = j.value("salvage", "");
        m.item = j.value("item", "");
        m.raw = j.value("raw", true);
        for (const auto& s : j.value("states", Json::array())) {
            MatStateDef st;
            st.id = s.value("id", "");
            st.when = exprJ(s, "when", where + ".states." + st.id, E);
            st.temp = s.value("temp", false);
            st.add = numMap(s, "add");
            m.states.push_back(st);
        }
        for (const auto& h : j.value("heat", Json::array())) {
            HeatReactDef r;
            r.id = h.value("id", "");
            warnUnknown(h, {"id", "at", "no_air", "needs_dry", "needs", "modes", "when", "to", "to_state", "set", "result"},
                        where + ".heat." + r.id, W);
            r.at = h.value("at", 1);
            r.noAir = h.value("no_air", false);
            r.needsDry = h.value("needs_dry", false);
            r.needs = strs(h, "needs");
            r.modes = strs(h, "modes");
            r.when = exprJ(h, "when", where + ".heat." + r.id, E);
            r.to = h.value("to", "");
            r.toState = h.value("to_state", "");
            r.set = numMap(h, "set");
            r.result = h.value("result", "");
            m.heat.push_back(r);
        }
        for (const auto& r : j.value("refine", Json::array())) {
            RefineDef f;
            f.process = r.value("process", "");
            f.when = exprJ(r, "when", where + ".refine", E);
            f.to = r.value("to", "");
            m.refine.push_back(f);
        }
        for (const auto& e : j.value("effects", Json::array())) {
            EffectDef ef;
            ef.id = e.value("id", "");
            ef.k = e.value("k", 0.0);
            ef.extract = strs(e, "extract");
            m.effects.push_back(ef);
        }
        if (m.id.empty()) E.add("materials: an entry without id");
        else if (!d.materials.emplace(m.id, m).second) E.add("materials: duplicate " + m.id);
    }

    // --- processes
    for (const auto& j : processes.value("processes", Json::array())) {
        ProcessDef p;
        p.id = j.value("id", "");
        const std::string where = "processes." + p.id;
        warnUnknown(j, {"id", "op", "skill", "skill_by", "category", "requires", "inputs", "time", "difficulty",
                        "shape_map", "assemble", "transform", "set_state", "to_material", "extract", "marks", "fixes",
                        "reacts", "risky", "flaws", "risk", "modes", "no_air_modes", "ordered", "tool_wear", "needs",
                        "fsm", "chain_in", "chain_out", "worth", "concentrate"},
                    where, W);
        p.worth = worthField(j, where, E);
        std::string op = j.value("op", "shape");
        if (op == "shape") p.op = ProcOp::Shape;
        else if (op == "heat") p.op = ProcOp::Heat;
        else if (op == "blend") p.op = ProcOp::Blend;
        else if (op == "join") p.op = ProcOp::Join;
        else if (op == "soak") p.op = ProcOp::Soak;
        else if (op == "inscribe") p.op = ProcOp::Inscribe;
        else if (op == "bind") p.op = ProcOp::Bind;
        else E.add(where + ": unknown op " + op);
        p.skill = j.value("skill", "");
        if (j.contains("requires")) {
            const Json& r = j["requires"];
            warnUnknown(r, {"tool", "station", "heat_min", "clean", "tool_hardness_ge"}, where + ".requires", W);
            p.tools = strs(r, "tool");
            p.stations = strs(r, "station");
            p.heatMin = r.value("heat_min", 0);
            p.clean = r.value("clean", false);
            p.toolHardness = exprJ(r, "tool_hardness_ge", where + ".requires", E);
        }
        for (const auto& sb : j.value("skill_by", Json::array()))
            p.skillBy.emplace_back(sb.value("tag", ""), sb.value("skill", ""));
        for (const auto& s : j.value("inputs", Json::array())) {
            SlotDef sl;
            sl.name = s.value("slot", "");
            warnUnknown(s, {"slot", "shape", "tag", "either", "p", "optional", "min", "max"}, where + ".inputs." + sl.name, W);
            sl.shapes = strs(s, "shape");
            sl.tags = strs(s, "tag");
            sl.either = s.value("either", false);
            sl.optional = s.value("optional", false);
            sl.min = s.value("min", sl.optional ? 0 : 1);
            sl.max = s.value("max", std::max(1, sl.min));
            if (s.contains("p"))
                for (auto it = s["p"].begin(); it != s["p"].end(); ++it) {
                    Constraint c;
                    std::string err;
                    if (!Constraint::parse(it.value().get<std::string>(), c, &err))
                        E.add(where + ".inputs." + sl.name + ".p." + it.key() + ": " + err);
                    sl.p[it.key()] = c;
                    sl.pSrc[it.key()] = it.value().get<std::string>();
                }
            p.slots.push_back(sl);
        }
        p.time = j.value("time", 1);
        if (j.contains("difficulty")) {
            p.diffBase = j["difficulty"].value("base", 0.3);
            p.diffAdd = exprJ(j["difficulty"], "add", where + ".difficulty", E);
        }
        if (j.contains("shape_map"))
            for (auto it = j["shape_map"].begin(); it != j["shape_map"].end(); ++it)
                p.shapeMap[it.key()] = it.value().get<std::vector<std::string>>();
        for (const auto& a : j.value("assemble", Json::array()))
            p.assemble.push_back({a.value("a", "any"), a.value("b", "any"), a.value("binder", false), a.value("to", "")});
        if (j.contains("transform"))
            for (auto it = j["transform"].begin(); it != j["transform"].end(); ++it)
                p.transform.emplace_back(it.key(), exprJ(j["transform"], it.key().c_str(), where + ".transform", E));
        p.setState = j.value("set_state", "");
        p.toMaterial = j.value("to_material", "");
        p.extract = j.value("extract", "");
        p.marks = strs(j, "marks");
        p.fixes = strs(j, "fixes");
        p.reacts = j.value("reacts", false);
        p.risky = j.value("risky", false);
        for (const auto& f : j.value("flaws", Json::array())) {
            FlawRule r;
            r.id = f.value("id", "");
            r.p = exprJ(f, "p", where + ".flaws." + r.id, E);
            r.when = exprJ(f, "when", where + ".flaws." + r.id, E);
            p.flaws.push_back(r);
        }
        for (const auto& r : j.value("risk", Json::array()))
            p.risks.push_back({r.value("id", ""), r.value("p", 0.0), strs(r, "eff")});
        p.modes = strs(j, "modes");
        p.noAirModes = strs(j, "no_air_modes");
        p.ordered = j.value("ordered", false);
        p.toolWear = j.value("tool_wear", 0.01);
        p.needs = strs(j, "needs");
        p.fsm = j.value("fsm", "Work");
        p.chainIn = strs(j, "chain_in");
        p.chainOut = strs(j, "chain_out");
        if (j.contains("concentrate") && j["concentrate"].is_object())
            for (auto it = j["concentrate"].begin(); it != j["concentrate"].end(); ++it) {
                if (!it.value().is_number() || it.value().get<double>() < 0)
                    E.add(where + ".concentrate." + it.key() + ": a number >= 0");
                else p.concentrate.push_back({it.key(), it.value().get<double>()});
            }
        if (p.id.empty()) E.add("processes: an entry without id");
        else if (!d.processes.emplace(p.id, p).second) E.add("processes: duplicate " + p.id);
        else d.processOrder.push_back(p.id);
    }

    // --- functions, blend, effect rules
    for (const auto& j : functions.value("functions", Json::array())) {
        FunctionDef f;
        f.id = j.value("id", "");
        f.f = exprJ(j, "f", "functions." + f.id, E);
        d.functions.push_back(f);
    }
    if (functions.contains("blend"))
        for (auto it = functions["blend"].begin(); it != functions["blend"].end(); ++it) {
            std::string rule = it.value().get<std::string>();
            if (rule != "avg_mass" && rule != "max" && rule != "min" && rule != "sum_cap" && rule != "product")
                E.add("functions.blend." + it.key() + ": unknown rule " + rule);
            d.blend[it.key()] = rule;
        }
    if (functions.contains("effect_rules")) {
        const Json& r = functions["effect_rules"];
        d.effStack = strs(r, "stack");
        d.effDominant = strs(r, "dominant");
        for (const auto& pr : r.value("cancel", Json::array()))
            if (pr.is_array() && pr.size() == 2)
                d.effCancel.emplace_back(pr[0].get<std::string>(), pr[1].get<std::string>());
    }

    // --- forms
    static const std::set<std::string> hardKeys{"shape", "mat", "state", "profile", "effects"};
    for (const auto& j : forms.value("forms", Json::array())) {
        FormDef f;
        f.id = j.value("id", "");
        const std::string where = "forms." + f.id;
        warnUnknown(j, {"id", "category", "needs", "derive", "named", "item", "worth"}, where, W);
        f.worth = worthField(j, where, E);
        f.category = j.value("category", "");
        f.named = j.value("named", true);
        f.item = j.value("item", "");
        if (j.contains("needs")) {
            const Json& n = j["needs"];
            f.shape = strs(n, "shape");
            f.mat = strs(n, "mat");
            f.state = strs(n, "state");
            f.profile = strs(n, "profile");
            f.effects = strs(n, "effects");
            for (auto it = n.begin(); it != n.end(); ++it) {
                if (hardKeys.count(it.key())) continue;
                FormNeed fn;
                fn.key = it.key();
                fn.expr = expr(it.key(), where + ".needs", E);
                std::string err;
                if (!it.value().is_string() || !Constraint::parse(it.value().get<std::string>(), fn.c, &err))
                    E.add(where + ".needs." + it.key() + ": " + (err.empty() ? "expected a constraint string" : err));
                f.needs.push_back(fn);
            }
        }
        if (j.contains("derive")) {
            const Json& dj = j["derive"];
            for (auto it = dj.begin(); it != dj.end(); ++it) {
                if (it.key() == "kind") f.kind = it.value().get<std::string>();
                else if (it.key() == "tags") f.deriveTags = it.value().get<std::vector<std::string>>();
                else if (it.key() == "weapon_id" || it.key() == "layer")
                    f.deriveStr[it.key()] = it.value().get<std::string>();
                else f.derive.emplace_back(it.key(), exprJ(dj, it.key().c_str(), where + ".derive", E));
            }
        }
        if (f.id.empty()) E.add("forms: an entry without id");
        else if (d.form(f.id)) E.add("forms: duplicate " + f.id);
        else d.forms.push_back(f);
    }
    for (const auto& j : forms.value("improvised", Json::array()))
        d.improvised.push_back({j.value("id", ""), j.value("for", "any")});

    // --- reacts
    for (const auto& j : reacts.value("reacts", Json::array())) {
        ReactDef r;
        r.id = j.value("id", "");
        const std::string where = "reacts." + r.id;
        warnUnknown(j, {"id", "when", "at_heat", "make", "replace", "p", "extract", "risk_tainted", "ratio", "time"}, where, W);
        for (const auto& w : strs(j, "when")) {
            size_t op = w.find_first_of("<>=!");
            if (op != std::string::npos && w.rfind("eff:", 0) != 0) {
                Constraint c;
                std::string err;
                if (!Constraint::parse(w.substr(op), c, &err)) E.add(where + ".when: " + err);
                r.propConds.emplace_back(w.substr(0, op), c);
            } else {
                r.when.push_back(w);
            }
        }
        r.atHeat = j.value("at_heat", 0);
        r.make = j.value("make", "");
        r.replace = j.value("replace", false);
        if (j.contains("p"))
            for (auto it = j["p"].begin(); it != j["p"].end(); ++it)
                r.p[it.key()] = exprJ(j["p"], it.key().c_str(), where + ".p", E);
        r.extract = j.value("extract", "");
        r.riskTainted = j.value("risk_tainted", 0.0);
        if (j.contains("ratio")) {
            std::istringstream is(j["ratio"].get<std::string>());
            std::string rng;
            is >> r.ratioMat >> rng;
            std::string err;
            if (!Constraint::parse(rng, r.ratio, &err)) E.add(where + ".ratio: " + err);
            r.hasRatio = true;
        }
        d.reacts.push_back(r);
    }

    // --- stations
    for (const auto& j : stations.value("stations", Json::array())) {
        StationDef s;
        s.id = j.value("id", "");
        warnUnknown(j, {"id", "tags", "temp_max", "quality_bonus", "even", "clean_default", "needs", "no_air"},
                    "stations." + s.id, W);
        s.tags = strs(j, "tags");
        s.tempMax = j.value("temp_max", -1);
        s.qualityBonus = j.value("quality_bonus", 0.0);
        s.even = j.value("even", 0.5);
        s.cleanDefault = j.value("clean_default", 1.0);
        s.needs = strs(j, "needs");
        s.noAir = j.value("no_air", false);
        if (!d.stations.emplace(s.id, s).second) E.add("stations: duplicate " + s.id);
    }

    // --- flaws
    for (const auto& j : flaws.value("flaws", Json::array())) {
        FlawDef f;
        f.id = j.value("id", "");
        f.visible = j.value("visible", true);
        f.fix = opt<std::string>(j, "fix", "");
        f.mods = numMap(j, "mods");
        f.salvage = opt<std::string>(j, "salvage", "");
        f.worth = worthField(j, "flaws." + f.id, E);
        if (!d.flaws.emplace(f.id, f).second) E.add("flaws: duplicate " + f.id);
    }

    // --- designs (starter chains, §7 / §8)
    for (const auto& j : designs.value("designs", Json::array())) {
        DesignDef ds;
        ds.id = j.value("id", "");
        ds.form = j.value("form", ds.id);
        ds.tools = strs(j, "tools");
        ds.stations = strs(j, "stations");
        for (const auto& s : j.value("steps", Json::array())) {
            DesignStep st;
            st.process = s.value("process", "");
            st.shape = s.value("shape", "");
            st.len = s.value("len", 0);
            st.mode = s.value("mode", "");
            st.heat = s.value("heat", -1);
            if (s.contains("slots"))
                for (auto it = s["slots"].begin(); it != s["slots"].end(); ++it) {
                    std::vector<DesignInput> ins;
                    auto one = [&](const Json& x) {
                        if (x.is_string()) ins.push_back({x.get<std::string>(), 1});
                        else if (x.is_object()) ins.push_back({x.value("ref", ""), x.value("n", 1)});
                    };
                    if (it.value().is_array())
                        for (const auto& x : it.value()) one(x);
                    else one(it.value());
                    st.slots[it.key()] = ins;
                }
            ds.steps.push_back(st);
        }
        d.designs.push_back(ds);
    }

    d.loaded = true;
    return d;
}

CraftData CraftData::load(const std::string& dir) {
    auto f = [&](const char* name) -> Json {
        data::InFile in(dir + "/" + name);
        if (!in) return Json::object();
        Json j;
        in >> j;
        return j;
    };
    data::InFile probe(dir + "/materials.json");
    if (!probe) return CraftData{};
    CraftData d = fromJson(f("materials.json"), f("shapes.json"), f("processes.json"), f("functions.json"),
                           f("forms.json"), f("flaws.json"), f("reacts.json"), f("stations.json"),
                           f("crafting_config.json"), f("designs.json"));
    // AL-117: the aspects of magic live in content/magic (dir is content/ai/craft)
    data::InFile am(dir + "/../../magic/aspects.json");
    if (am) {
        Json a;
        try {
            am >> a;
            loadAspects(d, a);
        } catch (const std::exception& e) {
            d.errors.push_back(std::string("magic/aspects.json: ") + e.what());
        }
    }
    return d;
}

void loadAspects(CraftData& d, const Json& aspects) {
    d.aspects.clear();
    for (const auto& j : aspects.value("aspects", Json::array())) {
        AspectInfo a;
        a.id = j.value("id", "");
        if (a.id.empty()) continue;
        a.opposes = j.value("opposes", std::vector<std::string>{});
        a.enabled = j.value("enabled", true);
        if (j.contains("body") && j["body"].is_object())
            for (auto v = j["body"].begin(); v != j["body"].end(); ++v)
                if (v.value().is_object())
                    for (auto f = v.value().begin(); f != v.value().end(); ++f)
                        if (f.value().is_string()) a.body[v.key()][f.key()] = f.value().get<std::string>();
        d.aspects[a.id] = a;
    }
}

const AspectInfo* CraftData::aspect(const std::string& id) const { return findIn(aspects, id); }

std::vector<std::string> craftLocKeys(const CraftData& d) {
    std::vector<std::string> k;
    for (const auto& [id, m] : d.materials) {
        k.push_back("material." + id + ".name");
        k.push_back("material." + id + ".made");
    }
    for (const auto& [id, s] : d.shapes) k.push_back("shape." + id + ".name");
    for (const auto& id : d.processOrder) {
        k.push_back("process." + id + ".name");
        for (const auto& m : d.processes.at(id).modes) k.push_back("process.mode." + m + ".name");
    }
    for (const auto& f : d.forms) k.push_back("form." + f.id + ".name");
    for (const auto& f : d.improvised) k.push_back("form." + f.id + ".name");
    for (const auto& [id, f] : d.flaws) k.push_back("flaw." + id + ".name");
    for (const auto& [id, s] : d.stations) k.push_back("station." + id + ".name");
    for (const char* t : {"crude", "common", "good", "masterwork"}) k.push_back(std::string("craft.tier.") + t);
    for (const char* o : {"catastrophe", "ruined", "flawed", "ok", "good", "masterwork"})
        k.push_back(std::string("craft.outcome.") + o);
    for (const char* r : {"unknown_process", "missing_input", "too_many", "wrong_input", "unknown_slot", "no_tool",
                          "tool_too_soft", "no_station", "station_needs", "too_cold", "bad_mode", "bad_target",
                          "too_many_parts", "carrier_cannot_hold", "no_rune", "no_aspect"})
        k.push_back(std::string("craft.reason.") + r);
    for (const char* s : {"missing", "cannot_start", "ruined", "hurt", "too_long", "unknown_step", "done"})
        k.push_back(std::string("craft.stop.") + s);
    for (const char* s : {"craft.unknown", "craft.likely", "craft.hidden_flaw", "craft.name.raw"}) k.push_back(s);
    std::sort(k.begin(), k.end());
    k.erase(std::unique(k.begin(), k.end()), k.end());
    return k;
}

} // namespace lr::sim::craft
