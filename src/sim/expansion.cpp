// Content expansion (content/ai/expansion): merge into the base JSON and check its reference books.
// The base files win on a shared id (they carry the working mechanics); the expansion adds what is
// missing, annotates base knowledge (category, attrs) and keeps interests / attributes / temperaments /
// calendar / social position as data. See content/ai/expansion/README.md.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "sim/content.hpp"

namespace altera::sim {
namespace {

Json readOpt(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Json::object();
    try {
        return Json::parse(f);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

std::vector<std::string> strings(const Json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& v : j)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::map<std::string, double> numbers(const Json& j) {
    std::map<std::string, double> out;
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_number() && (k.empty() || k[0] != '_')) out[k] = v.get<double>();
    return out;
}

std::map<std::string, std::pair<double, double>> ranges(const Json& j) {
    std::map<std::string, std::pair<double, double>> out;
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
                out[k] = {v[0].get<double>(), v[1].get<double>()};
    return out;
}

// Appends overlay[key] entries whose id is not in base[key]. On a shared id, fills only the fields
// the base entry lacks when fillMissing (knowledge: category, attrs), otherwise keeps the base entry.
void mergeList(Json& base, const Json& overlay, const char* key, const char* what, ExpansionData& x,
               bool fillMissing = false) {
    if (!overlay.is_object() || !overlay.contains(key)) return;
    if (!base.is_object()) base = Json::object();
    Json& list = base[key];
    if (!list.is_array()) list = Json::array();
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].contains("id")) at[list[i]["id"].get<std::string>()] = i;
    for (const auto& e : overlay[key]) {
        if (!e.is_object() || !e.contains("id")) {
            x.errors.push_back(std::string("expansion ") + what + ": entry without id");
            continue;
        }
        const std::string id = e["id"].get<std::string>();
        auto it = at.find(id);
        if (it == at.end()) {
            at[id] = list.size();
            list.push_back(e);
            list.back()["_part"] = "expansion";
            ++x.added;
            continue;
        }
        x.kept.push_back(std::string(what) + ":" + id);
        if (fillMissing)
            for (const auto& [k, v] : e.items())
                if (!list[it->second].contains(k)) list[it->second][k] = v;
    }
}

} // namespace

ExpansionData mergeExpansion(const std::string& dir, Json& items, Json& objects, Json& needs, Json& traits,
                             Json& htn, Json& interactions, Json& config, Json& knowledge) {
    ExpansionData x;
    if (!std::filesystem::is_directory(dir)) return x;
    x.loaded = true;
    auto f = [&](const char* name) { return readOpt(dir + "/" + name); };

    const Json xi = f("items.json"), xo = f("objects.json"), xn = f("needs.json"), xt = f("traits.json"),
               xh = f("htn.json"), xa = f("interactions.json"), xc = f("config.json"), xk = f("knowledge.json");
    mergeList(items, xi, "items", "item", x);
    mergeList(objects, xo, "objects", "object", x);
    mergeList(needs, xn, "moodlets", "moodlet", x);
    mergeList(interactions, xa, "interactions", "interaction", x);
    mergeList(knowledge, xk, "knowledge", "knowledge", x, true);

    // Traits: new ones and the extended fields of every expansion trait.
    mergeList(traits, xt, "traits", "trait", x);
    for (const auto& t : xt.value("traits", Json::array())) {
        Json ext = Json::object();
        for (const auto& [k, v] : t.items())
            if (k != "id" && k != "goal_mult" && k != "need_mult" && k != "legal_penalty" && k != "method_bias" &&
                k != "hysteresis_mult" && k != "inherit" && k != "weight_in_pool" && k[0] != '_')
                ext[k] = v;
        x.traitExt[t.at("id").get<std::string>()] = ext;
    }
    if (xt.contains("exclusive")) {
        Json& ex = traits["exclusive"];
        if (!ex.is_array()) ex = Json::array();
        std::set<std::pair<std::string, std::string>> have;
        for (const auto& p : ex) have.insert({p.at(0).get<std::string>(), p.at(1).get<std::string>()});
        for (const auto& p : xt["exclusive"]) {
            std::pair<std::string, std::string> pr{p.at(0).get<std::string>(), p.at(1).get<std::string>()};
            if (!have.count(pr) && !have.count({pr.second, pr.first})) ex.push_back(p), have.insert(pr);
        }
    }

    // Knowledge: annotations of base skills, categories, tiers and the growth model.
    const Json annotations = xk.value("annotations", Json::object());
    for (const auto& [id, ann] : annotations.items()) {
        bool found = false;
        for (auto& e : knowledge["knowledge"])
            if (e.value("id", std::string()) == id) {
                for (const auto& [k, v] : ann.items())
                    if (!e.contains(k)) e[k] = v;
                found = true;
            }
        if (!found) x.errors.push_back("expansion knowledge: annotation for unknown skill '" + id + "'");
    }
    // The parser knows only the v0.2 fields: the extended ones move to knowledgeExt / traitExt.
    for (auto& e : knowledge["knowledge"]) {
        Json ext = Json::object();
        for (const char* k : {"category", "attrs"})
            if (e.contains(k)) ext[k] = e[k], e.erase(k);
        if (!ext.empty()) x.knowledgeExt[e.at("id").get<std::string>()] = ext;
    }
    for (auto& e : traits["traits"]) {
        auto it = x.traitExt.find(e.value("id", std::string()));
        if (it != x.traitExt.end())
            for (const auto& [k, v] : it->second.items()) e.erase(k);
    }
    x.skillCategories = strings(xk.value("categories", Json::array()));
    x.skillModel = xk.value("skill_model", Json::object());
    if (xk.contains("tiers")) x.skillModel["tiers"] = xk["tiers"];

    // Config: new memory types.
    if (xc.contains("memory_types")) {
        Json& mt = config["memory_types"];
        if (!mt.is_object()) mt = Json::object();
        for (const auto& [k, v] : xc["memory_types"].items()) {
            if (mt.contains(k)) x.kept.push_back("memory_type:" + k);
            else mt[k] = v, ++x.added;
        }
    }

    // HTN: methods added to existing tasks.
    const Json addMethods = xh.value("add_methods", Json::object());
    for (const auto& [task, methods] : addMethods.items()) {
        Json* tj = nullptr;
        for (auto& t : htn["tasks"])
            if (t.value("id", std::string()) == task) tj = &t;
        if (!tj) {
            x.errors.push_back("expansion htn: add_methods to unknown task '" + task + "'");
            continue;
        }
        Json& ms = (*tj)["methods"];
        std::set<std::string> have;
        for (const auto& m : ms) have.insert(m.value("id", std::string()));
        for (const auto& m : methods) {
            const std::string id = m.value("id", std::string());
            if (have.count(id)) x.kept.push_back("method:" + id);
            else ms.push_back(m), have.insert(id), ++x.added;
        }
    }

    // Reference books.
    const Json attrs = f("attributes.json");
    x.attributes = strings(attrs.value("attributes", Json::array()));
    x.attributeModel = attrs.value("model", Json::object());
    const Json ints = f("interests.json");
    x.interestCategories = strings(ints.value("categories", Json::array()));
    x.interestModel = ints.value("model", Json::object());
    for (const auto& e : ints.value("interests", Json::array())) {
        InterestDef d;
        d.id = e.at("id").get<std::string>();
        d.category = e.value("category", std::string());
        d.skills = strings(e.value("skills", Json::array()));
        d.interactions = strings(e.value("interactions", Json::array()));
        d.opposes = strings(e.value("opposes", Json::array()));
        d.goalMult = numbers(e.value("goal_mult", Json::object()));
        d.methodBias = numbers(e.value("method_bias", Json::object()));
        d.weightInPool = e.value("weight_in_pool", 1.0);
        if (!x.interests.emplace(d.id, d).second) x.errors.push_back("expansion interest: duplicate id '" + d.id + "'");
    }
    const Json temps = f("temperaments.json");
    x.temperamentModel = temps.value("model", Json::object());
    for (const auto& e : temps.value("temperaments", Json::array())) {
        TemperamentDef d;
        d.id = e.at("id").get<std::string>();
        d.weight = e.value("weight", 1.0);
        d.core = ranges(e.value("core", Json::object()));
        d.attrs = ranges(e.value("attrs", Json::object()));
        d.traits = numbers(e.value("traits", Json::object()));
        d.interestCategories = numbers(e.value("interests", Json::object()));
        d.speech = e.value("register", std::string());
        if (!x.temperaments.emplace(d.id, d).second)
            x.errors.push_back("expansion temperament: duplicate id '" + d.id + "'");
    }
    x.calendar = f("calendar.json");
    x.socialPosition = f("social_position.json");
    return x;
}

void validateExpansion(const Content& c, ValidationReport& r) {
    const ExpansionData& x = c.expansion;
    if (!x.loaded) return;
    for (const auto& e : x.errors) r.errors.push_back(e);
    auto err = [&](const std::string& s) { r.errors.push_back("expansion " + s); };
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    std::set<std::string> methods;
    for (const auto& [tid, t] : c.tasks)
        for (const auto& m : t.methods) methods.insert(m.id);

    for (const auto& [id, k] : x.knowledgeExt) {
        if (k.contains("category") && !x.skillCategories.empty() && !has(x.skillCategories, k["category"].get<std::string>()))
            err("knowledge " + id + ": unknown category '" + k["category"].get<std::string>() + "'");
        if (k.contains("attrs"))
            for (const auto& [slot, a] : k["attrs"].items())
                if (a.is_string() && !has(x.attributes, a.get<std::string>()))
                    err("knowledge " + id + ": attrs." + slot + " names unknown attribute '" + a.get<std::string>() + "'");
    }
    for (const auto& [id, t] : x.traitExt) {
        for (const auto& [k, v] : numbers(t.value("skill_mult", Json::object())))
            if (k != "*" && !c.knowledge.count(k)) err("trait " + id + ": skill_mult names unknown skill '" + k + "'");
        for (const auto& [k, v] : numbers(t.value("attr_mod", Json::object()))) {
            if (!has(x.attributes, k)) err("trait " + id + ": attr_mod names unknown attribute '" + k + "'");
            if (v < -0.5 || v > 0.5) err("trait " + id + ": attr_mod." + k + " out of [-0.5,0.5]");
        }
        for (const auto& [k, v] : numbers(t.value("interest_seeds", Json::object())))
            if (!x.interests.count(k)) err("trait " + id + ": interest_seeds names unknown interest '" + k + "'");
    }
    for (const auto& [id, d] : x.interests) {
        if (!has(x.interestCategories, d.category)) err("interest " + id + ": unknown category '" + d.category + "'");
        for (const auto& s : d.skills)
            if (!c.knowledge.count(s)) err("interest " + id + ": unknown skill '" + s + "'");
        for (const auto& a : d.interactions)
            if (!c.interactions.count(a)) err("interest " + id + ": unknown interaction '" + a + "'");
        for (const auto& o : d.opposes)
            if (!x.interests.count(o)) err("interest " + id + ": opposes unknown interest '" + o + "'");
        for (const auto& [g, m] : d.goalMult)
            if (!c.goal(g)) err("interest " + id + ": goal_mult names unknown goal '" + g + "'");
        for (const auto& [m, b] : d.methodBias)
            if (!methods.count(m)) err("interest " + id + ": method_bias names unknown method '" + m + "'");
    }
    for (const auto& [id, d] : x.temperaments) {
        for (const auto& [t, p] : d.traits) {
            if (!c.traits.count(t)) err("temperament " + id + ": unknown trait '" + t + "'");
            if (p < 0.0 || p > 1.0) err("temperament " + id + ": trait chance " + t + " out of [0,1]");
        }
        for (const auto& [cat, w] : d.interestCategories)
            if (!has(x.interestCategories, cat)) err("temperament " + id + ": unknown interest category '" + cat + "'");
        for (const auto& [a, rg] : d.attrs)
            if (!has(x.attributes, a)) err("temperament " + id + ": unknown attribute '" + a + "'");
        for (const auto& [axis, rg] : d.core)
            if (rg.first > rg.second || rg.first < -1.0 || rg.second > 1.0) // moral axes are -1..1
                err("temperament " + id + ": core." + axis + " range bad");
    }
}

} // namespace altera::sim
