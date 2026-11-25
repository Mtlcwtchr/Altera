#include "sim/data_fs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lr::sim::data {

namespace fs = std::filesystem;

namespace {

struct State {
    std::mutex mu;
    std::string root;                 // absolute, normalised, no trailing slash
    std::vector<std::string> layers;  // lowest first
    std::uint64_t gen = 0;
    // watched: real path -> stamp (mtime ticks; -1 = absent). Dirs: a hash of their listing.
    std::map<std::string, long long> files;
    std::map<std::string, std::string> dirs;
    std::vector<std::pair<int, std::function<void()>>> listeners;
    int nextId = 1;
};

State& st() {
    static State s;
    return s;
}

std::string norm(const std::string& p) {
    if (p.empty()) return p;
    std::error_code ec;
    fs::path a = fs::absolute(fs::path(p), ec);
    if (ec) a = fs::path(p);
    std::string s = a.lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

long long stampOf(const std::string& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return -1;
    auto t = fs::last_write_time(p, ec);
    if (ec) return -1;
    return static_cast<long long>(t.time_since_epoch().count()) ^ static_cast<long long>(fs::file_size(p, ec));
}

// The part of `p` under the root ("ai/items.json"), or false when p is not under it.
bool relOf(const State& s, const std::string& p, std::string& rel) {
    if (s.root.empty()) return false;
    const std::string n = norm(p);
    if (n == s.root) {
        rel.clear();
        return true;
    }
    if (n.size() > s.root.size() && n.compare(0, s.root.size(), s.root) == 0 && n[s.root.size()] == '/') {
        rel = n.substr(s.root.size() + 1);
        return true;
    }
    return false;
}

// Every real file a content path stands for, lowest layer first (only existing ones).
std::vector<std::string> stackOf(State& s, const std::string& p, bool watch) {
    std::vector<std::string> out;
    std::string rel;
    if (!relOf(s, p, rel)) {
        const std::string n = norm(p);
        if (watch) s.files[n] = stampOf(n);
        std::error_code ec;
        if (fs::is_regular_file(n, ec)) out.push_back(n);
        return out;
    }
    auto consider = [&](const std::string& dir) {
        const std::string real = rel.empty() ? dir : dir + "/" + rel;
        const long long stamp = stampOf(real);
        if (watch) s.files[real] = stamp;
        if (stamp != -1) out.push_back(real);
    };
    consider(s.root);
    for (const auto& l : s.layers) consider(l);
    return out;
}

std::string slurp(const std::string& real) {
    std::ifstream f(real, std::ios::binary);
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

Json parseFile(const std::string& real) {
    std::ifstream f(real, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + real);
    try {
        return Json::parse(f, nullptr, true, true); // comments allowed
    } catch (const std::exception& e) {
        throw std::runtime_error(real + ": " + e.what());
    }
}

bool isIdArray(const Json& a) {
    if (!a.is_array() || a.empty()) return false;
    for (const auto& e : a)
        if (!e.is_object() || !e.contains("id")) return false;
    return true;
}

std::string listingHash(const std::string& real) {
    std::error_code ec;
    if (!fs::is_directory(real, ec)) return "-";
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(real, ec)) names.push_back(e.path().filename().generic_string());
    std::sort(names.begin(), names.end());
    std::string h;
    for (const auto& n : names) h += n + "\n";
    return h;
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ';') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else
            cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace

void mergeJson(Json& base, const Json& over) {
    if (base.is_object() && over.is_object()) {
        for (const auto& [k, v] : over.items()) {
            if (k == "_merge") continue;
            auto it = base.find(k);
            if (it == base.end()) base[k] = v;
            else mergeJson(*it, v);
        }
        return;
    }
    if (isIdArray(base) && over.is_array()) {
        bool allId = true;
        for (const auto& e : over)
            if (!e.is_object() || !e.contains("id")) allId = false;
        if (allId) {
            for (const auto& e : over) {
                auto same = std::find_if(base.begin(), base.end(), [&](const Json& b) { return b["id"] == e["id"]; });
                if (e.value("_remove", false)) {
                    if (same != base.end()) base.erase(same);
                } else if (same != base.end())
                    mergeJson(*same, e);
                else
                    base.push_back(e);
            }
            return;
        }
    }
    base = over;
}

std::string findContentRoot(const std::string& exePath, const std::string& fallback) {
    auto ok = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p / "ai", ec);
    };
    if (const char* env = std::getenv("LR_DATA"); env && *env) {
        fs::path p(env);
        if (ok(p / "content")) return norm((p / "content").string());
        if (ok(p)) return norm(p.string());
    }
    if (!exePath.empty()) {
        std::error_code ec;
        fs::path dir = fs::absolute(fs::path(exePath), ec).parent_path();
        if (ok(dir / "data" / "content")) return norm((dir / "data" / "content").string());
        if (ok(dir / "content")) return norm((dir / "content").string());
        // a build tree inside the repo (cmake-build-*/, build/...): content/ a few levels up
        fs::path up = dir;
        for (int i = 0; i < 4 && up.has_parent_path() && up.parent_path() != up; ++i) {
            up = up.parent_path();
            if (ok(up / "content")) return norm((up / "content").string());
        }
    }
    return norm(fallback);
}

std::vector<std::string> discoverMods(const std::vector<std::string>& modRoots) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& r : modRoots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(r, ec))
            if (e.is_directory(ec)) names.push_back(e.path().filename().generic_string());
        std::sort(names.begin(), names.end());
        std::vector<std::string> order;
        const fs::path lo = fs::path(r) / "load_order.json";
        if (fs::is_regular_file(lo, ec)) {
            try {
                const Json j = parseFile(lo.string());
                const Json list = j.is_array() ? j : j.value("mods", Json::array());
                for (const auto& n : list)
                    if (n.is_string() && std::find(names.begin(), names.end(), n.get<std::string>()) != names.end())
                        order.push_back(n.get<std::string>());
                // a mod listed as {"off": [...]} stays out; the unlisted follow by name
                const Json off = j.is_object() ? j.value("off", Json::array()) : Json::array();
                for (const auto& n : names)
                    if (std::find(order.begin(), order.end(), n) == order.end() &&
                        std::find(off.begin(), off.end(), Json(n)) == off.end())
                        order.push_back(n);
            } catch (...) {
                order = names;
            }
        } else
            order = names;
        for (const auto& n : order) {
            if (!n.empty() && (n[0] == '.' || n[0] == '_')) continue; // _disabled, .git
            const std::string p = norm((fs::path(r) / n).string());
            if (seen.insert(p).second) out.push_back(p);
        }
    }
    return out;
}

void mount(const std::string& contentRoot, const std::vector<std::string>& layers) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.root = norm(contentRoot);
    s.layers.clear();
    for (const auto& l : layers) {
        const std::string n = norm(l);
        if (n != s.root) s.layers.push_back(n);
    }
    s.files.clear();
    s.dirs.clear();
}

std::string mountDefault(const std::string& exePath, const std::string& fallback) {
    const std::string root = findContentRoot(exePath, fallback);
    std::vector<std::string> modRoots = {norm((fs::path(root).parent_path() / "mods").string())};
    if (!exePath.empty()) {
        std::error_code ec;
        modRoots.push_back(norm((fs::absolute(fs::path(exePath), ec).parent_path() / "mods").string()));
    }
    std::vector<std::string> layers = discoverMods(modRoots);
    if (const char* env = std::getenv("LR_MODS"); env && *env)
        for (const auto& p : splitList(env)) layers.push_back(p);
    mount(root, layers);
    return root;
}

const std::string& contentRoot() { return st().root; }
const std::vector<std::string>& layers() { return st().layers; }

std::string resolve(const std::string& path) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    auto stack = stackOf(s, path, false);
    return stack.empty() ? path : stack.back();
}

bool exists(const std::string& path) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return !stackOf(s, path, true).empty();
}

bool isDir(const std::string& path) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::string rel;
    std::error_code ec;
    if (!relOf(s, path, rel)) return fs::is_directory(path, ec);
    if (fs::is_directory(rel.empty() ? s.root : s.root + "/" + rel, ec)) return true;
    for (const auto& l : s.layers)
        if (fs::is_directory(rel.empty() ? l : l + "/" + rel, ec)) return true;
    return false;
}

bool readText(const std::string& path, std::string& out) {
    std::vector<std::string> stack;
    {
        State& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        stack = stackOf(s, path, true);
    }
    if (stack.empty()) return false;
    out = slurp(stack.back());
    return true;
}

Json readJson(const std::string& path) {
    std::vector<std::string> stack;
    {
        State& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        stack = stackOf(s, path, true);
    }
    if (stack.empty()) throw std::runtime_error("cannot open " + path);
    // from the top down to the last layer that replaces; then merge upwards
    size_t from = stack.size() - 1;
    std::vector<Json> parsed(stack.size());
    for (size_t k = stack.size(); k-- > 0;) {
        parsed[k] = parseFile(stack[k]);
        from = k;
        if (!(parsed[k].is_object() && parsed[k].value("_merge", false))) break;
    }
    Json out = parsed[from];
    if (out.is_object()) out.erase("_merge");
    for (size_t k = from + 1; k < stack.size(); ++k) mergeJson(out, parsed[k]);
    return out;
}

Json readJsonOr(const std::string& path, Json dflt) {
    if (!exists(path)) return dflt;
    return readJson(path);
}

std::vector<std::string> listFiles(const std::string& dir, const std::string& ext) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<std::string> reals;
    std::string rel;
    if (relOf(s, dir, rel)) {
        reals.push_back(rel.empty() ? s.root : s.root + "/" + rel);
        for (const auto& l : s.layers) reals.push_back(rel.empty() ? l : l + "/" + rel);
    } else
        reals.push_back(norm(dir));
    std::set<std::string> names;
    for (const auto& r : reals) {
        s.dirs[r] = listingHash(r);
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (const auto& e : fs::directory_iterator(r, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string n = e.path().filename().generic_string();
            if (!ext.empty() && (n.size() < ext.size() || n.compare(n.size() - ext.size(), ext.size(), ext) != 0)) continue;
            names.insert(n);
        }
    }
    return {names.begin(), names.end()};
}

std::vector<std::string> listDirs(const std::string& dir) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<std::string> reals;
    std::string rel;
    if (relOf(s, dir, rel)) {
        reals.push_back(rel.empty() ? s.root : s.root + "/" + rel);
        for (const auto& l : s.layers) reals.push_back(rel.empty() ? l : l + "/" + rel);
    } else
        reals.push_back(norm(dir));
    std::set<std::string> names;
    for (const auto& r : reals) {
        s.dirs[r] = listingHash(r);
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (const auto& e : fs::directory_iterator(r, ec))
            if (e.is_directory(ec)) names.insert(e.path().filename().generic_string());
    }
    return {names.begin(), names.end()};
}

std::vector<std::string> changed() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::vector<std::string> out;
    for (auto& [p, stamp] : s.files) {
        const long long now = stampOf(p);
        if (now != stamp) {
            out.push_back(p);
            stamp = now;
        }
    }
    for (auto& [p, h] : s.dirs) {
        const std::string now = listingHash(p);
        if (now != h) {
            out.push_back(p + "/");
            h = now;
        }
    }
    return out;
}

std::uint64_t generation() { return st().gen; }

void reload() {
    std::vector<std::function<void()>> calls;
    {
        State& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        ++s.gen;
        for (const auto& [id, fn] : s.listeners) calls.push_back(fn);
    }
    for (auto& fn : calls) fn();
}

void restoreGeneration(std::uint64_t gen) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.gen = gen;
}

std::string cacheKey(const std::string& dir) { return dir + "#" + std::to_string(st().gen); }

int onReload(std::function<void()> fn) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    const int id = s.nextId++;
    s.listeners.emplace_back(id, std::move(fn));
    return id;
}

InFile::InFile(const std::filesystem::path& p, std::ios::openmode) {
    const std::string s = p.string();
    if (!exists(s)) {
        setstate(std::ios::failbit);
        return;
    }
    try {
        str(readJson(s).dump());
    } catch (const std::exception& e) {
        error_ = e.what();
        std::fprintf(stderr, "data: %s\n", error_.c_str());
        setstate(std::ios::failbit);
    }
}

} // namespace lr::sim::data
