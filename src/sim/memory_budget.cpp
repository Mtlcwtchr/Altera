#include "sim/memory_budget.hpp"
#include "sim/data_fs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>

#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/world.hpp"

namespace lr::sim {

int MemoryBudget::Pool::per(int people) const {
    const int n = std::max(1, people);
    const int share = total > 0 ? total / n : perMax;
    return std::max(perMin, std::min(perMax, share));
}

namespace {

MemoryBudget::Pool poolFrom(const Json& j, const char* key, MemoryBudget::Pool def) {
    if (!j.contains(key) || !j[key].is_object()) return def;
    const Json& p = j[key];
    def.total = p.value("total", def.total);
    def.perMin = p.value("per_min", def.perMin);
    def.perMax = std::max(def.perMin, p.value("per_max", def.perMax));
    return def;
}

} // namespace

MemoryBudget MemoryBudget::fromJson(const Json& j) {
    MemoryBudget b;
    b.memories = poolFrom(j, "memories", b.memories);
    b.recipes = poolFrom(j, "recipes", b.recipes);
    b.tried = poolFrom(j, "tried", b.tried);
    b.said = poolFrom(j, "said", b.said);
    b.saidKeepDays = j.value("said_keep_days", b.saidKeepDays);
    b.maxRecipeSteps = j.value("max_recipe_steps", b.maxRecipeSteps);
    return b;
}

const MemoryBudget& memoryBudget(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<MemoryBudget>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return *it->second;
    auto b = std::make_unique<MemoryBudget>();
    if (!c.dir.empty()) {
        data::InFile f(std::filesystem::path(c.dir) / "memory_budget.json");
        if (f) {
            try {
                *b = MemoryBudget::fromJson(Json::parse(f));
            } catch (const std::exception&) {
                // a broken file keeps the defaults; validate_content reports it
            }
        }
    }
    auto* out = b.get();
    cache[data::cacheKey(c.dir)] = std::move(b);
    return *out;
}

int budgetPeople(const World& w) {
    int n = 0;
    for (const auto& [id, c] : w.characters())
        if (c.body.alive && !c.isAnimal()) ++n;
    return n;
}

namespace {

std::size_t trimMemories(std::vector<MemoryEntry>& mem, std::size_t cap) {
    if (mem.size() <= cap) return 0;
    const std::size_t drop = mem.size() - cap;
    // the `drop` least salient (oldest first among equals) leave; the rest keep their order
    std::vector<std::size_t> idx(mem.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return mem[a].salience != mem[b].salience ? mem[a].salience < mem[b].salience : mem[a].tick < mem[b].tick;
    });
    std::vector<bool> gone(mem.size(), false);
    for (std::size_t i = 0; i < drop; ++i) gone[idx[i]] = true;
    std::vector<MemoryEntry> kept;
    kept.reserve(cap);
    for (std::size_t i = 0; i < mem.size(); ++i)
        if (!gone[i]) kept.push_back(std::move(mem[i]));
    mem = std::move(kept);
    return drop;
}

bool keepRecipe(const craft::Recipe& r, const craft::CraftConfig& cfg) {
    if (r.master(cfg) || !r.nameUser.empty() || !r.tags.empty()) return true; // masters, the player's own, breakthroughs
    return false;
}

std::size_t trimRecipes(std::vector<craft::Recipe>& book, std::size_t cap, const craft::CraftConfig& cfg) {
    if (book.size() <= cap) return 0;
    std::vector<std::size_t> loose; // what may go
    for (std::size_t i = 0; i < book.size(); ++i)
        if (!keepRecipe(book[i], cfg)) loose.push_back(i);
    std::size_t drop = std::min(loose.size(), book.size() - cap);
    if (drop == 0) return 0;
    auto worth = [&](const craft::Recipe& r) {
        return r.successes + 4.0 * r.mastery + r.bestQ + 0.25 * r.knownCount() / std::max<std::size_t>(1, r.steps.size());
    };
    std::stable_sort(loose.begin(), loose.end(), [&](std::size_t a, std::size_t b) {
        const double wa = worth(book[a]), wb = worth(book[b]);
        if (wa != wb) return wa < wb;
        return a < b; // the older of equals first
    });
    std::vector<bool> gone(book.size(), false);
    for (std::size_t i = 0; i < drop; ++i) gone[loose[i]] = true;
    std::vector<craft::Recipe> kept;
    kept.reserve(book.size() - drop);
    for (std::size_t i = 0; i < book.size(); ++i)
        if (!gone[i]) kept.push_back(std::move(book[i]));
    book = std::move(kept);
    return drop;
}

std::size_t trimTried(std::map<std::string, int>& tried, std::size_t cap) {
    if (tried.size() <= cap) return 0;
    std::vector<std::pair<int, std::string>> v;
    v.reserve(tried.size());
    for (const auto& [k, n] : tried) v.emplace_back(n, k);
    std::sort(v.begin(), v.end()); // the least tried first, then by key: deterministic
    const std::size_t drop = tried.size() - cap;
    for (std::size_t i = 0; i < drop; ++i) tried.erase(v[i].second);
    return drop;
}

} // namespace

BudgetTrim trimToBudget(const MemoryBudget& b, const craft::CraftConfig& cfg, int people, Character& c) {
    BudgetTrim t;
    t.memories = trimMemories(c.memory, static_cast<std::size_t>(b.memories.per(people)));
    t.recipes = trimRecipes(c.craft.book.recipes, static_cast<std::size_t>(b.recipes.per(people)), cfg);
    t.tried = trimTried(c.craft.tried, static_cast<std::size_t>(b.tried.per(people)));
    return t;
}

BudgetTrim budgetDaily(World& w) {
    const MemoryBudget& b = memoryBudget(w.content());
    const int people = budgetPeople(w);
    const craft::CraftConfig& cfg = w.content().craft.cfg;
    BudgetTrim all;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() && c.memory.empty() && c.craft.empty()) continue;
        const BudgetTrim t = trimToBudget(b, cfg, people, c);
        all.memories += t.memories;
        all.recipes += t.recipes;
        all.tried += t.tried;
    }
    SaidMemory& said = w.said();
    const std::size_t before = said.log.size();
    const std::size_t saidCap = static_cast<std::size_t>(b.said.per(people)) * static_cast<std::size_t>(std::max(1, people));
    said.prune(w.now(), static_cast<Tick>(b.saidKeepDays) * kTicksPerDay,
               std::min(saidCap, static_cast<std::size_t>(std::max(b.said.total, b.said.perMin))));
    all.said = before - said.log.size();
    return all;
}

// --- save form ---------------------------------------------------------------

namespace {

const char* const kShared[] = {"tick", "type", "who", "object", "fact", "goods", "prop"};

} // namespace

Json sharedMemoryJson(Json characters, Json& eventsOut) {
    eventsOut = Json::array();
    std::map<std::string, std::size_t> index; // the shared part's dump -> row
    for (auto& cj : characters) {
        if (!cj.is_object() || !cj.contains("memory") || !cj["memory"].is_array()) continue;
        for (auto& m : cj["memory"]) {
            if (!m.is_object()) continue;
            Json core = Json::object();
            for (const char* k : kShared)
                if (m.contains(k)) {
                    core[k] = m[k];
                    m.erase(k);
                }
            const std::string key = core.dump();
            auto [it, fresh] = index.emplace(key, eventsOut.size());
            if (fresh) eventsOut.push_back(std::move(core));
            m["e"] = it->second;
        }
    }
    return characters;
}

void expandSharedMemory(Json& characters, const Json& events) {
    if (!events.is_array() || events.empty()) return;
    for (auto& cj : characters) {
        if (!cj.is_object() || !cj.contains("memory") || !cj["memory"].is_array()) continue;
        for (auto& m : cj["memory"]) {
            if (!m.is_object() || !m.contains("e")) continue;
            const std::size_t i = m["e"].get<std::size_t>();
            m.erase("e");
            if (i >= events.size()) continue;
            for (auto it = events[i].begin(); it != events[i].end(); ++it) m[it.key()] = it.value();
        }
    }
}

} // namespace lr::sim
