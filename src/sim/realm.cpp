// The realm (AL-85): see realm.hpp and gdd/realm/REALM_v0.1.md.
#include "sim/realm.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "sim/ai.hpp"
#include "sim/content.hpp"
#include "sim/loc.hpp"
#include "sim/village_life.hpp"
#include "sim/world.hpp"
#include "sim/worldgen.hpp"

namespace fs = std::filesystem;

namespace altera::sim {

// --- rules ---------------------------------------------------------------------

struct RealmKind {
    std::array<double, 2> pop = {40, 100}, foodDays = {60, 120};
    double wealthPerHead = 1.0, incomePerHeadYear = 0.5, militiaShare = 0.2, yield = 1.0;
    std::string institution = "chief";
    std::array<int, 2> knights = {0, 0}, children = {1, 3};
    bool peaceful = false, celibate = false, livesByRaids = false;
};

struct RealmQuestKind {
    std::array<double, 2> reward = {10, 30};
    double danger = 0.3, renown = 0.05;
    int days = 40, takers = 2;
    std::vector<std::string> tags;
};

struct RealmRules {
    bool loaded = false, enabled = true;
    std::set<std::string> disabledTags;
    std::array<double, 2> regionKm = {12, 38};
    std::vector<std::pair<std::string, int>> layout;
    std::map<std::string, RealmKind> kinds;
    std::map<std::string, std::vector<std::string>> names;
    std::map<std::string, RealmQuestKind> quests;
    std::map<std::string, std::vector<std::string>> arcTags;
    Json eco, pol, war, suc, rev, ban, fig, wan, loc;
    std::map<std::string, std::map<std::string, double>> traitAxes;
    std::map<std::string, double> grudgeMult;

    bool questOn(const std::string& k) const {
        auto it = quests.find(k);
        if (it == quests.end()) return false;
        for (const auto& t : it->second.tags)
            if (disabledTags.count(t)) return false;
        return true;
    }
    bool arcOn(const std::string& k) const {
        auto it = arcTags.find(k);
        if (it == arcTags.end()) return true;
        for (const auto& t : it->second)
            if (disabledTags.count(t)) return false;
        return true;
    }
};

namespace {

double num(const Json& j, const char* k, double d) { return j.is_object() ? j.value(k, d) : d; }
int inum(const Json& j, const char* k, int d) { return j.is_object() ? j.value(k, d) : d; }
std::array<double, 2> pair2(const Json& j, const char* k, std::array<double, 2> d) {
    if (!j.is_object() || !j.contains(k) || !j[k].is_array() || j[k].size() < 2) return d;
    return {j[k][0].get<double>(), j[k][1].get<double>()};
}

RealmRules parseRules(const Json& j) {
    RealmRules r;
    r.loaded = true;
    r.enabled = j.value("enabled", true);
    for (const auto& t : j.value("disabled_tags", Json::array())) r.disabledTags.insert(t.get<std::string>());
    r.regionKm = pair2(j, "region_km", r.regionKm);
    for (const auto& e : j.value("layout", Json::array())) r.layout.push_back({e.value("kind", ""), e.value("n", 1)});
    const Json kinds = j.value("kinds", Json::object());
    for (auto it = kinds.begin(); it != kinds.end(); ++it) {
        const Json& k = it.value();
        RealmKind d;
        d.pop = pair2(k, "pop", d.pop);
        d.foodDays = pair2(k, "food_days", d.foodDays);
        d.wealthPerHead = num(k, "wealth_per_head", d.wealthPerHead);
        d.incomePerHeadYear = num(k, "income_per_head_year", d.incomePerHeadYear);
        d.militiaShare = num(k, "militia_share", d.militiaShare);
        d.yield = num(k, "yield", d.yield);
        d.institution = k.value("institution", d.institution);
        auto kn = pair2(k, "knights", {0, 0});
        d.knights = {static_cast<int>(kn[0]), static_cast<int>(kn[1])};
        auto ch = pair2(k, "children", {1, 3});
        d.children = {static_cast<int>(ch[0]), static_cast<int>(ch[1])};
        d.peaceful = k.value("peaceful", false);
        d.celibate = k.value("celibate", false);
        d.livesByRaids = k.value("lives_by_raids", false);
        r.kinds[it.key()] = d;
    }
    const Json names = j.value("names", Json::object());
    for (auto it = names.begin(); it != names.end(); ++it) r.names[it.key()] = it.value().get<std::vector<std::string>>();
    const Json qs = j.value("quests", Json::object());
    for (auto it = qs.begin(); it != qs.end(); ++it) {
        const Json& q = it.value();
        RealmQuestKind d;
        d.reward = pair2(q, "reward", d.reward);
        d.danger = num(q, "danger", d.danger);
        d.renown = num(q, "renown", d.renown);
        d.days = inum(q, "days", d.days);
        d.takers = inum(q, "takers", d.takers);
        d.tags = q.value("tags", std::vector<std::string>{});
        r.quests[it.key()] = d;
    }
    const Json arcs = j.value("arcs", Json::object());
    for (auto it = arcs.begin(); it != arcs.end(); ++it)
        r.arcTags[it.key()] = it.value().value("tags", std::vector<std::string>{});
    r.eco = j.value("economy", Json::object());
    r.pol = j.value("politics", Json::object());
    r.war = j.value("war", Json::object());
    r.suc = j.value("succession", Json::object());
    r.rev = j.value("revolt", Json::object());
    r.ban = j.value("bandits", Json::object());
    r.fig = j.value("figures", Json::object());
    r.wan = j.value("wanderers", Json::object());
    r.loc = j.value("local", Json::object());
    const Json ta = r.fig.value("trait_axes", Json::object());
    for (auto it = ta.begin(); it != ta.end(); ++it)
        for (auto a = it.value().begin(); a != it.value().end(); ++a) r.traitAxes[it.key()][a.key()] = a.value().get<double>();
    const Json gm = r.fig.value("grudge_mult", Json::object());
    for (auto it = gm.begin(); it != gm.end(); ++it) r.grudgeMult[it.key()] = it.value().get<double>();
    return r;
}

std::vector<RealmHook>& hooks() {
    static std::vector<RealmHook> h;
    return h;
}

} // namespace

const RealmRules& realmRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<RealmRules>> cache;
    static const RealmRules kOff;
    if (c.dir.empty()) return kOff;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<RealmRules>();
    const fs::path p = fs::path(c.dir) / "realm.json";
    if (fs::exists(p)) {
        std::ifstream in(p);
        *r = parseRules(Json::parse(in));
    }
    return *cache.emplace(c.dir, std::move(r)).first->second;
}

void addRealmHook(RealmHook h) { hooks().push_back(std::move(h)); }

bool realmLoaded(const Content& c) { return realmRules(c).loaded; }

bool realmQuestOn(const Content& c, const std::string& kind) { return realmRules(c).questOn(kind); }

// --- state ---------------------------------------------------------------------

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmGrievance, cause, weight, day)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmView, opinion, grievances)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmPolity, id, kind, name, pos, settlement, pop, food, wealth, militia,
                                                morale, unrest, ruler, heir, house, institution, liege, tributeTo,
                                                tribute, ruined, lastRaid, lastRaided, lastRevolt, lastRefugees, unrestDays, founded)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmFigure, id, name, house, sex, born, home, at, role, life, traits,
                                                martial, ambition, greed, honor, wanderlust, renown, wealth, health,
                                                alive, death, deathDay, spouse, father, mother, children, travelTo,
                                                arrive, quest, claim, captor, opinions, aptitudes, deeds, present,
                                                leaveDay, parked)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmQuest, id, kind, giver, polity, target, reward, posted, deadline,
                                                takers, status, closed)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmWar, id, attacker, defender, claimant, cause, start, last, score,
                                                battles, raids, startMilA, startMilD, alliesA, alliesD, siege, over,
                                                outcome)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmOffer, id, kind, from, to, envoy, amount, groom, bride, war, day,
                                                deadline, status)

void to_json(Json& j, const RealmState& s) {
    j = Json{{"inited", s.inited},       {"next_id", s.nextId}, {"year", s.year},     {"harvest", s.harvest},
             {"polities", s.polities},   {"figures", s.figures}, {"views", s.views},  {"alliances", s.alliances},
             {"quests", s.quests},       {"wars", s.wars},       {"offers", s.offers}, {"stats", s.stats}};
}

void from_json(const Json& j, RealmState& s) {
    s = RealmState{};
    if (!j.is_object()) return;
    s.inited = j.value("inited", false);
    s.nextId = j.value("next_id", 1);
    s.year = j.value("year", -1);
    s.harvest = j.value("harvest", 1.0);
    s.polities = j.value("polities", std::vector<RealmPolity>{});
    s.figures = j.value("figures", std::vector<RealmFigure>{});
    s.views = j.value("views", std::map<std::string, RealmView>{});
    s.alliances = j.value("alliances", std::vector<std::pair<EntityId, EntityId>>{});
    s.quests = j.value("quests", std::vector<RealmQuest>{});
    s.wars = j.value("wars", std::vector<RealmWar>{});
    s.offers = j.value("offers", std::vector<RealmOffer>{});
    s.stats = j.value("stats", std::map<std::string, int>{});
}

double RealmView::grudge() const {
    double g = 0.0;
    for (const auto& x : grievances) g += x.weight;
    return g;
}

RealmPolity* RealmState::polity(EntityId id) {
    for (auto& p : polities)
        if (p.id == id) return &p;
    return nullptr;
}
const RealmPolity* RealmState::polity(EntityId id) const {
    for (const auto& p : polities)
        if (p.id == id) return &p;
    return nullptr;
}
RealmFigure* RealmState::figure(EntityId id) {
    if (id == kNone) return nullptr;
    for (auto& f : figures)
        if (f.id == id) return &f;
    return nullptr;
}
const RealmFigure* RealmState::figure(EntityId id) const {
    if (id == kNone) return nullptr;
    for (const auto& f : figures)
        if (f.id == id) return &f;
    return nullptr;
}
RealmView& RealmState::view(EntityId a, EntityId b) { return views[std::to_string(a) + ">" + std::to_string(b)]; }
const RealmPolity* RealmState::local() const {
    for (const auto& p : polities)
        if (p.settlement != kNone) return &p;
    return nullptr;
}
RealmPolity* RealmState::local() {
    for (auto& p : polities)
        if (p.settlement != kNone) return &p;
    return nullptr;
}

RealmState loadRealm(const World& w) {
    RealmState s;
    if (const Json* j = w.moduleState("realm"); j && j->is_object()) from_json(*j, s);
    return s;
}

void saveRealm(World& w, const RealmState& s) { w.moduleState("realm") = Json(s); }

// --- helpers -------------------------------------------------------------------

namespace {

constexpr const char* kSource = "realm";

struct Ctx {
    World& w;
    RealmState& s;
    const RealmRules& r;
    int day;
};

bool person(const Character* c) { return c && c->body.alive && !c->isAnimal(); }
int years(const Character& c) { return c.body.ageDays / kDaysPerYear; }
double dkm(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

Rng rngOf(const Ctx& x, EntityId id, std::string_view salt) { return x.w.rng(id, salt); }

void log(Ctx& x, const std::string& code, Json params, std::vector<EntityId> who = {}) {
    ++x.s.stats[code];
    x.w.chronicle("realm", std::move(who), "chronicle.realm." + code, std::move(params));
}

void deed(RealmFigure& f, int day, const std::string& code, Json extra = Json::object()) {
    extra["day"] = day;
    extra["code"] = code;
    f.deeds.push_back(std::move(extra));
    if (f.deeds.size() > 24) f.deeds.erase(f.deeds.begin());
}

// The ruler of a polity: a figure, or (the village) a Character.
bool rulerAlive(const Ctx& x, const RealmPolity& p) {
    if (p.settlement != kNone) return person(x.w.character(p.ruler));
    const RealmFigure* f = x.s.figure(p.ruler);
    return f && f->alive;
}

struct Axes {
    double martial = 0.3, ambition = 0.3, greed = 0.3, honor = 0.5, wanderlust = 0.2, renown = 0.0, grudge = 1.0;
    bool player = false;
};

Axes axesOfTraits(const RealmRules& r, const std::vector<std::string>& traits) {
    Axes a;
    for (const auto& t : traits) {
        auto it = r.traitAxes.find(t);
        if (it != r.traitAxes.end())
            for (const auto& [k, v] : it->second) {
                if (k == "martial") a.martial += v;
                else if (k == "ambition") a.ambition += v;
                else if (k == "greed") a.greed += v;
                else if (k == "honor") a.honor += v;
                else if (k == "wanderlust") a.wanderlust += v;
            }
        auto g = r.grudgeMult.find(t);
        if (g != r.grudgeMult.end()) a.grudge *= g->second;
    }
    return a;
}

Axes rulerAxes(const Ctx& x, const RealmPolity& p) {
    Axes a;
    if (p.settlement != kNone) {
        if (const Character* c = x.w.character(p.ruler)) {
            a = axesOfTraits(x.r, c->traits);
            a.player = c->controller.isPlayer();
            double fight = 0.0;
            for (const char* k : {"combat", "hunting", "spear", "melee"})
                if (auto it = c->skills.find(k); it != c->skills.end()) fight = std::max(fight, it->second);
            a.martial = clamp01(a.martial * 0.5 + fight * 0.5);
        }
    } else if (const RealmFigure* f = x.s.figure(p.ruler)) {
        a.martial = f->martial, a.ambition = f->ambition, a.greed = f->greed, a.honor = f->honor;
        a.wanderlust = f->wanderlust, a.renown = f->renown;
        for (const auto& t : f->traits)
            if (auto g = x.r.grudgeMult.find(t); g != x.r.grudgeMult.end()) a.grudge *= g->second;
    }
    a.martial = clamp01(a.martial), a.ambition = clamp01(a.ambition), a.greed = clamp01(a.greed);
    a.honor = clamp01(a.honor);
    return a;
}

std::vector<Character*> localPeople(Ctx& x, const RealmPolity& p) {
    std::vector<Character*> out;
    for (auto& [id, c] : x.w.characters())
        if (person(&c) && c.status.settlement == p.settlement) out.push_back(&c);
    return out;
}

std::vector<Character*> levy(Ctx& x, const RealmPolity& p) {
    std::vector<Character*> out;
    const int lo = inum(x.r.loc, "levy_from_years", 16), hi = inum(x.r.loc, "levy_to_years", 55);
    for (Character* c : localPeople(x, p))
        if (c->body.sex == 'm' && years(*c) >= lo && years(*c) <= hi && !c->captivity.held()) out.push_back(c);
    return out;
}

std::vector<WorldObject*> localStores(Ctx& x, const RealmPolity& p) {
    std::vector<WorldObject*> out;
    auto st = x.w.settlements().find(p.settlement);
    if (st == x.w.settlements().end()) return out;
    for (EntityId fid : st->second.families) {
        auto f = x.w.families().find(fid);
        if (f == x.w.families().end()) continue;
        if (WorldObject* o = x.w.object(f->second.storage)) out.push_back(o);
    }
    return out;
}

const ItemDef* itemDef(const Content& c, const std::string& id) {
    auto it = c.items.find(id);
    return it == c.items.end() ? nullptr : &it->second;
}

double itemFood(const Content& c, const std::string& item) {
    const ItemDef* d = itemDef(c, item);
    return d ? d->nutrition : 0.0;
}

// The village's numbers come from the real sim.
void syncLocal(Ctx& x, RealmPolity& p) {
    auto st = x.w.settlements().find(p.settlement);
    if (st == x.w.settlements().end()) {
        p.ruined = true;
        return;
    }
    const auto people = localPeople(x, p);
    p.pop = static_cast<double>(people.size());
    p.ruined = people.empty();
    p.ruler = st->second.chief;
    double food = 0.0, coin = 0.0;
    for (WorldObject* o : localStores(x, p))
        for (const auto& [item, n] : o->stock) {
            if (n <= 0) continue;
            if (item == "coin") coin += n;
            else food += n * itemFood(x.w.content(), item);
        }
    for (Character* c : people) coin += c->has("coin");
    const double meal = num(x.r.eco, "local_meal_per_day", 1.0);
    p.food = p.pop > 0 ? food / meal : 0.0; // person-days
    p.wealth = coin * num(x.r.eco, "local_wealth_per_coin", 1.0);
    p.militia = static_cast<double>(levy(x, p).size());
}

const RealmKind& kindOf(const RealmRules& r, const RealmPolity& p) {
    static const RealmKind kDef;
    auto it = r.kinds.find(p.kind);
    return it == r.kinds.end() ? kDef : it->second;
}

double heads(const RealmPolity& p) { return std::max(1.0, p.pop); }

std::vector<EntityId> mercsOf(const Ctx& x, EntityId polity) {
    std::vector<EntityId> out;
    for (const auto& q : x.s.quests)
        if (q.kind == "war_service" && q.status == "open" && q.polity == polity)
            for (EntityId t : q.takers)
                if (const RealmFigure* f = x.s.figure(t); f && f->alive && f->at == polity && f->travelTo == kNone)
                    out.push_back(t);
    return out;
}

} // namespace

double realmStrength(const World& w, const RealmState& s, const RealmPolity& p) {
    if (p.ruined) return 0.0;
    const RealmRules& r = realmRules(w.content());
    Ctx x{const_cast<World&>(w), const_cast<RealmState&>(s), r, w.day()};
    const Axes a = rulerAxes(x, p);
    double str = p.militia * (0.4 + p.morale) * (0.7 + 0.6 * a.martial);
    for (EntityId m : mercsOf(x, p.id))
        if (const RealmFigure* f = s.figure(m)) str += 4.0 * f->martial * f->health;
    for (const auto& f : s.figures)
        if (f.alive && f.home == p.id && f.at == p.id && f.role == "knight" && f.captor == kNone && !f.present)
            str += 3.0 * f.martial * f.health;
    return str;
}

namespace {

double strength(const Ctx& x, const RealmPolity& p) { return realmStrength(x.w, x.s, p); }

bool allied(const Ctx& x, EntityId a, EntityId b) {
    for (const auto& [p, q] : x.s.alliances)
        if ((p == a && q == b) || (p == b && q == a)) return true;
    return false;
}

void ally(Ctx& x, EntityId a, EntityId b) {
    if (!allied(x, a, b)) x.s.alliances.push_back({std::min(a, b), std::max(a, b)});
}

void unally(Ctx& x, EntityId a, EntityId b) {
    auto& v = x.s.alliances;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const auto& e) { return (e.first == a && e.second == b) || (e.first == b && e.second == a); }),
            v.end());
}

RealmWar* warBetween(Ctx& x, EntityId a, EntityId b) {
    for (auto& wr : x.s.wars) {
        if (wr.over) continue;
        auto side = [&](EntityId p, bool att) {
            if ((att ? wr.attacker : wr.defender) == p) return true;
            const auto& al = att ? wr.alliesA : wr.alliesD;
            return std::find(al.begin(), al.end(), p) != al.end();
        };
        if ((side(a, true) && side(b, false)) || (side(a, false) && side(b, true))) return &wr;
    }
    return nullptr;
}

int warsOf(const Ctx& x, EntityId p) {
    int n = 0;
    for (const auto& wr : x.s.wars)
        if (!wr.over && wr.attacker != wr.defender && (wr.attacker == p || wr.defender == p)) ++n;
    return n;
}

bool inTruce(Ctx& x, EntityId a, EntityId b) {
    const int truce = inum(x.r.pol, "truce_days", 360);
    for (const auto& wr : x.s.wars)
        if (wr.over && ((wr.attacker == a && wr.defender == b) || (wr.attacker == b && wr.defender == a)) &&
            x.day - wr.last < truce)
            return true;
    return false;
}

void grieve(Ctx& x, EntityId who, EntityId at, const std::string& cause, double weight) {
    if (who == at || who == kNone || at == kNone) return;
    RealmView& v = x.s.view(who, at);
    v.grievances.push_back({cause, weight, x.day});
    v.opinion = std::clamp(v.opinion - weight * 0.5, -1.0, 1.0);
}

// --- figures ---------------------------------------------------------------------

std::string pickTrait(Rng& rng, const Content& ct, const std::vector<std::string>& have) {
    std::vector<std::string> pool;
    for (const auto& [id, t] : ct.traits) pool.push_back(id);
    if (pool.empty()) return "";
    for (int tries = 0; tries < 6; ++tries) {
        const std::string& t = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
        bool clash = std::find(have.begin(), have.end(), t) != have.end();
        for (const auto& [a, b] : ct.exclusiveTraits)
            for (const auto& o : have) clash |= (a == t && b == o) || (b == t && a == o);
        if (!clash) return t;
    }
    return "";
}

RealmFigure& newFigure(Ctx& x, Rng& rng, EntityId home, const std::string& role, char sex, int ageYears,
                       const std::string& house) {
    RealmFigure f;
    f.id = x.w.newId();
    f.sex = sex;
    f.house = house;
    for (int tries = 0; tries < 10; ++tries) {
        f.name = randomPersonName(rng, sex == 'f', &x.w.content()) + " " + house;
        const bool taken = std::any_of(x.s.figures.begin(), x.s.figures.end(),
                                       [&](const RealmFigure& o) { return o.alive && o.name == f.name; }) ||
                           std::any_of(x.w.characters().begin(), x.w.characters().end(),
                                       [&](const auto& kv) { return kv.second.name == f.name; });
        if (!taken) break;
    }
    f.born = x.day - ageYears * kDaysPerYear - rng.irange(0, kDaysPerYear - 1);
    f.home = f.at = home;
    f.role = role;
    const int nt = inum(x.r.fig, "traits", 2);
    for (int i = 0; i < nt; ++i)
        if (auto t = pickTrait(rng, x.w.content(), f.traits); !t.empty()) f.traits.push_back(t);
    Axes a = axesOfTraits(x.r, f.traits);
    f.martial = clamp01(a.martial + rng.range(-0.2, 0.35) + (sex == 'm' ? 0.1 : -0.1));
    f.ambition = clamp01(a.ambition + rng.range(-0.25, 0.4));
    f.greed = clamp01(a.greed + rng.range(-0.2, 0.35));
    f.honor = clamp01(a.honor + rng.range(-0.3, 0.3));
    f.wanderlust = clamp01(a.wanderlust + rng.range(-0.15, 0.4));
    f.renown = role == "ruler" ? rng.range(0.15, 0.45) : (role == "knight" ? rng.range(0.1, 0.3) : rng.range(0.0, 0.1));
    f.wealth = role == "ruler" ? rng.range(20, 60) : rng.range(2, 15);
    x.s.figures.push_back(std::move(f));
    return x.s.figures.back();
}

void marry(RealmFigure& a, RealmFigure& b) {
    a.spouse = b.id;
    b.spouse = a.id;
}

RealmFigure& childOf(Ctx& x, Rng& rng, RealmFigure& father, RealmFigure& mother, int ageYears) {
    const std::string house = father.house;
    const EntityId fid = father.id, mid = mother.id, home = father.home;
    RealmFigure& k = newFigure(x, rng, home, "kin", rng.chance(0.5) ? 'm' : 'f', ageYears, house);
    k.father = fid;
    k.mother = mid;
    const EntityId kid = k.id;
    if (RealmFigure* f = x.s.figure(fid)) f->children.push_back(kid);
    if (RealmFigure* m = x.s.figure(mid)) m->children.push_back(kid);
    return *x.s.figure(kid);
}

std::string pickHouse(Ctx& x, Rng& rng, std::set<std::string>& used) {
    for (int i = 0; i < 12; ++i) {
        const std::string h = randomFamilyName(rng, &x.w.content());
        if (!used.count(h)) {
            used.insert(h);
            return h;
        }
    }
    return randomFamilyName(rng, &x.w.content());
}

// --- genesis ---------------------------------------------------------------------

void genesis(Ctx& x) {
    RealmState& s = x.s;
    s.inited = true;
    Rng rng = agentRng(x.w.seed(), 0, 0, "realm:genesis");
    std::set<std::string> houses, places;
    for (const auto& [id, f] : x.w.families()) houses.insert(f.name);
    // the village of the map
    for (const auto& [sid, st] : x.w.settlements()) {
        RealmPolity p;
        p.id = x.w.newId();
        p.kind = "village";
        p.name = st.name;
        p.settlement = sid;
        p.institution = st.institution;
        s.polities.push_back(p);
        syncLocal(x, s.polities.back());
        break; // one village is played
    }
    // the region around it
    std::vector<std::pair<std::string, int>> layout = x.r.layout;
    const double tau = 6.283185307179586;
    double ang0 = rng.range(0, tau);
    int slot = 0, total = 0;
    for (const auto& [k, n] : layout) total += n;
    for (const auto& [kind, n] : layout)
        for (int i = 0; i < n; ++i, ++slot) {
            const RealmKind& kd = x.r.kinds.count(kind) ? x.r.kinds.at(kind) : RealmKind{};
            RealmPolity p;
            p.id = x.w.newId();
            p.kind = kind;
            const auto& pool = x.r.names.count(kind) ? x.r.names.at(kind) : x.r.names.at("*");
            std::string nm;
            for (int t = 0; t < 10 && !pool.empty(); ++t) {
                nm = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
                if (!places.count(nm)) break;
            }
            if (nm.empty() || places.count(nm)) nm = kind + std::to_string(slot);
            places.insert(nm);
            p.name = nm;
            const double ang = ang0 + tau * slot / std::max(1, total) + rng.range(-0.35, 0.35);
            const double d = rng.range(x.r.regionKm[0], x.r.regionKm[1]);
            p.pos = {std::cos(ang) * d, std::sin(ang) * d};
            p.pop = std::round(rng.range(kd.pop[0], kd.pop[1]));
            p.food = p.pop * rng.range(kd.foodDays[0], kd.foodDays[1]);
            p.wealth = p.pop * kd.wealthPerHead * rng.range(0.7, 1.3);
            p.militia = std::round(p.pop * kd.militiaShare);
            p.morale = rng.range(0.5, 0.7);
            p.unrest = rng.range(0.05, 0.2);
            p.institution = kd.institution;
            p.house = kind == "monastery" ? "" : pickHouse(x, rng, houses);
            s.polities.push_back(p);
            const EntityId pid = p.id;
            // the ruling family
            const bool celibate = kd.celibate;
            const std::string role = "ruler";
            const char sex = kind == "monastery" ? (rng.chance(0.3) ? 'f' : 'm') : (rng.chance(0.15) ? 'f' : 'm');
            RealmFigure& ruler = newFigure(x, rng, pid, role, sex, rng.irange(28, 58),
                                           celibate ? pickHouse(x, rng, houses) : p.house);
            if (kind == "bandit_camp") ruler.honor = clamp01(ruler.honor - 0.35), ruler.greed = clamp01(ruler.greed + 0.3);
            const EntityId rid = ruler.id;
            s.polity(pid)->ruler = rid;
            if (!celibate && kind != "bandit_camp" && rng.chance(0.85)) {
                RealmFigure& sp = newFigure(x, rng, pid, "consort", sex == 'm' ? 'f' : 'm',
                                            std::max(18, s.figure(rid)->ageYears(x.day) - rng.irange(-3, 8)),
                                            pickHouse(x, rng, houses));
                const EntityId spid = sp.id;
                marry(*s.figure(rid), *s.figure(spid));
                const EntityId faId = s.figure(rid)->sex == 'm' ? rid : spid;
                const EntityId moId = s.figure(rid)->sex == 'm' ? spid : rid;
                const int maxAge = std::max(1, s.figure(moId)->ageYears(x.day) - 18);
                const int kids = rng.irange(kd.children[0], kd.children[1]);
                for (int k = 0; k < kids; ++k) {
                    RealmFigure& ch = childOf(x, rng, *s.figure(faId), *s.figure(moId), rng.irange(0, std::min(maxAge, 26)));
                    ch.house = s.polity(pid)->house;
                    ch.name = ch.name.substr(0, ch.name.rfind(' ')) + " " + ch.house;
                }
                // the heir: the eldest child
                const RealmFigure* r0 = s.figure(rid);
                EntityId heir = kNone;
                int best = -1;
                for (EntityId c : r0->children)
                    if (const RealmFigure* k = s.figure(c); k && k->ageYears(x.day) > best) best = k->ageYears(x.day), heir = c;
                s.polity(pid)->heir = heir;
                if (RealmFigure* h = s.figure(heir)) h->role = "heir";
            }
            const int kn = rng.irange(kd.knights[0], kd.knights[1]);
            for (int k = 0; k < kn; ++k) {
                RealmFigure& f = newFigure(x, rng, pid, kind == "bandit_camp" ? "outlaw" : "knight", 'm',
                                           rng.irange(20, 45), pickHouse(x, rng, houses));
                f.martial = clamp01(f.martial + 0.25);
                if (kind == "bandit_camp") f.life = "outlaw", f.honor = clamp01(f.honor - 0.3);
            }
            if (kind == "bandit_camp") s.polity(pid)->ruler = rid, s.figure(rid)->life = "outlaw";
        }
    // views: neighbours start mildly wary or friendly; bandits are everyone's enemy
    for (auto& a : s.polities)
        for (auto& b : s.polities) {
            if (a.id == b.id) continue;
            RealmView& v = s.view(a.id, b.id);
            v.opinion = rng.range(-0.2, 0.3);
            if (a.kind == "bandit_camp" || b.kind == "bandit_camp") v.opinion = -0.6;
            if (a.kind == "monastery" || b.kind == "monastery") v.opinion = std::max(v.opinion, 0.2);
        }
    // an old border grudge or two (the region has a past, AL-85)
    for (size_t i = 0; i < s.polities.size(); ++i)
        for (size_t j = i + 1; j < s.polities.size(); ++j) {
            const auto& a = s.polities[i];
            const auto& b = s.polities[j];
            if (a.kind == "bandit_camp" || b.kind == "bandit_camp" || a.kind == "monastery" || b.kind == "monastery") continue;
            if (dkm(a.pos, b.pos) < num(x.r.pol, "neighbour_km", 25) && rng.chance(0.4)) {
                const bool which = rng.chance(0.5);
                const EntityId from = which ? a.id : b.id, at = which ? b.id : a.id;
                s.view(from, at).grievances.push_back({"border", rng.range(0.2, 0.5), x.day - rng.irange(30, 600)});
            }
        }
    // wanderers already on the road
    const auto ws = pair2(x.r.fig, "wanderers_start", {4, 6});
    const int nw = rng.irange(static_cast<int>(ws[0]), static_cast<int>(ws[1]));
    std::vector<EntityId> homes;
    for (const auto& p : s.polities)
        if (p.settlement == kNone && p.kind != "bandit_camp") homes.push_back(p.id);
    for (int i = 0; i < nw && !homes.empty(); ++i) {
        const EntityId home = homes[static_cast<size_t>(rng.irange(0, static_cast<int>(homes.size()) - 1))];
        RealmFigure& f = newFigure(x, rng, home, "wanderer", rng.chance(0.82) ? 'm' : 'f', rng.irange(19, 40),
                                   pickHouse(x, rng, houses));
        const auto m = pair2(x.r.wan, "start_martial", {0.35, 0.8});
        f.martial = rng.range(m[0], m[1]);
        f.wanderlust = clamp01(f.wanderlust + 0.35);
        f.life = "road";
        const auto wl = pair2(x.r.wan, "start_wealth", {3, 20});
        f.wealth = rng.range(wl[0], wl[1]);
    }
    log(x, "region", {{"n", static_cast<int>(s.polities.size()) - 1}});
}

// --- materialization: a figure in the village --------------------------------------

Vec2 arrivalPoint(Ctx& x, const RealmPolity& local, Vec2 fromKm, Rng& rng) {
    auto st = x.w.settlements().find(local.settlement);
    Vec2 c = st != x.w.settlements().end() ? st->second.center : Vec2{x.w.map().sizeM * 0.5, x.w.map().sizeM * 0.5};
    double len = std::hypot(fromKm.x, fromKm.y);
    Vec2 dir = len > 1e-6 ? Vec2{fromKm.x / len, fromKm.y / len} : Vec2{1.0, 0.0};
    const double d = rng.range(150.0, 260.0);
    Vec2 p{c.x + dir.x * d, c.y + dir.y * d};
    const double sz = x.w.map().sizeM;
    if (sz > 0) p.x = std::clamp(p.x, 5.0, sz - 5.0), p.y = std::clamp(p.y, 5.0, sz - 5.0);
    return p;
}

Character* materialize(Ctx& x, RealmFigure& f, int stayDays) {
    RealmPolity* local = x.s.local();
    if (!local || local->ruined || f.present || !f.alive) return nullptr;
    if (x.w.character(f.id)) return nullptr;
    Rng rng = rngOf(x, f.id, "realm:arrive");
    const RealmPolity* from = x.s.polity(f.at);
    Character c;
    if (f.parked.is_object()) {
        c = f.parked.get<Character>();
    } else {
        const Content& ct = x.w.content();
        c.name = f.name;
        c.body.sex = f.sex;
        c.traits = f.traits;
        for (const auto& n : ct.needs) c.needs[n.id] = rng.range(std::max(0.5, n.startLo), std::max(0.7, n.startHi));
        for (const auto& k : ct.householdMust) {
            c.knowledge.insert(k);
            c.skills[k] = rng.range(0.2, 0.45);
        }
        for (const char* k : {"hunting", "combat"})
            if (ct.knowledge.count(k)) {
                c.knowledge.insert(k);
                c.skills[k] = clamp01(0.2 + f.martial * 0.7);
            }
        c.status.role = f.role == "wanderer" ? "wanderer" : "guest";
        for (const char* it : {"spear"})
            if (itemDef(ct, it)) c.inventory[it] = 1;
        if (itemDef(ct, f.martial > 0.6 ? "mail_shirt" : "gambeson")) c.inventory[f.martial > 0.6 ? "mail_shirt" : "gambeson"] = 1;
        for (const char* it : {"bread", "dried_meat"})
            if (itemDef(ct, it)) c.inventory[it] = 3;
    }
    c.id = f.id;
    c.body.ageDays = std::max(0, x.day - f.born);
    c.body.alive = true;
    c.body.health = std::clamp(f.health, 0.3, 1.0);
    c.body.downUntil = 0;
    c.status.family = kNone;
    c.status.settlement = kNone;
    c.status.spouse = kNone;
    c.ai.queue.clear();
    c.ai.goal = GoalInstance{};
    c.ai.plan = Plan{};
    if (c.need("hunger") < 0.5) c.needs["hunger"] = 0.6;
    if (c.need("thirst") < 0.5) c.needs["thirst"] = 0.6;
    const int coin = static_cast<int>(std::floor(f.wealth));
    if (itemDef(x.w.content(), "coin")) c.inventory["coin"] = std::max(c.inventory["coin"], coin);
    c.pos = arrivalPoint(x, *local, from ? from->pos : Vec2{1, 0}, rng);
    for (const auto& [oid, o] : x.w.objects())
        if (!o.ownerFamily && dist(o.pos, c.pos) <= 1500.0) c.knownObjects.insert(oid);
    Character& in = x.w.addCharacter(std::move(c));
    f.present = true;
    f.at = local->id;
    f.leaveDay = x.day + std::max(1, stayDays);
    // the village sees a stranger
    for (auto& [id, o] : x.w.characters()) {
        if (!person(&o) || id == in.id || o.status.settlement != local->settlement) continue;
        Relation& to = o.relations[in.id];
        to.familiarity = std::max(to.familiarity, 0.03);
    }
    return &in;
}

// Whether a present figure can leave now (not bound to the village).
bool canLeave(const Ctx& x, const Character& c) {
    if (!c.body.alive) return true;
    if (c.captivity.held() || c.body.downUntil > x.w.now() || c.ai.social.active()) return false;
    if (c.status.spouse != kNone || c.status.family != kNone) return false;
    return true;
}

// The Character goes back into its figure (parked), and leaves the map.
void park(Ctx& x, RealmFigure& f) {
    Character* c = x.w.character(f.id);
    f.present = false;
    if (!c) return;
    if (!c->body.alive) {
        f.alive = false;
        f.death = c->body.causeOfDeath.empty() ? "village" : c->body.causeOfDeath;
        f.deathDay = x.day;
        f.parked = Json();
        return; // the body stays where it fell
    }
    f.health = c->body.health;
    f.wealth = std::max(0.0, static_cast<double>(c->has("coin")));
    Character keep = *c;
    keep.ai.queue.clear();
    keep.ai.goal = GoalInstance{};
    keep.ai.plan = Plan{};
    keep.ai.trace.clear();
    f.parked = keep;
    const EntityId id = f.id;
    x.w.characters().erase(id);
    for (auto& [oid, o] : x.w.characters()) {
        auto& q = o.ai.queue;
        q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedTask& t) { return t.target == id; }), q.end());
    }
}

// A figure that settled in the village (married in, taken into a household) is a villager now.
void settleInVillage(Ctx& x, RealmFigure& f) {
    f.present = false;
    f.life = "settled";
    f.role = "commoner";
    if (RealmPolity* l = x.s.local()) f.home = f.at = l->id;
    f.parked = Json();
    log(x, "wanderer_settles", {{"who", f.id}});
}

// --- local needs: quests the village posts ------------------------------------------

EntityId giverOf(Ctx& x, const RealmPolity& p) { return p.ruler; }

bool hasOpenQuest(const Ctx& x, const std::string& kind, EntityId polity, EntityId target) {
    for (const auto& q : x.s.quests)
        if (q.status == "open" && q.kind == kind && q.polity == polity && (target == kNone || q.target == target)) return true;
    return false;
}

RealmQuest* postQuest(Ctx& x, const std::string& kind, EntityId polity, EntityId giver, EntityId target, double reward) {
    if (!x.r.questOn(kind)) return nullptr;
    const RealmQuestKind& qk = x.r.quests.at(kind);
    RealmQuest q;
    q.id = static_cast<EntityId>(x.s.nextId++);
    q.kind = kind;
    q.polity = polity;
    q.giver = giver;
    q.target = target;
    q.reward = std::round(reward);
    q.posted = x.day;
    q.deadline = x.day + qk.days;
    x.s.quests.push_back(q);
    log(x, "quest_posted", {{"giver", giver}, {"quest", kind}, {"pol", polity}, {"n", static_cast<int>(q.reward)}},
        x.w.character(giver) ? std::vector<EntityId>{giver} : std::vector<EntityId>{});
    return &x.s.quests.back();
}

// What the village can pay: coin from the chief's household and the rest in grain.
double localPurse(Ctx& x, const RealmPolity& p) {
    const Character* chief = x.w.character(p.ruler);
    if (!chief) return 0.0;
    double coin = chief->has("coin");
    auto f = x.w.families().find(chief->status.family);
    if (f != x.w.families().end())
        if (const WorldObject* o = x.w.object(f->second.storage)) {
            auto it = o->stock.find("coin");
            if (it != o->stock.end()) coin += it->second;
            auto g = o->stock.find("grain");
            if (g != o->stock.end()) coin += g->second * 0.5;
        }
    return coin;
}

// Pays `amount` from the chief's purse to the figure; returns what was paid.
double payFromVillage(Ctx& x, const RealmPolity& p, double amount, RealmFigure* to) {
    Character* chief = x.w.character(p.ruler);
    if (!chief || amount <= 0) return 0.0;
    double left = amount;
    auto take = [&](std::map<std::string, int>& stock, const std::string& item, double rate) {
        auto it = stock.find(item);
        if (it == stock.end() || it->second <= 0 || left <= 0) return;
        const int n = std::min(it->second, static_cast<int>(std::ceil(left / rate)));
        it->second -= n;
        left -= n * rate;
    };
    take(chief->inventory, "coin", 1.0);
    auto f = x.w.families().find(chief->status.family);
    if (f != x.w.families().end())
        if (WorldObject* o = x.w.object(f->second.storage)) {
            take(o->stock, "coin", 1.0);
            take(o->stock, "grain", 0.5);
        }
    const double paid = amount - std::max(0.0, left);
    if (to) {
        to->wealth += paid;
        if (Character* c = x.w.character(to->id); c && itemDef(x.w.content(), "coin")) c->inventory["coin"] += static_cast<int>(paid);
    }
    return paid;
}

void localNeeds(Ctx& x, RealmPolity& p) {
    if (p.ruined || !x.r.questOn("beast_bounty")) return;
    const Character* chief = x.w.character(p.ruler);
    if (!person(chief) || chief->controller.isPlayer()) return; // the player posts with realm.post_quest
    const Tick since = x.w.now() - static_cast<Tick>(inum(x.r.loc, "beast_memory_days", 30)) * kTicksPerDay;
    std::map<EntityId, int> mauled; // pack -> attacks on villagers lately
    for (Character* c : localPeople(x, p))
        for (const auto& m : c->memory)
            if (m.type == "attacked_by_beast" && m.tick >= since && !m.who.empty()) ++mauled[m.who[0]];
    for (const auto& [pack, n] : mauled) {
        const Character* beast = x.w.character(pack);
        if (!beast || !beast->body.alive || beast->group <= 0 || hasOpenQuest(x, "beast_bounty", p.id, pack)) continue;
        const RealmQuestKind& qk = x.r.quests.at("beast_bounty");
        const double purse = localPurse(x, p);
        const double reward = std::min(purse * 0.5, qk.reward[1]);
        if (reward < qk.reward[0] * 0.5) continue; // nothing to pay with
        postQuest(x, "beast_bounty", p.id, p.ruler, pack, reward);
    }
}

// --- raids ----------------------------------------------------------------------------

void hurtVillager(Ctx& x, Character& c, double dmg, const std::string& cause, EntityId by) {
    c.body.health = clamp01(c.body.health - dmg);
    MemoryEntry m;
    m.type = cause == "battle" ? "fought_war" : "raided";
    m.valence = -0.7;
    m.salience = 0.8;
    x.w.remember(c, m);
    if (c.body.health <= 0.0 && c.body.alive && !c.controller.isPlayer()) {
        c.body.alive = false;
        c.body.causeOfDeath = cause;
        x.w.chronicle("death", {c.id}, "chronicle.death", {{"who", c.id}, {"cause", cause}});
        log(x, cause == "battle" ? "levy_fell" : "villager_killed_raid", {{"who", c.id}, {"pol", by}}, {c.id});
    } else {
        if (c.body.health <= 0.0) c.body.health = 0.05;
        log(x, cause == "battle" ? "levy_wounded" : "villager_hurt_raid", {{"who", c.id}, {"pol", by}}, {c.id});
    }
}

double raidLocal(Ctx& x, RealmPolity& by, RealmPolity& on, double share) {
    double taken = 0.0;
    std::map<std::string, int> loot;
    for (WorldObject* o : localStores(x, on))
        for (auto& [item, n] : o->stock) {
            if (n <= 0) continue;
            const double food = itemFood(x.w.content(), item);
            if (item != "coin" && food <= 0.0) continue;
            const int t = static_cast<int>(std::floor(n * share));
            if (t <= 0) continue;
            n -= t;
            loot[item] += t;
            taken += item == "coin" ? t : t * food;
        }
    Rng rng = rngOf(x, on.id, "realm:raid_hurt");
    auto people = levy(x, on);
    const auto hp = pair2(x.r.loc, "raid_hurt_people", {1, 2});
    const int n = std::min(static_cast<int>(people.size()), rng.irange(static_cast<int>(hp[0]), static_cast<int>(hp[1])));
    const auto hurt = pair2(x.r.loc, "raid_hurt", {0.15, 0.55});
    for (int i = 0; i < n && !people.empty(); ++i) {
        const size_t k = static_cast<size_t>(rng.irange(0, static_cast<int>(people.size()) - 1));
        hurtVillager(x, *people[k], rng.range(hurt[0], hurt[1]), "raid", by.id);
        people.erase(people.begin() + static_cast<long>(k));
    }
    for (Character* c : localPeople(x, on)) {
        if (c->body.stage() == "child") continue;
        MemoryEntry m;
        m.type = "raided";
        m.valence = -0.6;
        m.salience = 0.6;
        x.w.remember(*c, m);
        if (auto md = x.w.content().moodlets.find("frightened"); md != x.w.content().moodlets.end())
            c->moodlets.push_back({"frightened", md->second.value, x.w.now() + md->second.ttl});
    }
    by.food += taken;
    by.wealth += loot.count("coin") ? loot["coin"] : 0;
    Json goods = Json::object();
    for (const auto& [k, v] : loot) goods[k] = v;
    log(x, "raid_village", {{"pol", by.id}, {"goods", goods}, {"n", static_cast<int>(std::round(taken))}});
    return taken;
}

} // namespace

double realmRaid(World& w, RealmState& s, EntityId byId, EntityId onId) {
    const RealmRules& r = realmRules(w.content());
    Ctx x{w, s, r, w.day()};
    RealmPolity* by = s.polity(byId);
    RealmPolity* on = s.polity(onId);
    if (!by || !on || by->ruined || on->ruined) return 0.0;
    Rng rng = rngOf(x, by->id, "realm:raid");
    const double sa = strength(x, *by) * rng.range(0.7, 1.3);
    const double sd = strength(x, *on) * rng.range(0.7, 1.3) * num(r.war, "defender_bonus", 1.25);
    by->lastRaid = x.day;
    on->lastRaided = x.day;
    grieve(x, on->id, by->id, "raid", 0.25);
    on->unrest = clamp01(on->unrest + 0.03);
    if (sa <= sd || sa <= 0.0) {
        by->militia = std::max(0.0, by->militia * (1.0 - rng.range(0.08, 0.2)));
        by->morale = clamp01(by->morale - 0.05);
        on->morale = clamp01(on->morale + 0.03);
        log(x, on->settlement != kNone ? "raid_village_repelled" : "raid_repelled", {{"pol", by->id}, {"pol2", on->id}});
        if (on->settlement != kNone) {
            auto lv = levy(x, *on);
            if (!lv.empty() && rng.chance(0.5)) {
                const auto hurt = pair2(r.loc, "raid_hurt", {0.15, 0.55});
                hurtVillager(x, *lv[static_cast<size_t>(rng.irange(0, static_cast<int>(lv.size()) - 1))],
                             rng.range(hurt[0], hurt[1]) * 0.6, "raid", by->id);
            }
        }
        return 0.0;
    }
    const auto lr = pair2(r.war, "raid_loot", {0.15, 0.4});
    const double share = std::clamp(rng.range(lr[0], lr[1]) * std::min(1.5, sa / std::max(1.0, sd)), 0.03, 0.35);
    double taken = 0.0;
    if (on->settlement != kNone) {
        taken = raidLocal(x, *by, *on, share);
    } else {
        const double food = std::max(0.0, on->food) * share;
        const double coin = std::max(0.0, on->wealth) * share;
        on->food -= food, on->wealth -= coin;
        by->food += food, by->wealth += coin;
        on->pop = std::max(1.0, on->pop - std::round(rng.range(0, 3)));
        on->militia = std::max(0.0, on->militia * (1.0 - rng.range(0.03, 0.1)));
        taken = food + coin;
        log(x, "raid", {{"pol", by->id}, {"pol2", on->id}, {"n", static_cast<int>(std::round(taken))}});
    }
    by->morale = clamp01(by->morale + 0.04);
    on->morale = clamp01(on->morale - 0.04);
    // the raided want the raiders gone
    if (by->kind == "bandit_camp" && !hasOpenQuest(x, "bandit_bounty", on->id, by->id)) {
        const RealmQuestKind* qk = r.quests.count("bandit_bounty") ? &r.quests.at("bandit_bounty") : nullptr;
        if (qk) {
            if (on->settlement != kNone) {
                const Character* chief = w.character(on->ruler);
                if (person(chief) && !chief->controller.isPlayer()) {
                    const double reward = std::min(localPurse(x, *on) * 0.6, qk->reward[1]);
                    if (reward >= qk->reward[0] * 0.4) postQuest(x, "bandit_bounty", on->id, on->ruler, by->id, reward);
                }
            } else if (on->wealth > qk->reward[0] * 1.5) {
                const double reward = std::min(on->wealth * 0.3, qk->reward[1]);
                on->wealth -= reward * 0.0; // paid when done
                postQuest(x, "bandit_bounty", on->id, on->ruler, by->id, reward);
            }
        }
    }
    return taken;
}

namespace {

double raid(Ctx& x, RealmPolity& by, RealmPolity& on) { return realmRaid(x.w, x.s, by.id, on.id); }

// --- economy ---------------------------------------------------------------------------

void economyDaily(Ctx& x) {
    const int year = x.day / kDaysPerYear;
    if (year != x.s.year) {
        x.s.year = year;
        Rng rng = agentRng(x.w.seed(), 0, static_cast<Tick>(year), "realm:harvest");
        // the sum of three uniforms: a rough bell around the mean
        const double sd = num(x.r.eco, "harvest_sd", 0.28);
        const double z = (rng.uniform() + rng.uniform() + rng.uniform() - 1.5) * 2.0;
        x.s.harvest = std::max(0.15, num(x.r.eco, "harvest_mean", 1.0) + z * sd);
    }
    const bool harvestDay = x.w.season() == "autumn" && (x.day == 0 || [&] {
        // the first day of autumn
        const int doy = (x.w.startDay() + x.day) % kDaysPerYear;
        return doy % 90 == 0;
    }());
    if (harvestDay && x.day > 0) {
        if (x.s.harvest < 0.75) log(x, "harvest_bad", {{"n", static_cast<int>(std::round(x.s.harvest * 100))}});
        else if (x.s.harvest > 1.25) log(x, "harvest_good", {{"n", static_cast<int>(std::round(x.s.harvest * 100))}});
    }
    const double yieldDays = num(x.r.eco, "year_yield_days", 400);
    const double cap = num(x.r.eco, "food_cap_days", 420);
    const double growth = num(x.r.eco, "growth_per_year", 0.025) / kDaysPerYear;
    for (auto& p : x.s.polities) {
        if (p.ruined || p.settlement != kNone) continue;
        const RealmKind& kd = kindOf(x.r, p);
        Rng rng = rngOf(x, p.id, "realm:eco");
        if (harvestDay && x.day > 0) {
            const double local = 1.0 + (rng.uniform() - 0.5) * 2.0 * num(x.r.eco, "harvest_local_sd", 0.12);
            p.food += p.pop * yieldDays * kd.yield * x.s.harvest * local;
        }
        p.food = std::min(p.food, p.pop * cap);
        p.food -= p.pop;
        p.wealth += p.pop * kd.incomePerHeadYear / kDaysPerYear;
        const double target = std::round(p.pop * kd.militiaShare);
        if (p.militia < target) p.militia = std::min(target, p.militia + std::max(0.05, target * num(x.r.eco, "militia_recover_per_day", 0.01)));
        p.morale += (0.6 - p.morale) * 0.01;
        p.unrest = clamp01(p.unrest - num(x.r.rev, "decay_per_day", 0.0025));
        if (p.food < 0.0) {
            // famine: the weak die, the rest grumble and some leave
            p.food = 0.0;
            const double lost = std::max(0.0, p.pop * num(x.r.eco, "famine_death_per_day", 0.004));
            p.pop = std::max(0.0, p.pop - lost);
            if (kd.livesByRaids) {
                p.militia = std::max(0.0, p.militia * (1.0 - num(x.r.ban, "starve_desert", 0.02)));
                p.pop = std::max(p.militia, p.pop);
            }
            p.unrest = clamp01(p.unrest + num(x.r.eco, "famine_unrest_per_day", 0.012));
            if (!x.s.stats.count("famine_" + std::to_string(p.id) + "_" + std::to_string(x.day / 60))) {
                x.s.stats["famine_" + std::to_string(p.id) + "_" + std::to_string(x.day / 60)] = 1;
                log(x, "famine", {{"pol", p.id}});
            }
            // refugees: to the village of the map when it is near
            RealmPolity* local = x.s.local();
            if (local && !local->ruined && local->pop < num(x.r.eco, "refugee_village_cap", 40) &&
                dkm(p.pos, local->pos) <= num(x.r.eco, "refugee_reach_km", 45) &&
                x.day - p.lastRefugees >= inum(x.r.eco, "refugee_gap_days", 18) &&
                rng.chance(num(x.r.eco, "refugee_chance_per_day", 0.06)) && !kd.livesByRaids) {
                p.lastRefugees = x.day;
                const int before = static_cast<int>(local->pop);
                if (strangersArrive(x.w, local->settlement, rng.chance(0.5))) {
                    p.pop = std::max(0.0, p.pop - 2.0);
                    log(x, "refugees", {{"pol", p.id}, {"n", std::max(1, static_cast<int>(localPeople(x, *local).size()) - before)}});
                }
            }
        } else if (p.food > p.pop * 30) {
            p.pop += p.pop * growth;
        }
        if (p.pop < 1.0 && !p.ruined) {
            p.ruined = true;
            log(x, kd.livesByRaids ? "camp_disbanded" : "polity_ruined", {{"pol", p.id}});
        }
    }
}

// --- figures: life and death ----------------------------------------------------------------

void succession(Ctx& x, RealmPolity& p, const std::string& why);

void figureDies(Ctx& x, RealmFigure& fig, const std::string& cause) {
    if (!fig.alive) return;
    const EntityId fid = fig.id;
    RealmFigure& f = *x.s.figure(fid);
    if (f.present) {
        if (Character* c = x.w.character(f.id); c && c->body.alive) {
            c->body.alive = false;
            c->body.causeOfDeath = cause;
            x.w.chronicle("death", {c->id}, "chronicle.death", {{"who", c->id}, {"cause", cause}});
        }
        f.present = false;
    }
    f.alive = false;
    f.death = cause;
    f.deathDay = x.day;
    f.parked = Json();
    if (RealmFigure* sp = x.s.figure(f.spouse)) sp->spouse = kNone;
    log(x, "fig_died", {{"fig", fid}, {"cause", cause}, {"pol", f.home}});
    for (auto& p : x.s.polities)
        if (p.ruler == fid && p.settlement == kNone) succession(x, p, cause);
        else if (p.heir == fid) p.heir = kNone;
}

double ageHazard(const RealmRules& r, int y) {
    const int from = inum(r.fig, "death_from_years", 45);
    if (y < from) return num(r.fig, "ill_daily", 0.00003);
    return num(r.fig, "death_daily_at_from", 0.00006) * std::pow(2.0, (y - from) / num(r.fig, "double_years", 8)) +
           num(r.fig, "ill_daily", 0.00003);
}

void figuresDaily(Ctx& x) {
    const int n = static_cast<int>(x.s.figures.size());
    for (int i = 0; i < n; ++i) {
        RealmFigure& f = x.s.figures[static_cast<size_t>(i)];
        if (!f.alive || f.present) continue;
        Rng rng = rngOf(x, f.id, "realm:life");
        if (f.health < 1.0) f.health = std::min(1.0, f.health + num(x.r.wan, "heal_per_day", 0.03));
        if (rng.chance(ageHazard(x.r, f.ageYears(x.day)))) {
            figureDies(x, x.s.figures[static_cast<size_t>(i)], f.ageYears(x.day) >= 55 ? "old_age" : "illness");
            continue;
        }
    }
    // births (once a year per couple, on its own day)
    for (int i = 0; i < n; ++i) {
        RealmFigure& m = x.s.figures[static_cast<size_t>(i)];
        if (!m.alive || m.sex != 'f' || m.spouse == kNone || m.present) continue;
        if ((x.day + static_cast<int>(m.id)) % kDaysPerYear != 0) continue;
        RealmFigure* fa = x.s.figure(m.spouse);
        if (!fa || !fa->alive) continue;
        const auto ff = pair2(x.r.fig, "fertile_f", {17, 42});
        const int y = m.ageYears(x.day);
        if (y < ff[0] || y > ff[1]) continue;
        Rng rng = rngOf(x, m.id, "realm:birth");
        if (!rng.chance(num(x.r.fig, "birth_yearly", 0.3))) continue;
        const EntityId mid = m.id;
        RealmFigure& k = childOf(x, rng, *fa, m, 0);
        const EntityId kid = k.id;
        RealmFigure* mo = x.s.figure(mid);
        log(x, "birth", {{"fig", kid}, {"fig2", mo->id}, {"pol", mo->home}});
        for (auto& p : x.s.polities)
            if ((p.ruler == mo->id || p.ruler == mo->spouse) && p.heir == kNone) {
                p.heir = kid;
                x.s.figure(kid)->role = "heir";
            }
    }
}

int aliveFigures(const Ctx& x) {
    int n = 0;
    for (const auto& f : x.s.figures) n += f.alive ? 1 : 0;
    return n;
}

// New notables: a smith's son who took up the sword, a widow's daughter with a grudge...
void notables(Ctx& x) {
    if (aliveFigures(x) >= inum(x.r.fig, "max_alive", 120)) return;
    Rng rng = agentRng(x.w.seed(), 0, x.w.now(), "realm:notables");
    const double perDay = num(x.r.fig, "notables_yearly", 2.0) / kDaysPerYear;
    if (!rng.chance(perDay)) return;
    std::vector<EntityId> homes;
    std::vector<double> wts;
    double sum = 0.0;
    for (const auto& p : x.s.polities)
        if (!p.ruined && p.settlement == kNone && p.kind != "monastery") homes.push_back(p.id), wts.push_back(p.pop), sum += p.pop;
    if (homes.empty()) return;
    double pick = rng.range(0, sum);
    size_t k = 0;
    for (; k + 1 < homes.size() && pick > wts[k]; ++k) pick -= wts[k];
    std::set<std::string> used;
    RealmFigure& f = newFigure(x, rng, homes[k], "commoner", rng.chance(0.75) ? 'm' : 'f', rng.irange(17, 30),
                               pickHouse(x, rng, used));
    const RealmPolity* p = x.s.polity(homes[k]);
    if (p && (p->unrest > 0.5 || p->food <= 0)) f.greed = clamp01(f.greed + 0.2), f.honor = clamp01(f.honor - 0.15);
    f.wanderlust = clamp01(f.wanderlust + 0.15);
    log(x, "notable", {{"fig", f.id}, {"pol", f.home}});
}

// Life strategy once a season: settle, the road, or outside the law.
void lifestyle(Ctx& x) {
    const int every = inum(x.r.fig, "lifestyle_every_days", 90);
    const double hyst = num(x.r.fig, "lifestyle_hysteresis", 0.15);
    for (auto& f : x.s.figures) {
        if (!f.alive || f.present || f.travelTo != kNone || f.captor != kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % every != 0) continue;
        if (f.role == "ruler" || f.role == "consort" || f.role == "heir") continue;
        if (f.ageYears(x.day) < inum(x.r.suc, "adult_years", 16)) continue;
        const RealmPolity* home = x.s.polity(f.home);
        const bool rooted = home && !home->ruined && f.claim == kNone;
        const double poor = f.wealth < 5 ? 0.3 : 0.0;
        const double kin = (f.spouse != kNone ? 0.35 : 0.0) + (f.children.empty() ? 0.0 : 0.25);
        double road = f.wanderlust + 0.35 * f.martial + 0.25 * f.ambition * (1.0 - f.renown) - kin - (rooted ? 0.15 : 0.0);
        double outlaw = f.greed * (1.0 - f.honor) + poor + (rooted ? 0.0 : 0.25) - 0.45 * f.honor - kin * 0.5;
        double settled = 0.45 + kin + 0.15 * f.honor + (home && home->unrest > 0.6 ? -0.2 : 0.0);
        if (f.role == "knight") settled += 0.25;
        if (f.life == "road") road += hyst;
        if (f.life == "outlaw") outlaw += hyst;
        if (f.life == "settled") settled += hyst;
        std::string pick = settled >= road && settled >= outlaw ? "settled" : (road >= outlaw ? "road" : "outlaw");
        if (pick == f.life) continue;
        f.life = pick;
        if (pick == "road") {
            if (f.role != "knight") f.role = "wanderer";
            log(x, "took_road", {{"fig", f.id}, {"pol", f.home}});
            deed(f, x.day, "took_road");
        } else if (pick == "outlaw") {
            f.role = "outlaw";
            // to the camp, if there is one
            RealmPolity* camp = nullptr;
            for (auto& p : x.s.polities)
                if (!p.ruined && p.kind == "bandit_camp") camp = &p;
            if (camp) {
                f.home = camp->id;
                f.travelTo = camp->id;
                f.arrive = x.day + 2;
                camp->pop += 1, camp->militia += 1;
            }
            log(x, "went_outlaw", {{"fig", f.id}, {"pol", camp ? camp->id : f.home}});
            deed(f, x.day, "went_outlaw");
        } else {
            if (f.role == "wanderer" || f.role == "outlaw") f.role = "commoner";
            if (f.at != kNone && f.at != f.home) {
                // settles where he is, unless that is a camp
                const RealmPolity* at = x.s.polity(f.at);
                if (at && at->kind != "bandit_camp" && at->settlement == kNone) f.home = f.at;
            }
            log(x, "settled_down", {{"fig", f.id}, {"pol", f.home}});
        }
    }
}

// A new camp: an outlaw with no camp to go to, in a hungry, restless land.
void banditsFound(Ctx& x) {
    if (!x.r.arcOn("bandits")) return;
    for (const auto& p : x.s.polities)
        if (!p.ruined && p.kind == "bandit_camp") return;
    double unrest = 0.0;
    int n = 0;
    for (const auto& p : x.s.polities)
        if (!p.ruined && p.settlement == kNone) unrest += p.unrest, ++n;
    if (n == 0 || unrest / n < num(x.r.ban, "found_unrest", 0.5) * 0.6) return;
    RealmFigure* chief = nullptr;
    for (auto& f : x.s.figures)
        if (f.alive && !f.present && f.life == "outlaw" && f.captor == kNone && (!chief || f.ambition > chief->ambition)) chief = &f;
    if (!chief || chief->ambition < 0.3) return;
    Rng rng = rngOf(x, chief->id, "realm:camp");
    RealmPolity c;
    c.id = x.w.newId();
    c.kind = "bandit_camp";
    const auto& pool = x.r.names.count("bandit_camp") ? x.r.names.at("bandit_camp") : std::vector<std::string>{"camp"};
    c.name = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
    const double ang = rng.range(0, 6.283185307179586), d = rng.range(x.r.regionKm[0], x.r.regionKm[1]);
    c.pos = {std::cos(ang) * d, std::sin(ang) * d};
    c.pop = 6, c.militia = 5, c.food = 80, c.wealth = chief->wealth;
    c.institution = "ataman";
    c.ruler = chief->id;
    c.founded = x.day;
    chief->home = chief->at = c.id;
    chief->role = "ruler";
    for (auto& f : x.s.figures)
        if (f.alive && f.life == "outlaw" && f.id != chief->id && !f.present) f.home = c.id, c.pop += 1, c.militia += 1;
    for (const auto& p : x.s.polities) {
        x.s.view(c.id, p.id).opinion = -0.5;
        x.s.view(p.id, c.id).opinion = -0.6;
    }
    x.s.polities.push_back(c);
    log(x, "camp_founded", {{"fig", chief->id}, {"pol", c.id}});
}

// A ruined village or town is settled again after some years, from its strongest neighbour.
void resettle(Ctx& x) {
    for (auto& p : x.s.polities) {
        if (!p.ruined || p.settlement != kNone || p.kind == "bandit_camp") continue;
        int ruinedOn = -1;
        for (const auto& [k, v] : x.s.stats)
            if (k == "ruined_" + std::to_string(p.id)) ruinedOn = v;
        if (ruinedOn < 0) {
            x.s.stats["ruined_" + std::to_string(p.id)] = x.day;
            continue;
        }
        if (x.day - ruinedOn < inum(x.r.eco, "resettle_after_days", 720)) continue;
        RealmPolity* from = nullptr;
        for (auto& q : x.s.polities)
            if (!q.ruined && q.settlement == kNone && q.kind != "bandit_camp" && q.kind != "monastery" && q.pop > 40 &&
                (!from || q.pop - dkm(q.pos, p.pos) > from->pop - dkm(from->pos, p.pos)))
                from = &q;
        if (!from) continue;
        Rng rng = rngOf(x, p.id, "realm:resettle");
        const double n = std::round(std::min(from->pop * 0.15, 30.0));
        from->pop -= n;
        p.ruined = false;
        p.pop = n;
        p.food = n * 120;
        p.wealth = n;
        p.militia = std::round(n * kindOf(x.r, p).militiaShare);
        p.morale = 0.6, p.unrest = 0.1;
        p.liege = from->id;
        p.tributeTo = kNone, p.tribute = 0;
        x.s.stats.erase("ruined_" + std::to_string(p.id));
        const EntityId pid = p.id, fromId = from->id;
        std::set<std::string> used;
        RealmFigure& head = newFigure(x, rng, pid, "ruler", 'm', rng.irange(25, 45), pickHouse(x, rng, used));
        x.s.polity(pid)->ruler = head.id;
        x.s.polity(pid)->house = head.house;
        log(x, "resettled", {{"pol", pid}, {"pol2", fromId}, {"fig", head.id}});
    }
}

void banditsDaily(Ctx& x) {
    if (!x.r.arcOn("bandits")) return;
    // outlaws come from restless land
    for (auto& camp : x.s.polities) {
        if (camp.ruined || camp.kind != "bandit_camp") continue;
        Rng rng = rngOf(x, camp.id, "realm:bandits");
        for (auto& p : x.s.polities)
            if (!p.ruined && p.settlement == kNone && p.kind != "bandit_camp" && p.unrest > 0.4 &&
                rng.chance(num(x.r.ban, "outlaws_from_unrest", 0.02) * p.unrest)) {
                p.pop = std::max(1.0, p.pop - 1);
                camp.pop += 1, camp.militia += 1;
            }
        const RealmFigure* at = x.s.figure(camp.ruler);
        if (!at || !at->alive) continue;
        const auto every = pair2(x.r.ban, "raid_every_days", {14, 30});
        const double hunger = camp.food < camp.pop * 10 ? 0.5 : 1.0;
        const int gap = static_cast<int>(std::round((every[0] + (every[1] - every[0]) * (1.0 - at->greed)) * hunger));
        if (x.day - camp.lastRaid < gap) continue;
        // the softest, richest target near enough
        RealmPolity* best = nullptr;
        double bestU = 0.0;
        const double sa = strength(x, camp);
        for (auto& p : x.s.polities) {
            if (p.ruined || p.id == camp.id || p.kind == "bandit_camp") continue;
            if (x.day - p.lastRaided < (p.settlement != kNone ? inum(x.r.war, "village_raid_gap_days", 30) : inum(x.r.war, "raid_gap_days", 15))) continue;
            if (x.s.view(p.id, camp.id).grievances.size() > 0 && x.day - x.s.view(p.id, camp.id).grievances.back().day < inum(x.r.ban, "same_target_gap_days", 40)) continue;
            const double sd = strength(x, p) * num(x.r.war, "defender_bonus", 1.25);
            const double loot = p.settlement != kNone ? std::max(5.0, p.food * 0.5 + p.wealth) : p.food * 0.2 + p.wealth;
            const double u = loot / (sd + 5.0) * (sa / (sa + sd + 1.0)) / (1.0 + dkm(camp.pos, p.pos) / 20.0) *
                             rng.range(0.6, 1.4);
            if (u > bestU) bestU = u, best = &p;
        }
        if (best) raid(x, camp, *best);
    }
}

// --- succession, revolt, reclaim -------------------------------------------------------------

EntityId pickHeir(Ctx& x, const RealmPolity& p, EntityId dead) {
    const RealmFigure* r = x.s.figure(dead);
    auto ok = [&](EntityId id) {
        const RealmFigure* f = x.s.figure(id);
        return f && f->alive && f->captor == kNone && f->home == p.id;
    };
    if (ok(p.heir)) return p.heir;
    if (r) {
        EntityId best = kNone;
        int age = -1;
        for (EntityId c : r->children)
            if (ok(c) && x.s.figure(c)->ageYears(x.day) > age) age = x.s.figure(c)->ageYears(x.day), best = c;
        if (best != kNone) return best;
        if (ok(r->spouse)) return r->spouse;
    }
    EntityId best = kNone;
    double renown = -1.0;
    for (const auto& f : x.s.figures)
        if (f.alive && f.home == p.id && f.captor == kNone && f.id != dead && !f.present &&
            (f.role == "knight" || f.role == "kin" || f.role == "outlaw" || f.role == "commoner") && f.renown > renown)
            renown = f.renown, best = f.id;
    return best;
}

void crown(Ctx& x, RealmPolity& p, EntityId who) {
    p.ruler = who;
    RealmFigure* f = x.s.figure(who);
    if (!f) return;
    f->role = "ruler";
    f->life = "settled";
    f->claim = kNone;
    f->home = f->at = p.id;
    if (!f->house.empty() && p.kind != "monastery" && p.kind != "bandit_camp") p.house = f->house;
    // the new heir
    p.heir = kNone;
    int age = -1;
    for (EntityId c : f->children)
        if (const RealmFigure* k = x.s.figure(c); k && k->alive && k->ageYears(x.day) > age) age = k->ageYears(x.day), p.heir = c;
    if (RealmFigure* h = x.s.figure(p.heir)) h->role = "heir";
}

void exile(Ctx& x, RealmFigure& f, RealmPolity& from, const std::string& code) {
    f.role = "wanderer";
    f.life = "road";
    f.claim = from.id;
    f.home = kNone;
    // the nearest other polity takes him in for now
    RealmPolity* to = nullptr;
    double best = 1e9;
    for (auto& p : x.s.polities)
        if (!p.ruined && p.id != from.id && p.kind != "bandit_camp" && p.settlement == kNone && dkm(p.pos, from.pos) < best)
            best = dkm(p.pos, from.pos), to = &p;
    if (to) f.travelTo = to->id, f.arrive = x.day + std::max(1, static_cast<int>(best / num(x.r.wan, "speed_km_day", 30))), f.home = to->id;
    deed(f, x.day, code, {{"pol", from.id}});
    log(x, code, {{"fig", f.id}, {"pol", from.id}});
}

void succession(Ctx& x, RealmPolity& p, const std::string& why) {
    if (p.ruined) return;
    const EntityId dead = p.ruler;
    const RealmFigure* old = x.s.figure(dead);
    const bool monastery = p.kind == "monastery";
    Rng rng = rngOf(x, p.id, "realm:succession");
    EntityId heir = monastery ? kNone : pickHeir(x, p, dead);
    if (heir == kNone) {
        // a new head from among the people: an elder monk, the strongest of the band, a reeve
        std::set<std::string> used;
        RealmFigure& n = newFigure(x, rng, p.id, "ruler", monastery && rng.chance(0.3) ? 'f' : 'm', rng.irange(30, 55),
                                   p.house.empty() ? pickHouse(x, rng, used) : p.house);
        if (p.kind == "bandit_camp") n.life = "outlaw", n.honor = clamp01(n.honor - 0.3);
        heir = n.id;
    }
    RealmFigure* h = x.s.figure(heir);
    // a claimant: an ambitious kinsman or knight when the heir is a child or weak
    const int adult = inum(x.r.suc, "adult_years", 16);
    const bool weak = h->ageYears(x.day) < adult || h->renown < num(x.r.suc, "weak_heir_renown", 0.12);
    RealmFigure* claimant = nullptr;
    if (weak && !monastery && x.r.arcOn("succession"))
        for (auto& f : x.s.figures)
            if (f.alive && !f.present && f.home == p.id && f.id != heir && f.captor == kNone &&
                f.ageYears(x.day) >= adult && f.ambition >= num(x.r.suc, "claim_ambition", 0.62) &&
                f.martial >= num(x.r.suc, "claim_martial", 0.3) && (!claimant || f.ambition + f.renown > claimant->ambition + claimant->renown))
                claimant = &f;
    log(x, "ruler_died", {{"fig", dead}, {"pol", p.id}, {"cause", why}});
    crown(x, p, heir);
    h = x.s.figure(heir);
    if (!claimant) {
        log(x, "succession", {{"fig", heir}, {"pol", p.id}});
        return;
    }
    // the strife: a war inside the walls
    RealmWar wr;
    wr.id = static_cast<EntityId>(x.s.nextId++);
    wr.attacker = wr.defender = p.id;
    wr.claimant = claimant->id;
    wr.cause = "succession";
    wr.start = wr.last = x.day;
    const auto sd = pair2(x.r.suc, "strife_days", {15, 45});
    wr.score = rng.irange(static_cast<int>(sd[0]), static_cast<int>(sd[1])); // days the strife lasts
    x.s.wars.push_back(wr);
    claimant->opinions[std::to_string(heir)] = -0.6;
    p.unrest = clamp01(p.unrest + 0.2);
    log(x, "succession_crisis", {{"fig", claimant->id}, {"fig2", heir}, {"pol", p.id}});
}

void strifeDaily(Ctx& x, RealmWar& wr) {
    RealmPolity* p = x.s.polity(wr.attacker);
    RealmFigure* c = x.s.figure(wr.claimant);
    if (!p || !c || !c->alive) {
        wr.over = true, wr.last = x.day, wr.outcome = "claimant_gone";
        return;
    }
    if (x.day - wr.start < static_cast<int>(wr.score)) {
        p->militia = std::max(0.0, p->militia - 0.05);
        return;
    }
    RealmFigure* h = x.s.figure(p->ruler);
    Rng rng = rngOf(x, p->id, "realm:strife");
    const double sc = 0.2 + c->renown + 0.5 * c->martial + 0.3 * c->ambition;
    const double sh = 0.35 + (h ? h->renown + 0.3 * h->martial : 0.0) + 0.2; // loyalty to the line
    wr.over = true;
    wr.last = x.day;
    p->unrest = clamp01(p->unrest - 0.1);
    if (rng.uniform() * (sc + sh) < sc) {
        wr.outcome = "usurped";
        const EntityId old = p->ruler;
        crown(x, *p, c->id);
        log(x, "usurped", {{"fig", c->id}, {"fig2", old}, {"pol", p->id}});
        if (RealmFigure* o = x.s.figure(old); o && o->alive) {
            // a child heir to the cloister, a grown one into exile
            RealmPolity* cloister = nullptr;
            for (auto& q : x.s.polities)
                if (!q.ruined && q.kind == "monastery") cloister = &q;
            if (o->ageYears(x.day) < inum(x.r.suc, "adult_years", 16) && cloister) {
                o->home = o->at = cloister->id;
                o->role = "commoner";
                o->claim = p->id;
                log(x, "heir_to_monastery", {{"fig", o->id}, {"pol", cloister->id}, {"pol2", p->id}});
            } else if (c->honor < 0.3 && rng.chance(0.5)) {
                figureDies(x, *o, "murdered");
            } else {
                exile(x, *o, *p, "exiled");
            }
            if (o->alive) o->opinions[std::to_string(c->id)] = -0.9;
        }
    } else {
        wr.outcome = "held";
        log(x, "claim_failed", {{"fig", c->id}, {"pol", p->id}});
        if (h && h->honor < 0.35 && rng.chance(0.6)) figureDies(x, *c, "executed");
        else exile(x, *c, *p, "exiled");
    }
}

void revolts(Ctx& x) {
    if (!x.r.arcOn("revolt")) return;
    const double line = num(x.r.rev, "unrest_line", 0.72);
    for (auto& p : x.s.polities) {
        if (p.ruined || p.settlement != kNone || p.kind == "bandit_camp") continue;
        p.unrestDays = p.unrest >= line ? p.unrestDays + 1 : 0;
        if (p.unrestDays < inum(x.r.rev, "days", 20) || x.day - p.lastRevolt < 180) continue;
        p.unrestDays = 0;
        p.lastRevolt = x.day;
        Rng rng = rngOf(x, p.id, "realm:revolt");
        RealmFigure* leader = nullptr;
        for (auto& f : x.s.figures)
            if (f.alive && !f.present && f.home == p.id && f.id != p.ruler && f.role != "consort" && f.captor == kNone &&
                f.ageYears(x.day) >= 18 && (!leader || f.ambition > leader->ambition))
                leader = &f;
        if (!leader || leader->ambition < 0.4) {
            std::set<std::string> used;
            leader = &newFigure(x, rng, p.id, "commoner", 'm', rng.irange(22, 45), pickHouse(x, rng, used));
            leader->ambition = clamp01(leader->ambition + 0.3);
        }
        const EntityId lid = leader->id;
        log(x, "revolt", {{"fig", lid}, {"pol", p.id}});
        const double rebels = p.pop * p.unrest * num(x.r.rev, "rebel_per_head", 0.12) * (0.5 + x.s.figure(lid)->martial);
        const double loyal = p.militia * (0.3 + p.morale) * (1.0 - p.unrest * 0.5);
        if (rng.uniform() * (rebels + loyal) < rebels) {
            const EntityId old = p.ruler;
            crown(x, p, lid);
            p.unrest = 0.2;
            p.militia *= 0.7;
            log(x, "revolt_won", {{"fig", lid}, {"fig2", old}, {"pol", p.id}});
            if (RealmFigure* o = x.s.figure(old); o && o->alive) {
                if (rng.chance(0.4)) figureDies(x, *o, "revolt");
                else exile(x, *o, p, "exiled");
            }
        } else {
            p.unrest = clamp01(p.unrest - 0.3);
            p.morale = clamp01(p.morale - 0.1);
            p.pop = std::max(1.0, p.pop - std::round(rebels * 0.3));
            log(x, "revolt_crushed", {{"fig", lid}, {"pol", p.id}});
            figureDies(x, *x.s.figure(lid), "executed");
        }
    }
}

// The exile comes back with hired swords.
void reclaims(Ctx& x) {
    if (!x.r.arcOn("reclaim")) return;
    for (auto& f : x.s.figures) {
        if (!f.alive || f.present || f.claim == kNone || f.captor != kNone || f.travelTo != kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % 30 != 0) continue;
        RealmPolity* target = x.s.polity(f.claim);
        if (!target || target->ruined || target->ruler == f.id) {
            f.claim = kNone;
            continue;
        }
        if (f.ageYears(x.day) < inum(x.r.suc, "adult_years", 16) || f.ambition < 0.45) continue;
        int exiledOn = -99999;
        for (const auto& d : f.deeds)
            if (d.value("code", "") == "exiled" || d.value("code", "") == "heir_to_monastery") exiledOn = d.value("day", 0);
        if (exiledOn < -9999) {
            // sent to the cloister as a child: the claim ripens when grown
            exiledOn = f.born + inum(x.r.suc, "adult_years", 16) * kDaysPerYear;
        }
        if (x.day - exiledOn < inum(x.r.suc, "reclaim_after_days", 360)) continue;
        // a hired band: an open war_service quest of his
        RealmQuest* q = nullptr;
        for (auto& qq : x.s.quests)
            if (qq.kind == "war_service" && qq.giver == f.id && qq.status == "open") q = &qq;
        if (!q) {
            if (f.wealth < num(x.r.suc, "reclaim_wealth", 30) && f.renown < 0.3) continue;
            RealmQuest* nq = postQuest(x, "war_service", f.claim, f.id, f.claim, std::min(f.wealth * 0.6, 45.0) + 10.0);
            if (nq) {
                nq->target = f.claim;
                log(x, "reclaim_start", {{"fig", f.id}, {"pol", f.claim}});
            }
            continue;
        }
        if (q->takers.size() < 2 && x.day - q->posted < inum(x.r.suc, "reclaim_wait_days", 45)) continue;
        // the attack
        Rng rng = rngOf(x, f.id, "realm:reclaim");
        double band = 2.0 + 4.0 * f.martial + 6.0 * f.renown;
        for (EntityId t : q->takers)
            if (RealmFigure* m = x.s.figure(t); m && m->alive) band += 4.0 * m->martial * m->health;
        const double def = strength(x, *target) * 0.6 * (1.0 - target->unrest * 0.5);
        q->status = "done";
        q->closed = x.day;
        if (rng.uniform() * (band + def) < band) {
            const EntityId old = target->ruler;
            crown(x, *target, f.id);
            log(x, "reclaimed", {{"fig", f.id}, {"fig2", old}, {"pol", target->id}});
            for (EntityId t : q->takers)
                if (RealmFigure* m = x.s.figure(t)) m->renown = clamp01(m->renown + 0.08), m->wealth += q->reward, deed(*m, x.day, "reclaim_won", {{"pol", target->id}});
            if (RealmFigure* o = x.s.figure(old); o && o->alive) exile(x, *o, *target, "exiled");
        } else {
            log(x, "reclaim_failed", {{"fig", f.id}, {"pol", target->id}});
            for (EntityId t : q->takers)
                if (RealmFigure* m = x.s.figure(t); m && m->alive && rng.chance(0.3)) figureDies(x, *m, "battle");
            if (rng.chance(0.45)) figureDies(x, f, "battle");
            else f.wealth *= 0.3;
        }
    }
}

// --- politics ------------------------------------------------------------------------------

void decayGrievances(Ctx& x) {
    const double hl = num(x.r.pol, "grievance_halflife_days", 420);
    const double k = std::pow(0.5, 1.0 / hl);
    for (auto& [key, v] : x.s.views) {
        for (auto& g : v.grievances) g.weight *= k;
        v.grievances.erase(std::remove_if(v.grievances.begin(), v.grievances.end(), [](const RealmGrievance& g) { return g.weight < 0.03; }),
                           v.grievances.end());
        // opinion drifts back toward what the grudges allow
        const double target = std::clamp(-v.grudge(), -1.0, 0.3);
        v.opinion += (target - v.opinion) * num(x.r.pol, "opinion_drift", 0.01);
    }
}

RealmWar& startWar(Ctx& x, RealmPolity& a, RealmPolity& d, const std::string& cause) {
    RealmWar wr;
    wr.id = static_cast<EntityId>(x.s.nextId++);
    wr.attacker = a.id;
    wr.defender = d.id;
    wr.cause = cause;
    wr.start = wr.last = x.day;
    wr.startMilA = std::max(1.0, a.militia);
    wr.startMilD = std::max(1.0, d.militia);
    unally(x, a.id, d.id);
    if (d.liege == a.id) d.liege = kNone;
    x.s.wars.push_back(wr);
    RealmWar& w = x.s.wars.back();
    log(x, "war_declared", {{"pol", a.id}, {"pol2", d.id}, {"casus", cause}},
        a.settlement != kNone || d.settlement != kNone ? std::vector<EntityId>{} : std::vector<EntityId>{});
    grieve(x, d.id, a.id, "war", 0.3);
    // the allies are called
    const EntityId wid = w.id;
    for (auto& p : x.s.polities) {
        if (p.ruined || p.id == a.id || p.id == d.id || p.kind == "bandit_camp") continue;
        const Axes ax = rulerAxes(x, p);
        if (ax.player || kindOf(x.r, p).peaceful) continue;
        if (p.settlement != kNone && ax.honor < 0.6) continue;
        const bool vassalD = d.liege == p.id || p.liege == d.id;
        const bool vassalA = a.liege == p.id || p.liege == a.id;
        if ((allied(x, p.id, d.id) || vassalD) && !allied(x, p.id, a.id)) {
            if (vassalD || ax.honor > 0.35) {
                x.s.wars.back().alliesD.push_back(p.id);
                log(x, "ally_joins", {{"pol", p.id}, {"pol2", d.id}});
            } else {
                grieve(x, d.id, p.id, "betrayed", 0.35);
                unally(x, p.id, d.id);
                log(x, "ally_refuses", {{"pol", p.id}, {"pol2", d.id}});
            }
        } else if ((allied(x, p.id, a.id) || vassalA) && x.s.view(p.id, d.id).opinion < 0.0) {
            x.s.wars.back().alliesA.push_back(p.id);
            log(x, "ally_joins", {{"pol", p.id}, {"pol2", a.id}});
        }
    }
    // a lord at war hires swords
    for (RealmPolity* side : {&a, &d}) {
        if (side->settlement != kNone || side->wealth < 40 || !x.r.questOn("war_service")) continue;
        const RealmQuestKind& qk = x.r.quests.at("war_service");
        postQuest(x, "war_service", side->id, side->ruler, side == &a ? d.id : a.id,
                  std::clamp(side->wealth * 0.15, qk.reward[0], qk.reward[1]));
    }
    return *std::find_if(x.s.wars.begin(), x.s.wars.end(), [&](const RealmWar& q) { return q.id == wid; });
}

void endWar(Ctx& x, RealmWar& wr, const std::string& outcome, EntityId winner, EntityId loser) {
    wr.over = true;
    wr.last = x.day;
    wr.outcome = outcome;
    RealmPolity* W = x.s.polity(winner);
    RealmPolity* L = x.s.polity(loser);
    Json p = {{"pol", winner}, {"pol2", loser}};
    if (W && L && outcome != "white") {
        const Axes wa = rulerAxes(x, *W);
        const double ratio = strength(x, *W) / std::max(1.0, strength(x, *L));
        if (outcome == "dictated" && wa.ambition > 0.55 && ratio > 1.8 && L->kind != "bandit_camp" && L->liege == kNone &&
            W->liege != L->id) {
            L->liege = W->id;
            L->tributeTo = W->id;
            L->tribute = std::max(2.0, L->wealth * num(x.r.pol, "tribute_share", 0.06));
            log(x, "peace_vassal", p);
        } else if (wa.greed > 0.35 || outcome == "dictated") {
            L->tributeTo = W->id;
            L->tribute = std::max(2.0, std::max(L->wealth, L->pop) * num(x.r.pol, "tribute_share", 0.06));
            p["n"] = static_cast<int>(std::round(L->tribute));
            log(x, "peace_tribute", p);
        } else {
            const double rep = std::max(0.0, L->wealth) * num(x.r.war, "reparation_share", 0.3);
            L->wealth -= rep, W->wealth += rep;
            p["n"] = static_cast<int>(std::round(rep));
            log(x, "peace_reparation", p);
        }
        grieve(x, loser, winner, "war", 0.35); // the loser remembers (revanchism)
        L->unrest = clamp01(L->unrest + num(x.r.war, "defeat_unrest", 0.06));
    } else {
        log(x, "peace_white", p);
    }
    // the hired swords are paid off and go
    for (auto& q : x.s.quests)
        if (q.kind == "war_service" && q.status == "open" && (q.polity == wr.attacker || q.polity == wr.defender) && q.giver != wr.claimant) {
            q.status = "done";
            q.closed = x.day;
            RealmPolity* emp = x.s.polity(q.polity);
            for (EntityId t : q.takers)
                if (RealmFigure* m = x.s.figure(t); m && m->alive) {
                    const double pay = emp ? std::min(q.reward, std::max(0.0, emp->wealth)) : 0.0;
                    if (emp) emp->wealth -= pay;
                    m->wealth += pay;
                    m->renown = clamp01(m->renown + x.r.quests.at("war_service").renown * (q.polity == winner ? 1.5 : 0.7));
                    m->quest = kNone;
                    deed(*m, x.day, q.polity == winner ? "war_won" : "war_lost", {{"pol", q.polity}});
                }
        }
}

// The captives: ransom or the axe.
void captives(Ctx& x) {
    for (auto& f : x.s.figures) {
        if (!f.alive || f.captor == kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % 7 != 0) continue;
        RealmPolity* captor = x.s.polity(f.captor);
        RealmPolity* home = x.s.polity(f.home);
        const double ransom = num(x.r.war, "ransom_base", 20) + f.renown * num(x.r.war, "ransom_per_renown", 60);
        if (!captor || captor->ruined) {
            f.captor = kNone;
            log(x, "captive_escaped", {{"fig", f.id}});
            continue;
        }
        if (home && !home->ruined && home->wealth >= ransom) {
            home->wealth -= ransom;
            captor->wealth += ransom;
            f.captor = kNone;
            f.at = f.home;
            log(x, "ransomed", {{"fig", f.id}, {"pol", captor->id}, {"n", static_cast<int>(std::round(ransom))}});
            continue;
        }
        int taken = x.day;
        for (const auto& d : f.deeds)
            if (d.value("code", "") == "captured") taken = d.value("day", x.day);
        if (x.day - taken > inum(x.r.war, "captive_days", 150)) {
            const Axes a = rulerAxes(x, *captor);
            Rng rng = rngOf(x, f.id, "realm:captive");
            if (a.honor < 0.35 && rng.chance(0.5)) {
                figureDies(x, f, "executed");
            } else {
                f.captor = kNone;
                f.at = f.home;
                log(x, "released", {{"fig", f.id}, {"pol", captor->id}});
            }
        }
    }
}

// A local battle: the village's own men march and some do not come back.
void localLevy(Ctx& x, RealmPolity& local, EntityId enemy, bool lost) {
    Rng rng = rngOf(x, local.id, "realm:levy");
    auto men = levy(x, local);
    if (men.empty()) return;
    const double p = num(x.r.war, "local_casualty", 0.45) * (lost ? 1.0 : 0.5);
    if (!rng.chance(p)) return;
    Character& c = *men[static_cast<size_t>(rng.irange(0, static_cast<int>(men.size()) - 1))];
    hurtVillager(x, c, rng.range(0.25, lost ? 1.1 : 0.7), "battle", enemy);
}

void fieldFigures(Ctx& x, RealmPolity& p, bool lost, EntityId enemy, Rng& rng) {
    const double death = num(x.r.war, "figure_death", 0.07);
    const double cap = num(x.r.war, "figure_capture", 0.12);
    std::vector<EntityId> field;
    for (const auto& f : x.s.figures)
        if (f.alive && !f.present && f.captor == kNone && f.travelTo == kNone && f.at == p.id &&
            (f.role == "knight" || (f.role == "ruler" && f.martial > 0.4 && f.ageYears(x.day) < 60) ||
             (f.role == "heir" && f.ageYears(x.day) >= 16 && f.martial > 0.5)))
            field.push_back(f.id);
    for (EntityId m : mercsOf(x, p.id)) field.push_back(m);
    for (EntityId id : field) {
        RealmFigure* f = x.s.figure(id);
        if (!f || !f->alive) continue;
        const double d = lost ? death * (1.6 - f->martial) : death * 0.3 * (1.6 - f->martial);
        if (rng.chance(d)) {
            log(x, "fell_in_battle", {{"fig", id}, {"pol", p.id}, {"pol2", enemy}});
            if (RealmPolity* e = x.s.polity(enemy)) grieve(x, f->home == kNone ? p.id : f->home, e->id, "killed_kin", 0.25);
            figureDies(x, *f, "battle");
            continue;
        }
        if (lost && f->role != "wanderer" && rng.chance(cap)) {
            f->captor = enemy;
            deed(*f, x.day, "captured", {{"pol", enemy}});
            log(x, "captured", {{"fig", id}, {"pol", enemy}});
            if (p.ruler == id && p.settlement == kNone) p.morale = clamp01(p.morale - 0.15);
            continue;
        }
        if (!lost) f->renown = clamp01(f->renown + 0.02);
        if (rng.chance(0.25)) f->health = std::max(0.2, f->health - rng.range(0.1, 0.4));
    }
}

double sideStrength(Ctx& x, EntityId main, const std::vector<EntityId>& allies) {
    double s = 0.0;
    if (RealmPolity* p = x.s.polity(main)) s += strength(x, *p);
    for (EntityId a : allies)
        if (RealmPolity* p = x.s.polity(a)) s += 0.5 * strength(x, *p);
    return s;
}

void warStep(Ctx& x, RealmWar& wr) {
    RealmPolity* A = x.s.polity(wr.attacker);
    RealmPolity* D = x.s.polity(wr.defender);
    if (!A || !D || A->ruined || D->ruined) {
        endWar(x, wr, "white", A && !A->ruined ? wr.attacker : wr.defender, A && !A->ruined ? wr.defender : wr.attacker);
        return;
    }
    // the siege eats the defender's stores
    if (wr.siege) {
        D->food -= D->pop * (num(x.r.war, "siege_food_mult", 1.5) - 1.0);
        D->morale = clamp01(D->morale - num(x.r.war, "siege_morale_per_day", 0.008));
    }
    if ((x.day - wr.start) % inum(x.r.war, "action_every_days", 7) != 0 || x.day == wr.start) return;
    Rng rng = rngOf(x, static_cast<EntityId>(wr.id), "realm:war");
    if (wr.siege) {
        if (D->food <= 0.0 || D->morale < 0.2) {
            log(x, "siege_falls", {{"pol", A->id}, {"pol2", D->id}});
            D->pop *= 0.9;
            D->wealth *= 0.6;
            A->wealth += D->wealth * 0.3;
            endWar(x, wr, "dictated", A->id, D->id);
            return;
        }
    }
    const double sa = sideStrength(x, A->id, wr.alliesA), sd = sideStrength(x, D->id, wr.alliesD);
    const bool battle = rng.chance(num(x.r.war, "battle_chance", 0.45));
    if (!battle) {
        // a raid: the stronger side's men burn the other's fields
        const bool aRaids = rng.uniform() * (sa + sd) < sa;
        RealmPolity& by = aRaids ? *A : *D;
        RealmPolity& on = aRaids ? *D : *A;
        const int gap = on.settlement != kNone ? inum(x.r.war, "village_raid_gap_days", 30) : inum(x.r.war, "raid_gap_days", 15);
        if (x.day - on.lastRaided >= gap) {
            const double t = raid(x, by, on);
            ++wr.raids;
            wr.score += (aRaids ? 1 : -1) * (t > 0 ? 0.5 : -0.25);
        }
    } else {
        ++wr.battles;
        const double pa = sa / std::max(1e-6, sa + sd * num(x.r.war, "defender_bonus", 1.25) * (wr.siege ? 1.2 : 1.0));
        const bool aWins = rng.uniform() < pa;
        RealmPolity& W = aWins ? *A : *D;
        RealmPolity& L = aWins ? *D : *A;
        const auto ll = pair2(x.r.war, "loser_losses", {0.15, 0.3}), wl = pair2(x.r.war, "winner_losses", {0.04, 0.12});
        if (L.settlement == kNone) L.militia *= 1.0 - rng.range(ll[0], ll[1]);
        if (W.settlement == kNone) W.militia *= 1.0 - rng.range(wl[0], wl[1]);
        if (L.settlement == kNone) L.pop = std::max(1.0, L.pop - std::round(rng.range(1, 4)));
        W.morale = clamp01(W.morale + 0.08);
        L.morale = clamp01(L.morale - 0.12);
        const double margin = std::abs(pa - 0.5) * 2.0;
        wr.score += (aWins ? 1 : -1) * (1.0 + margin);
        log(x, "battle", {{"pol", W.id}, {"pol2", L.id}});
        fieldFigures(x, W, false, L.id, rng);
        fieldFigures(x, L, true, W.id, rng);
        if (W.settlement != kNone) localLevy(x, W, L.id, false);
        if (L.settlement != kNone) localLevy(x, L, W.id, true);
        grieve(x, L.id, W.id, "battle", 0.06);
    }
    wr.last = x.day;
    const double siegeScore = num(x.r.war, "siege_score", 3.0);
    if (!wr.siege && wr.score >= siegeScore && D->settlement == kNone) {
        wr.siege = true;
        log(x, "siege_begins", {{"pol", A->id}, {"pol2", D->id}});
    }
    if (wr.score >= siegeScore + 2.5 && D->settlement != kNone) {
        endWar(x, wr, "dictated", A->id, D->id);
        return;
    }
    if (wr.score <= -siegeScore) {
        endWar(x, wr, "won", D->id, A->id);
        return;
    }
    const double ex = num(x.r.war, "exhaustion", 0.35);
    const bool tiredA = A->settlement == kNone && A->militia < wr.startMilA * ex;
    const bool tiredD = D->settlement == kNone && D->militia < wr.startMilD * ex;
    if (x.day - wr.start > inum(x.r.war, "max_days", 300) || tiredA || tiredD) {
        if (wr.score > 1.0) endWar(x, wr, "won", A->id, D->id);
        else if (wr.score < -1.0) endWar(x, wr, "won", D->id, A->id);
        else endWar(x, wr, "white", A->id, D->id);
    }
}

void warsDaily(Ctx& x) {
    for (size_t i = 0; i < x.s.wars.size(); ++i) {
        RealmWar& wr = x.s.wars[i];
        if (wr.over) continue;
        if (wr.attacker == wr.defender) strifeDaily(x, x.s.wars[i]);
        else warStep(x, x.s.wars[i]);
    }
    // the record keeps the last wars only
    if (x.s.wars.size() > 60) {
        auto& v = x.s.wars;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const RealmWar& q) { return q.over && x.day - q.last > 720; }), v.end());
    }
}

void tributeDaily(Ctx& x) {
    for (auto& p : x.s.polities) {
        if (p.ruined || p.tributeTo == kNone) continue;
        if ((x.day + static_cast<int>(p.id)) % 30 != 0) continue;
        RealmPolity* to = x.s.polity(p.tributeTo);
        if (!to || to->ruined) {
            p.tributeTo = kNone, p.tribute = 0, p.liege = p.liege == kNone ? kNone : p.liege;
            continue;
        }
        double paid = 0.0;
        if (p.settlement != kNone) {
            // the village pays from its real stores (grain, coin)
            for (WorldObject* o : localStores(x, p)) {
                for (auto& [item, n] : o->stock) {
                    if (paid >= p.tribute || n <= 0) continue;
                    if (item != "coin" && item != "grain") continue;
                    const double rate = item == "coin" ? 1.0 : 0.5;
                    const int t = std::min(n, static_cast<int>(std::ceil((p.tribute - paid) / rate)));
                    n -= t;
                    paid += t * rate;
                }
            }
            for (Character* c : localPeople(x, p))
                if (c->body.stage() != "child") {
                    MemoryEntry m;
                    m.type = "paid_tribute";
                    m.valence = -0.3;
                    m.salience = 0.3;
                    x.w.remember(*c, m);
                }
        } else {
            paid = std::min(p.tribute, std::max(0.0, p.wealth));
            p.wealth -= paid;
            p.unrest = clamp01(p.unrest + num(x.r.pol, "tribute_unrest", 0.03));
        }
        to->wealth += paid;
        if (paid + 0.5 < p.tribute) {
            grieve(x, to->id, p.id, "tribute", 0.25);
            const std::string once = "unpaid_" + std::to_string(p.id) + "_" + std::to_string(x.day / kDaysPerYear);
            if (!x.s.stats.count(once)) x.s.stats[once] = 1, log(x, "tribute_unpaid", {{"pol", p.id}, {"pol2", to->id}});
        }
        // tribute ends after its years, vassalage does not
        int since = -1;
        for (const auto& wr : x.s.wars)
            if (wr.over && wr.attacker != wr.defender && ((wr.attacker == to->id && wr.defender == p.id) || (wr.attacker == p.id && wr.defender == to->id)))
                since = std::max(since, wr.last);
        for (const auto& o : x.s.offers)
            if (o.kind == "tribute" && o.status == "accepted" && o.from == to->id && o.to == p.id) since = std::max(since, o.day);
        if (p.liege != to->id && since >= 0 && x.day - since > inum(x.r.pol, "tribute_years", 3) * kDaysPerYear) {
            p.tributeTo = kNone;
            p.tribute = 0;
            log(x, "tribute_ends", {{"pol", p.id}, {"pol2", to->id}});
        }
    }
}

// --- offers and envoys --------------------------------------------------------------------

RealmFigure* envoyOf(Ctx& x, const RealmPolity& from) {
    RealmFigure* best = nullptr;
    for (auto& f : x.s.figures)
        if (f.alive && !f.present && f.captor == kNone && f.travelTo == kNone && f.home == from.id && f.at == from.id &&
            f.ageYears(x.day) >= 18 && (f.role == "knight" || f.role == "kin" || f.role == "heir"))
            if (!best || (f.role == "knight" && best->role != "knight")) best = &f;
    if (!best) best = x.s.figure(from.ruler);
    return best && best->alive && !best->present ? best : nullptr;
}

bool accepts(Ctx& x, const RealmOffer& o, const RealmPolity& to, const RealmPolity& from) {
    const Axes a = rulerAxes(x, to);
    const double ratio = strength(x, from) / std::max(1.0, strength(x, to));
    const RealmView& v = x.s.view(to.id, from.id);
    Rng rng = rngOf(x, to.id, "realm:answer:" + o.kind);
    if (o.kind == "tribute") return ratio > 1.6 + a.honor * 0.8 && rng.chance(0.85);
    if (o.kind == "alliance") return v.opinion > 0.05 && v.grudge() < 0.3;
    if (o.kind == "marriage") return v.opinion > -0.1 && v.grudge() < 0.35 && rng.chance(0.75);
    if (o.kind == "peace") return true;
    return false;
}

void applyOffer(Ctx& x, RealmOffer& o, bool yes) {
    RealmPolity* from = x.s.polity(o.from);
    RealmPolity* to = x.s.polity(o.to);
    o.status = yes ? "accepted" : "refused";
    if (!from || !to) return;
    Json p = {{"pol", o.to}, {"pol2", o.from}, {"offer", o.kind}};
    log(x, yes ? "offer_accepted" : "offer_refused", p);
    if (o.kind == "tribute") {
        if (yes) {
            to->tributeTo = from->id;
            to->tribute = std::max(2.0, o.amount);
            x.s.view(to->id, from->id).grievances.push_back({"tribute", 0.2, x.day});
        } else {
            grieve(x, from->id, to->id, "refused", 0.35);
            const Axes fa = rulerAxes(x, *from);
            if (fa.ambition + fa.greed > 0.7 && warsOf(x, from->id) < inum(x.r.pol, "max_wars", 2) && x.r.arcOn("war"))
                startWar(x, *from, *to, "refused_tribute");
        }
    } else if (o.kind == "alliance") {
        if (yes) {
            ally(x, from->id, to->id);
            x.s.view(from->id, to->id).opinion += 0.2;
            x.s.view(to->id, from->id).opinion += 0.2;
            log(x, "alliance", {{"pol", from->id}, {"pol2", to->id}});
        }
    } else if (o.kind == "marriage") {
        RealmFigure* g = x.s.figure(o.groom);
        RealmFigure* b = x.s.figure(o.bride);
        if (yes && g && b && g->alive && b->alive && g->spouse == kNone && b->spouse == kNone) {
            marry(*g, *b);
            b->home = b->at = g->home;
            x.s.view(from->id, to->id).opinion = std::min(1.0, x.s.view(from->id, to->id).opinion + 0.25);
            x.s.view(to->id, from->id).opinion = std::min(1.0, x.s.view(to->id, from->id).opinion + 0.25);
            log(x, "marriage", {{"fig", g->id}, {"fig2", b->id}, {"pol", g->home}, {"pol2", from->id == g->home ? to->id : from->id}});
            if (x.s.view(from->id, to->id).opinion > num(x.r.pol, "alliance_opinion", 0.3)) {
                ally(x, from->id, to->id);
                log(x, "alliance", {{"pol", from->id}, {"pol2", to->id}});
            }
        } else if (!yes) {
            const Axes fa = rulerAxes(x, *from);
            if (fa.honor > 0.5 || fa.ambition > 0.5) grieve(x, from->id, to->id, "spurned", 0.25);
        }
    } else if (o.kind == "peace") {
        if (RealmWar* wr = warBetween(x, from->id, to->id); wr && yes)
            endWar(x, *wr, "sued", to->id, from->id);
    }
}

// An offer to the village goes by envoy; between others it is answered at once.
void makeOffer(Ctx& x, RealmOffer o) {
    RealmPolity* to = x.s.polity(o.to);
    RealmPolity* from = x.s.polity(o.from);
    if (!to || !from) return;
    if (o.kind == "tribute" && to->tributeTo != kNone) return; // pays someone already
    for (const auto& p : x.s.offers)
        if (p.status == "pending" && p.from == o.from && p.to == o.to) return; // one embassy at a time
    o.id = static_cast<EntityId>(x.s.nextId++);
    o.day = x.day;
    log(x, "offer", {{"pol", o.from}, {"pol2", o.to}, {"offer", o.kind}, {"n", static_cast<int>(std::round(o.amount))}});
    if (to->settlement == kNone) {
        const bool yes = accepts(x, o, *to, *from);
        x.s.offers.push_back(o);
        applyOffer(x, x.s.offers.back(), yes);
        return;
    }
    RealmFigure* env = envoyOf(x, *from);
    if (env) {
        o.envoy = env->id;
        const int days = std::max(1, static_cast<int>(dkm(from->pos, to->pos) / num(x.r.wan, "speed_km_day", 30)));
        env->travelTo = to->id;
        env->arrive = x.day + days;
        env->quest = kNone;
        o.deadline = env->arrive + inum(x.r.pol, "envoy_stay_days", 3);
    } else {
        o.deadline = x.day + inum(x.r.pol, "envoy_stay_days", 3);
    }
    x.s.offers.push_back(o);
}

// The village's chief answers at the envoy's leave (the player answers with realm.answer).
void offersDaily(Ctx& x) {
    for (auto& o : x.s.offers) {
        if (o.status != "pending") continue;
        RealmPolity* to = x.s.polity(o.to);
        RealmPolity* from = x.s.polity(o.from);
        if (!to || !from) {
            o.status = "refused";
            continue;
        }
        if (x.day < o.deadline) continue;
        const Axes a = rulerAxes(x, *to);
        const bool yes = a.player ? false : accepts(x, o, *to, *from);
        if (a.player) log(x, "offer_unanswered", {{"pol", o.from}, {"offer", o.kind}});
        applyOffer(x, o, yes);
    }
    if (x.s.offers.size() > 80) x.s.offers.erase(x.s.offers.begin(), x.s.offers.begin() + 20);
}

// Once a month each ruler weighs his neighbours.
void councils(Ctx& x) {
    const int every = inum(x.r.pol, "council_every_days", 30);
    for (auto& P : x.s.polities) {
        if (P.ruined || P.kind == "bandit_camp" || !rulerAlive(x, P)) continue;
        if ((x.day + static_cast<int>(P.id)) % every != 0) continue;
        const RealmKind& kd = kindOf(x.r, P);
        const Axes a = rulerAxes(x, P);
        if (a.player) continue; // the player's chief acts by command
        Rng rng = rngOf(x, P.id, "realm:council");
        const double sP = strength(x, P);
        // marriage: the ruler's grown, unwed children
        if (x.r.arcOn("marriage") && rng.chance(0.35) && P.settlement == kNone) {
            const RealmFigure* rf = x.s.figure(P.ruler);
            if (rf)
                for (EntityId cid : rf->children) {
                    const RealmFigure* ch = x.s.figure(cid);
                    if (!ch || !ch->alive || ch->spouse != kNone || ch->ageYears(x.day) < inum(x.r.fig, "marry_from", 17)) continue;
                    // a match from another ruling family
                    const RealmFigure* match = nullptr;
                    EntityId matchPol = kNone;
                    double best = -1e9;
                    for (const auto& Q : x.s.polities) {
                        if (Q.id == P.id || Q.ruined || Q.settlement != kNone || Q.kind == "bandit_camp" || Q.kind == "monastery") continue;
                        if (warBetween(x, P.id, Q.id)) continue;
                        for (const auto& f : x.s.figures)
                            if (f.alive && !f.present && f.home == Q.id && f.spouse == kNone && f.sex != ch->sex &&
                                f.ageYears(x.day) >= inum(x.r.fig, "marry_from", 17) && std::abs(f.ageYears(x.day) - ch->ageYears(x.day)) < 15 &&
                                (f.role == "heir" || f.role == "kin" || f.role == "ruler")) {
                                const double u = strength(x, Q) * 0.01 + x.s.view(P.id, Q.id).opinion + rng.range(0, 0.3);
                                if (u > best) best = u, match = &f, matchPol = Q.id;
                            }
                    }
                    if (match) {
                        RealmOffer o;
                        o.kind = "marriage";
                        o.from = P.id;
                        o.to = matchPol;
                        o.groom = ch->sex == 'm' ? ch->id : match->id;
                        o.bride = ch->sex == 'm' ? match->id : ch->id;
                        makeOffer(x, o);
                        break;
                    }
                }
        }
        if (kd.peaceful) continue;
        // a camp that bleeds us: the lord's men ride out against it
        bool rode = false;
        for (auto& Q : x.s.polities) {
            if (rode || Q.ruined || Q.kind != "bandit_camp" || P.settlement != kNone) continue;
            const double g = x.s.view(P.id, Q.id).grudge() * a.grudge;
            if (g > 0.3 && sP > 1.5 * strength(x, Q) && x.day - Q.lastRaided >= inum(x.r.war, "raid_gap_days", 15) && rng.chance(0.5)) {
                log(x, "punitive", {{"pol", P.id}, {"pol2", Q.id}});
                raid(x, P, Q);
                {
                    Q.militia *= 0.75;
                    if (Q.militia < num(x.r.ban, "min_pop", 5) * 0.5 && rng.chance(0.5)) {
                        Q.ruined = true;
                        log(x, "camp_destroyed", {{"pol", Q.id}, {"fig", P.ruler}});
                        for (auto& f : x.s.figures)
                            if (f.alive && f.home == Q.id) f.home = kNone, f.role = "outlaw";
                    }
                }
                rode = true;
            }
        }
        if (rode) continue;
        // war, tribute, alliance: the best of what this ruler wants
        RealmPolity* target = nullptr;
        double bestU = 0.0;
        std::string act;
        for (auto& Q : x.s.polities) {
            if (Q.id == P.id || Q.ruined) continue;
            if (Q.kind == "monastery" || Q.kind == "bandit_camp") continue; // camps are hunted, not fought
            if (warBetween(x, P.id, Q.id) || allied(x, P.id, Q.id) || P.liege == Q.id || Q.liege == P.id) continue;
            const RealmView& v = x.s.view(P.id, Q.id);
            const double sQ = strength(x, Q) + 0.5 * [&] {
                double s = 0.0;
                for (const auto& [m, n] : x.s.alliances)
                    if (m == Q.id || n == Q.id)
                        if (const RealmPolity* al = x.s.polity(m == Q.id ? n : m)) s += strength(x, *al);
                return s;
            }();
            const double ratio = sP / std::max(1.0, sQ);
            const double grudge = v.grudge() * a.grudge;
            const double lure = std::min(1.0, (Q.wealth + Q.food * 0.05) / (P.wealth + P.food * 0.05 + 50.0));
            const double bread = P.food < P.pop * 30 && Q.food > Q.pop * 90 ? num(x.r.pol, "w_bread", 0.35) : 0.0;
            double u = num(x.r.pol, "w_grudge", 1.1) * grudge + num(x.r.pol, "w_greed", 0.45) * a.greed * lure +
                       num(x.r.pol, "w_ambition", 0.35) * a.ambition * (ratio > 1.0 ? 1.0 : 0.3) +
                       num(x.r.pol, "w_strength", 0.45) * std::clamp(ratio - 1.0, -1.0, 1.5) + bread -
                       num(x.r.pol, "w_honor", 0.45) * a.honor * (v.opinion > 0 ? 1.0 : 0.4) -
                       num(x.r.pol, "w_fear", 0.8) * std::max(0.0, 1.0 - ratio) - 0.25 * std::max(0.0, v.opinion);
            if (inTruce(x, P.id, Q.id)) u -= 0.6;
            u += rng.range(-0.1, 0.1);
            // nobody starts a war he expects to lose, unless the grudge is burning
            const bool canWar = warsOf(x, P.id) < inum(x.r.pol, "max_wars", 2) && x.r.arcOn("war") &&
                                (ratio >= num(x.r.pol, "war_min_ratio", 1.1) || (grudge >= 1.0 && ratio >= 0.8));
            if (canWar && u > num(x.r.pol, "war_threshold", 0.62) && u > bestU) {
                // an honourable ruler asks first; a greedy strong one demands tribute
                if (Q.kind != "bandit_camp" && ratio > 1.5 && (a.honor > 0.5 || a.greed > 0.5)) act = "tribute";
                else act = "war";
                bestU = u, target = &Q;
            } else if (Q.kind != "bandit_camp" && ratio > 1.8 && a.greed > 0.5 &&
                       u > num(x.r.pol, "demand_threshold", 0.45) && u > bestU && Q.tributeTo == kNone) {
                act = "tribute", bestU = u, target = &Q;
            }
        }
        // alliance against a common enemy, or with friends
        if (!target) {
            for (auto& Q : x.s.polities) {
                if (Q.id == P.id || Q.ruined || Q.kind == "bandit_camp" || allied(x, P.id, Q.id) || warBetween(x, P.id, Q.id)) continue;
                const double op = x.s.view(P.id, Q.id).opinion;
                bool commonFoe = false;
                for (const auto& wr : x.s.wars)
                    if (!wr.over && wr.attacker != wr.defender && (wr.defender == P.id || wr.defender == Q.id))
                        commonFoe |= warBetween(x, wr.attacker, P.id == wr.defender ? Q.id : P.id) == nullptr;
                if (op > num(x.r.pol, "alliance_opinion", 0.3) || (commonFoe && op > 0.0)) {
                    if (rng.chance(0.3)) {
                        RealmOffer o;
                        o.kind = "alliance";
                        o.from = P.id;
                        o.to = Q.id;
                        makeOffer(x, o);
                        break;
                    }
                }
            }
        }
        // suing for peace when losing
        for (auto& wr : x.s.wars) {
            if (wr.over || wr.attacker == wr.defender) continue;
            const bool att = wr.attacker == P.id, def = wr.defender == P.id;
            if (!att && !def) continue;
            const double mine = att ? wr.score : -wr.score;
            if (mine < -2.0 && rng.chance(0.5)) {
                RealmOffer o;
                o.kind = "peace";
                o.from = P.id;
                o.to = att ? wr.defender : wr.attacker;
                o.war = wr.id;
                makeOffer(x, o);
                break;
            }
        }
        if (!target) continue;
        if (act == "tribute") {
            RealmOffer o;
            o.kind = "tribute";
            o.from = P.id;
            o.to = target->id;
            o.amount = std::max(2.0, std::max(target->wealth, target->pop) * num(x.r.pol, "tribute_share", 0.06));
            makeOffer(x, o);
        } else {
            startWar(x, P, *target, x.s.view(P.id, target->id).grudge() > 0.3 ? "grievance" : (target->kind == "bandit_camp" ? "bandits" : "conquest"));
        }
    }
}

// --- wanderers and quests -----------------------------------------------------------------

double questUtility(const Ctx& x, const RealmFigure& f, const RealmQuest& q) {
    const RealmQuestKind& qk = x.r.quests.at(q.kind);
    const RealmPolity* p = x.s.polity(q.polity);
    const RealmPolity* at = x.s.polity(f.at);
    if (!p || p->ruined) return -1.0;
    if (static_cast<int>(q.takers.size()) >= qk.takers) return -1.0;
    if (std::find(q.takers.begin(), q.takers.end(), f.id) != q.takers.end()) return -1.0;
    if (q.giver == f.id || p->kind == "bandit_camp") return -1.0;
    if (f.life == "outlaw" && q.kind == "bandit_bounty") return -1.0;
    // never for those who wronged him
    if (f.claim != kNone && (q.polity == f.claim || p->id == f.claim)) return -1.0;
    for (const auto& [id, op] : f.opinions)
        if (op < -0.5 && std::to_string(p->ruler) == id) return -1.0;
    const double distKm = at ? dkm(at->pos, p->pos) : 20.0;
    double just = q.kind == "war_service" ? 0.05 : 0.3;
    if (q.kind == "war_service") {
        for (const auto& wr : x.s.wars)
            if (!wr.over && wr.defender == q.polity) just += 0.2;
        if (const RealmFigure* g = x.s.figure(q.giver); g && g->claim != kNone) just += 0.15; // a rightful claim
    }
    const double danger = qk.danger;
    return f.greed * q.reward / 80.0 + f.ambition * danger * 0.6 + f.honor * just - std::max(0.0, danger - f.martial) * 1.6 -
           distKm / 150.0 - (f.health < 0.7 ? 1.0 : 0.0);
}

// The road from `from` to `to` passes the village: a stop there.
bool passesVillage(const Ctx& x, Vec2 a, Vec2 b) {
    const RealmPolity* l = x.s.local();
    if (!l || l->ruined) return false;
    const Vec2 v{b.x - a.x, b.y - a.y};
    const double len2 = v.x * v.x + v.y * v.y;
    if (len2 < 1e-6) return false;
    const double t = std::clamp(((l->pos.x - a.x) * v.x + (l->pos.y - a.y) * v.y) / len2, 0.0, 1.0);
    const Vec2 c{a.x + v.x * t, a.y + v.y * t};
    if (dkm(c, l->pos) > num(x.r.wan, "stop_radius_km", 9) || t <= 0.05 || t >= 0.95) return false;
    Rng rng = agentRng(x.w.seed(), static_cast<EntityId>(a.x * 1000 + b.y), x.w.now(), "realm:stop");
    return rng.chance(num(x.r.wan, "stop_chance", 0.35));
}

void travel(Ctx& x, RealmFigure& f, EntityId to) {
    const RealmPolity* from = x.s.polity(f.at);
    const RealmPolity* dest = x.s.polity(to);
    if (!dest) return;
    const double spd = num(x.r.wan, "speed_km_day", 30);
    const Vec2 a = from ? from->pos : Vec2{0, 0};
    if (from && passesVillage(x, a, dest->pos) && dest->settlement == kNone) {
        const RealmPolity* l = x.s.local();
        f.travelTo = l->id;
        f.arrive = x.day + std::max(1, static_cast<int>(dkm(a, l->pos) / spd));
        f.claim = f.claim; // unchanged
        f.deeds.push_back({{"day", x.day}, {"code", "via_village"}, {"then", to}});
        return;
    }
    f.travelTo = to;
    f.arrive = x.day + std::max(1, static_cast<int>(dkm(a, dest->pos) / spd));
}

EntityId thenOf(const RealmFigure& f) {
    if (f.deeds.empty()) return kNone;
    const Json& d = f.deeds.back();
    return d.value("code", "") == "via_village" ? d.value("then", kNone) : kNone;
}

void questDone(Ctx& x, RealmQuest& q, bool ok, RealmFigure* hero) {
    q.status = ok ? "done" : "failed";
    q.closed = x.day;
    RealmPolity* p = x.s.polity(q.polity);
    const RealmQuestKind& qk = x.r.quests.at(q.kind);
    for (EntityId t : q.takers)
        if (RealmFigure* m = x.s.figure(t); m && m->alive) {
            m->quest = kNone;
            if (!ok) continue;
            const double share = q.reward / std::max<size_t>(1, q.takers.size());
            double paid = share;
            if (p && p->settlement != kNone) paid = payFromVillage(x, *p, share, m);
            else if (p) paid = std::min(share, std::max(0.0, p->wealth)), p->wealth -= paid, m->wealth += paid;
            m->renown = clamp01(m->renown + qk.renown);
            deed(*m, x.day, "quest_done", {{"quest", q.kind}, {"pol", q.polity}});
            log(x, "quest_done", {{"fig", m->id}, {"quest", q.kind}, {"pol", q.polity}, {"n", static_cast<int>(std::round(paid))}},
                x.w.character(m->id) ? std::vector<EntityId>{m->id} : std::vector<EntityId>{});
            if (p && p->settlement != kNone)
                if (Character* giver = x.w.character(q.giver))
                    if (Character* c = x.w.character(m->id)) {
                        giver->relations[c->id].respect = std::clamp(giver->relations[c->id].respect + 0.3, -1.0, 1.0);
                        giver->relations[c->id].affinity = std::clamp(giver->relations[c->id].affinity + 0.2, -1.0, 1.0);
                    }
        }
    if (!ok) log(x, "quest_failed", {{"quest", q.kind}, {"pol", q.polity}, {"fig", hero ? hero->id : kNone}});
}

// Away from the village: the bounty is a test of arms.
void resolveAway(Ctx& x, RealmQuest& q) {
    const RealmQuestKind& qk = x.r.quests.at(q.kind);
    Rng rng = rngOf(x, static_cast<EntityId>(q.id), "realm:quest");
    double party = 0.0;
    std::vector<RealmFigure*> men;
    for (EntityId t : q.takers)
        if (RealmFigure* m = x.s.figure(t); m && m->alive && m->captor == kNone) party += m->martial * m->health, men.push_back(m);
    if (men.empty()) {
        q.status = "failed", q.closed = x.day;
        return;
    }
    bool ok = false;
    if (q.kind == "bandit_bounty") {
        RealmPolity* camp = x.s.polity(q.target);
        if (!camp || camp->ruined) {
            questDone(x, q, true, men[0]);
            return;
        }
        const double sp = 8.0 * party + 2.0;
        const double sc = strength(x, *camp) * 0.7;
        ok = rng.uniform() * (sp + sc) < sp;
        if (ok) {
            camp->militia *= 0.3;
            camp->pop *= 0.4;
            camp->wealth *= 0.3;
            log(x, "camp_broken", {{"pol", camp->id}, {"fig", men[0]->id}});
            if (RealmFigure* at = x.s.figure(camp->ruler); at && at->alive) {
                if (rng.chance(0.5)) {
                    log(x, "ataman_slain", {{"fig", at->id}, {"fig2", men[0]->id}, {"pol", camp->id}});
                    figureDies(x, *at, "slain");
                } else {
                    at->life = "outlaw";
                    at->role = "outlaw";
                    at->home = kNone;
                    at->opinions[std::to_string(men[0]->id)] = -0.9; // he remembers
                }
            }
            if (camp->pop < num(x.r.ban, "min_pop", 5)) {
                camp->ruined = true;
                log(x, "camp_destroyed", {{"pol", camp->id}, {"fig", men[0]->id}});
                for (auto& f : x.s.figures)
                    if (f.alive && f.home == camp->id) f.home = kNone, f.role = "outlaw";
            }
        }
    } else {
        const double p = std::clamp(0.35 + 0.9 * (party / men.size() - qk.danger) + 0.08 * (men.size() - 1), 0.05, 0.95);
        ok = rng.chance(p);
        if (q.kind == "escort" && ok) {
            // the road was clear or the ambush beaten
            for (const auto& c : x.s.polities)
                if (!c.ruined && c.kind == "bandit_camp" && dkm(c.pos, x.s.polity(q.polity)->pos) < 30 && rng.chance(0.4)) {
                    log(x, "ambush_beaten", {{"fig", men[0]->id}, {"pol", c.id}});
                    break;
                }
        }
    }
    for (RealmFigure* m : men) {
        const double hurt = ok ? 0.08 : 0.3;
        if (rng.chance(ok ? qk.danger * 0.12 : qk.danger * 0.45)) {
            log(x, "hero_died", {{"fig", m->id}, {"quest", q.kind}, {"pol", q.polity}});
            figureDies(x, *m, "quest");
            continue;
        }
        if (rng.chance(0.5)) m->health = std::max(0.2, m->health - rng.range(0.05, hurt + 0.2));
    }
    questDone(x, q, ok, men[0]);
}

// In the village: the bounty is a real pack to kill.
void villageQuest(Ctx& x, RealmQuest& q, RealmFigure& f) {
    Character* c = x.w.character(f.id);
    if (!c || !c->body.alive) return;
    if (q.kind == "beast_bounty") {
        Character* pack = x.w.character(q.target);
        if (!pack || !pack->body.alive || pack->group <= 0) return; // done: checked in questsDaily
        if (c->controller.isPlayer()) return;
        bool queued = false;
        for (const auto& t : c->ai.queue) queued |= t.target == pack->id && t.source == kSource;
        if (!queued && x.w.content().interaction("attack"))
            c->ai.queue.push_back(QueuedTask{"attack", pack->id, num(x.r.loc, "hunt_queue_priority", 0.85), x.w.now(),
                                             static_cast<Tick>(inum(x.r.loc, "hunt_queue_ttl", 288)), "realm.bounty", kSource});
    }
}

void questsDaily(Ctx& x) {
    // escorts and beasts far away (aggregate fauna and trade, LOD)
    for (auto& p : x.s.polities) {
        if (p.ruined || p.settlement != kNone || p.kind == "bandit_camp") continue;
        if ((x.day + static_cast<int>(p.id)) % 30 != 0) continue;
        Rng rng = rngOf(x, p.id, "realm:needs");
        if (x.r.questOn("beast_bounty") && (p.kind == "village" || p.kind == "keep") && rng.chance(0.12) &&
            !hasOpenQuest(x, "beast_bounty", p.id, kNone) && p.wealth > 20) {
            const auto& rw = x.r.quests.at("beast_bounty").reward;
            postQuest(x, "beast_bounty", p.id, p.ruler, kNone, rng.range(rw[0], rw[1]));
        }
        bool bandits = false;
        for (const auto& c : x.s.polities) bandits |= !c.ruined && c.kind == "bandit_camp" && dkm(c.pos, p.pos) < 35;
        if (x.r.questOn("escort") && p.kind == "town" && bandits && p.wealth > 100 && rng.chance(0.35) &&
            !hasOpenQuest(x, "escort", p.id, kNone)) {
            const auto& rw = x.r.quests.at("escort").reward;
            postQuest(x, "escort", p.id, p.ruler, kNone, rng.range(rw[0], rw[1]));
        }
    }
    for (auto& q : x.s.quests) {
        if (q.status != "open") continue;
        RealmPolity* p = x.s.polity(q.polity);
        if (!p || p->ruined) {
            q.status = "expired", q.closed = x.day;
            continue;
        }
        if (q.kind == "beast_bounty" && p->settlement != kNone) {
            const Character* pack = x.w.character(q.target);
            if (!pack || !pack->body.alive || pack->group <= 0) {
                // the pack is gone: credit those who were here hunting it
                std::vector<EntityId> here;
                for (EntityId t : q.takers)
                    if (const RealmFigure* m = x.s.figure(t); m && m->present) here.push_back(t);
                q.takers = here;
                if (!here.empty()) questDone(x, q, true, x.s.figure(here[0]));
                else q.status = "expired", q.closed = x.day;
                continue;
            }
        }
        if (x.day > q.deadline && q.kind != "war_service") {
            if (!q.takers.empty()) {
                questDone(x, q, false, x.s.figure(q.takers[0]));
            } else {
                q.status = "expired";
                q.closed = x.day;
                log(x, "quest_expired", {{"quest", q.kind}, {"pol", q.polity}});
            }
        }
    }
    if (x.s.quests.size() > 120) {
        auto& v = x.s.quests;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const RealmQuest& q) { return q.status != "open" && x.day - q.closed > 360; }), v.end());
    }
}

RealmQuest* questById(Ctx& x, EntityId id) {
    for (auto& q : x.s.quests)
        if (q.id == id) return &q;
    return nullptr;
}

void arriveAt(Ctx& x, RealmFigure& f) {
    const EntityId to = f.travelTo;
    f.travelTo = kNone;
    f.at = to;
    RealmPolity* p = x.s.polity(to);
    if (!p) return;
    if (p->settlement != kNone) {
        // the village: as a Character
        Rng rng = rngOf(x, f.id, "realm:stay");
        const auto sd = pair2(x.r.wan, "stay_days", {1, 3});
        int stay = rng.irange(static_cast<int>(sd[0]), static_cast<int>(sd[1]));
        RealmQuest* q = questById(x, f.quest);
        if (q && q->kind == "beast_bounty" && q->polity == p->id) stay = inum(x.r.loc, "visit_hunt_days", 10);
        for (const auto& o : x.s.offers)
            if (o.status == "pending" && o.envoy == f.id) stay = std::max(stay, o.deadline - x.day + 1);
        if (Character* c = materialize(x, f, stay)) {
            const bool envoy = std::any_of(x.s.offers.begin(), x.s.offers.end(), [&](const RealmOffer& o) { return o.status == "pending" && o.envoy == f.id; });
            const RealmPolity* home = x.s.polity(f.home);
            log(x, envoy ? "envoy_arrives" : "arrives_village", {{"who", c->id}, {"pol", home ? home->id : kNone}}, {c->id});
            if (q && q->status == "open" && q->polity == p->id) villageQuest(x, *q, f);
            if (envoy) {
                // he goes to talk to the chief
                if (Character* chief = x.w.character(p->ruler); chief && x.w.content().interaction("chat"))
                    c->ai.queue.push_back(QueuedTask{"chat", chief->id, 0.9, x.w.now(), kTicksPerDay * 2, "realm.envoy", kSource});
            }
        }
        return;
    }
    RealmQuest* q = questById(x, f.quest);
    if (!q || q->status != "open") {
        f.quest = kNone;
        return;
    }
    if (q->kind == "war_service") return; // serves until the peace
    // the destination of the quest: the camp for a bounty, the polity otherwise
    const EntityId where = q->kind == "bandit_bounty" ? q->target : q->polity;
    if (to != where) {
        travel(x, f, where);
        return;
    }
    // a party waits a few days for the others on the way
    bool waiting = false;
    for (EntityId t : q->takers)
        if (const RealmFigure* m = x.s.figure(t); m && m->alive && m->id != f.id && m->travelTo != kNone && m->arrive <= x.day + 4)
            waiting = true;
    if (waiting) {
        f.arrive = x.day + 1;
        f.travelTo = to; // checks again tomorrow
        return;
    }
    resolveAway(x, *q);
}

void wanderersDaily(Ctx& x) {
    const int n = static_cast<int>(x.s.figures.size());
    for (int i = 0; i < n; ++i) {
        RealmFigure& f = x.s.figures[static_cast<size_t>(i)];
        if (!f.alive || f.captor != kNone) continue;
        if (f.present) {
            Character* c = x.w.character(f.id);
            if (!c) {
                f.present = false;
                continue;
            }
            if (!c->body.alive) {
                park(x, f);
                log(x, "died_in_village", {{"fig", f.id}});
                continue;
            }
            if (c->status.family != kNone || c->status.spouse != kNone) {
                settleInVillage(x, f);
                continue;
            }
            RealmQuest* q = questById(x, f.quest);
            if (q && q->status == "open" && q->polity == x.s.local()->id) villageQuest(x, *q, f);
            // stays while the hunt is on and the time is not up
            if (x.day >= f.leaveDay && canLeave(x, *c)) {
                const EntityId then = thenOf(f);
                log(x, "leaves_village", {{"who", c->id}}, {c->id});
                park(x, f);
                RealmFigure& g = x.s.figures[static_cast<size_t>(i)];
                if (then != kNone && then != g.at) travel(x, g, then);
                else if (g.home != kNone && g.home != g.at && g.life != "road") travel(x, g, g.home);
            }
            continue;
        }
        if (f.travelTo != kNone) {
            if (x.day >= f.arrive) arriveAt(x, f);
            continue;
        }
        if (f.life != "road" && f.life != "outlaw") continue;
        if (f.life == "outlaw") continue; // the band lives at the camp
        if (f.quest != kNone) {
            RealmQuest* q = questById(x, f.quest);
            if (!q || q->status != "open") f.quest = kNone;
            else continue;
        }
        if (f.ageYears(x.day) < 16) continue;
        // pick work
        RealmQuest* best = nullptr;
        double bestU = num(x.r.wan, "quest_threshold", 0.12);
        for (auto& q : x.s.quests) {
            if (q.status != "open" || !x.r.quests.count(q.kind)) continue;
            const double u = questUtility(x, f, q);
            if (u > bestU) bestU = u, best = &q;
        }
        if (best) {
            best->takers.push_back(f.id);
            f.quest = best->id;
            log(x, "quest_taken", {{"fig", f.id}, {"quest", best->kind}, {"pol", best->polity}});
            const EntityId where = best->kind == "bandit_bounty" ? best->target : best->polity;
            if (where != f.at) travel(x, f, where);
            continue;
        }
        // drift to the next town for work
        Rng rng = rngOf(x, f.id, "realm:drift");
        const auto dd = pair2(x.r.wan, "drift_days", {10, 25});
        if (!rng.chance(1.0 / std::max(1.0, rng.range(dd[0], dd[1])))) continue;
        std::vector<EntityId> to;
        std::vector<double> wt;
        double sum = 0.0;
        for (const auto& p : x.s.polities) {
            if (p.ruined || p.id == f.at || p.kind == "bandit_camp") continue;
            if (p.id == f.claim) continue;
            // the village draws wanderers when it has work for them
            const double w = p.settlement != kNone ? (hasOpenQuest(x, "beast_bounty", p.id, kNone) || hasOpenQuest(x, "bandit_bounty", p.id, kNone) ? 1.5 : 0.15)
                                                   : (p.kind == "town" ? 3.0 : 1.0);
            to.push_back(p.id), wt.push_back(w), sum += w;
        }
        if (to.empty()) continue;
        double pick = rng.range(0, sum);
        size_t k = 0;
        for (; k + 1 < to.size() && pick > wt[k]; ++k) pick -= wt[k];
        travel(x, f, to[k]);
    }
}

} // namespace

// --- the daily pass -------------------------------------------------------------------

void realmDaily(World& w) {
    const RealmRules& r = realmRules(w.content());
    if (!r.loaded || !r.enabled) return;
    RealmState s = loadRealm(w);
    Ctx x{w, s, r, w.day()};
    // Figures and polities are added during the pass while others hold references to them:
    // room enough that the vectors never move within a day.
    s.figures.reserve(s.figures.size() + 512);
    s.polities.reserve(s.polities.size() + 16);
    if (!s.inited) {
        if (w.settlements().empty()) return;
        genesis(x);
    }
    if (RealmPolity* l = s.local()) {
        syncLocal(x, *l);
        localNeeds(x, *l);
    }
    economyDaily(x);
    figuresDaily(x);
    notables(x);
    lifestyle(x);
    banditsFound(x);
    banditsDaily(x);
    resettle(x);
    decayGrievances(x);
    tributeDaily(x);
    councils(x);
    offersDaily(x);
    warsDaily(x);
    captives(x);
    revolts(x);
    reclaims(x);
    questsDaily(x);
    wanderersDaily(x);
    for (auto& h : hooks()) h(w, s);
    saveRealm(w, s);
}

// --- commands ---------------------------------------------------------------------------

namespace {

bool realmCommand(World& w, Command& cmd) {
    if (cmd.type.rfind("realm.", 0) != 0) return false;
    const RealmRules& r = realmRules(w.content());
    RealmState s = loadRealm(w);
    Ctx x{w, s, r, w.day()};
    RealmPolity* local = s.local();
    const EntityId actor = cmd.args.value("actor", kNone);
    auto reject = [&](const std::string& why) {
        cmd.result = "rejected:" + why;
        return true;
    };
    if (!r.loaded || !s.inited || !local) return reject("no_realm");
    const bool chief = actor != kNone && actor == local->ruler;
    const std::string t = cmd.type.substr(6);
    if (t == "take_quest") {
        RealmQuest* q = questById(x, cmd.args.value("quest", kNone));
        if (!q || q->status != "open" || q->polity != local->id) return reject("no_quest");
        if (std::find(q->takers.begin(), q->takers.end(), actor) == q->takers.end()) q->takers.push_back(actor);
        cmd.result = "ok";
        saveRealm(w, s);
        return true;
    }
    if (!chief) return reject("no_mandate");
    if (t == "declare_war" || t == "post_quest" || t == "answer" || t == "make_peace") {
        if (t == "declare_war") {
            RealmPolity* q = s.polity(cmd.args.value("polity", kNone));
            if (!q || q->ruined || q->id == local->id || warBetween(x, local->id, q->id)) return reject("bad_target");
            startWar(x, *local, *q, "chief");
        } else if (t == "make_peace") {
            RealmPolity* q = s.polity(cmd.args.value("polity", kNone));
            RealmWar* wr = q ? warBetween(x, local->id, q->id) : nullptr;
            if (!wr) return reject("no_war");
            RealmOffer o;
            o.kind = "peace";
            o.from = local->id;
            o.to = q->id;
            o.war = wr->id;
            makeOffer(x, o);
        } else if (t == "post_quest") {
            const std::string kind = cmd.args.value("kind", "beast_bounty");
            if (!r.questOn(kind)) return reject("kind");
            if (!postQuest(x, kind, local->id, actor, cmd.args.value("target", kNone), cmd.args.value("reward", 20.0)))
                return reject("kind");
        } else {
            const EntityId oid = cmd.args.value("offer", kNone);
            RealmOffer* o = nullptr;
            for (auto& q : s.offers)
                if (q.id == oid && q.status == "pending" && q.to == local->id) o = &q;
            if (!o) return reject("no_offer");
            applyOffer(x, *o, cmd.args.value("accept", false));
        }
        cmd.result = "ok";
        saveRealm(w, s);
        return true;
    }
    return reject("unknown");
}

} // namespace

void installRealm(World& w) { w.addCommandHandler(realmCommand); }

// --- checks and loc --------------------------------------------------------------------

std::vector<std::string> checkRealm(const Content& c) {
    std::vector<std::string> err;
    const RealmRules& r = realmRules(c);
    if (!r.loaded) return err;
    for (const auto& [k, n] : r.layout)
        if (!r.kinds.count(k)) err.push_back("realm.json: layout kind '" + k + "' is not in kinds");
    for (const auto& [t, ax] : r.traitAxes)
        if (!c.traits.count(t)) err.push_back("realm.json: trait_axes '" + t + "' is not a trait");
    for (const auto& [t, m] : r.grudgeMult)
        if (!c.traits.count(t)) err.push_back("realm.json: grudge_mult '" + t + "' is not a trait");
    for (const char* q : {"beast_bounty", "bandit_bounty", "war_service", "escort"})
        if (!r.quests.count(q)) err.push_back(std::string("realm.json: quest kind '") + q + "' is missing");
    if (!c.interaction("attack")) err.push_back("realm.json: the act 'attack' is missing (bounties in the village)");
    return err;
}

std::vector<std::string> realmLocKeys() {
    std::vector<std::string> k;
    for (const char* c :
         {"region", "harvest_bad", "harvest_good", "famine", "refugees", "polity_ruined", "camp_disbanded", "raid",
          "raid_repelled", "raid_village", "raid_village_repelled", "villager_hurt_raid", "villager_killed_raid",
          "levy_fell", "levy_wounded", "offer", "offer_accepted", "offer_refused", "offer_unanswered", "alliance",
          "marriage", "war_declared", "ally_joins", "ally_refuses", "battle", "siege_begins", "siege_falls",
          "peace_white", "peace_tribute", "peace_vassal", "peace_reparation", "tribute_unpaid", "tribute_ends",
          "fell_in_battle", "captured", "ransomed", "released", "captive_escaped", "fig_died", "birth", "notable",
          "took_road", "went_outlaw", "settled_down", "camp_founded", "camp_broken", "camp_destroyed", "ataman_slain",
          "ruler_died", "succession", "succession_crisis", "usurped", "claim_failed", "heir_to_monastery", "exiled",
          "reclaim_start", "reclaimed", "reclaim_failed", "revolt", "revolt_won", "revolt_crushed", "quest_posted",
          "quest_taken", "quest_done", "quest_failed", "quest_expired", "hero_died", "ambush_beaten", "punitive", "resettled",
          "arrives_village", "envoy_arrives", "leaves_village", "died_in_village", "wanderer_settles"})
        k.push_back(std::string("chronicle.realm.") + c);
    for (const char* q : {"beast_bounty", "bandit_bounty", "war_service", "escort", "haunting"})
        k.push_back(std::string("realm.quest.") + q + ".name");
    for (const char* o : {"tribute", "alliance", "marriage", "peace"}) k.push_back(std::string("realm.offer.") + o + ".name");
    for (const char* c : {"grievance", "conquest", "bandits", "refused_tribute", "chief"})
        k.push_back(std::string("realm.casus.") + c + ".name");
    for (const char* c : {"raid", "battle", "quest", "executed", "murdered", "revolt", "slain", "village"})
        k.push_back(std::string("cause.") + c + ".name");
    for (const char* m : {"raided", "fought_war", "paid_tribute"}) k.push_back(std::string("memory.") + m + ".text");
    return k;
}

Json realmNoun(const Loc& loc, const std::string& lang, const World& w, EntityId id) {
    if (id == kNone) return nullptr;
    const Json* j = w.moduleState("realm");
    if (!j || !j->is_object()) return nullptr;
    for (const auto& p : j->value("polities", Json::array()))
        if (p.value("id", kNone) == id) {
            const std::string nm = p.value("name", "");
            return loc.noun(lang, "realm.place." + nm + ".name", nm);
        }
    for (const auto& f : j->value("figures", Json::array()))
        if (f.value("id", kNone) == id) {
            const Json sx = f.value("sex", Json('m'));
            const char sex = sx.is_number() ? static_cast<char>(sx.get<int>()) : (sx.is_string() && !sx.get<std::string>().empty() ? sx.get<std::string>()[0] : 'm');
            return loc.personNoun(lang, f.value("name", ""), sex);
        }
    return nullptr;
}

} // namespace altera::sim
