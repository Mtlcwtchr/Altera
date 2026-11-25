// News and things worth telling (AL-111). See news.hpp.

#include "sim/news.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/dialogue_memory.hpp"
#include "sim/dialogue_rules.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/world.hpp"

namespace lr::sim {

namespace {

// params of chronicle entries that name a person (the loc args of chronicle.*)
const std::set<std::string>& personParams() {
    static const std::set<std::string> k = {"who",     "a",       "b",        "mother",  "father",  "child",
                                            "captor",  "captive", "judge",    "accused", "accuser", "heir",
                                            "spouse",  "witness", "real_doer", "doer",   "owner",   "suspect",
                                            "fig",     "fig2",    "winner",   "loser",   "actor",   "target",
                                            "victim",  "giver",   "speaker",  "listener"};
    return k;
}
// ... and a settlement
const std::set<std::string>& placeParams() {
    static const std::set<std::string> k = {"pol", "pol2"};
    return k;
}

NewsKind kindFrom(const Json& j) {
    NewsKind k;
    if (!j.is_object()) return k;
    k.kind = j.value("kind", std::string("none"));
    k.mood = j.value("mood", std::string("neutral"));
    k.scope = j.value("scope", std::string("village"));
    k.interest = j.value("interest", 0.0);
    const Json tags = j.value("tags", Json::array());
    for (const auto& t : tags)
        if (t.is_string()) k.tags.push_back(t.get<std::string>());
    return k;
}

std::set<std::string> strSet(const Json& j, const char* key) {
    std::set<std::string> out;
    const Json list = j.value(key, Json::array());
    for (const auto& v : list)
        if (v.is_string()) out.insert(v.get<std::string>());
    return out;
}

// [0, 1), stable for a person and an entry (no RNG state, no tick)
double hash01(EntityId who, size_t index, std::uint64_t salt) {
    Rng r((static_cast<std::uint64_t>(who) * 0x9e3779b97f4a7c15ull) ^ ((index + 1) * 0xc2b2ae3d27d4eb4full) ^ salt);
    return r.uniform();
}

const Settlement* localSettlement(const World& w) {
    for (const auto& [id, s] : w.settlements())
        if (s.full()) return &s;
    return nullptr;
}

EntityId settlementOf(const World& w, const Character& c) {
    if (c.status.settlement != kNone) return c.status.settlement;
    if (const Family* f = w.familyOf(c)) return f->settlement;
    if (c.standing.at != kNone) return c.standing.at;
    return kNone;
}

// Where an entry happened (for scope village): a person in it, else a settlement param, else the map's village.
EntityId placeOf(const World& w, const ChronicleEntry& e) {
    for (EntityId id : e.who)
        if (const Character* c = w.person(id))
            if (EntityId s = settlementOf(w, *c); s != kNone) return s;
    for (const auto& k : placeParams())
        if (e.params.contains(k) && e.params[k].is_number_integer()) {
            const EntityId id = e.params[k].get<EntityId>();
            if (w.settlements().count(id)) return id;
        }
    const Settlement* l = localSettlement(w);
    return l ? l->id : kNone;
}

bool kin(const Character& a, const Character& b) {
    if (a.id == b.id) return true;
    if (a.status.spouse == b.id || b.status.spouse == a.id) return true;
    if (a.status.mother == b.id || a.status.father == b.id || b.status.mother == a.id || b.status.father == a.id)
        return true;
    return a.status.family != kNone && a.status.family == b.status.family;
}

bool early(const NewsRules& R, const Character& c) {
    if (!c.occupation.empty() && R.earlyOccupations.count(c.occupation)) return true;
    if (R.earlyRoles.count(c.status.role)) return true;
    if (!c.standing.life.empty() && R.earlyLife.count(c.standing.life)) return true;
    for (const auto& t : c.traits)
        if (R.earlyTraits.count(t)) return true;
    return false;
}

bool disabled(const NewsRules& R, const NewsKind& k) {
    for (const auto& t : k.tags)
        if (R.disabledTags.count(t)) return true;
    return false;
}

// What `c` was told: entry index -> the memory (the surest one)
std::map<size_t, const MemoryEntry*> heardOf(const Character& c) {
    std::map<size_t, const MemoryEntry*> out;
    for (const auto& m : c.memory) {
        if (m.type != "heard_news" || m.object == kNone) continue;
        const size_t i = static_cast<size_t>(m.object - 1);
        auto it = out.find(i);
        if (it == out.end() || it->second->confidence < m.confidence) out[i] = &m;
    }
    return out;
}

std::optional<Epistemic> knownWith(const World& w, const Character& c, size_t index,
                                   const std::map<size_t, const MemoryEntry*>& heard) {
    const auto& log = w.chronicleLog();
    if (index >= log.size()) return std::nullopt;
    const ChronicleEntry& e = log[index];
    const NewsRules& R = newsRules(w.content());
    const NewsKind* k = R.of(e);
    const Tick age = std::max<Tick>(0, w.now() - e.tick);
    Epistemic epi;
    epi.age = age;
    // in it: seen
    if (std::find(e.who.begin(), e.who.end(), c.id) != e.who.end() || newsInvolves(e, c.id)) {
        epi.source = "seen";
        epi.confidence = R.seenConf;
        return epi;
    }
    // told
    if (auto it = heard.find(index); it != heard.end()) {
        const MemoryEntry& m = *it->second;
        epi.source = m.source;
        epi.confidence = m.confidence;
        epi.chain = m.chain;
        return epi;
    }
    if (!k || k->interest <= 0.0) return std::nullopt;
    const double days = static_cast<double>(age) / kTicksPerDay;
    // the household of someone in it
    if (k->scope != "who")
        for (EntityId id : e.who)
            if (const Character* p = w.person(id); p && p->id != c.id && kin(*p, c)) {
                epi.source = "heard";
                epi.confidence = R.villageConf;
                return epi;
            }
    if (k->scope == "who" || k->scope == "family") return std::nullopt;
    const EntityId at = placeOf(w, e);
    const EntityId mine = settlementOf(w, c);
    if (k->scope == "village" || (at != kNone && at == mine && !e.who.empty())) {
        if (at != kNone && at != mine) return std::nullopt;
        const double d = R.villageDelay0 + (R.villageDelay1 - R.villageDelay0) * hash01(c.id, index, 1);
        if (days < d) return std::nullopt;
        epi.source = "heard";
        epi.confidence = R.villageConf;
        return epi;
    }
    // realm: word from the road
    double d;
    if (early(R, c)) d = R.realmDelay0 + (R.realmDelay1 - R.realmDelay0) * 0.3 * hash01(c.id, index, 2);
    else if (hash01(c.id, index, 3) < R.realmReach) d = R.realmDelay1 * (0.6 + 0.4 * hash01(c.id, index, 4));
    else return std::nullopt;
    if (days < d) return std::nullopt;
    epi.source = "heard";
    epi.confidence = R.realmConf;
    return epi;
}

NewsItem itemOf(const World& w, const Character& c, size_t index, const NewsKind& k, const Epistemic& epi,
                const NewsRules& R, bool story = false) {
    NewsItem it;
    it.index = index;
    it.kind = k;
    it.epi = epi;
    const ChronicleEntry& e = w.chronicleLog()[index];
    for (EntityId id : e.who)
        if (const Character* p = w.person(id); p && kin(*p, c)) it.mine = true;
    const double days = static_cast<double>(epi.age) / kTicksPerDay;
    const double fresh = story ? 1.0 : std::pow(0.5, days / std::max(0.5, R.halfLifeDays));
    it.weight = k.interest * fresh * std::sqrt(std::max(0.05, epi.confidence)) * (it.mine ? R.mineMult : 1.0);
    return it;
}

void sortItems(std::vector<NewsItem>& v) {
    std::stable_sort(v.begin(), v.end(), [](const NewsItem& a, const NewsItem& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.index > b.index;
    });
}

// The first index of the log that is not older than `days`.
size_t firstWithin(const World& w, int days) {
    const auto& log = w.chronicleLog();
    const Tick from = w.now() - static_cast<Tick>(days) * kTicksPerDay;
    size_t lo = 0, hi = log.size();
    while (lo < hi) { // the log is in tick order
        const size_t mid = (lo + hi) / 2;
        if (log[mid].tick < from) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::string lowerFirstLetter(std::string s) {
    if (s.empty()) return s;
    unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        s[0] = static_cast<char>(std::tolower(c0));
        return s;
    }
    if (s.size() < 2) return s;
    unsigned char c1 = static_cast<unsigned char>(s[1]);
    if (c0 == 0xD0 && c1 >= 0x90 && c1 <= 0x9F) s[1] = static_cast<char>(c1 + 0x20);        // А..П
    else if (c0 == 0xD0 && c1 >= 0xA0 && c1 <= 0xAF) {                                       // Р..Я
        s[0] = static_cast<char>(0xD1);
        s[1] = static_cast<char>(c1 - 0x20);
    } else if (c0 == 0xD0 && c1 == 0x81) {                                                   // Ё
        s[0] = static_cast<char>(0xD1);
        s[1] = static_cast<char>(0x91);
    }
    return s;
}

// The skill a person is known for (its loc trade noun news.trade.<skill>), "" = none.
std::string tradeOf(const Character& c) {
    std::string best;
    double bv = 0.35;
    for (const auto& [id, v] : c.skills)
        if (v > bv) {
            bv = v;
            best = id;
        }
    return best;
}

Json tradeNoun(const Loc& loc, const std::string& lang, const Character& c) {
    const std::string sx = c.body.sex == 'm' ? "m" : "f";
    std::vector<std::string> keys;
    if (!c.occupation.empty()) keys.push_back("news.trade." + c.occupation + "." + sx);
    if (const std::string t = tradeOf(c); !t.empty()) keys.push_back("news.trade." + t + "." + sx);
    if (c.body.stage() == "child") keys.push_back("news.trade.child." + sx);
    if (c.body.stage() == "elder") keys.push_back("news.trade.elder." + sx);
    keys.push_back("news.trade.none." + sx);
    for (const auto& k : keys)
        if (loc.has(lang, k)) return loc.noun(lang, k, "");
    return Json("");
}

const char* dirOf(Vec2 from, Vec2 to) {
    const double dx = to.x - from.x, dy = to.y - from.y;
    if (std::abs(dx) < 1e-6 && std::abs(dy) < 1e-6) return "here";
    static const char* names[8] = {"e", "ne", "n", "nw", "w", "sw", "s", "se"};
    double a = std::atan2(dy, dx); // y grows to the north on the region map
    int i = static_cast<int>(std::lround(a / (3.14159265358979 / 4.0)));
    i = ((i % 8) + 8) % 8;
    return names[i];
}

const Ref* arg(const Utterance& u, const char* k) {
    auto it = u.args.find(k);
    return it == u.args.end() ? nullptr : &it->second;
}

bool atWar(const RealmState& s, EntityId a, EntityId b) {
    for (const auto& wr : s.wars)
        if (!wr.over && ((wr.attacker == a && wr.defender == b) || (wr.attacker == b && wr.defender == a))) return true;
    return false;
}
bool allied(const RealmState& s, EntityId a, EntityId b) {
    for (const auto& [x, y] : s.alliances)
        if ((x == a && y == b) || (x == b && y == a)) return true;
    return false;
}

} // namespace

// --- rules ------------------------------------------------------------------------------

const NewsKind* NewsRules::of(const ChronicleEntry& e) const {
    const NewsKind* k = nullptr;
    if (auto it = codes.find(e.code); it != codes.end()) k = &it->second;
    if (!k) {
        size_t best = 0;
        for (const auto& [p, v] : prefixes)
            if (p.size() > best && e.code.compare(0, p.size(), p) == 0) {
                best = p.size();
                k = &v;
            }
    }
    if (!k)
        if (auto it = types.find(e.type); it != types.end()) k = &it->second;
    if (!k || k->interest <= 0.0 || disabled(*this, *k)) return nullptr;
    return k;
}

NewsRules NewsRules::fromJson(const Json& j) {
    NewsRules r;
    if (!j.is_object()) return r;
    r.maxAgeDays = j.value("max_age_days", r.maxAgeDays);
    r.storyAgeDays = j.value("story_age_days", r.storyAgeDays);
    r.halfLifeDays = j.value("half_life_days", r.halfLifeDays);
    if (auto v = j.value("village_delay_days", Json::array()); v.is_array() && v.size() == 2) {
        r.villageDelay0 = v[0].get<double>();
        r.villageDelay1 = v[1].get<double>();
    }
    if (auto v = j.value("realm_delay_days", Json::array()); v.is_array() && v.size() == 2) {
        r.realmDelay0 = v[0].get<double>();
        r.realmDelay1 = v[1].get<double>();
    }
    r.realmReach = j.value("realm_reach", r.realmReach);
    r.earlyOccupations = strSet(j, "early_occupations");
    r.earlyRoles = strSet(j, "early_roles");
    r.earlyTraits = strSet(j, "early_traits");
    r.earlyLife = strSet(j, "early_life");
    r.seenConf = j.value("seen_conf", r.seenConf);
    r.villageConf = j.value("village_conf", r.villageConf);
    r.realmConf = j.value("realm_conf", r.realmConf);
    r.toldConf = j.value("told_conf", r.toldConf);
    r.rumorBelow = j.value("rumor_below", r.rumorBelow);
    r.mineMult = j.value("mine_mult", r.mineMult);
    r.maxHeard = j.value("max_heard", r.maxHeard);
    r.declineAffinity = j.value("decline_affinity", r.declineAffinity);
    r.declineEnmity = j.value("decline_enmity", r.declineEnmity);
    r.disabledTags = strSet(j, "disabled_tags");
    for (const char* sec : {"types", "codes", "prefixes"}) {
        auto& into = std::string(sec) == "types" ? r.types : std::string(sec) == "codes" ? r.codes : r.prefixes;
        const Json part = j.value(sec, Json::object());
        for (const auto& [k, v] : part.items()) into[k] = kindFrom(v);
    }
    return r;
}

const NewsRules& newsRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<NewsRules>> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<NewsRules>();
    if (!c.dir.empty()) {
        std::ifstream f(std::filesystem::path(c.dir) / "news.json");
        if (f) {
            try {
                *r = NewsRules::fromJson(Json::parse(f));
            } catch (const std::exception&) {
                // a broken file means no news; validate_content reports it
            }
        }
    }
    auto* out = r.get();
    cache[c.dir] = std::move(r);
    return *out;
}

// --- knowing ------------------------------------------------------------------------------

bool newsInvolves(const ChronicleEntry& e, EntityId id) {
    if (id == kNone) return false;
    if (std::find(e.who.begin(), e.who.end(), id) != e.who.end()) return true;
    if (!e.params.is_object()) return false;
    for (auto it = e.params.begin(); it != e.params.end(); ++it)
        if (it->is_number_integer() && (personParams().count(it.key()) || placeParams().count(it.key())) &&
            it->get<EntityId>() == id)
            return true;
    return false;
}

std::optional<Epistemic> newsKnown(const World& w, const Character& c, size_t index) {
    return knownWith(w, c, index, heardOf(c));
}

std::vector<NewsItem> knownNews(const World& w, const Character& c, size_t max, int maxAgeDays) {
    const NewsRules& R = newsRules(w.content());
    const auto heard = heardOf(c);
    const auto& log = w.chronicleLog();
    std::vector<NewsItem> out;
    for (size_t i = firstWithin(w, maxAgeDays < 0 ? R.maxAgeDays : maxAgeDays); i < log.size(); ++i) {
        const NewsKind* k = R.of(log[i]);
        if (!k) continue;
        if (auto epi = knownWith(w, c, i, heard)) out.push_back(itemOf(w, c, i, *k, *epi, R));
    }
    sortItems(out);
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<NewsItem> newsToTell(const World& w, const Character& teller, const Character& hearer, size_t max) {
    std::vector<NewsItem> out;
    if (teller.id == hearer.id) return out;
    const auto heard = heardOf(hearer);
    for (auto& it : knownNews(w, teller, 64)) {
        if (knownWith(w, hearer, it.index, heard)) continue;
        // one's own quarrels and scandals are not spread by oneself
        if (it.kind.mood == "spicy" && newsInvolves(w.chronicleLog()[it.index], teller.id)) continue;
        out.push_back(it);
        if (out.size() >= max) break;
    }
    return out;
}

std::vector<NewsItem> newsAbout(const World& w, const Character& teller, EntityId about, size_t max) {
    const NewsRules& R = newsRules(w.content());
    const auto heard = heardOf(teller);
    const auto& log = w.chronicleLog();
    std::vector<NewsItem> out;
    for (size_t i = firstWithin(w, R.storyAgeDays); i < log.size(); ++i) {
        if (!newsInvolves(log[i], about)) continue;
        const NewsKind* k = R.of(log[i]);
        if (!k) continue;
        if (auto epi = knownWith(w, teller, i, heard)) out.push_back(itemOf(w, teller, i, *k, *epi, R));
    }
    sortItems(out);
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<NewsItem> lifeStory(const World& w, const Character& c, size_t max) {
    const NewsRules& R = newsRules(w.content());
    const auto& log = w.chronicleLog();
    std::vector<NewsItem> out;
    for (size_t i = firstWithin(w, R.storyAgeDays); i < log.size(); ++i) {
        const ChronicleEntry& e = log[i];
        bool in = newsInvolves(e, c.id);
        for (EntityId id : e.who) // and his own people: a husband lost, a child born
            if (const Character* p = w.person(id); !in && p && kin(*p, c)) in = true;
        if (!in) continue;
        const NewsKind* k = R.of(e);
        if (!k || k->mood == "spicy") continue; // a life is told by its joys and griefs, not its squabbles
        Epistemic epi;
        epi.source = "seen";
        epi.confidence = 1.0;
        epi.age = std::max<Tick>(0, w.now() - e.tick);
        NewsItem it = itemOf(w, c, i, *k, epi, R, true);
        it.mine = true;
        out.push_back(it);
    }
    sortItems(out);
    if (out.size() > max) out.resize(max);
    return out;
}

double freshNewsWeight(const World& w, const Character& c) {
    if (c.isAnimal() || !c.body.alive) return 0.0;
    double best = 0.0;
    for (const auto& it : knownNews(w, c, 4, 6)) {
        if (it.kind.mood == "spicy" && newsInvolves(w.chronicleLog()[it.index], c.id)) continue;
        best = std::max(best, it.weight);
    }
    return best;
}

bool learnNews(World& w, Character& hearer, const Character& teller, size_t index, const Epistemic& tellerEpi) {
    if (hearer.id == teller.id || hearer.isAnimal()) return false;
    if (index >= w.chronicleLog().size()) return false;
    if (newsKnown(w, hearer, index)) return false;
    const NewsRules& R = newsRules(w.content());
    MemoryEntry m;
    m.tick = w.now();
    m.type = "heard_news";
    m.who = {teller.id};
    m.object = static_cast<EntityId>(index + 1);
    m.confidence = std::clamp(tellerEpi.confidence * R.toldConf, 0.05, 1.0);
    m.chain.push_back(teller.id);
    for (EntityId id : tellerEpi.chain) {
        if (m.chain.size() >= Epistemic::kMaxChain) break;
        m.chain.push_back(id);
    }
    m.source = (m.chain.size() >= 2 || m.confidence < R.rumorBelow) ? "rumor" : "told";
    const NewsKind* k = R.of(w.chronicleLog()[index]);
    m.salience = k ? k->interest : 0.3;
    m.valence = k && k->mood == "sad" ? -0.3 : k && k->mood == "glad" ? 0.3 : 0.0;
    hearer.memory.push_back(std::move(m));
    // keep the newest max_heard
    size_t n = 0;
    for (const auto& e : hearer.memory) n += e.type == "heard_news";
    if (n > R.maxHeard) {
        auto it = std::find_if(hearer.memory.begin(), hearer.memory.end(),
                               [](const MemoryEntry& e) { return e.type == "heard_news"; });
        if (it != hearer.memory.end()) hearer.memory.erase(it);
    }
    return true;
}

Ref newsRef(size_t index) { return Ref{"news", static_cast<EntityId>(index + 1), "", 0.0, {}}; }

std::optional<size_t> newsIndex(const World& w, const Ref& r) {
    if (r.kind != "news" || r.id == kNone) return std::nullopt;
    const size_t i = static_cast<size_t>(r.id - 1);
    if (i >= w.chronicleLog().size()) return std::nullopt;
    return i;
}

std::string newsText(const Loc& loc, const std::string& lang, const World& w, size_t index, bool lowerFirst) {
    const auto& log = w.chronicleLog();
    if (index >= log.size()) return "";
    const ChronicleEntry& e = log[index];
    if (!loc.has(lang, e.code)) return "";
    Json args = locArgs(loc, lang, w, e.params);
    // the persons of the entry by name, whatever the param is called (locArgs knows a few)
    if (e.params.is_object())
        for (auto it = e.params.begin(); it != e.params.end(); ++it)
            if (it->is_number_integer() && personParams().count(it.key()))
                if (const Character* c = w.person(it->get<EntityId>())) args[it.key()] = loc.personNoun(lang, *c);
    std::string s = loc.text(lang, e.code, args);
    if (lowerFirst) {
        const Json* raw = loc.find(lang, e.code);
        std::string pat;
        if (raw && raw->is_string()) pat = raw->get<std::string>();
        else if (raw && raw->is_array() && !raw->empty() && (*raw)[0].is_string()) pat = (*raw)[0].get<std::string>();
        if (pat.empty() || pat[0] != '{') s = lowerFirstLetter(s);
    }
    return s;
}

// --- lines --------------------------------------------------------------------------------

void noteSaid(World& w, const Utterance& u, const std::string& /*rule*/) {
    const Ref* r = arg(u, "news");
    if (!r || u.speaker == u.listener) return;
    const auto idx = newsIndex(w, *r);
    if (!idx) return;
    Character* hearer = w.character(u.listener);
    const Character* teller = w.character(u.speaker);
    if (!hearer || !teller) return;
    Epistemic epi = u.epi;
    if (auto k = newsKnown(w, *teller, *idx)) epi = *k;
    learnNews(w, *hearer, *teller, *idx, epi);
}

void addNewsArgs(const Loc& loc, const std::string& lang, const World& w, const Utterance& u, Json& args) {
    if (const Ref* r = arg(u, "news"))
        if (auto idx = newsIndex(w, *r)) {
            args["news"] = newsText(loc, lang, w, *idx, false);
            args["news_lc"] = newsText(loc, lang, w, *idx, true);
        }
    if (const Ref* r = arg(u, "person"); r && r->kind == "char")
        if (const Character* p = w.person(r->id)) {
            if (!w.character(r->id)) args["person"] = loc.personNoun(lang, *p);
            args["person_trade"] = tradeNoun(loc, lang, *p);
        }
    if (const Ref* r = arg(u, "place"); r && r->kind == "place" && r->id != kNone) {
        const Json n = realmNoun(loc, lang, w, r->id);
        args["place"] = n.is_null() ? Json(r->key) : n;
        auto it = w.settlements().find(r->id);
        if (it != w.settlements().end()) {
            const Settlement& s = it->second;
            const Settlement* home = localSettlement(w);
            const Vec2 from = home ? home->km : Vec2{};
            args["place_km"] = static_cast<int>(std::lround(dist(from, s.km)));
            args["place_dir"] = loc.noun(lang, std::string("news.dir.") + dirOf(from, s.km), "");
            args["place_size"] = loc.noun(lang, "news.size." + (s.size.empty() ? std::string("village") : s.size), "");
            if (const Character* ruler = w.person(s.chief)) args["ruler"] = loc.personNoun(lang, *ruler);
            else args["ruler"] = loc.noun(lang, "news.nobody", "");
        }
    }
    // what a line may ask for that the moment does not have: nothing raw is left
    const Json something = loc.noun(lang, "thing.something", "");
    auto fill = [&](const char* k, const Json& v) {
        if (!args.contains(k)) args[k] = v;
    };
    fill("news", something.value("nom", std::string()));
    fill("news_lc", something.value("nom", std::string()));
    fill("person_trade", loc.noun(lang, "news.trade.none.m", ""));
    fill("place", something);
    fill("place_dir", loc.noun(lang, "news.dir.here", ""));
    fill("place_size", loc.noun(lang, "news.size.village", ""));
    fill("place_km", 0);
    fill("ruler", loc.noun(lang, "news.nobody", ""));
    fill("self_trade", loc.noun(lang, "news.trade.none.m", ""));
    fill("years", 0);
    fill("children", 0);
    fill("spouse", something);
    if (const Character* self = w.person(u.speaker)) {
        args["self_trade"] = tradeNoun(loc, lang, *self);
        args["years"] = self->body.ageDays / kDaysPerYear;
        args["children"] = static_cast<int>(self->status.children.size());
        if (const Character* sp = w.person(self->status.spouse)) args["spouse"] = loc.personNoun(lang, *sp);
    }
}

void installNewsFacts() {
    registerFactProvider(
        "news",
        [](ContextFacts& f, const World& w, const Utterance& u) {
            const Character* s = w.person(u.speaker);
            // news.*: the entry the line tells
            if (const Ref* r = arg(u, "news"))
                if (auto idx = newsIndex(w, *r)) {
                    const ChronicleEntry& e = w.chronicleLog()[*idx];
                    f.set("news", 1.0);
                    f.set("news.code", e.code);
                    f.set("news.type", e.type);
                    f.set("news.days", static_cast<double>(std::max<Tick>(0, w.now() - e.tick)) / kTicksPerDay);
                    if (const NewsKind* k = newsRules(w.content()).of(e)) {
                        f.set("news.kind", k->kind);
                        f.set("news.mood", k->mood);
                        f.set("news.scope", k->scope);
                        for (const auto& t : k->tags) f.add("news.tag", t);
                    }
                    if (newsInvolves(e, u.speaker)) f.set("news.self", 1.0);
                    if (newsInvolves(e, u.listener)) f.set("news.about_listener", 1.0);
                    if (s)
                        for (EntityId id : e.who)
                            if (const Character* p = w.person(id); p && p->id != s->id && kin(*p, *s)) f.set("news.mine", 1.0);
                }
            // person.*: whom the line is about (ask_person / tell_person)
            if (const Ref* r = arg(u, "person"); r && r->kind == "char" && s)
                if (const Character* p = w.person(r->id)) {
                    f.set("person", 1.0);
                    f.set("person.sex", std::string(1, p->body.sex));
                    f.set("person.stage", p->body.stage());
                    if (!p->body.alive) f.set("person.dead", 1.0);
                    if (p->id == u.speaker) f.set("person.is_speaker", 1.0);
                    if (p->id == u.listener) f.set("person.is_listener", 1.0);
                    if (!p->occupation.empty()) f.set("person.occupation", p->occupation);
                    f.set("person.role", p->status.role);
                    for (const auto& [sid, st] : w.settlements())
                        if (st.chief == p->id) f.set("person.chief", 1.0);
                    if (auto it = s->relations.find(p->id); it != s->relations.end())
                        for (const char* axis : {"affinity", "trust", "fear", "respect", "familiarity", "enmity"})
                            f.set(std::string("person.") + axis, it->second.get(axis));
                    if (s->status.spouse == p->id) f.add("person.kin", "spouse");
                    if (s->status.mother == p->id || s->status.father == p->id) f.add("person.kin", "parent");
                    if (p->status.mother == s->id || p->status.father == s->id) f.add("person.kin", "child");
                    if (s->status.family != kNone && s->status.family == p->status.family && p->id != s->id)
                        f.add("person.kin", "family");
                    const bool sameVillage = settlementOf(w, *s) != kNone && settlementOf(w, *s) == settlementOf(w, *p);
                    if (sameVillage) f.set("person.neighbour", 1.0);
                    const double fam = s->relations.count(p->id) ? s->relations.at(p->id).familiarity : 0.0;
                    if (fam > 0.05 || sameVillage || kin(*s, *p)) f.set("person.known", 1.0);
                }
            // place.*: the settlement the line is about
            if (const Ref* r = arg(u, "place"); r && r->kind == "place")
                if (auto it = w.settlements().find(r->id); it != w.settlements().end()) {
                    const Settlement& p = it->second;
                    f.set("place", 1.0);
                    f.set("place.size", p.size.empty() ? std::string("village") : p.size);
                    for (const auto& t : p.tags) f.add("place.tag", t);
                    if (p.ruined) f.set("place.ruined", 1.0);
                    if (p.full()) f.set("place.local", 1.0);
                    if (const Settlement* home = localSettlement(w); home && home->id != p.id) {
                        f.set("place.km", dist(home->km, p.km));
                        if (realmLoaded(w.content())) {
                            const RealmState rs = loadRealm(w);
                            if (atWar(rs, home->id, p.id)) f.set("place.war", 1.0);
                            if (allied(rs, home->id, p.id)) f.set("place.ally", 1.0);
                            if (p.tributeTo == home->id) f.set("place.pays_us", 1.0);
                            if (home->tributeTo == p.id) f.set("place.we_pay", 1.0);
                            if (home->liege == p.id) f.set("place.our_liege", 1.0);
                            auto v = rs.views.find(std::to_string(home->id) + ">" + std::to_string(p.id));
                            if (v != rs.views.end()) {
                                f.set("place.opinion", v->second.opinion);
                                f.set("place.grudge", v->second.grudge());
                            }
                        }
                    }
                    if (w.person(p.chief)) f.set("place.ruler", 1.0);
                }
            // story.*: the speaker's own life (tell_self)
            if (s) {
                f.set("story.years", static_cast<double>(s->body.ageDays / kDaysPerYear));
                if (const Character* sp = w.person(s->status.spouse)) f.set(sp->body.alive ? "story.married" : "story.widowed", 1.0);
                f.set("story.children", static_cast<double>(s->status.children.size()));
                if (!s->occupation.empty()) f.set("story.occupation", s->occupation);
                if (const std::string t = tradeOf(*s); !t.empty()) f.set("story.trade", t);
                for (const auto& [sid, st] : w.settlements())
                    if (st.chief == s->id) f.set("story.chief", 1.0);
            }
        },
        {"news", "person", "place", "story."});
}

namespace {
[[maybe_unused]] const bool kNewsFactsInstalled = (installNewsFacts(), true);
} // namespace

// --- asking --------------------------------------------------------------------------------

std::vector<MenuTopic> newsTopics(const World& w, const Character& speaker, const Character& listener, size_t max) {
    std::vector<MenuTopic> out;
    for (const auto& it : newsToTell(w, speaker, listener, max)) {
        MenuTopic t;
        t.act = "tell_news";
        t.epi = it.epi;
        t.topic = "news:" + std::to_string(it.index);
        t.weight = it.weight;
        t.args["news"] = newsRef(it.index);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<MenuTopic> askTopics(const World& w, const Character& asker, const Character& answerer, size_t maxPeople,
                                 size_t maxPlaces) {
    std::vector<MenuTopic> out;
    if (asker.id == answerer.id || answerer.isAnimal() || !answerer.body.alive) return out;
    auto plain = [&](const char* act) {
        MenuTopic t;
        t.act = act;
        t.topic = act;
        out.push_back(std::move(t));
    };
    plain("ask_news");
    plain("ask_self");
    // people: the answerer's spouse, the chief, then whom the asker knows best
    std::vector<EntityId> people;
    auto addPerson = [&](EntityId id) {
        if (people.size() >= maxPeople || id == kNone || id == asker.id || id == answerer.id) return;
        if (std::find(people.begin(), people.end(), id) != people.end()) return;
        const Character* p = w.person(id);
        if (!p || p->isAnimal() || !p->body.alive) return;
        people.push_back(id);
    };
    addPerson(answerer.status.spouse);
    if (const Settlement* home = localSettlement(w)) addPerson(home->chief);
    std::vector<std::pair<double, EntityId>> known;
    for (const auto& [id, rel] : asker.relations)
        if (rel.familiarity > 0.05) known.push_back({-rel.familiarity, id});
    std::sort(known.begin(), known.end());
    for (const auto& [f, id] : known) addPerson(id);
    for (EntityId id : people) {
        MenuTopic t;
        t.act = "ask_person";
        t.topic = "person:" + std::to_string(id);
        t.args["person"] = Ref{"char", id, "", 0.0, {}};
        out.push_back(std::move(t));
    }
    // places: the nearest settlements beyond the village
    if (const Settlement* home = localSettlement(w)) {
        std::vector<std::pair<double, EntityId>> near;
        for (const auto& [id, s] : w.settlements())
            if (id != home->id && !s.ruined && s.polity()) near.push_back({dist(home->km, s.km), id});
        std::sort(near.begin(), near.end());
        for (size_t i = 0; i < near.size() && i < maxPlaces; ++i) {
            MenuTopic t;
            t.act = "ask_place";
            t.topic = "place:" + std::to_string(near[i].second);
            t.args["place"] = Ref{"place", near[i].second, std::to_string(near[i].second), 0.0, {}};
            out.push_back(std::move(t));
        }
    }
    return out;
}

bool talkOverNews(World& w, Character& teller, Character& hearer, const std::string& talk, int turn) {
    if (teller.id == hearer.id) return false;
    std::vector<NewsItem> items;
    const char* act = "discuss_news";
    if (talk == "story") {
        items = lifeStory(w, teller, 3);
        act = "tell_story";
    } else if (talk == "chat" || talk == "gossip") {
        const auto heard = heardOf(hearer);
        for (const auto& it : knownNews(w, teller, 6, 10))
            if (knownWith(w, hearer, it.index, heard)) items.push_back(it);
    }
    if (items.empty()) return false;
    const NewsItem& it = items[static_cast<size_t>(std::max(0, turn)) % items.size()];
    std::map<std::string, Ref> args;
    args["news"] = newsRef(it.index);
    if (!say(w, teller, hearer, act, std::nullopt, it.epi, "", talk, args).ok()) return false;
    Epistemic back = it.epi;
    if (auto k = newsKnown(w, hearer, it.index)) back = *k;
    say(w, hearer, teller, "react_news", std::nullopt, back, "", talk, args);
    return true;
}

bool declinesToAnswer(const World& w, const Character& answerer, const Character& asker) {
    const NewsRules& R = newsRules(w.content());
    auto it = answerer.relations.find(asker.id);
    if (it == answerer.relations.end()) return false;
    return it->second.affinity <= R.declineAffinity || it->second.enmity >= R.declineEnmity;
}

int askAndAnswer(World& w, Character& asker, Character& answerer, const MenuTopic& ask) {
    say(w, asker, answerer, ask.act, std::nullopt, Epistemic{}, "", "", ask.args);
    if (!answerer.body.alive) return 0;
    if (declinesToAnswer(w, answerer, asker))
        return say(w, answerer, asker, "decline_ask", std::nullopt, Epistemic{}, "", ask.act, ask.args).ok() ? 1 : 0;
    int n = 0;
    auto tell = [&](const NewsItem& it, const char* act) {
        std::map<std::string, Ref> a = ask.args;
        a["news"] = newsRef(it.index);
        n += say(w, answerer, asker, act, std::nullopt, it.epi, "", ask.act, a).ok();
    };
    if (ask.act == "ask_news") {
        const auto items = newsToTell(w, answerer, asker, 2);
        if (items.empty()) n += say(w, answerer, asker, "no_news", std::nullopt, Epistemic{}, "", ask.act).ok();
        for (const auto& it : items) tell(it, "tell_news");
    } else if (ask.act == "ask_self") {
        n += say(w, answerer, asker, "tell_self", std::nullopt, Epistemic{}, "", ask.act).ok();
        for (const auto& it : lifeStory(w, answerer, 1)) tell(it, "tell_story");
    } else if (ask.act == "ask_person") {
        auto pi = ask.args.find("person");
        if (pi == ask.args.end()) return n;
        const EntityId pid = pi->second.id;
        const SaidLine first = say(w, answerer, asker, "tell_person", std::nullopt, Epistemic{}, "", ask.act, ask.args);
        n += first.ok();
        // an opinion of whom they know (not of a stranger, not of the asker himself)
        const bool knows = first.ok() && first.rule.find(".unknown") == std::string::npos &&
                           first.rule.find(".listener") == std::string::npos && first.rule.find(".dead") == std::string::npos;
        if (knows) n += say(w, answerer, asker, "person_remark", std::nullopt, Epistemic{}, "", ask.act, ask.args).ok();
        bool told = false;
        if (w.character(pid))
            for (const auto& rc : recall(w, answerer, pid))
                if (rc.kind == "deed") {
                    told = say(w, answerer, asker, "inform", rc.prop, rc.epi).ok();
                    n += told;
                    break;
                }
        if (!told)
            for (const auto& it : newsAbout(w, answerer, pid, 1)) tell(it, "tell_news");
    } else if (ask.act == "ask_place") {
        auto pl = ask.args.find("place");
        if (pl == ask.args.end()) return n;
        n += say(w, answerer, asker, "tell_place", std::nullopt, Epistemic{}, "", ask.act, ask.args).ok();
        n += say(w, answerer, asker, "place_remark", std::nullopt, Epistemic{}, "", ask.act, ask.args).ok();
        for (const auto& it : newsAbout(w, answerer, pl->second.id, 1))
            if (!newsKnown(w, asker, it.index)) tell(it, "tell_news");
    }
    return n;
}

} // namespace lr::sim

namespace lr::sim {

bool newsFactKey(const std::string& k) {
    for (const char* p : {"news", "person", "place"})
        if (k == p || k.rfind(std::string(p) + ".", 0) == 0) return true;
    return k.rfind("story.", 0) == 0;
}

void sampleNewsFacts(const std::string& act, const DialogueRuleDef* target, ContextFacts& f) {
    static const std::set<std::string> newsActs = {"tell_news", "tell_story", "discuss_news", "react_news"};
    static const std::set<std::string> personActs = {"ask_person", "tell_person", "person_remark"};
    static const std::set<std::string> placeActs = {"ask_place", "tell_place", "place_remark"};
    if (newsActs.count(act)) {
        f.set("news", 1.0);
        f.set("news.kind", "death");
        f.set("news.mood", "sad");
        f.set("news.scope", "village");
        f.set("news.days", 2.0);
    }
    if (personActs.count(act)) {
        f.set("person", 1.0);
        f.set("person.known", 1.0);
    }
    if (placeActs.count(act)) {
        f.set("place", 1.0);
        f.set("place.size", "village");
    }
    if (!target) return;
    for (const auto& c : target->when) {
        if (!newsFactKey(c.key)) continue;
        switch (c.op) {
        case Criterion::Op::Eq:
            if (c.values.empty()) break;
            if (c.key == "person.kin" || c.key == "place.tag" || c.key == "news.tag") {
                f.list[c.key] = {c.values.front()};
            } else f.set(c.key, c.values.front());
            break;
        case Criterion::Op::Has: f.set(c.key, 1.0); break;
        case Criterion::Op::Gt: f.set(c.key, c.number + 0.1); break;
        case Criterion::Op::Ge: f.set(c.key, c.number); break;
        case Criterion::Op::Lt: f.set(c.key, c.number - 0.1); break;
        case Criterion::Op::Le: f.set(c.key, c.number); break;
        case Criterion::Op::Not:
            f.str.erase(c.key);
            f.num.erase(c.key);
            f.list.erase(c.key);
            break;
        default: break;
        }
        if (c.key == "person.kin" && c.op == Criterion::Op::Has) f.list[c.key] = {"family"};
        if (c.key == "place.tag" && c.op == Criterion::Op::Has) f.list[c.key] = {"farming"};
    }
}

void sampleNewsArgs(const Loc& loc, const std::string& lang, Json& args) {
    const Json who = args.contains("person") ? args["person"] : args.value("listener", Json());
    const Json other = args.contains("src") ? args["src"] : args.value("speaker", Json());
    Json a = {{"a", who}, {"b", other}, {"who", who}};
    std::string news = loc.has(lang, "chronicle.village.wedding") ? loc.text(lang, "chronicle.village.wedding", a) : "";
    if (news.empty()) news = loc.text(lang, "news.size.village");
    args["news"] = news;
    args["news_lc"] = lowerFirstLetter(news);
    args["person_trade"] = loc.noun(lang, "news.trade.smithing.m", "");
    args["self_trade"] = loc.noun(lang, "news.trade.none.m", "");
    args["place"] = loc.noun(lang, "news.sample_place", "");
    args["place_dir"] = loc.noun(lang, "news.dir.n", "");
    args["place_size"] = loc.noun(lang, "news.size.village", "");
    args["place_km"] = 12;
    args["ruler"] = other;
    args["years"] = 34;
    args["children"] = 3;
    args["spouse"] = other;
}

} // namespace lr::sim
