#include "sim/templates.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

#include "sim/data_fs.hpp"

namespace lr::sim {

namespace {

const std::set<std::string>& templateKeys() {
    static const std::set<std::string> k{"extends", "use", "args", "params", "defaults", "abstract"};
    return k;
}

bool identChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

bool bindString(const std::string& s, const std::map<std::string, Json>& args, Json& out, std::string* why) {
    if (s.find('$') == std::string::npos) {
        out = s;
        return true;
    }
    // exactly "$name": the argument's own JSON value
    if (s.size() > 1 && s[0] == '$') {
        bool whole = true;
        for (size_t i = 1; i < s.size(); ++i)
            if (!identChar(s[i])) whole = false;
        if (whole) {
            auto it = args.find(s.substr(1));
            if (it == args.end()) {
                if (why) *why = "unbound parameter '" + s + "'";
                return false;
            }
            out = it->second;
            return true;
        }
    }
    std::string r;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '$' || i + 1 >= s.size() || !identChar(s[i + 1])) {
            r += s[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && identChar(s[j])) ++j;
        const std::string name = s.substr(i + 1, j - i - 1);
        auto it = args.find(name);
        if (it == args.end()) {
            if (why) *why = "unbound parameter '$" + name + "' in \"" + s + "\"";
            return false;
        }
        r += it->second.is_string() ? it->second.get<std::string>() : it->second.dump();
        i = j;
    }
    out = r;
    return true;
}

} // namespace

void templateMerge(Json& base, const Json& over) {
    if (!(base.is_object() && over.is_object())) {
        base = over;
        return;
    }
    for (const auto& [k, v] : over.items()) {
        if (k.size() > 1 && k[0] == '+') {
            const std::string key = k.substr(1);
            Json& dst = base[key];
            if (!dst.is_array()) dst = dst.is_null() ? Json::array() : Json::array({dst});
            if (v.is_array())
                for (const auto& e : v) dst.push_back(e);
            else
                dst.push_back(v);
            continue;
        }
        auto it = base.find(k);
        if (it != base.end() && it->is_object() && v.is_object()) templateMerge(*it, v);
        else base[k] = v;
    }
}

bool bindTemplateParams(Json& j, const std::map<std::string, Json>& args, std::string* why) {
    if (j.is_string()) {
        Json out;
        if (!bindString(j.get<std::string>(), args, out, why)) return false;
        j = std::move(out);
        return true;
    }
    if (j.is_array()) {
        for (auto& e : j)
            if (!bindTemplateParams(e, args, why)) return false;
        return true;
    }
    if (j.is_object()) {
        Json out = Json::object();
        for (auto& [k, v] : j.items()) {
            Json key;
            if (!bindString(k, args, key, why)) return false;
            Json val = v;
            if (!bindTemplateParams(val, args, why)) return false;
            out[key.is_string() ? key.get<std::string>() : key.dump()] = std::move(val);
        }
        j = std::move(out);
    }
    return true;
}

void TemplateBook::add(const Json& entries, const std::string& file, std::vector<std::string>* errors) {
    const Json list = entries.is_array() ? entries : entries.value("templates", Json::array());
    for (const auto& e : list) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string()) {
            if (errors) errors->push_back(file + ": a template needs a string id");
            continue;
        }
        byId[e["id"].get<std::string>()] = e;
    }
}

TemplateBook TemplateBook::load(const std::string& dir, std::vector<std::string>* errors) {
    TemplateBook b;
    if (!data::isDir(dir)) return b;
    for (const auto& n : data::listFiles(dir, ".json")) {
        try {
            b.add(data::readJson(dir + "/" + n), "templates/" + n, errors);
        } catch (const std::exception& e) {
            if (errors) errors->push_back(e.what());
        }
    }
    return b;
}

void expandTemplates(Json& list, const TemplateBook& book, const std::string& what, std::vector<std::string>* errors) {
    if (!list.is_array()) return;
    bool any = false;
    for (const auto& e : list)
        if (e.is_object() && (e.contains("extends") || e.contains("use") || e.value("abstract", false))) any = true;
    if (!any) return;
    std::map<std::string, const Json*> local;
    for (const auto& e : list)
        if (e.is_object() && e.contains("id") && e["id"].is_string()) local[e["id"].get<std::string>()] = &e;
    auto find = [&](const std::string& id) -> const Json* {
        if (auto it = local.find(id); it != local.end()) return it->second;
        if (auto it = book.byId.find(id); it != book.byId.end()) return &it->second;
        return nullptr;
    };
    std::map<std::string, Json> done;
    std::set<std::string> onStack;
    auto fail = [&](const std::string& m) {
        if (errors) errors->push_back(what + ": " + m);
    };
    // The entry merged over its parent and mixins, parameters not yet bound.
    std::function<bool(const Json&, Json&)> resolve = [&](const Json& e, Json& out) -> bool {
        const std::string id = e.value("id", std::string());
        if (auto it = done.find(id); !id.empty() && it != done.end() && &e == find(id)) {
            out = it->second;
            return true;
        }
        if (!id.empty() && !onStack.insert(id).second) {
            fail("template cycle through '" + id + "'");
            return false;
        }
        Json acc = Json::object();
        auto pull = [&](const std::string& pid) -> bool {
            const Json* p = find(pid);
            if (!p) {
                fail("'" + id + "' uses unknown template '" + pid + "'");
                return false;
            }
            Json pr;
            if (!resolve(*p, pr)) return false;
            // the parent's own params stay with it: they are bound below with the entry's args
            templateMerge(acc, pr);
            return true;
        };
        bool ok = true;
        if (e.contains("extends")) {
            if (e["extends"].is_string()) ok = pull(e["extends"].get<std::string>()) && ok;
            else fail("'" + id + "': extends must be one id"), ok = false;
        }
        for (const auto& u : e.value("use", Json::array())) {
            if (u.is_string()) ok = pull(u.get<std::string>()) && ok;
            else fail("'" + id + "': use takes ids"), ok = false;
        }
        Json own = e;
        own.erase("extends");
        own.erase("use");
        // a child is never abstract because its parent was
        acc.erase("abstract");
        // params and defaults accumulate (the child's win)
        Json params = acc.value("params", Json::array());
        for (const auto& p : own.value("params", Json::array()))
            if (std::find(params.begin(), params.end(), p) == params.end()) params.push_back(p);
        Json defaults = acc.value("defaults", Json::object());
        templateMerge(defaults, own.value("defaults", Json::object()));
        templateMerge(acc, own);
        if (!params.empty()) acc["params"] = params;
        if (!defaults.empty()) acc["defaults"] = defaults;
        if (!id.empty()) {
            onStack.erase(id);
            if (&e == find(id)) done[id] = acc;
        }
        out = std::move(acc);
        return ok;
    };
    Json result = Json::array();
    for (const auto& e : list) {
        if (!e.is_object()) {
            result.push_back(e);
            continue;
        }
        if (e.value("abstract", false)) continue;
        if (!e.contains("extends") && !e.contains("use")) {
            result.push_back(e);
            continue;
        }
        Json r;
        if (!resolve(e, r)) continue;
        std::map<std::string, Json> args;
        const Json defs = r.value("defaults", Json::object()), given = e.value("args", Json::object()); // locals: items() of a temporary dangles
        for (const auto& [k, v] : defs.items()) args[k] = v;
        for (const auto& [k, v] : given.items()) args[k] = v;
        for (const auto& p : r.value("params", Json::array()))
            if (p.is_string() && !args.count(p.get<std::string>()))
                fail("'" + e.value("id", std::string()) + "': no value for parameter '" + p.get<std::string>() + "'");
        for (const auto& k : templateKeys()) r.erase(k);
        std::string why;
        if (!args.empty() && !bindTemplateParams(r, args, &why)) {
            fail("'" + e.value("id", std::string()) + "': " + why);
            continue;
        }
        result.push_back(std::move(r));
    }
    list = std::move(result);
}

} // namespace lr::sim
