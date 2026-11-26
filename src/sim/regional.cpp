#include "sim/regional.hpp"

#include <memory>
#include <mutex>

#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/culture.hpp"
#include "sim/data_fs.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

RegionalSet readSet(const Json& j) {
    RegionalSet s;
    if (j.contains("clothes") && j["clothes"].is_object()) {
        s.hasClothes = true;
        const Json& clothes = j["clothes"];
        for (const auto& [who, set] : clothes.items())
            if (set.is_object())
                for (const auto& [item, n] : set.items())
                    if (n.is_number_integer()) s.clothes[who][item] = n.get<int>();
                    else s.clothes[who];   // an empty set is a set: nothing given
    }
    s.knowledge = j.value("knowledge", std::vector<std::string>{});
    return s;
}

} // namespace

const RegionalRules& regionalRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RegionalRules>> cache;
    static const RegionalRules none;
    if (c.dir.empty()) return none;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = data::cacheKey(c.dir);
    if (auto it = cache.find(key); it != cache.end()) return *it->second;
    auto r = std::make_unique<RegionalRules>();
    const std::string p = c.dir + "/regional/regions.json";
    if (data::exists(p)) {
        const Json j = data::readJson(p);
        r->loaded = true;
        const Json belts = j.value("belts", Json::object());   // locals: items() of a temporary dangles
        const Json cultures = j.value("cultures", Json::object());
        const Json races = j.value("races", Json::object());
        for (const auto& [id, b] : belts.items())
            if (b.is_object()) r->belts[id] = readSet(b);
        for (const auto& [id, b] : cultures.items()) {
            if (!b.is_object()) continue;
            r->cultures[id] = readSet(b);
            if (b.contains("belt")) r->cultureBelt[id] = b["belt"].get<std::string>();
        }
        for (const auto& [id, b] : races.items())
            if (b.is_object())
                for (const auto& s : b.value("skip_slots", std::vector<std::string>{})) r->skipSlots[id].insert(s);
    }
    return *cache.emplace(key, std::move(r)).first->second;
}

std::string regionalBelt(const Content& c, const std::string& culture) {
    if (culture.empty()) return {};
    if (auto it = c.peoples.cultures.find(culture); it != c.peoples.cultures.end()) {
        const Json& d = it->second.data;
        if (d.is_object() && d.contains("belt") && d["belt"].is_string()) return d["belt"].get<std::string>();
    }
    const RegionalRules& r = regionalRules(c);
    auto it = r.cultureBelt.find(culture);
    return it == r.cultureBelt.end() ? std::string() : it->second;
}

const std::map<std::string, int>* regionalClothes(const World& w, const Character& c, const std::string& who) {
    const RegionalRules& r = regionalRules(w.content());
    if (!r.loaded) return nullptr;
    const std::string culture = c.culture.empty() ? cultureOf(w, c) : c.culture;
    auto pick = [&](const std::map<std::string, RegionalSet>& from, const std::string& id)
        -> const std::map<std::string, int>* {
        auto it = from.find(id);
        if (it == from.end() || !it->second.hasClothes) return nullptr;
        auto s = it->second.clothes.find(who);
        return s == it->second.clothes.end() ? nullptr : &s->second;
    };
    if (const auto* s = pick(r.cultures, culture)) return s;
    return pick(r.belts, regionalBelt(w.content(), culture));
}

bool regionalSkipsSlot(const Content& c, const Character& ch, const std::string& slot) {
    const RegionalRules& r = regionalRules(c);
    auto it = r.skipSlots.find(ch.race);
    return it != r.skipSlots.end() && it->second.count(slot) > 0;
}

void giveRegionalKnowledge(const World& w, Character& c) {
    const RegionalRules& r = regionalRules(w.content());
    if (!r.loaded) return;
    const std::string culture = c.culture.empty() ? cultureOf(w, c) : c.culture;
    if (auto it = r.belts.find(regionalBelt(w.content(), culture)); it != r.belts.end())
        for (const auto& k : it->second.knowledge) c.knowledge.insert(k);
    if (auto it = r.cultures.find(culture); it != r.cultures.end())
        for (const auto& k : it->second.knowledge) c.knowledge.insert(k);
}

} // namespace lr::sim
