#include "sim/bestiary.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "sim/data_fs.hpp"
#include "sim/species.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> strs(const Json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key)) return out;
    const Json& v = j[key];
    if (v.is_string()) out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const auto& e : v)
            if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

struct Rules {
    std::map<std::string, std::string> beltOfCulture;
    std::string defaultBelt = "forest";
    std::set<std::string> belts;
};

const Rules& rulesOf(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Rules>> cache;
    static const Rules none;
    if (c.dir.empty()) return none;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<Rules>();
    const fs::path p = fs::path(c.dir) / ".." / "creatures" / "_bestiary.json";
    const Json j = data::readJsonOr(p.string(), Json::object());
    if (j.is_object()) {
        r->beltOfCulture = j.value("belt_of_culture", std::map<std::string, std::string>{});
        r->defaultBelt = j.value("default_belt", r->defaultBelt);
        for (const auto& b : strs(j, "belts")) r->belts.insert(b);
    }
    return *(cache[key] = std::move(r));
}

} // namespace

void parseMythic(const Json& j, SpeciesDef::Mythic& m) {
    m.set = true;
    m.belts = strs(j, "belts");
    m.undead = j.value("undead", m.undead);
    m.active = j.value("active", m.active);
    m.sunHarm = j.value("sun_harm", m.sunHarm);
    m.attackMinGroup = j.value("attack_min_group", m.attackMinGroup);
    m.guardM = j.value("guard_m", m.guardM);
    if (j.contains("lure") && j["lure"].is_object()) {
        const Json& l = j["lure"];
        m.lure.radiusM = l.value("radius_m", m.lure.radiusM);
        m.lure.pullM = l.value("pull_m", m.lure.pullM);
        m.lure.chance = l.value("chance", m.lure.chance);
        m.lure.resist = l.value("resist", m.lure.resist);
        m.lure.resistStat = l.value("resist_stat", m.lure.resistStat);
        m.lure.moodlet = l.value("moodlet", m.lure.moodlet);
        m.lure.waterOnly = l.value("water_only", m.lure.waterOnly);
        m.lure.strike = l.value("strike", m.lure.strike);
        m.lure.lostMoodlet = l.value("lost_moodlet", m.lure.lostMoodlet);
    }
    m.regrowDays = j.value("regrow_days", m.regrowDays);
    m.regrowBlockedBy = strs(j, "regrow_blocked_by");
    m.growsOnKill = j.value("grows_on_kill", m.growsOnKill);
    m.riseDays = j.value("rise_days", m.riseDays);
    m.laidBy = j.value("laid_by", m.laidBy);
    if (j.contains("combat") && j["combat"].is_object()) {
        const Json& c = j["combat"];
        m.weapon = c.value("weapon", m.weapon);
        m.skill = c.value("skill", m.skill);
        m.armor = strs(c, "armor");
        m.staminaMult = c.value("stamina_mult", m.staminaMult);
    }
    if (j.contains("lair") && j["lair"].is_object()) m.lair = j["lair"];
}

std::string beltOfCulture(const Content& c, const std::string& culture) {
    if (culture.empty()) return "";
    auto it = c.peoples.cultures.find(culture);
    if (it != c.peoples.cultures.end()) {
        const Json& d = it->second.data;
        if (d.is_object() && d.contains("belt") && d["belt"].is_string()) return d["belt"].get<std::string>();
    }
    const Rules& r = rulesOf(c);
    auto f = r.beltOfCulture.find(culture);
    return f == r.beltOfCulture.end() ? "" : f->second;
}

std::string localBelt(const World& w) {
    const Content& c = w.content();
    std::map<std::string, int> count;
    for (const auto& [id, s] : w.settlements())
        if (s.full()) {
            const std::string b = beltOfCulture(c, s.culture);
            if (!b.empty()) count[b] += 1 + static_cast<int>(s.families.size());
        }
    std::string best;
    int bn = 0;
    for (const auto& [b, n] : count)
        if (n > bn) best = b, bn = n;
    if (!best.empty()) return best;
    const std::string m = beltOfCulture(c, c.peoples.mapCulture);
    return m.empty() ? rulesOf(c).defaultBelt : m;
}

bool creatureInBelt(const SpeciesDef& s, const std::string& belt) {
    if (s.mythic.belts.empty()) return true;
    for (const auto& b : s.mythic.belts)
        if (b == belt) return true;
    return false;
}

std::vector<std::string> checkBestiary(const Content& c) {
    std::vector<std::string> out;
    const Rules& r = rulesOf(c);
    static const std::set<std::string> kActive = {"any", "night", "day"};
    for (const auto& [id, s] : c.species) {
        if (!s.mythic.set) continue;
        const auto& m = s.mythic;
        const std::string at = "creature " + id + ": ";
        if (!kActive.count(m.active)) out.push_back(at + "active '" + m.active + "' is any | night | day");
        if (!r.belts.empty())
            for (const auto& b : m.belts)
                if (!r.belts.count(b)) out.push_back(at + "unknown belt '" + b + "'");
        if (m.attackMinGroup < 1) out.push_back(at + "attack_min_group < 1");
        if (m.lure.radiusM > 0.0 && (m.lure.pullM <= 0.0 || m.lure.chance <= 0.0))
            out.push_back(at + "lure needs pull_m and chance");
        if (m.regrowDays > 0 && s.maxGroup < 2) out.push_back(at + "regrow_days with max_group < 2");
        if (m.lair.is_object() && !m.lair.empty() && m.lair.value("poi", std::string()).empty())
            out.push_back(at + "lair without poi");
    }
    return out;
}

} // namespace lr::sim
