// The realm (AL-85, AL-92): see realm.hpp and gdd/realm/REALM_v0.2_unified.md.
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

// A start template (realm.json "templates"): only the numbers a settlement is
// laid out with at the world's start. Nothing reads it afterwards: what a
// settlement is comes from classify().
struct RealmTemplate {
    std::array<double, 2> pop = {40, 100}, foodDays = {60, 120};
    double wealthPerHead = 1.0, land = 1.0, crafts = 0.0, fort = 0.0;
    std::string institution = "chief";
    std::array<int, 2> knights = {0, 0}, children = {1, 3};
    std::vector<std::string> tags;   // the history it starts with (outlaw...)
    std::string names;               // the name pool (realm.json names), "" = "*"
};

// What an institution means for whoever holds it (realm.json "institutions").
struct RealmInstitution {
    bool celibate = false;     // the head has no house to pass on (an abbot)
    bool elective = false;     // a new head from among the people, not an heir
    bool dynastic = true;      // the house of the head names the settlement's house
    bool solitary = false;     // AL-114: nobody joins it (a dragon's lair is a settlement of one)
    std::string tag;           // the tag it gives (cult)
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
    std::map<std::string, RealmTemplate> templates;
    std::map<std::string, RealmInstitution> institutions;
    Json cls;   // classification thresholds
    std::map<std::string, std::vector<std::string>> names;
    std::map<std::string, RealmQuestKind> quests;
    std::map<std::string, std::vector<std::string>> arcTags;
    Json eco, pol, war, suc, rev, ban, fig, wan, loc, ppl;
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
    const Json tpl = j.value("templates", Json::object());
    for (auto it = tpl.begin(); it != tpl.end(); ++it) {
        const Json& k = it.value();
        RealmTemplate d;
        d.pop = pair2(k, "pop", d.pop);
        d.foodDays = pair2(k, "food_days", d.foodDays);
        d.wealthPerHead = num(k, "wealth_per_head", d.wealthPerHead);
        d.land = num(k, "land", d.land);
        d.crafts = num(k, "crafts", d.crafts);
        d.fort = num(k, "fort", d.fort);
        d.institution = k.value("institution", d.institution);
        auto kn = pair2(k, "knights", {0, 0});
        d.knights = {static_cast<int>(kn[0]), static_cast<int>(kn[1])};
        auto ch = pair2(k, "children", {1, 3});
        d.children = {static_cast<int>(ch[0]), static_cast<int>(ch[1])};
        d.tags = k.value("tags", std::vector<std::string>{});
        d.names = k.value("names", std::string());
        r.templates[it.key()] = d;
    }
    const Json ins = j.value("institutions", Json::object());
    for (auto it = ins.begin(); it != ins.end(); ++it) {
        RealmInstitution d;
        d.celibate = it.value().value("celibate", false);
        d.elective = it.value().value("elective", false);
        d.dynastic = it.value().value("dynastic", true);
        d.solitary = it.value().value("solitary", false);
        d.tag = it.value().value("tag", std::string());
        r.institutions[it.key()] = d;
    }
    r.cls = j.value("classify", Json::object());
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
    r.ppl = j.value("people", Json::object());
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmQuest, id, kind, giver, polity, target, reward, posted, deadline,
                                                takers, status, closed)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmWar, id, attacker, defender, claimant, cause, start, last, score,
                                                battles, raids, startMilA, startMilD, alliesA, alliesD, siege, over,
                                                outcome)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RealmOffer, id, kind, from, to, envoy, amount, groom, bride, war, day,
                                                deadline, status)

void to_json(Json& j, const RealmState& s) {
    j = Json{{"format", 2},          {"inited", s.inited}, {"next_id", s.nextId}, {"year", s.year},
             {"harvest", s.harvest},  {"views", s.views},   {"alliances", s.alliances}, {"quests", s.quests},
             {"wars", s.wars},        {"offers", s.offers}, {"stats", s.stats},        {"once", s.once}};
}

void from_json(const Json& j, RealmState& s) {
    s = RealmState{};
    if (!j.is_object() || j.value("format", 1) < 2) return; // v0.1 (its own polities and figures): the region starts anew
    s.inited = j.value("inited", false);
    s.nextId = j.value("next_id", 1);
    s.year = j.value("year", -1);
    s.harvest = j.value("harvest", 1.0);
    s.views = j.value("views", std::map<std::string, RealmView>{});
    s.alliances = j.value("alliances", std::vector<std::pair<EntityId, EntityId>>{});
    s.quests = j.value("quests", std::vector<RealmQuest>{});
    s.wars = j.value("wars", std::vector<RealmWar>{});
    s.offers = j.value("offers", std::vector<RealmOffer>{});
    s.stats = j.value("stats", std::map<std::string, int>{});
    s.once = j.value("once", std::map<std::string, int>{});
}

double RealmView::grudge() const {
    double g = 0.0;
    for (const auto& x : grievances) g += x.weight;
    return g;
}

RealmView& RealmState::view(EntityId a, EntityId b) { return views[std::to_string(a) + ">" + std::to_string(b)]; }
Settlement* realmLocal(World& w) {
    for (auto& [id, p] : w.settlements())
        if (p.full()) return &p;
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
int years(const Character& c) { return c.body.lifeYears(); } // AL-114: in human years
double dkm(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

Rng rngOf(const Ctx& x, EntityId id, std::string_view salt) { return x.w.rng(id, salt); }

void log(Ctx& x, const std::string& code, Json params, std::vector<EntityId> who = {}) {
    ++x.s.stats[code];
    x.w.chronicle("realm", std::move(who), "chronicle.realm." + code, std::move(params));
}

// Once per key (a famine per season, unpaid tribute per year): true the first time.
bool firstTime(Ctx& x, const std::string& key) {
    if (x.s.once.count(key)) return false;
    x.s.once[key] = x.day;
    return true;
}

void deed(Character& f, int day, const std::string& code, Json extra = Json::object()) {
    extra["day"] = day;
    extra["code"] = code;
    f.standing.deeds.push_back(std::move(extra));
    if (f.standing.deeds.size() > 24) f.standing.deeds.erase(f.standing.deeds.begin());
}

// --- who and where: the world's own settlements and people (AL-92) ------------------

Settlement* pol(Ctx& x, EntityId id) {
    if (id == kNone) return nullptr;
    auto it = x.w.settlements().find(id);
    return it == x.w.settlements().end() ? nullptr : &it->second;
}
const Settlement* pol(const Ctx& x, EntityId id) {
    if (id == kNone) return nullptr;
    auto it = x.w.settlements().find(id);
    return it == x.w.settlements().end() ? nullptr : &it->second;
}
// A person, on the map or off it.
Character* fig(Ctx& x, EntityId id) { return id == kNone ? nullptr : x.w.person(id); }
const Character* fig(const Ctx& x, EntityId id) { return id == kNone ? nullptr : x.w.person(id); }
Settlement* localOf(const Ctx& x) { return realmLocal(x.w); }
// On the map now (computed in full), not offstage.
bool onMap(const Ctx& x, const Character& c) { return x.w.character(c.id) == &c; }
int bornDay(const Ctx& x, const Character& c) { return x.day - c.body.ageDays; }

// Every settlement of the world, in id order.
std::vector<std::reference_wrapper<Settlement>> polities(Ctx& x) {
    std::vector<std::reference_wrapper<Settlement>> out;
    for (auto& [id, p] : x.w.settlements()) out.push_back(p);
    return out;
}

// The people the realm moves: everyone offstage, and those on the map who have a
// place in the wider world (a role) - a wanderer passing through, an envoy. The
// map's villagers live by the village's own rules until they take a role.
std::vector<std::reference_wrapper<Character>> people(Ctx& x) {
    std::vector<std::reference_wrapper<Character>> out;
    // the people the chronicle follows: those with a Standing role (the rest are residents())
    for (auto& [id, c] : x.w.offstage())
        if (!c.isAnimal() && !c.standing.role.empty()) out.push_back(c);
    for (auto& [id, c] : x.w.characters())
        if (!c.isAnimal() && !c.standing.role.empty() && (c.body.alive || c.standing.deathDay == 0)) out.push_back(c);
    return out;
}

// Whether `c` lives in settlement `p` offstage (counted in its numbers): at home, not on the road, free.
bool residentOf(const Character& c, EntityId p) {
    return c.body.alive && !c.isAnimal() && c.status.settlement == p && c.standing.life != "road" && c.standing.captor == kNone;
}
// The people who live in a stat settlement, every one (AL-92): its numbers are their count.
std::vector<Character*> residents(Ctx& x, EntityId p) {
    std::vector<Character*> out;
    for (auto& [id, c] : x.w.offstage())
        if (residentOf(c, p)) out.push_back(&c);
    return out;
}

// Coin: the purse while away, the coin item on the map (moved across when the person crosses).
double wealthOf(const Ctx& x, const Character& c) { return c.standing.purse + (onMap(x, c) ? c.has("coin") : 0); }
void earn(Ctx& x, Character& c, double v) {
    c.standing.purse += v;
    if (onMap(x, c) && c.standing.purse >= 1.0 && x.w.content().items.count("coin")) {
        const int n = static_cast<int>(std::floor(c.standing.purse));
        c.inventory["coin"] += n;
        c.standing.purse -= n;
    }
    if (c.standing.purse < 0.0) {
        // spent more than the purse: from the coin
        const int need = static_cast<int>(std::ceil(-c.standing.purse));
        const int have = c.has("coin");
        const int take = std::min(have, need);
        if (take > 0) c.inventory["coin"] = have - take;
        c.standing.purse = std::min(0.0, c.standing.purse + take);
        if (c.standing.purse < 0.0) c.standing.purse = 0.0;
    }
}
void setWealth(Ctx& x, Character& c, double v) { earn(x, c, v - wealthOf(x, c)); }

// The ruler of a settlement: a person like any other.
bool rulerAlive(const Ctx& x, const Settlement& p) { return person(fig(x, p.chief)); }

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

// What drives a person: the rolled axes, or (never rolled: a villager) read from the traits and skills.
Axes axesOf(const Ctx& x, const Character& c) {
    Axes a;
    if (c.standing.drawn) {
        const Standing& s = c.standing;
        a.martial = s.martial, a.ambition = s.ambition, a.greed = s.greed, a.honor = s.honor;
        a.wanderlust = s.wanderlust;
        for (const auto& t : c.traits)
            if (auto g = x.r.grudgeMult.find(t); g != x.r.grudgeMult.end()) a.grudge *= g->second;
    } else {
        a = axesOfTraits(x.r, c.traits);
        double fight = 0.0;
        for (const char* k : {"combat", "hunting", "spear", "melee"})
            if (auto it = c.skills.find(k); it != c.skills.end()) fight = std::max(fight, it->second);
        a.martial = clamp01(a.martial * 0.5 + fight * 0.5);
    }
    a.renown = c.standing.renown;
    a.player = c.controller.isPlayer();
    a.martial = clamp01(a.martial), a.ambition = clamp01(a.ambition), a.greed = clamp01(a.greed);
    a.honor = clamp01(a.honor);
    return a;
}

Axes rulerAxes(const Ctx& x, const Settlement& p) {
    const Character* c = fig(x, p.chief);
    return c ? axesOf(x, *c) : Axes{};
}

// The people of a settlement on the map.
std::vector<Character*> localPeople(Ctx& x, const Settlement& p) {
    std::vector<Character*> out;
    for (auto& [id, c] : x.w.characters())
        if (person(&c) && c.status.settlement == p.id) out.push_back(&c);
    return out;
}

std::vector<Character*> levy(Ctx& x, const Settlement& p) {
    std::vector<Character*> out;
    const int lo = inum(x.r.loc, "levy_from_years", 16), hi = inum(x.r.loc, "levy_to_years", 55);
    for (Character* c : localPeople(x, p))
        if (c->body.sex == 'm' && years(*c) >= lo && years(*c) <= hi && !c->captivity.held()) out.push_back(c);
    return out;
}

std::vector<WorldObject*> localStores(Ctx& x, const Settlement& p) {
    std::vector<WorldObject*> out;
    for (EntityId fid : p.families) {
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

// A settlement in full mode: its numbers are read from the map (the people, the stores).
void syncFull(Ctx& x, Settlement& p) {
    const auto people = localPeople(x, p);
    p.pop = static_cast<double>(people.size());
    p.ruined = people.empty();
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

const RealmInstitution& institutionOf(const RealmRules& r, const Settlement& p) {
    static const RealmInstitution kDef;
    auto it = r.institutions.find(p.institution);
    return it == r.institutions.end() ? kDef : it->second;
}

// --- what a settlement is: classified, never set (AL-92) ------------------------------

double share(const Settlement& p, const char* src) {
    double sum = 0.0;
    for (const auto& [k, v] : p.income) sum += std::max(0.0, v);
    auto it = p.income.find(src);
    return sum > 1e-9 && it != p.income.end() ? std::max(0.0, it->second) / sum : 0.0;
}

void classify(const RealmRules& r, Settlement& p, int day) {
    const Json& c = r.cls;
    const double pop = p.pop;
    p.size = pop < num(c, "hamlet_below", 25) ? "hamlet"
             : pop < num(c, "village_below", 150) ? "village"
             : pop < num(c, "town_below", 600) ? "town" : "city";
    auto set = [&](const char* t, bool on) {
        if (on) p.tags.insert(t);
        else p.tags.erase(t);
    };
    const bool raidsFeed = share(p, "raids") >= num(c, "raiders_share", 0.35);
    const bool noFields = p.land < num(c, "no_fields_land", 0.3);
    // raiders: it lives by raids (or has no fields and is outside the law)
    set("raiders", raidsFeed || (noFields && p.tagged("outlaw")));
    // outlaw is history: it stays while the settlement raids, and fades after years of peace
    if (p.tagged("outlaw") && !p.tagged("raiders") && day - p.lastRaid > inum(c, "outlaw_fades_days", 720)) p.tags.erase("outlaw");
    if (raidsFeed && day - p.lastRaid < 120 && p.liege == kNone) p.tags.insert("outlaw");
    const RealmInstitution& in = institutionOf(r, p);
    for (const auto& [k, d] : r.institutions)
        if (!d.tag.empty() && d.tag != in.tag) p.tags.erase(d.tag);
    if (!in.tag.empty()) p.tags.insert(in.tag);
    set("fortified", p.fort >= num(c, "fortified_from", 0.5));
    // on the map the fields are real and worked: a settlement with land farms
    set("farming", !noFields && (p.full() || share(p, "fields") >= num(c, "farming_share", 0.5)));
    set("trade", p.crafts >= num(c, "trade_crafts", 0.3));
    set("vassal", p.liege != kNone);
    set("tributary", p.tributeTo != kNone);
    set("ruined", p.ruined);
}

// What its people can field: a share of the heads by what they are (classified).
double militiaShare(const RealmRules& r, const Settlement& p) {
    const Json& e = r.eco;
    double s = num(e, "militia_base", 0.18) + num(e, "militia_per_fort", 0.2) * p.fort - num(e, "militia_per_crafts", 0.2) * p.crafts;
    if (p.tagged("raiders")) s = num(e, "militia_raiders", 0.85);
    if (p.tagged("cult")) s = std::min(s, num(e, "militia_cult", 0.12));
    return std::clamp(s, 0.02, 0.95);
}

// Coin a head makes in a year by its work (crafts and trade make more).
double incomePerHead(const RealmRules& r, const Settlement& p) {
    if (p.tagged("raiders")) return 0.0;
    return num(r.eco, "income_base", 0.5) + num(r.eco, "income_per_crafts", 3.5) * p.crafts;
}

double heads(const Settlement& p) { return std::max(1.0, p.pop); }

// Peace-minded by what the settlement is (a cult's head does not lead men to war).
bool peaceful(const Settlement& p) { return p.tagged("cult"); }
// Outside the law: nobody treats with it, it is hunted.
bool outlaw(const Settlement& p) { return p.tagged("outlaw"); }

std::vector<EntityId> mercsOf(const Ctx& x, EntityId polity) {
    std::vector<EntityId> out;
    for (const auto& q : x.s.quests)
        if (q.kind == "war_service" && q.status == "open" && q.polity == polity)
            for (EntityId t : q.takers)
                if (const Character* f = fig(x, t); f && f->body.alive && f->standing.at == polity && f->standing.travelTo == kNone)
                    out.push_back(t);
    return out;
}

} // namespace

void realmClassify(World& w, Settlement& p) { classify(realmRules(w.content()), p, w.day()); }

double realmStrength(const World& w, const RealmState& s, const Settlement& p) {
    if (p.ruined) return 0.0;
    const RealmRules& r = realmRules(w.content());
    Ctx x{const_cast<World&>(w), const_cast<RealmState&>(s), r, w.day()};
    const Axes a = rulerAxes(x, p);
    double str = p.militia * (0.4 + p.morale) * (0.7 + 0.6 * a.martial);
    for (EntityId m : mercsOf(x, p.id))
        if (const Character* f = fig(x, m)) str += 4.0 * f->standing.martial * f->body.health;
    // AL-114: a body worth an army (a grown dragon) counts by its kind's might
    if (w.content().peoples.loaded)
        for (const auto* pool : {&w.offstage(), &w.characters()})
            for (const auto& [id, f] : *pool)
                if (f.body.alive && !f.race.empty() && f.status.settlement == p.id && f.standing.captor == kNone &&
                    f.body.stage() != "child")
                    if (const RaceDef* rd = w.content().raceOf(f.race); rd && rd->might > 0.0) str += rd->might * f.body.health;
    if (!p.full())  // on the map the knights are in the levy already
        for (const auto& [id, f] : w.offstage())
            if (f.body.alive && f.status.settlement == p.id && f.standing.at == p.id && f.standing.role == "knight" &&
                f.standing.captor == kNone)
                str += 3.0 * f.standing.martial * f.body.health;
    return str;
}

namespace {

double strength(const Ctx& x, const Settlement& p) { return realmStrength(x.w, x.s, p); }

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

// An exile's band for taking his seat back (not hired for someone's war).
bool isReclaim(const Ctx& x, const RealmQuest& q) {
    const Character* g = fig(x, q.giver);
    return q.kind == "war_service" && g && g->standing.claim == q.polity && g->status.settlement != q.polity;
}

bool inTruce(Ctx& x, EntityId a, EntityId b) {
    const int truce = inum(x.r.pol, "truce_days", 360);
    for (const auto& wr : x.s.wars) {
        if (!wr.over || x.day - wr.last >= truce) continue;
        // the sides of a war that ended lately, allies included, keep the truce
        auto side = [&](EntityId p, bool att) {
            if (p == (att ? wr.attacker : wr.defender)) return true;
            const auto& al = att ? wr.alliesA : wr.alliesD;
            return std::find(al.begin(), al.end(), p) != al.end();
        };
        if ((side(a, true) && side(b, false)) || (side(a, false) && side(b, true))) return true;
    }
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

// A new person of the world, offstage: a whole Character from the first day (needs,
// skills, gear), so that walking onto the map is only a move.
// One of the people (AL-92): a body, a name, a home, the hands of a household. Most
// people stay that; some draw a Standing (promote) and walk into the chronicle.
Character& newResident(Ctx& x, Rng& rng, EntityId home, char sex, int ageYears, const std::string& house) {
    const Content& ct = x.w.content();
    Character f;
    f.id = x.w.newId();
    f.body.sex = sex;
    f.standing.house = house;
    for (int tries = 0; tries < 10; ++tries) {
        f.name = randomPersonName(rng, sex == 'f', &ct) + " " + house;
        const bool taken = std::any_of(x.w.offstage().begin(), x.w.offstage().end(),
                                       [&](const auto& kv) { return kv.second.body.alive && kv.second.name == f.name; }) ||
                           std::any_of(x.w.characters().begin(), x.w.characters().end(),
                                       [&](const auto& kv) { return kv.second.name == f.name; });
        if (!taken) break;
    }
    f.body.ageDays = std::max(0, ageYears * kDaysPerYear + rng.irange(0, kDaysPerYear - 1));
    // AL-114: the kind from the settlement's peoples (a stream of its own: the rest draws as before)
    if (const Settlement* hp = pol(x, home); hp && !hp->peoples.empty() && ct.peoples.loaded) {
        Rng rr = rngOf(x, f.id, "race:kind");
        setRace(ct, f, drawRace(hp->peoples, rr, ct.peoples.defaultRace));
        if (!f.race.empty()) {
            f.body.ageDays = raceAgeDays(raceOf(ct, f), ageYears + rr.uniform());
            if (const std::string n = cultureName(ct, f.race, rr, sex == 'f', false); !n.empty())
                f.name = house.empty() ? n : n + " " + house;
        }
    }
    f.status.settlement = f.standing.at = home;
    const int nt = inum(x.r.fig, "traits", 2);
    for (int i = 0; i < nt; ++i)
        if (auto t = pickTrait(rng, ct, f.traits); !t.empty()) f.traits.push_back(t);
    for (const auto& n : ct.needs) f.needs[n.id] = rng.range(std::max(0.5, n.startLo), std::max(0.7, n.startHi));
    if (!f.race.empty()) { // AL-114: the kind's leanings among the traits drawn
        const RaceDef& rd = raceOf(ct, f);
        for (const auto& [t, wgt] : rd.traitsPool)
            if (wgt >= 2.0 && ct.traits.count(t) && !f.hasTrait(t) && rngOf(x, f.id, "race:trait:" + t).chance(0.25 * wgt))
                f.traits.push_back(t);
    }
    if (ageYears >= 12)
        for (const auto& k : ct.householdMust) {
            f.knowledge.insert(k);
            f.skills[k] = rng.range(0.2, 0.45);
        }
    const EntityId id = f.id;
    x.w.offstage().emplace(id, std::move(f));
    return x.w.offstage().at(id);
}

// Someone steps out of the crowd: a role, the axes the chronicle reads, a purse and a kit.
Character& promote(Ctx& x, Rng& rng, Character& f, const std::string& role) {
    const Content& ct = x.w.content();
    f.standing.role = role;
    Axes a = axesOfTraits(x.r, f.traits);
    Standing& s = f.standing;
    s.drawn = true;
    const char sex = f.body.sex;
    s.martial = clamp01(a.martial + rng.range(-0.2, 0.35) + (sex == 'm' ? 0.1 : -0.1));
    s.ambition = clamp01(a.ambition + rng.range(-0.25, 0.4));
    s.greed = clamp01(a.greed + rng.range(-0.2, 0.35));
    s.honor = clamp01(a.honor + rng.range(-0.3, 0.3));
    s.wanderlust = clamp01(a.wanderlust + rng.range(-0.15, 0.4));
    s.renown = role == "ruler" ? rng.range(0.15, 0.45) : (role == "knight" ? rng.range(0.1, 0.3) : rng.range(0.0, 0.1));
    s.purse = role == "ruler" ? rng.range(20, 60) : rng.range(2, 15);
    for (const auto& k : ct.householdMust)
        if (!f.knowledge.count(k)) {
            f.knowledge.insert(k);
            f.skills[k] = rng.range(0.2, 0.45);
        }
    for (const char* k : {"hunting", "combat"})
        if (ct.knowledge.count(k)) {
            f.knowledge.insert(k);
            f.skills[k] = std::max(f.skills[k], clamp01(0.2 + s.martial * 0.7));
        }
    if (itemDef(ct, "spear")) f.inventory["spear"] = 1;
    const char* coat = s.martial > 0.6 ? "mail_shirt" : "gambeson";
    if (itemDef(ct, coat)) f.inventory[coat] = 1;
    for (const char* it : {"bread", "dried_meat"})
        if (itemDef(ct, it)) f.inventory[it] = 3;
    return f;
}

Character& newFigure(Ctx& x, Rng& rng, EntityId home, const std::string& role, char sex, int ageYears,
                     const std::string& house) {
    Character& f = newResident(x, rng, home, sex, ageYears, house);
    return promote(x, rng, f, role);
}

void marry(Character& a, Character& b) {
    a.status.spouse = b.id;
    b.status.spouse = a.id;
}

Character& childOf(Ctx& x, Rng& rng, Character& father, Character& mother, int ageYears) {
    const std::string house = father.standing.house;
    const EntityId fid = father.id, mid = mother.id, home = father.status.settlement;
    Character& k = newFigure(x, rng, home, "kin", rng.chance(0.5) ? 'm' : 'f', ageYears, house);
    k.status.father = fid;
    k.status.mother = mid;
    const EntityId kid = k.id;
    if (Character* f = fig(x, fid)) f->status.children.push_back(kid);
    if (Character* m = fig(x, mid)) m->status.children.push_back(kid);
    return *fig(x, kid);
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

// --- the people of a settlement (AL-92) -------------------------------------------------

bool lawlessHome(const Ctx& x, EntityId p) {
    const Settlement* s = pol(x, p);
    return s && (outlaw(*s) || s->tagged("raiders"));
}

// Fills a stat settlement with its people up to its numbers: households (a couple,
// their children, now and then an old parent) and singles, by the demography in
// realm.json "people". A band outside the law is young men; a celibate house has no
// families. Existing people (the ruling family, knights) count.
void populate(Ctx& x, Settlement& p) {
    if (p.full() || p.ruined) return;
    const Json& d = x.r.ppl;
    Rng rng = rngOf(x, p.id, "realm:populate");
    int have = static_cast<int>(residents(x, p.id).size());
    const int want = static_cast<int>(std::round(p.pop));
    const bool lawless = p.tagged("outlaw") || p.tagged("raiders");
    const bool celibate = institutionOf(x.r, p).celibate;
    std::set<std::string> used;
    while (have < want) {
        const std::string house = pickHouse(x, rng, used);
        if (lawless || celibate || rng.chance(num(d, "single_share", 0.18)) || want - have < 2) {
            const char sex = lawless ? (rng.chance(0.9) ? 'm' : 'f') : celibate ? (rng.chance(0.6) ? 'm' : 'f') : (rng.chance(0.5) ? 'm' : 'f');
            Character& c = newResident(x, rng, p.id, sex, lawless ? rng.irange(17, 40) : rng.irange(16, 70), house);
            if (lawless) c.standing.life = "outlaw";
            ++have;
            continue;
        }
        Character& man = newResident(x, rng, p.id, 'm', rng.irange(19, 58), house);
        const EntityId mid = man.id;
        const int my = years(man);
        Character& wife = newResident(x, rng, p.id, 'f', std::clamp(my - rng.irange(-3, 7), 16, 60), house);
        const EntityId wid = wife.id;
        marry(*fig(x, mid), *fig(x, wid));
        have += 2;
        const int wy = years(*fig(x, wid));
        const auto kidsN = pair2(d, "children", {0, 5});
        const int kids = std::min(want - have, rng.irange(static_cast<int>(kidsN[0]), static_cast<int>(kidsN[1])));
        for (int k = 0; k < kids && wy > 17; ++k) {
            Character& ch = newResident(x, rng, p.id, rng.chance(0.5) ? 'm' : 'f', rng.irange(0, std::min(wy - 17, 24)), house);
            ch.status.father = mid, ch.status.mother = wid;
            const EntityId cid = ch.id;
            fig(x, mid)->status.children.push_back(cid);
            fig(x, wid)->status.children.push_back(cid);
            ++have;
        }
        if (have < want && rng.chance(num(d, "elder_share", 0.15))) {
            newResident(x, rng, p.id, rng.chance(0.6) ? 'f' : 'm', rng.irange(60, 78), house);
            ++have;
        }
    }
    // fields: how many heads they feed at full yield (more hands add little)
    if (p.fields <= 0.0 && p.land > 0.0) {
        const auto f = pair2(d, "fields_per_head", {1.3, 1.7});
        p.fields = std::round(std::max(10.0, p.pop) * rng.range(f[0], f[1]));
    }
    p.pop = have;
}

// The death of one of the people. A person of Standing dies through figureDies (the
// chronicle, a successor); the rest quietly: the widow is free to marry again.
void figureDies(Ctx& x, Character& who, const std::string& cause);
void personDies(Ctx& x, Character& c, const std::string& cause) {
    if (!c.body.alive) return;
    if (!c.standing.role.empty()) return figureDies(x, c, cause);
    c.body.alive = false;
    c.body.causeOfDeath = cause;
    c.standing.deathDay = x.day;
    if (Character* sp = fig(x, c.status.spouse)) sp->status.spouse = kNone;
    ++x.s.stats["died_" + cause];
}

// `n` of a settlement's people die by `cause` (a raid, a battle, a riot put down): the
// fighting age first for violence, anyone else otherwise. People of Standing are
// spared here (their deaths are the chronicle's own).
void losePeople(Ctx& x, Settlement& p, int n, const std::string& cause) {
    if (n <= 0 || p.full()) return;
    const bool violence = cause != "hunger" && cause != "illness";
    std::vector<Character*> pool;
    std::vector<double> wt;
    for (Character* c : residents(x, p.id)) {
        if (!c->standing.role.empty()) continue;
        const int y = years(*c);
        double w = 1.0;
        if (violence) w = (y >= 16 && y < 60) ? (c->body.sex == 'm' ? 3.0 : 1.0) : 0.3;
        pool.push_back(c), wt.push_back(w);
    }
    Rng rng = rngOf(x, p.id, "realm:lose:" + cause);
    for (int k = 0; k < n && !pool.empty(); ++k) {
        double sum = 0.0;
        for (double w : wt) sum += w;
        double pick = rng.range(0.0, sum);
        size_t i = 0;
        for (; i + 1 < pool.size() && pick > wt[i]; ++i) pick -= wt[i];
        personDies(x, *pool[i], cause);
        pool.erase(pool.begin() + static_cast<long>(i));
        wt.erase(wt.begin() + static_cast<long>(i));
    }
}

// A household of `p` (a couple and their children at home, or a single): who leaves together.
std::vector<EntityId> householdOf(Ctx& x, Character& c) {
    std::vector<EntityId> out = {c.id};
    if (Character* sp = fig(x, c.status.spouse); sp && residentOf(*sp, c.status.settlement) && sp->standing.role.empty())
        out.push_back(sp->id);
    for (EntityId k : c.status.children)
        if (Character* ch = fig(x, k); ch && residentOf(*ch, c.status.settlement) && ch->standing.role.empty() &&
                                       ch->status.spouse == kNone && years(*ch) < 16)
            out.push_back(k);
    return out;
}

// Up to `n` of the people of `from` move to `to` (whole households; `adults`: grown-ups
// one by one, as the restless run off to a band). Returns how many went.
int movePeople(Ctx& x, Settlement& from, Settlement& to, int n, bool adults, const std::string& salt) {
    if (n <= 0 || from.full()) return 0;
    Rng rng = rngOf(x, from.id, "realm:move:" + salt);
    std::vector<Character*> pool;
    for (Character* c : residents(x, from.id))
        if (c->standing.role.empty() && years(*c) >= 16 && (!adults || years(*c) < 45)) pool.push_back(c);
    int moved = 0;
    while (moved < n && !pool.empty()) {
        const size_t i = static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1));
        Character* c = pool[i];
        pool.erase(pool.begin() + static_cast<long>(i));
        if (c->status.settlement != from.id) continue; // went with a spouse already
        std::vector<EntityId> who = adults ? std::vector<EntityId>{c->id} : householdOf(x, *c);
        for (EntityId id : who) {
            Character* m = fig(x, id);
            if (!m) continue;
            if (adults && m->status.spouse != kNone)
                if (Character* sp = fig(x, m->status.spouse)) sp->status.spouse = kNone, m->status.spouse = kNone; // left behind
            m->status.settlement = m->standing.at = to.id;
            if (lawlessHome(x, to.id)) m->standing.life = "outlaw";
            else if (m->standing.life == "outlaw") m->standing.life = "settled";
            ++moved;
        }
    }
    return moved;
}

// A band broken: half its common men fall, the rest slip back to the nearest place
// that takes people in and live there as anyone else.
void scatterBand(Ctx& x, Settlement& camp) {
    int n = 0;
    for (Character* c : residents(x, camp.id)) n += c->standing.role.empty() ? 1 : 0;
    losePeople(x, camp, n / 2, "battle");
    Settlement* to = nullptr;
    for (Settlement& p : polities(x))
        if (p.id != camp.id && !p.ruined && !p.full() && !lawlessHome(x, p.id) && (!to || dkm(p.km, camp.km) < dkm(to->km, camp.km)))
            to = &p;
    for (Character* c : residents(x, camp.id)) {
        if (!c->standing.role.empty()) continue;
        if (to) c->status.settlement = c->standing.at = to->id, c->standing.life = "settled";
        else personDies(x, *c, "battle");
    }
    camp.pop = 0.0;
}

// One of the common people of `p` in an age range (of a sex, 0 = either), to step
// forward: null when there is none (then a newcomer is made).
Character* fromCrowd(Ctx& x, Rng& rng, EntityId p, int minAge, int maxAge, char sex) {
    std::vector<Character*> pool;
    for (Character* c : residents(x, p))
        if (c->standing.role.empty() && years(*c) >= minAge && years(*c) <= maxAge && (!sex || c->body.sex == sex)) pool.push_back(c);
    if (pool.empty()) return nullptr;
    return pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
}

// --- genesis ---------------------------------------------------------------------

// A settlement laid out from a start template: the numbers only, the rest it earns.
Settlement& newSettlement(Ctx& x, Rng& rng, const std::string& tpl, const std::string& name, Vec2 km) {
    const RealmTemplate& t = x.r.templates.count(tpl) ? x.r.templates.at(tpl) : RealmTemplate{};
    Settlement p;
    p.id = x.w.newId();
    p.mode = "stat";
    p.name = name;
    p.km = km;
    p.pop = std::round(rng.range(t.pop[0], t.pop[1]));
    p.food = p.pop * rng.range(t.foodDays[0], t.foodDays[1]);
    p.wealth = p.pop * t.wealthPerHead * rng.range(0.7, 1.3);
    p.land = t.land * rng.range(0.9, 1.1);
    p.crafts = t.crafts;
    p.fort = t.fort;
    p.institution = t.institution;
    p.founded = x.day;
    for (const auto& g : t.tags) p.tags.insert(g);
    if (auto mx = x.w.content().peoples.templateMix.find(tpl); mx != x.w.content().peoples.templateMix.end())
        p.peoples = mx->second; // AL-114: who lives there
    // what it has lived by so far (the last half-year): fields and work, as the numbers say
    p.income["fields"] = 0.5 * p.pop * num(x.r.eco, "year_yield_days", 400) * p.land * num(x.r.eco, "grain_price", 0.05);
    classify(x.r, p, x.day);
    p.income["crafts"] = 0.5 * p.pop * incomePerHead(x.r, p);
    classify(x.r, p, x.day);
    p.militia = std::round(p.pop * militiaShare(x.r, p));
    p.morale = rng.range(0.5, 0.7);
    p.unrest = rng.range(0.05, 0.2);
    const EntityId id = p.id;
    x.w.settlements().emplace(id, std::move(p));
    return x.w.settlements().at(id);
}

void genesis(Ctx& x) {
    RealmState& s = x.s;
    s.inited = true;
    Rng rng = agentRng(x.w.seed(), 0, 0, "realm:genesis");
    std::set<std::string> houses, places;
    for (const auto& [id, f] : x.w.families()) houses.insert(f.name);
    // the settlements of the map are settlements of the realm as they are
    for (auto& [sid, st] : x.w.settlements()) {
        st.km = {0, 0};
        st.land = 1.0;
        st.founded = x.day;
        syncFull(x, st);
        classify(x.r, st, x.day);
        places.insert(st.name);
    }
    // the region around it
    std::vector<std::pair<std::string, int>> layout = x.r.layout;
    const double tau = 6.283185307179586;
    double ang0 = rng.range(0, tau);
    int slot = 0, total = 0;
    for (const auto& [k, n] : layout) total += n;
    std::vector<EntityId> made;
    for (const auto& [tpl, n] : layout)
        for (int i = 0; i < n; ++i, ++slot) {
            const RealmTemplate& t = x.r.templates.count(tpl) ? x.r.templates.at(tpl) : RealmTemplate{};
            const std::string poolKey = t.names.empty() ? "*" : t.names;
            const auto& pool = x.r.names.count(poolKey) ? x.r.names.at(poolKey) : x.r.names.at("*");
            std::string nm;
            for (int k = 0; k < 10 && !pool.empty(); ++k) {
                nm = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
                if (!places.count(nm)) break;
            }
            if (nm.empty() || places.count(nm)) nm = tpl + std::to_string(slot);
            places.insert(nm);
            const double ang = ang0 + tau * slot / std::max(1, total) + rng.range(-0.35, 0.35);
            const double d = rng.range(x.r.regionKm[0], x.r.regionKm[1]);
            Settlement& p = newSettlement(x, rng, tpl, nm, {std::cos(ang) * d, std::sin(ang) * d});
            const EntityId pid = p.id;
            made.push_back(pid);
            const RealmInstitution& in = institutionOf(x.r, p);
            const bool lawless = outlaw(p);
            if (in.dynastic && !lawless) p.house = pickHouse(x, rng, houses);
            // the ruling family
            const char sex = in.celibate ? (rng.chance(0.3) ? 'f' : 'm') : (rng.chance(0.15) ? 'f' : 'm');
            Character& ruler = newFigure(x, rng, pid, "ruler", sex, rng.irange(28, 58),
                                         p.house.empty() ? pickHouse(x, rng, houses) : p.house);
            if (lawless) {
                ruler.standing.honor = clamp01(ruler.standing.honor - 0.35);
                ruler.standing.greed = clamp01(ruler.standing.greed + 0.3);
                ruler.standing.life = "outlaw";
            }
            const EntityId rid = ruler.id;
            pol(x, pid)->chief = rid;
            if (!in.celibate && !lawless && rng.chance(0.85)) {
                Character& sp = newFigure(x, rng, pid, "consort", sex == 'm' ? 'f' : 'm',
                                          std::max(18, years(*fig(x, rid)) - rng.irange(-3, 8)), pickHouse(x, rng, houses));
                const EntityId spid = sp.id;
                marry(*fig(x, rid), *fig(x, spid));
                const EntityId faId = fig(x, rid)->body.sex == 'm' ? rid : spid;
                const EntityId moId = fig(x, rid)->body.sex == 'm' ? spid : rid;
                const int maxAge = std::max(1, years(*fig(x, moId)) - 18);
                const int kids = rng.irange(t.children[0], t.children[1]);
                for (int k = 0; k < kids; ++k) {
                    Character& ch = childOf(x, rng, *fig(x, faId), *fig(x, moId), rng.irange(0, std::min(maxAge, 26)));
                    ch.standing.house = pol(x, pid)->house;
                    ch.name = ch.name.substr(0, ch.name.rfind(' ')) + " " + ch.standing.house;
                }
                // the heir: the eldest child
                const Character* r0 = fig(x, rid);
                EntityId heir = kNone;
                int best = -1;
                for (EntityId c : r0->status.children)
                    if (const Character* k = fig(x, c); k && years(*k) > best) best = years(*k), heir = c;
                pol(x, pid)->heir = heir;
                if (Character* h = fig(x, heir)) h->standing.role = "heir";
            }
            const int kn = rng.irange(t.knights[0], t.knights[1]);
            for (int k = 0; k < kn; ++k) {
                Character& f = newFigure(x, rng, pid, lawless ? "outlaw" : "knight", 'm', rng.irange(20, 45), pickHouse(x, rng, houses));
                f.standing.martial = clamp01(f.standing.martial + 0.25);
                if (lawless) f.standing.life = "outlaw", f.standing.honor = clamp01(f.standing.honor - 0.3);
            }
        }
    // views: neighbours start mildly wary or friendly; the lawless are everyone's enemy, a cult everyone's friend
    for (Settlement& a : polities(x))
        for (Settlement& b : polities(x)) {
            if (a.id == b.id) continue;
            RealmView& v = s.view(a.id, b.id);
            v.opinion = rng.range(-0.2, 0.3);
            if (outlaw(a) || outlaw(b)) v.opinion = -0.6;
            if (a.tagged("cult") || b.tagged("cult")) v.opinion = std::max(v.opinion, 0.2);
        }
    // an old border grudge or two (the region has a past)
    auto all = polities(x);
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j) {
            const Settlement& a = all[i];
            const Settlement& b = all[j];
            if (outlaw(a) || outlaw(b) || peaceful(a) || peaceful(b)) continue;
            if (dkm(a.km, b.km) < num(x.r.pol, "neighbour_km", 25) && rng.chance(0.4)) {
                const bool which = rng.chance(0.5);
                const EntityId from = which ? a.id : b.id, at = which ? b.id : a.id;
                s.view(from, at).grievances.push_back({"border", rng.range(0.2, 0.5), x.day - rng.irange(30, 600)});
            }
        }
    // wanderers already on the road
    const auto ws = pair2(x.r.fig, "wanderers_start", {4, 6});
    const int nw = rng.irange(static_cast<int>(ws[0]), static_cast<int>(ws[1]));
    std::vector<EntityId> homes;
    for (EntityId id : made)
        if (!outlaw(*pol(x, id))) homes.push_back(id);
    for (int i = 0; i < nw && !homes.empty(); ++i) {
        const EntityId home = homes[static_cast<size_t>(rng.irange(0, static_cast<int>(homes.size()) - 1))];
        Character& f = newFigure(x, rng, home, "wanderer", rng.chance(0.82) ? 'm' : 'f', rng.irange(19, 40),
                                 pickHouse(x, rng, houses));
        const auto m = pair2(x.r.wan, "start_martial", {0.35, 0.8});
        f.standing.martial = rng.range(m[0], m[1]);
        f.standing.wanderlust = clamp01(f.standing.wanderlust + 0.35);
        f.standing.life = "road";
        const auto wl = pair2(x.r.wan, "start_wealth", {3, 20});
        f.standing.purse = rng.range(wl[0], wl[1]);
    }
    // and the people of every settlement, the ruling families among them
    for (EntityId id : made) populate(x, *pol(x, id));
    s.once["peopled"] = 1;
    log(x, "region", {{"n", static_cast<int>(made.size())}});
}

// --- crossing onto the map and off it ----------------------------------------------

Vec2 arrivalPoint(Ctx& x, const Settlement& local, Vec2 fromKm, Rng& rng) {
    Vec2 c = local.full() ? local.center : Vec2{x.w.map().sizeM * 0.5, x.w.map().sizeM * 0.5};
    const Vec2 dk{fromKm.x - local.km.x, fromKm.y - local.km.y};
    double len = std::hypot(dk.x, dk.y);
    Vec2 dir = len > 1e-6 ? Vec2{dk.x / len, dk.y / len} : Vec2{1.0, 0.0};
    const double d = rng.range(150.0, 260.0);
    Vec2 p{c.x + dir.x * d, c.y + dir.y * d};
    const double sz = x.w.map().sizeM;
    if (sz > 0) p.x = std::clamp(p.x, 5.0, sz - 5.0), p.y = std::clamp(p.y, 5.0, sz - 5.0);
    return p;
}

// The person walks onto the map: the same object moves from offstage, takes a place
// at the edge on the side it came from, its purse becomes coin.
Character* materialize(Ctx& x, Character& f, Settlement& at, int stayDays, EntityId cameFrom) {
    if (at.ruined || !at.full() || !f.body.alive || onMap(x, f)) return nullptr;
    Rng rng = rngOf(x, f.id, "realm:arrive");
    const Settlement* from = pol(x, cameFrom);
    Character* c = x.w.toStage(f.id);
    if (!c) return nullptr;
    // a child on the way comes with her: the village's own birth (village_life) takes it
    for (const auto& m : c->moodlets)
        if (m.id == "pregnant" && m.until > x.w.now()) {
            Event e;
            e.tick = m.until - 1;
            e.type = "village_birth";
            e.a = c->id;
            e.b = c->status.spouse;
            x.w.schedule(e);
        }
    c->body.downUntil = 0;
    c->body.health = std::clamp(c->body.health, 0.3, 1.0);
    c->ai.queue.clear();
    c->ai.goal = GoalInstance{};
    c->ai.plan = Plan{};
    c->ai.nextThink = x.w.now();
    if (c->need("hunger") < 0.5) c->needs["hunger"] = 0.6;
    if (c->need("thirst") < 0.5) c->needs["thirst"] = 0.6;
    earn(x, *c, 0.0); // the purse into coin
    c->pos = arrivalPoint(x, at, from ? from->km : Vec2{1, 0}, rng);
    for (const auto& [oid, o] : x.w.objects())
        if (!o.ownerFamily && dist(o.pos, c->pos) <= 1500.0) c->knownObjects.insert(oid);
    c->standing.at = at.id;
    c->standing.leaveDay = x.day + std::max(1, stayDays);
    // the village sees a stranger
    for (auto& [id, o] : x.w.characters()) {
        if (!person(&o) || id == c->id || o.status.settlement != at.id) continue;
        Relation& to = o.relations[c->id];
        to.familiarity = std::max(to.familiarity, 0.03);
    }
    return c;
}

// Whether a person on the map can leave now (not bound to the village).
bool canLeave(const Ctx& x, const Character& c) {
    if (!c.body.alive) return true;
    if (c.captivity.held() || c.body.downUntil > x.w.now() || c.ai.social.active()) return false;
    if (c.status.family != kNone) return false;
    return true;
}

// The person leaves the map: the same object moves offstage, its coin into the purse.
void park(Ctx& x, Character& f) {
    if (!onMap(x, f) || !f.body.alive) return; // the dead stay where they fell
    const int coin = f.has("coin");
    if (coin > 0) f.inventory.erase("coin"), f.standing.purse += coin;
    f.ai.queue.clear();
    f.ai.goal = GoalInstance{};
    f.ai.plan = Plan{};
    f.ai.trace.clear();
    f.ai.social = SocialLink{};
    f.ai.followAfter = SocialLink{};
    const EntityId id = f.id;
    x.w.toOffstage(id);
    for (auto& [oid, o] : x.w.characters()) {
        auto& q = o.ai.queue;
        q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedTask& t) { return t.target == id; }), q.end());
    }
}

// A person who settled on the map (married in, taken into a household) lives by the village's rules now.
void settleInVillage(Ctx& x, Character& f) {
    f.standing.life = "settled";
    f.standing.role.clear();
    if (Settlement* l = pol(x, f.status.settlement); l && l->full()) f.standing.at = l->id;
    log(x, "wanderer_settles", {{"who", f.id}});
}

// --- local needs: quests the village posts ------------------------------------------

EntityId giverOf(Ctx& x, const Settlement& p) { return p.chief; }

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
double localPurse(Ctx& x, const Settlement& p) {
    const Character* chief = x.w.character(p.chief);
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
double payFromVillage(Ctx& x, const Settlement& p, double amount, Character* to) {
    Character* chief = x.w.character(p.chief);
    if (!chief || amount <= 0) return 0.0;
    double left = amount;
    auto take = [&](std::map<std::string, int>& stock, const std::string& item, double rate) {
        auto it = stock.find(item);
        if (it == stock.end() || it->second <= 0 || left <= 0) return;
        const int n = std::min(it->second, static_cast<int>(std::ceil(left / rate)));
        it->second -= n;
        interactionTally()["realm:pay_" + item] += n;
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
    if (to) earn(x, *to, paid);
    return paid;
}

void localNeeds(Ctx& x, Settlement& p) {
    if (p.ruined || !x.r.questOn("beast_bounty")) return;
    const Character* chief = x.w.character(p.chief);
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
        postQuest(x, "beast_bounty", p.id, p.chief, pack, reward);
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

double raidLocal(Ctx& x, Settlement& by, Settlement& on, double share) {
    // AL-114: a raider with a built-in spell (a dragon's breath) uses it on the village: the fields burn by the world's rules
    if (const Character* chief = fig(x, by.chief); chief && chief->body.alive && !chief->race.empty())
        if (const RaceDef* rd = x.w.content().raceOf(chief->race); rd && !rd->innateSpells.empty()) {
            const Vec2 at = on.center;
            if (innateStrike(x.w, nullptr, rd->innateSpells, kNone, &at, "raid:" + std::to_string(x.day)))
                log(x, "dragon_breath", {{"pol", by.id}, {"fig", chief->id}, {"pol2", on.id}});
        }
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
            interactionTally()["realm:raid_" + item] += t;
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
    Settlement* by = pol(x, byId);
    Settlement* on = pol(x, onId);
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
        log(x, on->full() ? "raid_village_repelled" : "raid_repelled", {{"pol", by->id}, {"pol2", on->id}});
        if (on->full()) {
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
    double taken = 0.0, worth = 0.0; // worth: in coin, as fields and work are counted
    const double price = num(r.eco, "grain_price", 0.05);
    if (on->full()) {
        taken = raidLocal(x, *by, *on, share);
        worth = taken * price;
    } else {
        const double food = std::max(0.0, on->food) * share;
        const double coin = std::max(0.0, on->wealth) * share;
        on->food -= food, on->wealth -= coin;
        by->food += food, by->wealth += coin;
        losePeople(x, *on, static_cast<int>(std::round(rng.range(0, 3))), "raid");
        on->militia = std::max(0.0, on->militia * (1.0 - rng.range(0.03, 0.1)));
        taken = food + coin;
        worth = food * price + coin;
        log(x, "raid", {{"pol", by->id}, {"pol2", on->id}, {"n", static_cast<int>(std::round(taken))}});
    }
    // what it lives by is what it is (classify); hunting down outlaws is not living by raids
    if (!outlaw(*on)) by->income["raids"] += worth;
    by->morale = clamp01(by->morale + 0.04);
    on->morale = clamp01(on->morale - 0.04);
    // the raided want the raiders gone
    if (outlaw(*by) && !hasOpenQuest(x, "bandit_bounty", on->id, by->id)) {
        const RealmQuestKind* qk = r.quests.count("bandit_bounty") ? &r.quests.at("bandit_bounty") : nullptr;
        if (qk) {
            if (on->full()) {
                const Character* chief = w.character(on->chief);
                if (person(chief) && !chief->controller.isPlayer()) {
                    const double reward = std::min(localPurse(x, *on) * 0.6, qk->reward[1]);
                    if (reward >= qk->reward[0] * 0.4) postQuest(x, "bandit_bounty", on->id, on->chief, by->id, reward);
                }
            } else if (on->wealth > qk->reward[0] * 1.5) {
                const double reward = std::min(on->wealth * 0.3, qk->reward[1]);
                on->wealth -= reward * 0.0; // paid when done
                postQuest(x, "bandit_bounty", on->id, on->chief, by->id, reward);
            }
        }
    }
    return taken;
}

namespace {

double raid(Ctx& x, Settlement& by, Settlement& on) { return realmRaid(x.w, x.s, by.id, on.id); }

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
    const double keep = std::pow(0.5, 1.0 / num(x.r.cls, "income_halflife_days", 180));
    for (Settlement& p : polities(x)) {
        // what it lives by fades unless renewed; once a season it is read again what it is
        for (auto& [k, v] : p.income) v *= keep;
        if ((x.day + static_cast<int>(p.id)) % inum(x.r.cls, "every_days", 90) == 0) classify(x.r, p, x.day);
        if (p.ruined || p.full()) continue; // on the map the people eat and work themselves
        Rng rng = rngOf(x, p.id, "realm:eco");
        if (harvestDay && x.day > 0) {
            const double local = 1.0 + (rng.uniform() - 0.5) * 2.0 * num(x.r.eco, "harvest_local_sd", 0.12);
            // the fields feed so many heads at full yield; more hands add little (realm.json people)
            const double hands = p.fields > 0.0 ? std::min(p.pop, p.fields) + num(x.r.ppl, "extra_hands", 0.2) * std::max(0.0, p.pop - p.fields) : p.pop;
            const double crop = hands * yieldDays * p.land * x.s.harvest * local;
            p.food += crop;
            p.income["fields"] += crop * num(x.r.eco, "grain_price", 0.05);
        }
        p.food = std::min(p.food, p.pop * cap);
        p.food -= p.pop;
        const double made = p.pop * incomePerHead(x.r, p) / kDaysPerYear;
        p.wealth += made;
        p.income["crafts"] += made;
        const double target = std::round(p.pop * militiaShare(x.r, p));
        if (p.militia < target) p.militia = std::min(target, p.militia + std::max(0.05, target * num(x.r.eco, "militia_recover_per_day", 0.01)));
        p.morale += (0.6 - p.morale) * 0.01;
        p.unrest = clamp01(p.unrest - num(x.r.rev, "decay_per_day", 0.0025));
        if (p.food < 0.0) {
            // famine: the weak die, the rest grumble and some leave
            p.food = 0.0;
            // the weak die of it in their own day (figuresDaily); a band deserts
            if (p.tagged("raiders")) p.militia = std::max(0.0, p.militia * (1.0 - num(x.r.ban, "starve_desert", 0.02)));
            p.unrest = clamp01(p.unrest + num(x.r.eco, "famine_unrest_per_day", 0.012));
            if (firstTime(x, "famine_" + std::to_string(p.id) + "_" + std::to_string(x.day / 60))) log(x, "famine", {{"pol", p.id}});
            // refugees: to the village of the map when it is near
            Settlement* local = localOf(x);
            if (local && !local->ruined && local->pop < num(x.r.eco, "refugee_village_cap", 40) &&
                dkm(p.km, local->km) <= num(x.r.eco, "refugee_reach_km", 45) &&
                x.day - p.lastRefugees >= inum(x.r.eco, "refugee_gap_days", 18) &&
                rng.chance(num(x.r.eco, "refugee_chance_per_day", 0.06)) && !p.tagged("raiders")) {
                p.lastRefugees = x.day;
                // they are the settlement's own people: a household, or one alone
                std::vector<Character*> pool;
                for (Character* c : residents(x, p.id))
                    if (c->standing.role.empty() && years(*c) >= 16 && years(*c) < 55) pool.push_back(c);
                if (!pool.empty()) {
                    Character* c = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
                    std::vector<EntityId> who = rng.chance(0.5) ? householdOf(x, *c) : std::vector<EntityId>{c->id};
                    // a family is headed by its man: him first, his wife next
                    std::stable_sort(who.begin(), who.end(), [&](EntityId a, EntityId b) {
                        auto rank = [&](EntityId i) {
                            const Character* m = fig(x, i);
                            return !m ? 3 : years(*m) < 16 ? 2 : m->body.sex == 'm' ? 0 : 1;
                        };
                        return rank(a) < rank(b);
                    });
                    if (who.size() > 1 && fig(x, who[0])->status.spouse != who[1]) who.resize(1);
                    const int n = static_cast<int>(who.size());
                    if (refugeesArrive(x.w, local->id, who)) log(x, "refugees", {{"pol", p.id}, {"n", n}});
                }
            }
        }
        if (p.pop < 1.0 && !p.ruined) {
            p.ruined = true;
            p.ruinedOn = x.day;
            log(x, outlaw(p) ? "camp_disbanded" : "polity_ruined", {{"pol", p.id}});
        }
    }
}

// Grain trade: once a month the hungry buy from the full, if they have the coin and are not at war.
void grainTrade(Ctx& x) {
    const double buyBelow = num(x.r.eco, "buy_below_days", 60), sellAbove = num(x.r.eco, "sell_above_days", 150);
    const double price = num(x.r.eco, "grain_price", 0.05), share = num(x.r.eco, "trade_share", 0.25);
    for (Settlement& b : polities(x)) {
        if (b.ruined || b.full() || outlaw(b)) continue;
        if ((x.day + static_cast<int>(b.id)) % 30 != 0 || b.food >= b.pop * buyBelow) continue;
        Settlement* best = nullptr;
        for (Settlement& s : polities(x)) {
            if (s.ruined || s.id == b.id || s.full() || outlaw(s)) continue;
            if (s.food <= s.pop * sellAbove || warBetween(x, b.id, s.id) || x.s.view(s.id, b.id).grudge() > 0.6) continue;
            if (!best || s.food / heads(s) > best->food / heads(*best)) best = &s;
        }
        if (!best) continue;
        const double want = b.pop * buyBelow - b.food;
        const double spare = (best->food - best->pop * sellAbove) * share;
        const double qty = std::min({want, spare, std::max(0.0, b.wealth) / price});
        if (qty < b.pop * 5) continue;
        b.food += qty, best->food -= qty;
        b.wealth -= qty * price, best->wealth += qty * price;
        best->income["trade"] += qty * price;
        x.s.view(b.id, best->id).opinion = std::min(1.0, x.s.view(b.id, best->id).opinion + 0.03);
        ++x.s.stats["grain_trade"];
    }
}

// --- figures: life and death ----------------------------------------------------------------

void succession(Ctx& x, Settlement& p, const std::string& why);

void figureDies(Ctx& x, Character& who, const std::string& cause) {
    if (!who.body.alive) return;
    const EntityId fid = who.id;
    Character& f = *fig(x, fid);
    f.body.alive = false;
    f.body.causeOfDeath = cause;
    f.standing.deathDay = x.day;
    if (onMap(x, f)) x.w.chronicle("death", {f.id}, "chronicle.death", {{"who", f.id}, {"cause", cause}});
    if (Character* sp = fig(x, f.status.spouse)) sp->status.spouse = kNone;
    log(x, "fig_died", {{"fig", fid}, {"cause", cause}, {"pol", f.status.settlement}});
    for (Settlement& p : polities(x))
        if (p.chief == fid && !p.full()) succession(x, p, cause);
        else if (p.heir == fid) p.heir = kNone;
}

// The stat executor of a person's life (AL-92): the rules of village_life.json, as
// chances for a day instead of acts. Offstage people age and heal; go hungry when
// their settlement has no bread and may die of it; fall ill (the same kinds, the same
// course) and die of old age by the village's own curve; the unmarried marry, a wife
// conceives, carries the child and gives birth. On the map the world's day and
// village_life do all this themselves; a settlement's numbers are its people's count.
bool hasMood(const Character& c, const std::string& id, Tick now) {
    for (const auto& m : c.moodlets)
        if (m.id == id && m.until > now) return true;
    return false;
}
void setMood(Character& c, const std::string& id, Tick until) {
    for (auto& m : c.moodlets)
        if (m.id == id) {
            m.until = until;
            return;
        }
    c.moodlets.push_back({id, 0.0, until});
}
bool illNow(const Character& c, const VillageLifeRules& vl, Tick now) {
    for (const auto& k : vl.illnesses)
        if (hasMood(c, k.moodlet, now)) return true;
    return false;
}

void birthOffstage(Ctx& x, Character& mother, const VillageLifeRules& vl) {
    Rng rng = rngOf(x, mother.id, "realm:birth");
    const EntityId mid = mother.id;
    Character* fa = fig(x, mother.status.spouse);
    const EntityId faId = fa ? fa->id : kNone;
    const bool ruling = [&] {
        for (Settlement& p : polities(x))
            if (p.chief == mid || (faId != kNone && p.chief == faId)) return true;
        return false;
    }();
    if (rng.chance(vl.stillbirth)) {
        ++x.s.stats["stillbirth"];
    } else {
        const int n = rng.chance(vl.twins) ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            Character* mo = fig(x, mid);
            const std::string house = fa && !fa->standing.house.empty() ? fa->standing.house : mo->standing.house;
            Character& k = newResident(x, rng, mo->status.settlement, rng.chance(0.5) ? 'm' : 'f', 0, house);
            k.body.ageDays = 0;
            if (x.w.content().peoples.loaded) { // AL-114: the parents' kind, not the settlement's draw
                std::string kind = childRace(x.w.content(), mo->race, fa ? fa->race : mo->race);
                setRace(x.w.content(), k, kind.empty() ? mo->race : kind);
                raceAtBirth(x.w, k);
            }
            k.status.mother = mid;
            k.status.father = faId;
            const EntityId kid = k.id;
            fig(x, mid)->status.children.push_back(kid);
            if (Character* f2 = fig(x, faId)) f2->status.children.push_back(kid);
            if (ruling) {
                promote(x, rng, *fig(x, kid), "kin");
                log(x, "birth", {{"fig", kid}, {"fig2", mid}, {"pol", fig(x, mid)->status.settlement}});
                for (Settlement& p : polities(x))
                    if ((p.chief == mid || p.chief == faId) && p.heir == kNone) {
                        p.heir = kid;
                        fig(x, kid)->standing.role = "heir";
                    }
            }
            ++x.s.stats["born"];
        }
    }
    if (rng.chance(vl.motherDeath)) personDies(x, *fig(x, mid), "childbirth");
}

void figuresDaily(Ctx& x) {
    const VillageLifeRules& vl = villageLifeRules(x.w.content());
    const Json& d = x.r.ppl;
    const Tick now = x.w.now();
    const std::string season = x.w.season();
    auto mult = [&](const std::string& k) {
        auto it = vl.illMult.find(k);
        return it == vl.illMult.end() ? 1.0 : it->second;
    };
    std::vector<EntityId> ids;
    for (const auto& [id, c] : x.w.offstage())
        if (c.body.alive && !c.isAnimal()) ids.push_back(id);
    for (EntityId id : ids) {
        Character* f = fig(x, id);
        if (!f || !f->body.alive || onMap(x, *f)) continue;
        ++f->body.ageDays;
        Rng rng = rngOf(x, id, "realm:life");
        const Settlement* home = pol(x, f->status.settlement);
        const bool resident = home && residentOf(*f, home->id) && !home->full();
        // hunger by expectation: the household eats while the settlement has bread
        const bool hungry = resident && home->food <= 0.0;
        double& h = f->needs["hunger"];
        h = hungry ? std::max(0.0, h - num(d, "hunger_drop_per_day", 0.12)) : 1.0;
        const std::string stage = f->body.stage();
        if (hungry && h < 0.25) {
            const double frail = stage == "adult" ? num(d, "famine_adult", 0.6) : num(d, "famine_frail", 1.8);
            if (rng.chance(num(x.r.eco, "famine_death_per_day", 0.004) * frail)) {
                personDies(x, *f, "hunger");
                continue;
            }
        }
        // illness: the village's kinds and course, resolved at once
        if (!illNow(*f, vl, now)) {
            if (f->body.health < 1.0) f->body.health = std::min(1.0, f->body.health + num(x.r.wan, "heal_per_day", 0.03));
            double p = vl.illDaily * mult(season);
            if (stage == "child") p *= mult("child");
            if (stage == "elder") p *= mult("elder");
            if (h < 0.25) p *= mult("hungry");
            if (!vl.illnesses.empty() && rng.chance(p)) {
                double total = 0.0;
                for (const auto& k : vl.illnesses) total += std::max(0.0, k.weight);
                double r = rng.uniform() * total;
                const IllnessKind* kind = &vl.illnesses.back();
                for (const auto& k : vl.illnesses)
                    if ((r -= std::max(0.0, k.weight)) <= 0.0) {
                        kind = &k;
                        break;
                    }
                const int days = rng.irange(kind->days[0], kind->days[1]);
                setMood(*f, kind->moodlet, now + static_cast<Tick>(days + vl.immuneDays) * kTicksPerDay);
                f->body.health -= kind->healthPerDay * (stage == "adult" ? 1.0 : 1.3) * days;
                if (f->body.health <= 0.0) {
                    personDies(x, *f, "illness");
                    continue;
                }
            }
        }
        // old age: the village's curve
        if (years(*f) >= vl.oldFromYears &&
            rng.chance(vl.oldDailyAtFrom * std::pow(2.0, (years(*f) - vl.oldFromYears) / vl.oldDoubleYears))) {
            personDies(x, *f, "old_age");
            continue;
        }
        if (!resident || f->body.sex != 'f') continue;
        // the child is due
        if (hasMood(*f, "pregnant", 0)) {
            if (!hasMood(*f, "pregnant", now + 1)) {
                for (auto it = f->moodlets.begin(); it != f->moodlets.end(); ++it)
                    if (it->id == "pregnant") {
                        f->moodlets.erase(it);
                        break;
                    }
                birthOffstage(x, *f, vl);
            }
            continue;
        }
        // a wife conceives (the act's daily chance in expectation, realm.json people)
        Character* hu = fig(x, f->status.spouse);
        if (!hu || !hu->body.alive || onMap(x, *hu) || hu->status.settlement != f->status.settlement) continue;
        const int fy = years(*f), my = years(*hu);
        if (fy < vl.fertileF[0] || fy > vl.fertileF[1] || my < vl.fertileM[0] || my > vl.fertileM[1]) continue;
        if (institutionOf(x.r, *home).celibate && home->chief == f->id) continue;
        bool spaced = true;
        for (EntityId k : f->status.children)
            if (const Character* ch = fig(x, k); ch && ch->body.alive && ch->body.ageDays < vl.birthSpacingDays) spaced = false;
        if (!spaced) continue;
        if (rng.chance(num(d, "intimacy_daily", 0.006) * conceiveChanceOf(*f, vl)))
            setMood(*f, "pregnant", now + static_cast<Tick>(vl.pregnancyDays) * kTicksPerDay + 1);
    }
    // the unmarried marry within their settlement (the courtship's outcome as a daily chance)
    for (Settlement& p : polities(x)) {
        if (p.full() || p.ruined || institutionOf(x.r, p).celibate || lawlessHome(x, p.id)) continue;
        std::vector<Character*> men, women;
        for (Character* c : residents(x, p.id))
            if (c->standing.role.empty() && c->status.spouse == kNone && years(*c) >= vl.minAgeYears && years(*c) < 60)
                (c->body.sex == 'm' ? men : women).push_back(c);
        if (men.empty() || women.empty()) continue;
        Rng rng = rngOf(x, p.id, "realm:marry");
        for (Character* m : men) {
            if (!rng.chance(num(d, "marry_daily", 0.004))) continue;
            Character* best = nullptr;
            double bestU = -1e9;
            for (Character* w : women) {
                if (w->status.spouse != kNone || std::abs(years(*w) - years(*m)) > vl.maxAgeGapYears) continue;
                if ((w->status.father != kNone && w->status.father == m->status.father) ||
                    (w->status.mother != kNone && w->status.mother == m->status.mother))
                    continue; // kin
                const double u = -std::abs(years(*w) - years(*m) + 3) + rng.range(0.0, 6.0);
                if (u > bestU) bestU = u, best = w;
            }
            if (!best) continue;
            marry(*m, *best);
            if (vl.takeFamilyName && !m->standing.house.empty()) {
                best->standing.house = m->standing.house;
                best->name = best->name.substr(0, best->name.rfind(' ')) + " " + m->standing.house;
            }
            ++x.s.stats["wedding"];
        }
    }
}

int aliveFigures(Ctx& x) {
    int n = 0;
    for (Character& f : people(x)) n += f.body.alive ? 1 : 0;
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
    for (Settlement& p : polities(x))
        if (!p.ruined && !p.full() && !p.tagged("cult")) homes.push_back(p.id), wts.push_back(p.pop), sum += p.pop;
    if (homes.empty()) return;
    double pick = rng.range(0, sum);
    size_t k = 0;
    for (; k + 1 < homes.size() && pick > wts[k]; ++k) pick -= wts[k];
    std::set<std::string> used;
    const char sex = rng.chance(0.75) ? 'm' : 'f';
    Character* one = fromCrowd(x, rng, homes[k], 17, 30, sex);
    Character& f = one ? promote(x, rng, *one, "commoner") : newFigure(x, rng, homes[k], "commoner", sex, rng.irange(17, 30), pickHouse(x, rng, used));
    const Settlement* p = pol(x, homes[k]);
    if (p && (p->unrest > 0.5 || p->food <= 0)) f.standing.greed = clamp01(f.standing.greed + 0.2), f.standing.honor = clamp01(f.standing.honor - 0.15);
    f.standing.wanderlust = clamp01(f.standing.wanderlust + 0.15);
    log(x, "notable", {{"fig", f.id}, {"pol", f.status.settlement}});
}

// Life strategy once a season: settle, the road, or outside the law.
void lifestyle(Ctx& x) {
    const int every = inum(x.r.fig, "lifestyle_every_days", 90);
    const double hyst = num(x.r.fig, "lifestyle_hysteresis", 0.15);
    for (Character& f : people(x)) {
        if (!f.body.alive || onMap(x, f) || f.standing.travelTo != kNone || f.standing.captor != kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % every != 0) continue;
        if (f.standing.role == "ruler" || f.standing.role == "consort" || f.standing.role == "heir") continue;
        if (years(f) < inum(x.r.suc, "adult_years", 16)) continue;
        const Settlement* home = pol(x, f.status.settlement);
        const bool rooted = home && !home->ruined && f.standing.claim == kNone;
        const double poor = wealthOf(x, f) < 5 ? 0.3 : 0.0;
        const double kin = (f.status.spouse != kNone ? 0.35 : 0.0) + (f.status.children.empty() ? 0.0 : 0.25);
        double road = f.standing.wanderlust + 0.35 * f.standing.martial + 0.25 * f.standing.ambition * (1.0 - f.standing.renown) - kin - (rooted ? 0.15 : 0.0);
        double lawless = f.standing.greed * (1.0 - f.standing.honor) + poor + (rooted ? 0.0 : 0.25) - 0.45 * f.standing.honor - kin * 0.5;
        double settled = 0.45 + kin + 0.15 * f.standing.honor + (home && home->unrest > 0.6 ? -0.2 : 0.0);
        if (f.standing.role == "knight") settled += 0.25;
        if (f.standing.life == "road") road += hyst;
        if (f.standing.life == "outlaw") lawless += hyst;
        if (f.standing.life == "settled") settled += hyst;
        std::string pick = settled >= road && settled >= lawless ? "settled" : (road >= lawless ? "road" : "outlaw");
        if (pick == f.standing.life) continue;
        f.standing.life = pick;
        if (pick == "road") {
            if (f.standing.role != "knight") f.standing.role = "wanderer";
            log(x, "took_road", {{"fig", f.id}, {"pol", f.status.settlement}});
            deed(f, x.day, "took_road");
        } else if (pick == "outlaw") {
            f.standing.role = "outlaw";
            // to the camp, if there is one
            Settlement* camp = nullptr;
            for (Settlement& p : polities(x))
                if (!p.ruined && outlaw(p)) camp = &p;
            if (camp) {
                f.status.settlement = camp->id;
                f.standing.travelTo = camp->id;
                f.standing.arrive = x.day + 2;
                camp->militia += 1;
            }
            log(x, "went_outlaw", {{"fig", f.id}, {"pol", camp ? camp->id : f.status.settlement}});
            deed(f, x.day, "went_outlaw");
        } else {
            if (f.standing.role == "wanderer" || f.standing.role == "outlaw") f.standing.role = "commoner";
            if (f.standing.at != kNone && f.standing.at != f.status.settlement) {
                // settles where he is, unless that is a camp
                const Settlement* at = pol(x, f.standing.at);
                if (at && !outlaw(*at) && !at->full()) f.status.settlement = f.standing.at;
            }
            if (f.status.settlement == kNone) // a wanderer with no home: the next settlement that takes people in
                for (Settlement& p : polities(x))
                    if (!p.ruined && !outlaw(p) && !p.full() && (f.status.settlement == kNone || p.pop > pol(x, f.status.settlement)->pop))
                        f.status.settlement = p.id;
            if (f.status.settlement != kNone) log(x, "settled_down", {{"fig", f.id}, {"pol", f.status.settlement}});
        }
    }
}

// A new camp: an outlaw with no camp to go to, in a hungry, restless land.
void banditsFound(Ctx& x) {
    if (!x.r.arcOn("bandits")) return;
    for (Settlement& p : polities(x))
        if (!p.ruined && outlaw(p)) return;
    double unrest = 0.0;
    int n = 0;
    for (Settlement& p : polities(x))
        if (!p.ruined && !p.full()) unrest += p.unrest, ++n;
    if (n == 0 || unrest / n < num(x.r.ban, "found_unrest", 0.5) * 0.6) return;
    Character* chief = nullptr;
    for (Character& f : people(x))
        if (f.body.alive && !onMap(x, f) && f.standing.life == "outlaw" && f.standing.captor == kNone && (!chief || f.standing.ambition > chief->standing.ambition)) chief = &f;
    if (!chief || chief->standing.ambition < 0.3) return;
    Rng rng = rngOf(x, chief->id, "realm:camp");
    // a new settlement like any other: a handful of people with no fields, outside the law
    const std::string tpl = x.r.ban.is_object() ? x.r.ban.value("found_template", std::string("bandit_camp")) : "bandit_camp";
    const RealmTemplate& t = x.r.templates.count(tpl) ? x.r.templates.at(tpl) : RealmTemplate{};
    const std::string poolKey = t.names.empty() ? "*" : t.names;
    const auto& pool = x.r.names.count(poolKey) ? x.r.names.at(poolKey) : x.r.names.at("*");
    const std::string name = pool[static_cast<size_t>(rng.irange(0, static_cast<int>(pool.size()) - 1))];
    const double ang = rng.range(0, 6.283185307179586), d = rng.range(x.r.regionKm[0], x.r.regionKm[1]);
    Settlement& c = newSettlement(x, rng, tpl, name, {std::cos(ang) * d, std::sin(ang) * d});
    c.tags.insert("outlaw");
    c.pop = 0, c.militia = 5, c.food = 80, c.wealth = wealthOf(x, *chief);
    setWealth(x, *chief, 0.0);
    c.chief = chief->id;
    classify(x.r, c, x.day);
    chief->status.settlement = chief->standing.at = c.id;
    chief->standing.role = "ruler";
    for (Character& f : people(x))
        if (f.body.alive && f.standing.life == "outlaw" && f.id != chief->id && !onMap(x, f)) f.status.settlement = c.id, c.militia += 1;
    // the band: young men of the most restless place run off with him
    {
        Settlement* restless = nullptr;
        for (Settlement& p : polities(x))
            if (p.id != c.id && !p.ruined && !p.full() && !lawlessHome(x, p.id) && (!restless || p.unrest > restless->unrest)) restless = &p;
        const int have = static_cast<int>(residents(x, c.id).size());
        if (restless) movePeople(x, *restless, c, std::max(0, inum(x.r.ban, "band_start", 6) - have), true, "band");
        c.pop = static_cast<double>(residents(x, c.id).size());
    }
    for (Settlement& p : polities(x)) {
        if (p.id == c.id) continue;
        x.s.view(c.id, p.id).opinion = -0.5;
        x.s.view(p.id, c.id).opinion = -0.6;
    }
    log(x, "camp_founded", {{"fig", chief->id}, {"pol", c.id}});
}

// A ruined village or town is settled again after some years, from its strongest neighbour.
void resettle(Ctx& x) {
    for (Settlement& p : polities(x)) {
        if (!p.ruined || p.full() || outlaw(p)) continue;
        if (p.ruinedOn < 0) p.ruinedOn = x.day;
        if (x.day - p.ruinedOn < inum(x.r.eco, "resettle_after_days", 720)) continue;
        Settlement* from = nullptr;
        for (Settlement& q : polities(x))
            if (!q.ruined && !q.full() && !outlaw(q) && !peaceful(q) && q.pop > 40 &&
                (!from || q.pop - dkm(q.km, p.km) > from->pop - dkm(from->km, p.km)))
                from = &q;
        if (!from) continue;
        Rng rng = rngOf(x, p.id, "realm:resettle");
        p.ruined = false;
        const double n = movePeople(x, *from, p, static_cast<int>(std::round(std::min(from->pop * 0.15, 30.0))), false, "resettle");
        p.pop = n;
        p.food = n * 120;
        p.wealth = n;
        p.militia = std::round(n * militiaShare(x.r, p));
        p.morale = 0.6, p.unrest = 0.1;
        p.liege = from->id;
        p.tributeTo = kNone, p.tribute = 0;
        p.ruinedOn = -1;
        p.income.clear();
        const EntityId pid = p.id, fromId = from->id;
        std::set<std::string> used;
        Character& head = newFigure(x, rng, pid, "ruler", 'm', rng.irange(25, 45), pickHouse(x, rng, used));
        pol(x, pid)->chief = head.id;
        pol(x, pid)->house = head.standing.house;
        classify(x.r, *pol(x, pid), x.day);
        log(x, "resettled", {{"pol", pid}, {"pol2", fromId}, {"fig", head.id}});
    }
}

void banditsDaily(Ctx& x) {
    if (!x.r.arcOn("bandits")) return;
    // outlaws come from restless land
    for (Settlement& camp : polities(x)) {
        // whoever lives by raids raids; the restless run off to whoever is outside the law
        if (camp.ruined || camp.full() || !camp.tagged("raiders")) continue;
        Rng rng = rngOf(x, camp.id, "realm:bandits");
        for (Settlement& p : polities(x))
            if (outlaw(camp) && !institutionOf(x.r, camp).solitary && !p.ruined && !p.full() && !p.tagged("raiders") && p.unrest > 0.4 &&
                rng.chance(num(x.r.ban, "outlaws_from_unrest", 0.02) * p.unrest)) {
                if (movePeople(x, p, camp, 1, true, "outlaws")) camp.militia += 1;
            }
        const Character* at = fig(x, camp.chief);
        if (!at || !at->body.alive) continue;
        const auto every = pair2(x.r.ban, "raid_every_days", {14, 30});
        const double hunger = camp.food < camp.pop * 10 ? 0.5 : 1.0;
        const int gap = static_cast<int>(std::round((every[0] + (every[1] - every[0]) * (1.0 - at->standing.greed)) * hunger));
        if (x.day - camp.lastRaid < gap) continue;
        // the softest, richest target near enough
        Settlement* best = nullptr;
        double bestU = 0.0;
        const double sa = strength(x, camp);
        for (Settlement& p : polities(x)) {
            if (p.ruined || p.id == camp.id || p.tagged("raiders") || allied(x, p.id, camp.id) || p.liege == camp.id) continue;
            if (x.day - p.lastRaided < (p.full() ? inum(x.r.war, "village_raid_gap_days", 30) : inum(x.r.war, "raid_gap_days", 15))) continue;
            if (x.s.view(p.id, camp.id).grievances.size() > 0 && x.day - x.s.view(p.id, camp.id).grievances.back().day < inum(x.r.ban, "same_target_gap_days", 40)) continue;
            const double sd = strength(x, p) * num(x.r.war, "defender_bonus", 1.25);
            const double loot = p.full() ? std::max(5.0, p.food * 0.5 + p.wealth) : p.food * 0.2 + p.wealth;
            const double u = loot / (sd + 5.0) * (sa / (sa + sd + 1.0)) / (1.0 + dkm(camp.km, p.km) / 20.0) *
                             rng.range(0.6, 1.4);
            if (u > bestU) bestU = u, best = &p;
        }
        if (best) raid(x, camp, *best);
    }
}

// --- succession, revolt, reclaim -------------------------------------------------------------

EntityId pickHeir(Ctx& x, const Settlement& p, EntityId dead) {
    const Character* r = fig(x, dead);
    auto ok = [&](EntityId id) {
        const Character* f = fig(x, id);
        return f && f->body.alive && f->standing.captor == kNone && f->status.settlement == p.id;
    };
    if (ok(p.heir)) return p.heir;
    if (r) {
        EntityId best = kNone;
        int age = -1;
        for (EntityId c : r->status.children)
            if (ok(c) && years(*fig(x, c)) > age) age = years(*fig(x, c)), best = c;
        if (best != kNone) return best;
        if (ok(r->status.spouse)) return r->status.spouse;
    }
    EntityId best = kNone;
    double renown = -1.0;
    for (Character& f : people(x))
        if (f.body.alive && f.status.settlement == p.id && f.standing.captor == kNone && f.id != dead && !onMap(x, f) &&
            (f.standing.role == "knight" || f.standing.role == "kin" || f.standing.role == "outlaw" || f.standing.role == "commoner") && f.standing.renown > renown)
            renown = f.standing.renown, best = f.id;
    return best;
}

void crown(Ctx& x, Settlement& p, EntityId who) {
    p.chief = who;
    Character* f = fig(x, who);
    if (!f) return;
    f->standing.role = "ruler";
    f->standing.life = "settled";
    f->standing.claim = kNone;
    f->status.settlement = f->standing.at = p.id;
    if (!f->standing.house.empty() && institutionOf(x.r, p).dynastic && !outlaw(p)) p.house = f->standing.house;
    // the new heir
    p.heir = kNone;
    int age = -1;
    for (EntityId c : f->status.children)
        if (const Character* k = fig(x, c); k && k->body.alive && years(*k) > age) age = years(*k), p.heir = c;
    if (Character* h = fig(x, p.heir)) h->standing.role = "heir";
}

void exile(Ctx& x, Character& f, Settlement& from, const std::string& code) {
    f.standing.role = "wanderer";
    f.standing.life = "road";
    f.standing.claim = from.id;
    f.status.settlement = kNone;
    // the nearest other polity takes him in for now
    Settlement* to = nullptr;
    double best = 1e9;
    for (Settlement& p : polities(x))
        if (!p.ruined && p.id != from.id && !outlaw(p) && !p.full() && dkm(p.km, from.km) < best)
            best = dkm(p.km, from.km), to = &p;
    if (to) f.standing.travelTo = to->id, f.standing.arrive = x.day + std::max(1, static_cast<int>(best / num(x.r.wan, "speed_km_day", 30))), f.status.settlement = to->id;
    deed(f, x.day, code, {{"pol", from.id}});
    log(x, code, {{"fig", f.id}, {"pol", from.id}});
}

void succession(Ctx& x, Settlement& p, const std::string& why) {
    if (p.ruined) return;
    const EntityId dead = p.chief;
    const Character* old = fig(x, dead);
    const bool monastery = institutionOf(x.r, p).elective; // a new head from among the people, not an heir
    Rng rng = rngOf(x, p.id, "realm:succession");
    EntityId heir = monastery ? kNone : pickHeir(x, p, dead);
    if (heir == kNone) {
        // a new head from among the people: an elder monk, the strongest of the band, a reeve
        std::set<std::string> used;
        const char sex = monastery && rng.chance(0.3) ? 'f' : 'm';
        Character* one = fromCrowd(x, rng, p.id, 30, 55, sex);
        Character& n = one ? promote(x, rng, *one, "ruler")
                           : newFigure(x, rng, p.id, "ruler", sex, rng.irange(30, 55), p.house.empty() ? pickHouse(x, rng, used) : p.house);
        if (outlaw(p)) n.standing.life = "outlaw", n.standing.honor = clamp01(n.standing.honor - 0.3);
        heir = n.id;
    }
    Character* h = fig(x, heir);
    // a claimant: an ambitious kinsman or knight when the heir is a child or weak
    const int adult = inum(x.r.suc, "adult_years", 16);
    const bool weak = years(*h) < adult || h->standing.renown < num(x.r.suc, "weak_heir_renown", 0.12);
    Character* claimant = nullptr;
    if (weak && !monastery && x.r.arcOn("succession"))
        for (Character& f : people(x))
            if (f.body.alive && !onMap(x, f) && f.status.settlement == p.id && f.id != heir && f.standing.captor == kNone &&
                years(f) >= adult && f.standing.ambition >= num(x.r.suc, "claim_ambition", 0.62) &&
                f.standing.martial >= num(x.r.suc, "claim_martial", 0.3) && (!claimant || f.standing.ambition + f.standing.renown > claimant->standing.ambition + claimant->standing.renown))
                claimant = &f;
    log(x, "ruler_died", {{"fig", dead}, {"pol", p.id}, {"cause", why}});
    crown(x, p, heir);
    h = fig(x, heir);
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
    claimant->relations[heir].affinity = -0.6;
    p.unrest = clamp01(p.unrest + 0.2);
    log(x, "succession_crisis", {{"fig", claimant->id}, {"fig2", heir}, {"pol", p.id}});
}

void strifeDaily(Ctx& x, RealmWar& wr) {
    Settlement* p = pol(x, wr.attacker);
    Character* c = fig(x, wr.claimant);
    if (!p || !c || !c->body.alive) {
        wr.over = true, wr.last = x.day, wr.outcome = "claimant_gone";
        return;
    }
    if (x.day - wr.start < static_cast<int>(wr.score)) {
        p->militia = std::max(0.0, p->militia - 0.05);
        return;
    }
    Character* h = fig(x, p->chief);
    Rng rng = rngOf(x, p->id, "realm:strife");
    const double sc = 0.2 + c->standing.renown + 0.5 * c->standing.martial + 0.3 * c->standing.ambition;
    const double sh = 0.35 + (h ? h->standing.renown + 0.3 * h->standing.martial : 0.0) + 0.2; // loyalty to the line
    wr.over = true;
    wr.last = x.day;
    p->unrest = clamp01(p->unrest - 0.1);
    if (rng.uniform() * (sc + sh) < sc) {
        wr.outcome = "usurped";
        const EntityId old = p->chief;
        crown(x, *p, c->id);
        log(x, "usurped", {{"fig", c->id}, {"fig2", old}, {"pol", p->id}});
        if (Character* o = fig(x, old); o && o->body.alive) {
            // a child heir to the cloister, a grown one into exile
            Settlement* cloister = nullptr;
            for (Settlement& q : polities(x))
                if (!q.ruined && q.tagged("cult")) cloister = &q;
            if (years(*o) < inum(x.r.suc, "adult_years", 16) && cloister) {
                o->status.settlement = o->standing.at = cloister->id;
                o->standing.role = "commoner";
                o->standing.claim = p->id;
                log(x, "heir_to_monastery", {{"fig", o->id}, {"pol", cloister->id}, {"pol2", p->id}});
            } else if (c->standing.honor < 0.3 && rng.chance(0.5)) {
                figureDies(x, *o, "murdered");
            } else {
                exile(x, *o, *p, "exiled");
            }
            if (o->body.alive) o->relations[c->id].affinity = -0.9;
        }
    } else {
        wr.outcome = "held";
        log(x, "claim_failed", {{"fig", c->id}, {"pol", p->id}});
        if (h && h->standing.honor < 0.35 && rng.chance(0.6)) figureDies(x, *c, "executed");
        else exile(x, *c, *p, "exiled");
    }
}

void revolts(Ctx& x) {
    if (!x.r.arcOn("revolt")) return;
    const double line = num(x.r.rev, "unrest_line", 0.72);
    for (Settlement& p : polities(x)) {
        if (p.ruined || p.full() || outlaw(p)) continue;
        p.unrestDays = p.unrest >= line ? p.unrestDays + 1 : 0;
        if (p.unrestDays < inum(x.r.rev, "days", 20) || x.day - p.lastRevolt < 180) continue;
        p.unrestDays = 0;
        p.lastRevolt = x.day;
        Rng rng = rngOf(x, p.id, "realm:revolt");
        Character* leader = nullptr;
        for (Character& f : people(x))
            if (f.body.alive && !onMap(x, f) && f.status.settlement == p.id && f.id != p.chief && f.standing.role != "consort" && f.standing.captor == kNone &&
                years(f) >= 18 && (!leader || f.standing.ambition > leader->standing.ambition))
                leader = &f;
        if (!leader || leader->standing.ambition < 0.4) {
            std::set<std::string> used;
            Character* one = fromCrowd(x, rng, p.id, 22, 45, 'm');
            leader = one ? &promote(x, rng, *one, "commoner") : &newFigure(x, rng, p.id, "commoner", 'm', rng.irange(22, 45), pickHouse(x, rng, used));
            leader->standing.ambition = clamp01(leader->standing.ambition + 0.3);
        }
        const EntityId lid = leader->id;
        log(x, "revolt", {{"fig", lid}, {"pol", p.id}});
        const double rebels = p.pop * p.unrest * num(x.r.rev, "rebel_per_head", 0.12) * (0.5 + fig(x, lid)->standing.martial);
        const double loyal = p.militia * (0.3 + p.morale) * (1.0 - p.unrest * 0.5);
        if (rng.uniform() * (rebels + loyal) < rebels) {
            const EntityId old = p.chief;
            crown(x, p, lid);
            p.unrest = 0.2;
            p.militia *= 0.7;
            log(x, "revolt_won", {{"fig", lid}, {"fig2", old}, {"pol", p.id}});
            if (Character* o = fig(x, old); o && o->body.alive) {
                if (rng.chance(0.4)) figureDies(x, *o, "revolt");
                else exile(x, *o, p, "exiled");
            }
        } else {
            p.unrest = clamp01(p.unrest - 0.3);
            p.morale = clamp01(p.morale - 0.1);
            losePeople(x, p, static_cast<int>(std::round(rebels * 0.3)), "revolt");
            log(x, "revolt_crushed", {{"fig", lid}, {"pol", p.id}});
            figureDies(x, *fig(x, lid), "executed");
        }
    }
}

// The exile comes back with hired swords.
void reclaims(Ctx& x) {
    if (!x.r.arcOn("reclaim")) return;
    for (Character& f : people(x)) {
        if (!f.body.alive || onMap(x, f) || f.standing.claim == kNone || f.standing.captor != kNone || f.standing.travelTo != kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % 30 != 0) continue;
        Settlement* target = pol(x, f.standing.claim);
        if (!target || target->ruined || target->chief == f.id) {
            f.standing.claim = kNone;
            continue;
        }
        if (years(f) < inum(x.r.suc, "adult_years", 16) || f.standing.ambition < 0.45) continue;
        int exiledOn = -99999;
        for (const auto& d : f.standing.deeds)
            if (d.value("code", "") == "exiled" || d.value("code", "") == "heir_to_monastery") exiledOn = d.value("day", 0);
        if (exiledOn < -9999) {
            // sent to the cloister as a child: the claim ripens when grown
            exiledOn = bornDay(x, f) + inum(x.r.suc, "adult_years", 16) * kDaysPerYear;
        }
        if (x.day - exiledOn < inum(x.r.suc, "reclaim_after_days", 360)) continue;
        // a hired band: an open war_service quest of his
        RealmQuest* q = nullptr;
        for (auto& qq : x.s.quests)
            if (qq.kind == "war_service" && qq.giver == f.id && qq.status == "open") q = &qq;
        if (!q) {
            if (wealthOf(x, f) < num(x.r.suc, "reclaim_wealth", 30) && f.standing.renown < 0.3) continue;
            // a band that melted away is gathered again only after a while
            int lastTry = -99999;
            for (const auto& d : f.standing.deeds)
                if (d.value("code", "") == "reclaim_start") lastTry = std::max(lastTry, d.value("day", 0));
            if (x.day - lastTry < inum(x.r.suc, "reclaim_after_days", 360)) continue;
            RealmQuest* nq = postQuest(x, "war_service", f.standing.claim, f.id, f.standing.claim, std::min(wealthOf(x, f) * 0.6, 45.0) + 10.0);
            if (nq) {
                nq->target = f.standing.claim;
                deed(f, x.day, "reclaim_start");
                log(x, "reclaim_start", {{"fig", f.id}, {"pol", f.standing.claim}});
            }
            continue;
        }
        if (q->takers.size() < 2 && x.day - q->posted < inum(x.r.suc, "reclaim_wait_days", 45)) continue;
        // the attack
        Rng rng = rngOf(x, f.id, "realm:reclaim");
        double band = 2.0 + 4.0 * f.standing.martial + 6.0 * f.standing.renown;
        for (EntityId t : q->takers)
            if (Character* m = fig(x, t); m && m->body.alive) band += 4.0 * m->standing.martial * m->body.health;
        const double def = strength(x, *target) * 0.6 * (1.0 - target->unrest * 0.5);
        q->status = "done";
        q->closed = x.day;
        if (rng.uniform() * (band + def) < band) {
            const EntityId old = target->chief;
            crown(x, *target, f.id);
            log(x, "reclaimed", {{"fig", f.id}, {"fig2", old}, {"pol", target->id}});
            for (EntityId t : q->takers)
                if (Character* m = fig(x, t)) m->standing.renown = clamp01(m->standing.renown + 0.08), earn(x, *m, q->reward), deed(*m, x.day, "reclaim_won", {{"pol", target->id}});
            if (Character* o = fig(x, old); o && o->body.alive) exile(x, *o, *target, "exiled");
        } else {
            log(x, "reclaim_failed", {{"fig", f.id}, {"pol", target->id}});
            for (EntityId t : q->takers)
                if (Character* m = fig(x, t); m && m->body.alive && rng.chance(0.3)) figureDies(x, *m, "battle");
            if (rng.chance(0.45)) figureDies(x, f, "battle");
            else setWealth(x, f, wealthOf(x, f) * 0.3);
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

RealmWar& startWar(Ctx& x, Settlement& a, Settlement& d, const std::string& cause) {
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
        a.full() || d.full() ? std::vector<EntityId>{} : std::vector<EntityId>{});
    grieve(x, d.id, a.id, "war", 0.3);
    // the allies are called
    const EntityId wid = w.id;
    for (Settlement& p : polities(x)) {
        if (p.ruined || p.id == a.id || p.id == d.id || outlaw(p)) continue;
        const Axes ax = rulerAxes(x, p);
        if (ax.player || (peaceful(p) && ax.honor < 0.8)) continue;
        if (p.full() && ax.honor < 0.6) continue;
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
    for (Settlement* side : {&a, &d}) {
        if (side->full() || side->wealth < 40 || !x.r.questOn("war_service")) continue;
        const RealmQuestKind& qk = x.r.quests.at("war_service");
        postQuest(x, "war_service", side->id, side->chief, side == &a ? d.id : a.id,
                  std::clamp(side->wealth * 0.15, qk.reward[0], qk.reward[1]));
    }
    return *std::find_if(x.s.wars.begin(), x.s.wars.end(), [&](const RealmWar& q) { return q.id == wid; });
}

void endWar(Ctx& x, RealmWar& wr, const std::string& outcome, EntityId winner, EntityId loser) {
    wr.over = true;
    wr.last = x.day;
    wr.outcome = outcome;
    Settlement* W = pol(x, winner);
    Settlement* L = pol(x, loser);
    Json p = {{"pol", winner}, {"pol2", loser}};
    if (W && L && outcome != "white") {
        const Axes wa = rulerAxes(x, *W);
        const double ratio = strength(x, *W) / std::max(1.0, strength(x, *L));
        if (outcome == "dictated" && wa.ambition > 0.55 && ratio > 1.8 && !outlaw(*L) && L->liege == kNone &&
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
            log(x, rep >= 1.0 ? "peace_reparation" : "peace_white", p); // nothing to take: just peace
        }
        grieve(x, loser, winner, "war", 0.35); // the loser remembers (revanchism)
        L->unrest = clamp01(L->unrest + num(x.r.war, "defeat_unrest", 0.06));
    } else {
        log(x, "peace_white", p);
    }
    // the hired swords are paid off and go
    for (auto& q : x.s.quests)
        if (q.kind == "war_service" && q.status == "open" && (q.polity == wr.attacker || q.polity == wr.defender) && q.giver != wr.claimant &&
            !isReclaim(x, q)) {
            q.status = "done";
            q.closed = x.day;
            Settlement* emp = pol(x, q.polity);
            for (EntityId t : q.takers)
                if (Character* m = fig(x, t); m && m->body.alive) {
                    const double pay = emp ? std::min(q.reward, std::max(0.0, emp->wealth)) : 0.0;
                    if (emp) emp->wealth -= pay;
                    earn(x, *m, pay);
                    m->standing.renown = clamp01(m->standing.renown + x.r.quests.at("war_service").renown * (q.polity == winner ? 1.5 : 0.7));
                    m->standing.quest = kNone;
                    deed(*m, x.day, q.polity == winner ? "war_won" : "war_lost", {{"pol", q.polity}});
                }
        }
}

// The captives: ransom or the axe.
void captives(Ctx& x) {
    for (Character& f : people(x)) {
        if (!f.body.alive || f.standing.captor == kNone) continue;
        if ((x.day + static_cast<int>(f.id)) % 7 != 0) continue;
        Settlement* captor = pol(x, f.standing.captor);
        Settlement* home = pol(x, f.status.settlement);
        const double ransom = num(x.r.war, "ransom_base", 20) + f.standing.renown * num(x.r.war, "ransom_per_renown", 60);
        if (!captor || captor->ruined) {
            f.standing.captor = kNone;
            log(x, "captive_escaped", {{"fig", f.id}});
            continue;
        }
        if (home && !home->ruined && home->wealth >= ransom) {
            home->wealth -= ransom;
            captor->wealth += ransom;
            f.standing.captor = kNone;
            f.standing.at = f.status.settlement;
            log(x, "ransomed", {{"fig", f.id}, {"pol", captor->id}, {"n", static_cast<int>(std::round(ransom))}});
            continue;
        }
        int taken = x.day;
        for (const auto& d : f.standing.deeds)
            if (d.value("code", "") == "captured") taken = d.value("day", x.day);
        if (x.day - taken > inum(x.r.war, "captive_days", 150)) {
            const Axes a = rulerAxes(x, *captor);
            Rng rng = rngOf(x, f.id, "realm:captive");
            if (a.honor < 0.35 && rng.chance(0.5)) {
                figureDies(x, f, "executed");
            } else {
                f.standing.captor = kNone;
                f.standing.at = f.status.settlement;
                log(x, "released", {{"fig", f.id}, {"pol", captor->id}});
            }
        }
    }
}

// A local battle: the village's own men march and some do not come back.
void localLevy(Ctx& x, Settlement& local, EntityId enemy, bool lost) {
    Rng rng = rngOf(x, local.id, "realm:levy");
    auto men = levy(x, local);
    if (men.empty()) return;
    const double p = num(x.r.war, "local_casualty", 0.45) * (lost ? 1.0 : 0.5);
    if (!rng.chance(p)) return;
    Character& c = *men[static_cast<size_t>(rng.irange(0, static_cast<int>(men.size()) - 1))];
    hurtVillager(x, c, rng.range(0.25, lost ? 1.1 : 0.7), "battle", enemy);
}

void fieldFigures(Ctx& x, Settlement& p, bool lost, EntityId enemy, Rng& rng) {
    const double death = num(x.r.war, "figure_death", 0.07);
    const double cap = num(x.r.war, "figure_capture", 0.12);
    std::vector<EntityId> field;
    for (Character& f : people(x))
        if (f.body.alive && !onMap(x, f) && f.standing.captor == kNone && f.standing.travelTo == kNone && f.standing.at == p.id &&
            (f.standing.role == "knight" || (f.standing.role == "ruler" && f.standing.martial > 0.4 && years(f) < 60) ||
             (f.standing.role == "heir" && years(f) >= 16 && f.standing.martial > 0.5)))
            field.push_back(f.id);
    for (EntityId m : mercsOf(x, p.id)) field.push_back(m);
    for (EntityId id : field) {
        Character* f = fig(x, id);
        if (!f || !f->body.alive) continue;
        const double d = lost ? death * (1.6 - f->standing.martial) : death * 0.3 * (1.6 - f->standing.martial);
        if (rng.chance(d)) {
            log(x, "fell_in_battle", {{"fig", id}, {"pol", p.id}, {"pol2", enemy}});
            if (Settlement* e = pol(x, enemy)) grieve(x, f->status.settlement == kNone ? p.id : f->status.settlement, e->id, "killed_kin", 0.25);
            figureDies(x, *f, "battle");
            continue;
        }
        if (lost && f->standing.role != "wanderer" && rng.chance(cap)) {
            f->standing.captor = enemy;
            deed(*f, x.day, "captured", {{"pol", enemy}});
            log(x, "captured", {{"fig", id}, {"pol", enemy}});
            if (p.chief == id && !p.full()) p.morale = clamp01(p.morale - 0.15);
            continue;
        }
        if (!lost) f->standing.renown = clamp01(f->standing.renown + 0.02);
        if (rng.chance(0.25)) f->body.health = std::max(0.2, f->body.health - rng.range(0.1, 0.4));
    }
}

double sideStrength(Ctx& x, EntityId main, const std::vector<EntityId>& allies) {
    double s = 0.0;
    if (Settlement* p = pol(x, main)) s += strength(x, *p);
    for (EntityId a : allies)
        if (Settlement* p = pol(x, a)) s += 0.5 * strength(x, *p);
    return s;
}

void warStep(Ctx& x, RealmWar& wr) {
    Settlement* A = pol(x, wr.attacker);
    Settlement* D = pol(x, wr.defender);
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
            losePeople(x, *D, static_cast<int>(std::round(D->pop * 0.1)), "war");
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
        Settlement& by = aRaids ? *A : *D;
        Settlement& on = aRaids ? *D : *A;
        const int gap = on.full() ? inum(x.r.war, "village_raid_gap_days", 30) : inum(x.r.war, "raid_gap_days", 15);
        if (x.day - on.lastRaided >= gap) {
            const double t = raid(x, by, on);
            ++wr.raids;
            wr.score += (aRaids ? 1 : -1) * (t > 0 ? 0.5 : -0.25);
        }
    } else {
        ++wr.battles;
        const double pa = sa / std::max(1e-6, sa + sd * num(x.r.war, "defender_bonus", 1.25) * (wr.siege ? 1.2 : 1.0));
        const bool aWins = rng.uniform() < pa;
        Settlement& W = aWins ? *A : *D;
        Settlement& L = aWins ? *D : *A;
        const auto ll = pair2(x.r.war, "loser_losses", {0.15, 0.3}), wl = pair2(x.r.war, "winner_losses", {0.04, 0.12});
        if (!L.full()) L.militia *= 1.0 - rng.range(ll[0], ll[1]);
        if (!W.full()) W.militia *= 1.0 - rng.range(wl[0], wl[1]);
        if (!L.full()) losePeople(x, L, static_cast<int>(std::round(rng.range(1, 4))), "battle");
        W.morale = clamp01(W.morale + 0.08);
        L.morale = clamp01(L.morale - 0.12);
        const double margin = std::abs(pa - 0.5) * 2.0;
        wr.score += (aWins ? 1 : -1) * (1.0 + margin);
        log(x, "battle", {{"pol", W.id}, {"pol2", L.id}});
        fieldFigures(x, W, false, L.id, rng);
        fieldFigures(x, L, true, W.id, rng);
        if (W.full()) localLevy(x, W, L.id, false);
        if (L.full()) localLevy(x, L, W.id, true);
        grieve(x, L.id, W.id, "battle", 0.06);
    }
    wr.last = x.day;
    const double siegeScore = num(x.r.war, "siege_score", 3.0);
    if (!wr.siege && wr.score >= siegeScore && !D->full()) {
        wr.siege = true;
        log(x, "siege_begins", {{"pol", A->id}, {"pol2", D->id}});
    }
    if (wr.score >= siegeScore + 2.5 && D->full()) {
        endWar(x, wr, "dictated", A->id, D->id);
        return;
    }
    if (wr.score <= -siegeScore) {
        endWar(x, wr, "won", D->id, A->id);
        return;
    }
    const double ex = num(x.r.war, "exhaustion", 0.35);
    const bool tiredA = !A->full() && A->militia < wr.startMilA * ex;
    const bool tiredD = !D->full() && D->militia < wr.startMilD * ex;
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

// AL-114: what a settlement's people are (dwarven, elven, mixed... by who lives there,
// once a season) and a hoarder's treasure (a dragon's hoard grows by the year).
void peoplesDaily(Ctx& x) {
    const Content& ct = x.w.content();
    if (!ct.peoples.loaded) return;
    std::set<std::string> kindTags = {"mixed"};
    for (const auto& [id, r] : ct.peoples.races)
        if (!r.settlementTag.empty()) kindTags.insert(r.settlementTag);
    for (Settlement& p : polities(x)) {
        if (p.ruined) continue;
        const int phase = (x.day + static_cast<int>(p.id)) % kDaysPerYear;
        const bool unread = std::none_of(kindTags.begin(), kindTags.end(), [&](const std::string& t) { return p.tagged(t); });
        if (phase % 90 == 0 || (unread && !p.peoples.empty())) {
            std::map<std::string, int> count;
            for (Character* c : residents(x, p.id)) ++count[raceId(ct, *c)];
            if (p.full())
                for (const auto& [id, c] : x.w.characters())
                    if (person(&c) && c.status.settlement == p.id) ++count[raceId(ct, c)];
            for (const auto& t : kindTags) p.tags.erase(t);
            for (const auto& t : peoplesTags(ct, count)) p.tags.insert(t);
        }
        if (phase != 0) continue;
        const Character* chief = fig(x, p.chief);
        if (!chief || !chief->body.alive || chief->race.empty()) continue;
        const RaceDef& rd = raceOf(ct, *chief);
        if (!rd.hoard.is_object() || rd.hoard.empty()) continue;
        const double grow = rd.hoard.value("grow_per_year", 0.0);
        if (grow > 0.0 && p.wealth > 0.0) {
            p.wealth *= 1.0 + grow;
            log(x, "hoard_grows", {{"pol", p.id}, {"fig", chief->id}, {"wealth", std::round(p.wealth)}});
        }
    }
}

void tributeDaily(Ctx& x) {
    for (Settlement& p : polities(x)) {
        if (p.ruined || p.tributeTo == kNone) continue;
        if ((x.day + static_cast<int>(p.id)) % 30 != 0) continue;
        Settlement* to = pol(x, p.tributeTo);
        if (!to || to->ruined) {
            p.tributeTo = kNone, p.tribute = 0, p.liege = p.liege == kNone ? kNone : p.liege;
            continue;
        }
        double paid = 0.0;
        if (p.full()) {
            // the village pays from its real stores (grain, coin)
            for (WorldObject* o : localStores(x, p)) {
                for (auto& [item, n] : o->stock) {
                    if (paid >= p.tribute || n <= 0) continue;
                    if (item != "coin" && item != "grain") continue;
                    const double rate = item == "coin" ? 1.0 : 0.5;
                    const int t = std::min(n, static_cast<int>(std::ceil((p.tribute - paid) / rate)));
                    n -= t;
                    interactionTally()["realm:tribute_" + item] += t;
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
            if (firstTime(x, "unpaid_" + std::to_string(p.id) + "_" + std::to_string(x.day / kDaysPerYear)))
                log(x, "tribute_unpaid", {{"pol", p.id}, {"pol2", to->id}});
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

Character* envoyOf(Ctx& x, const Settlement& from) {
    Character* best = nullptr;
    for (Character& f : people(x))
        if (f.body.alive && !onMap(x, f) && f.standing.captor == kNone && f.standing.travelTo == kNone && f.status.settlement == from.id && f.standing.at == from.id &&
            years(f) >= 18 && (f.standing.role == "knight" || f.standing.role == "kin" || f.standing.role == "heir"))
            if (!best || (f.standing.role == "knight" && best->standing.role != "knight")) best = &f;
    if (!best) best = fig(x, from.chief);
    return best && best->body.alive && !onMap(x, *best) ? best : nullptr;
}

bool accepts(Ctx& x, const RealmOffer& o, const Settlement& to, const Settlement& from) {
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
    Settlement* from = pol(x, o.from);
    Settlement* to = pol(x, o.to);
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
        Character* g = fig(x, o.groom);
        Character* b = fig(x, o.bride);
        if (yes && g && b && g->body.alive && b->body.alive && g->status.spouse == kNone && b->status.spouse == kNone) {
            marry(*g, *b);
            b->status.settlement = b->standing.at = g->status.settlement;
            x.s.view(from->id, to->id).opinion = std::min(1.0, x.s.view(from->id, to->id).opinion + 0.25);
            x.s.view(to->id, from->id).opinion = std::min(1.0, x.s.view(to->id, from->id).opinion + 0.25);
            log(x, "marriage", {{"fig", g->id}, {"fig2", b->id}, {"pol", g->status.settlement}, {"pol2", from->id == g->status.settlement ? to->id : from->id}});
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
    Settlement* to = pol(x, o.to);
    Settlement* from = pol(x, o.from);
    if (!to || !from) return;
    if (o.kind == "tribute" && to->tributeTo != kNone) return; // pays someone already
    for (const auto& p : x.s.offers)
        if (p.status == "pending" && p.from == o.from && p.to == o.to) return; // one embassy at a time
    o.id = static_cast<EntityId>(x.s.nextId++);
    o.day = x.day;
    log(x, "offer", {{"pol", o.from}, {"pol2", o.to}, {"offer", o.kind}, {"n", static_cast<int>(std::round(o.amount))}});
    if (!to->full()) {
        const bool yes = accepts(x, o, *to, *from);
        x.s.offers.push_back(o);
        applyOffer(x, x.s.offers.back(), yes);
        return;
    }
    Character* env = envoyOf(x, *from);
    if (env) {
        o.envoy = env->id;
        const int days = std::max(1, static_cast<int>(dkm(from->km, to->km) / num(x.r.wan, "speed_km_day", 30)));
        env->standing.travelTo = to->id;
        env->standing.arrive = x.day + days;
        env->standing.quest = kNone;
        o.deadline = env->standing.arrive + inum(x.r.pol, "envoy_stay_days", 3);
    } else {
        o.deadline = x.day + inum(x.r.pol, "envoy_stay_days", 3);
    }
    x.s.offers.push_back(o);
}

// The village's chief answers at the envoy's leave (the player answers with realm.answer).
void offersDaily(Ctx& x) {
    for (auto& o : x.s.offers) {
        if (o.status != "pending") continue;
        Settlement* to = pol(x, o.to);
        Settlement* from = pol(x, o.from);
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
    for (Settlement& P : polities(x)) {
        if (P.ruined || outlaw(P) || !rulerAlive(x, P)) continue; // outside the law: no councils, raids (banditsDaily)
        if ((x.day + static_cast<int>(P.id)) % every != 0) continue;
        const Axes a = rulerAxes(x, P);
        if (a.player) continue; // the player's chief acts by command
        Rng rng = rngOf(x, P.id, "realm:council");
        const double sP = strength(x, P);
        // marriage: the ruler's grown, unwed children
        if (x.r.arcOn("marriage") && rng.chance(0.35) && !P.full()) {
            const Character* rf = fig(x, P.chief);
            if (rf)
                for (EntityId cid : rf->status.children) {
                    const Character* ch = fig(x, cid);
                    if (!ch || !ch->body.alive || ch->status.spouse != kNone || years(*ch) < inum(x.r.fig, "marry_from", 17)) continue;
                    // a match from another ruling family
                    const Character* match = nullptr;
                    EntityId matchPol = kNone;
                    double best = -1e9;
                    for (Settlement& Q : polities(x)) {
                        if (Q.id == P.id || Q.ruined || Q.full() || outlaw(Q) || institutionOf(x.r, Q).celibate) continue;
                        if (warBetween(x, P.id, Q.id)) continue;
                        for (Character& f : people(x))
                            if (f.body.alive && !onMap(x, f) && f.status.settlement == Q.id && f.status.spouse == kNone && f.body.sex != ch->body.sex &&
                                years(f) >= inum(x.r.fig, "marry_from", 17) && std::abs(years(f) - years(*ch)) < 15 &&
                                (f.standing.role == "heir" || f.standing.role == "kin" || f.standing.role == "ruler")) {
                                const double u = strength(x, Q) * 0.01 + x.s.view(P.id, Q.id).opinion + rng.range(0, 0.3);
                                if (u > best) best = u, match = &f, matchPol = Q.id;
                            }
                    }
                    if (match) {
                        RealmOffer o;
                        o.kind = "marriage";
                        o.from = P.id;
                        o.to = matchPol;
                        o.groom = ch->body.sex == 'm' ? ch->id : match->id;
                        o.bride = ch->body.sex == 'm' ? match->id : ch->id;
                        makeOffer(x, o);
                        break;
                    }
                }
        }
        if (peaceful(P) && a.honor > 0.3) continue; // a cult's head does not lead men out (unless he is no cleric at heart)
        // a camp that bleeds us: the lord's men ride out against it
        bool rode = false;
        for (Settlement& Q : polities(x)) {
            if (rode || Q.ruined || !outlaw(Q) || P.full()) continue;
            const double g = x.s.view(P.id, Q.id).grudge() * a.grudge;
            if (g > 0.3 && sP > 1.5 * strength(x, Q) && x.day - Q.lastRaided >= inum(x.r.war, "raid_gap_days", 15) && rng.chance(0.5)) {
                log(x, "punitive", {{"pol", P.id}, {"pol2", Q.id}});
                raid(x, P, Q);
                {
                    Q.militia *= 0.75;
                    if (Q.militia < num(x.r.ban, "min_pop", 5) * 0.5 && rng.chance(0.5)) {
                        Q.ruined = true;
                        log(x, "camp_destroyed", {{"pol", Q.id}, {"fig", P.chief}});
                        for (Character& f : people(x))
                            if (f.body.alive && f.status.settlement == Q.id) f.status.settlement = kNone, f.standing.role = "outlaw";
                        scatterBand(x, Q);
                    }
                }
                rode = true;
            }
        }
        if (rode) continue;
        // war, tribute, alliance: the best of what this ruler wants
        Settlement* target = nullptr;
        double bestU = 0.0;
        std::string act;
        for (Settlement& Q : polities(x)) {
            if (Q.id == P.id || Q.ruined) continue;
            if (outlaw(Q)) continue; // the lawless are hunted, not treated with
            if (warBetween(x, P.id, Q.id) || allied(x, P.id, Q.id) || P.liege == Q.id || Q.liege == P.id) continue;
            if (Q.tributeTo == P.id || P.tributeTo == Q.id) continue; // the peace that pays holds
            const bool truce = inTruce(x, P.id, Q.id);
            const RealmView& v = x.s.view(P.id, Q.id);
            const double sQ = strength(x, Q) + 0.5 * [&] {
                double s = 0.0;
                for (const auto& [m, n] : x.s.alliances)
                    if (m == Q.id || n == Q.id)
                        if (const Settlement* al = pol(x, m == Q.id ? n : m)) s += strength(x, *al);
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
            if (truce) u -= 0.6;
            if (Q.tagged("cult")) u -= num(x.r.pol, "w_sacrilege", 0.9) * (0.4 + a.honor); // a holy place
            u += rng.range(-0.1, 0.1);
            // nobody starts a war he expects to lose, unless the grudge is burning
            const bool canWar = !truce && warsOf(x, P.id) < inum(x.r.pol, "max_wars", 2) && x.r.arcOn("war") &&
                                (ratio >= num(x.r.pol, "war_min_ratio", 1.1) || (grudge >= 1.0 && ratio >= 0.8));
            if (canWar && u > num(x.r.pol, "war_threshold", 0.62) && u > bestU) {
                // an honourable ruler asks first; a greedy strong one demands tribute
                if (ratio > 1.5 && (a.honor > 0.5 || a.greed > 0.5)) act = "tribute";
                else act = "war";
                bestU = u, target = &Q;
            } else if (ratio > 1.8 && a.greed > 0.5 &&
                       u > num(x.r.pol, "demand_threshold", 0.45) && u > bestU && Q.tributeTo == kNone) {
                act = "tribute", bestU = u, target = &Q;
            }
        }
        // alliance against a common enemy, or with friends
        if (!target) {
            for (Settlement& Q : polities(x)) {
                if (Q.id == P.id || Q.ruined || outlaw(Q) || allied(x, P.id, Q.id) || warBetween(x, P.id, Q.id)) continue;
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
            startWar(x, P, *target, x.s.view(P.id, target->id).grudge() > 0.3 ? "grievance" : (outlaw(*target) ? "bandits" : "conquest"));
        }
    }
}

// --- wanderers and quests -----------------------------------------------------------------

double questUtility(const Ctx& x, const Character& f, const RealmQuest& q) {
    const RealmQuestKind& qk = x.r.quests.at(q.kind);
    const Settlement* p = pol(x, q.polity);
    const Settlement* at = pol(x, f.standing.at);
    if (!p || p->ruined) return -1.0;
    if (static_cast<int>(q.takers.size()) >= qk.takers) return -1.0;
    if (std::find(q.takers.begin(), q.takers.end(), f.id) != q.takers.end()) return -1.0;
    if (q.giver == f.id || outlaw(*p)) return -1.0;
    if (f.standing.life == "outlaw" && q.kind == "bandit_bounty") return -1.0;
    // never for those who wronged him
    if (f.standing.claim != kNone && (q.polity == f.standing.claim || p->id == f.standing.claim)) return -1.0;
    if (auto it = f.relations.find(p->chief); it != f.relations.end() && it->second.affinity < -0.5) return -1.0;
    const double distKm = at ? dkm(at->km, p->km) : 20.0;
    double just = q.kind == "war_service" ? 0.05 : 0.3;
    if (q.kind == "war_service") {
        for (const auto& wr : x.s.wars)
            if (!wr.over && wr.defender == q.polity) just += 0.2;
        if (const Character* g = fig(x, q.giver); g && g->standing.claim != kNone) just += 0.15; // a rightful claim
    }
    const double danger = qk.danger;
    return f.standing.greed * q.reward / 80.0 + f.standing.ambition * danger * 0.6 + f.standing.honor * just - std::max(0.0, danger - f.standing.martial) * 1.6 -
           distKm / 150.0 - (f.body.health < 0.7 ? 1.0 : 0.0);
}

// The road from `from` to `to` passes the village: a stop there.
bool passesVillage(const Ctx& x, Vec2 a, Vec2 b) {
    const Settlement* l = localOf(x);
    if (!l || l->ruined) return false;
    const Vec2 v{b.x - a.x, b.y - a.y};
    const double len2 = v.x * v.x + v.y * v.y;
    if (len2 < 1e-6) return false;
    const double t = std::clamp(((l->km.x - a.x) * v.x + (l->km.y - a.y) * v.y) / len2, 0.0, 1.0);
    const Vec2 c{a.x + v.x * t, a.y + v.y * t};
    if (dkm(c, l->km) > num(x.r.wan, "stop_radius_km", 9) || t <= 0.05 || t >= 0.95) return false;
    Rng rng = agentRng(x.w.seed(), static_cast<EntityId>(a.x * 1000 + b.y), x.w.now(), "realm:stop");
    return rng.chance(num(x.r.wan, "stop_chance", 0.35));
}

void travel(Ctx& x, Character& f, EntityId to) {
    const Settlement* from = pol(x, f.standing.at);
    const Settlement* dest = pol(x, to);
    if (!dest) return;
    const double spd = num(x.r.wan, "speed_km_day", 30);
    const Vec2 a = from ? from->km : Vec2{0, 0};
    if (from && passesVillage(x, a, dest->km) && !dest->full()) {
        const Settlement* l = localOf(x);
        f.standing.travelTo = l->id;
        f.standing.arrive = x.day + std::max(1, static_cast<int>(dkm(a, l->km) / spd));
        f.standing.claim = f.standing.claim; // unchanged
        f.standing.deeds.push_back({{"day", x.day}, {"code", "via_village"}, {"then", to}});
        return;
    }
    f.standing.travelTo = to;
    f.standing.arrive = x.day + std::max(1, static_cast<int>(dkm(a, dest->km) / spd));
}

EntityId thenOf(const Character& f) {
    if (f.standing.deeds.empty()) return kNone;
    const Json& d = f.standing.deeds.back();
    return d.value("code", "") == "via_village" ? d.value("then", kNone) : kNone;
}

void questDone(Ctx& x, RealmQuest& q, bool ok, Character* hero) {
    q.status = ok ? "done" : "failed";
    q.closed = x.day;
    Settlement* p = pol(x, q.polity);
    const RealmQuestKind& qk = x.r.quests.at(q.kind);
    for (EntityId t : q.takers)
        if (Character* m = fig(x, t); m && m->body.alive) {
            m->standing.quest = kNone;
            if (!ok) continue;
            const double share = q.reward / std::max<size_t>(1, q.takers.size());
            double paid = share;
            if (p && p->full()) paid = payFromVillage(x, *p, share, m);
            else if (p) paid = std::min(share, std::max(0.0, p->wealth)), p->wealth -= paid, earn(x, *m, paid);
            m->standing.renown = clamp01(m->standing.renown + qk.renown);
            deed(*m, x.day, "quest_done", {{"quest", q.kind}, {"pol", q.polity}});
            log(x, "quest_done", {{"fig", m->id}, {"quest", q.kind}, {"pol", q.polity}, {"n", static_cast<int>(std::round(paid))}},
                x.w.character(m->id) ? std::vector<EntityId>{m->id} : std::vector<EntityId>{});
            if (p && p->full())
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
    std::vector<Character*> men;
    for (EntityId t : q.takers)
        if (Character* m = fig(x, t); m && m->body.alive && m->standing.captor == kNone) party += m->standing.martial * m->body.health, men.push_back(m);
    if (men.empty()) {
        q.status = "failed", q.closed = x.day;
        return;
    }
    bool ok = false;
    if (q.kind == "bandit_bounty") {
        Settlement* camp = pol(x, q.target);
        if (!camp || camp->ruined) {
            questDone(x, q, true, men[0]);
            return;
        }
        const double sp = 8.0 * party + 2.0;
        const double sc = strength(x, *camp) * 0.7;
        ok = rng.uniform() * (sp + sc) < sp;
        if (ok) {
            camp->militia *= 0.3;
            losePeople(x, *camp, static_cast<int>(std::round(camp->pop * 0.6)), "battle");
            camp->wealth *= 0.3;
            log(x, "camp_broken", {{"pol", camp->id}, {"fig", men[0]->id}});
            if (Character* at = fig(x, camp->chief); at && at->body.alive) {
                if (rng.chance(0.5)) {
                    log(x, "ataman_slain", {{"fig", at->id}, {"fig2", men[0]->id}, {"pol", camp->id}});
                    figureDies(x, *at, "slain");
                } else {
                    at->standing.life = "outlaw";
                    at->standing.role = "outlaw";
                    at->status.settlement = kNone;
                    at->relations[men[0]->id].affinity = -0.9; // he remembers
                }
            }
            if (camp->pop < num(x.r.ban, "min_pop", 5)) {
                camp->ruined = true;
                log(x, "camp_destroyed", {{"pol", camp->id}, {"fig", men[0]->id}});
                for (Character& f : people(x))
                    if (f.body.alive && f.status.settlement == camp->id) f.status.settlement = kNone, f.standing.role = "outlaw";
                scatterBand(x, *camp);
            }
        }
    } else {
        const double p = std::clamp(0.35 + 0.9 * (party / men.size() - qk.danger) + 0.08 * (men.size() - 1), 0.05, 0.95);
        ok = rng.chance(p);
        if (q.kind == "escort" && ok) {
            // the road was clear or the ambush beaten
            for (Settlement& c : polities(x))
                if (!c.ruined && c.tagged("raiders") && dkm(c.km, pol(x, q.polity)->km) < 30 && rng.chance(0.4)) {
                    log(x, "ambush_beaten", {{"fig", men[0]->id}, {"pol", c.id}});
                    break;
                }
        }
    }
    for (Character* m : men) {
        const double hurt = ok ? 0.08 : 0.3;
        if (rng.chance(ok ? qk.danger * 0.12 : qk.danger * 0.45)) {
            log(x, "hero_died", {{"fig", m->id}, {"quest", q.kind}, {"pol", q.polity}});
            figureDies(x, *m, "quest");
            continue;
        }
        if (rng.chance(0.5)) m->body.health = std::max(0.2, m->body.health - rng.range(0.05, hurt + 0.2));
    }
    questDone(x, q, ok, men[0]);
}

// In the village: the bounty is a real pack to kill.
void villageQuest(Ctx& x, RealmQuest& q, Character& f) {
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
    for (Settlement& p : polities(x)) {
        if (p.ruined || p.full() || outlaw(p)) continue;
        if ((x.day + static_cast<int>(p.id)) % 30 != 0) continue;
        Rng rng = rngOf(x, p.id, "realm:needs");
        // beasts trouble those with herds and fields; the roads need guarding where there is trade and robbers
        if (x.r.questOn("beast_bounty") && (p.tagged("farming") || p.tagged("fortified")) && rng.chance(0.12) &&
            !hasOpenQuest(x, "beast_bounty", p.id, kNone) && p.wealth > 20) {
            const auto& rw = x.r.quests.at("beast_bounty").reward;
            postQuest(x, "beast_bounty", p.id, p.chief, kNone, rng.range(rw[0], rw[1]));
        }
        bool bandits = false;
        for (Settlement& c : polities(x)) bandits |= !c.ruined && c.tagged("raiders") && dkm(c.km, p.km) < 35;
        if (x.r.questOn("escort") && p.tagged("trade") && bandits && p.wealth > 100 && rng.chance(0.35) &&
            !hasOpenQuest(x, "escort", p.id, kNone)) {
            const auto& rw = x.r.quests.at("escort").reward;
            postQuest(x, "escort", p.id, p.chief, kNone, rng.range(rw[0], rw[1]));
        }
    }
    for (auto& q : x.s.quests) {
        if (q.status != "open") continue;
        Settlement* p = pol(x, q.polity);
        if (!p || p->ruined) {
            q.status = "expired", q.closed = x.day;
            continue;
        }
        if (q.kind == "beast_bounty" && p->full()) {
            const Character* pack = x.w.character(q.target);
            if (!pack || !pack->body.alive || pack->group <= 0) {
                // the pack is gone: credit those who were here hunting it
                std::vector<EntityId> here;
                for (EntityId t : q.takers)
                    if (const Character* m = fig(x, t); m && onMap(x, *m)) here.push_back(t);
                q.takers = here;
                if (!here.empty()) questDone(x, q, true, fig(x, here[0]));
                else q.status = "expired", q.closed = x.day;
                continue;
            }
        }
        if (x.day > q.deadline && q.kind != "war_service") {
            if (!q.takers.empty()) {
                questDone(x, q, false, fig(x, q.takers[0]));
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

void arriveAt(Ctx& x, Character& f) {
    const EntityId to = f.standing.travelTo, came = f.standing.at;
    f.standing.travelTo = kNone;
    f.standing.at = to;
    Settlement* p = pol(x, to);
    if (!p) return;
    if (p->full()) {
        // the village: as a Character
        Rng rng = rngOf(x, f.id, "realm:stay");
        const auto sd = pair2(x.r.wan, "stay_days", {1, 3});
        int stay = rng.irange(static_cast<int>(sd[0]), static_cast<int>(sd[1]));
        RealmQuest* q = questById(x, f.standing.quest);
        if (q && q->kind == "beast_bounty" && q->polity == p->id) stay = inum(x.r.loc, "visit_hunt_days", 10);
        for (const auto& o : x.s.offers)
            if (o.status == "pending" && o.envoy == f.id) stay = std::max(stay, o.deadline - x.day + 1);
        if (Character* c = materialize(x, f, *p, stay, came)) {
            const bool envoy = std::any_of(x.s.offers.begin(), x.s.offers.end(), [&](const RealmOffer& o) { return o.status == "pending" && o.envoy == f.id; });
            const Settlement* home = pol(x, f.status.settlement);
            log(x, envoy ? "envoy_arrives" : "arrives_village", {{"who", c->id}, {"pol", home ? home->id : kNone}}, {c->id});
            if (q && q->status == "open" && q->polity == p->id) villageQuest(x, *q, f);
            if (envoy) {
                // he goes to talk to the chief
                if (Character* chief = x.w.character(p->chief); chief && x.w.content().interaction("chat"))
                    c->ai.queue.push_back(QueuedTask{"chat", chief->id, 0.9, x.w.now(), kTicksPerDay * 2, "realm.envoy", kSource});
            }
        }
        return;
    }
    RealmQuest* q = questById(x, f.standing.quest);
    if (!q || q->status != "open") {
        f.standing.quest = kNone;
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
        if (const Character* m = fig(x, t); m && m->body.alive && m->id != f.id && m->standing.travelTo != kNone && m->standing.arrive <= x.day + 4)
            waiting = true;
    if (waiting) {
        f.standing.arrive = x.day + 1;
        f.standing.travelTo = to; // checks again tomorrow
        return;
    }
    resolveAway(x, *q);
}

void wanderersDaily(Ctx& x) {
    std::vector<EntityId> ids;
    for (Character& f : people(x)) ids.push_back(f.id);
    for (EntityId id : ids) {
        Character* fp = fig(x, id);
        if (!fp) continue;
        Character& f = *fp;
        if (onMap(x, f)) {
            if (!f.body.alive) {
                if (f.standing.deathDay == 0) {
                    f.standing.deathDay = x.day;
                    log(x, "died_in_village", {{"fig", f.id}});
                }
                continue;
            }
            if (f.standing.captor != kNone) continue;
            const Character* sp = x.w.character(f.status.spouse);
            if (f.status.family != kNone || (sp && sp->status.settlement != kNone && pol(x, sp->status.settlement) &&
                                             pol(x, sp->status.settlement)->full())) {
                settleInVillage(x, f);
                continue;
            }
            RealmQuest* q = questById(x, f.standing.quest);
            if (q && q->status == "open" && q->polity == f.standing.at) villageQuest(x, *q, f);
            // stays while the hunt is on and the time is not up
            if (x.day >= f.standing.leaveDay && canLeave(x, f)) {
                const EntityId then = thenOf(f);
                log(x, "leaves_village", {{"who", f.id}}, {f.id});
                park(x, f);
                if (then != kNone && then != f.standing.at) travel(x, f, then);
                else if (f.status.settlement != kNone && f.status.settlement != f.standing.at && f.standing.life != "road")
                    travel(x, f, f.status.settlement);
            }
            continue;
        }
        if (!f.body.alive || f.standing.captor != kNone) continue;
        if (f.standing.travelTo != kNone) {
            if (x.day >= f.standing.arrive) arriveAt(x, f);
            continue;
        }
        if (f.standing.life != "road" && f.standing.life != "outlaw") continue;
        if (f.standing.life == "outlaw") continue; // the band lives at the camp
        if (f.standing.quest != kNone) {
            RealmQuest* q = questById(x, f.standing.quest);
            if (!q || q->status != "open") f.standing.quest = kNone;
            else continue;
        }
        if (years(f) < 16) continue;
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
            f.standing.quest = best->id;
            log(x, "quest_taken", {{"fig", f.id}, {"quest", best->kind}, {"pol", best->polity}});
            const EntityId where = best->kind == "bandit_bounty" ? best->target : best->polity;
            if (where != f.standing.at) travel(x, f, where);
            continue;
        }
        // drift to the next town for work
        Rng rng = rngOf(x, f.id, "realm:drift");
        const auto dd = pair2(x.r.wan, "drift_days", {10, 25});
        if (!rng.chance(1.0 / std::max(1.0, rng.range(dd[0], dd[1])))) continue;
        std::vector<EntityId> to;
        std::vector<double> wt;
        double sum = 0.0;
        for (Settlement& p : polities(x)) {
            if (p.ruined || p.id == f.standing.at || outlaw(p)) continue;
            if (p.id == f.standing.claim) continue;
            // the village draws wanderers when it has work for them
            const double w = p.full() ? (hasOpenQuest(x, "beast_bounty", p.id, kNone) || hasOpenQuest(x, "bandit_bounty", p.id, kNone) ? 1.5 : 0.15)
                                                   : (p.size == "town" || p.size == "city" ? 3.0 : 1.0);
            to.push_back(p.id), wt.push_back(w), sum += w;
        }
        if (to.empty()) continue;
        double pick = rng.range(0, sum);
        size_t k = 0;
        for (; k + 1 < to.size() && pick > wt[k]; ++k) pick -= wt[k];
        travel(x, f, to[k]);
    }
}

// A settlement's numbers are its people: counted at the end of the day. The common
// dead leave the world (people of Standing stay, the chronicle names them).
void countPeople(Ctx& x) {
    std::vector<EntityId> gone;
    for (const auto& [id, c] : x.w.offstage())
        if (!c.body.alive && c.standing.role.empty() && !c.isAnimal()) gone.push_back(id);
    for (EntityId id : gone) x.w.offstage().erase(id);
    std::map<EntityId, int> n;
    for (const auto& [id, c] : x.w.offstage())
        if (c.body.alive && !c.isAnimal() && c.standing.life != "road" && c.standing.captor == kNone) ++n[c.status.settlement];
    for (Settlement& p : polities(x)) {
        if (p.full()) continue;
        p.pop = n.count(p.id) ? n[p.id] : 0;
        p.militia = std::min(p.militia, p.pop);
    }
}

} // namespace

// --- the daily pass -------------------------------------------------------------------

void realmDaily(World& w) {
    const RealmRules& r = realmRules(w.content());
    if (!r.loaded || !r.enabled) return;
    RealmState s = loadRealm(w);
    Ctx x{w, s, r, w.day()};
    // People and settlements live in the world's maps: adding one never moves another.
    if (!s.inited) {
        if (w.settlements().empty()) return;
        genesis(x);
    }
    if (!s.once.count("peopled")) { // a region laid out before its people were (AL-92 stage 1 saves)
        for (Settlement& p : polities(x)) populate(x, p);
        s.once["peopled"] = 1;
    }
    for (Settlement& p : polities(x))
        if (p.full() && !p.ruined) {
            syncFull(x, p);  // on the map the numbers are read, not counted
            localNeeds(x, p);
        }
    economyDaily(x);
    grainTrade(x);
    figuresDaily(x);
    notables(x);
    lifestyle(x);
    banditsFound(x);
    banditsDaily(x);
    resettle(x);
    decayGrievances(x);
    tributeDaily(x);
    peoplesDaily(x);
    councils(x);
    offersDaily(x);
    warsDaily(x);
    captives(x);
    revolts(x);
    reclaims(x);
    questsDaily(x);
    wanderersDaily(x);
    for (auto& h : hooks()) h(w, s);
    countPeople(x);
    saveRealm(w, s);
}

// --- commands ---------------------------------------------------------------------------

namespace {

bool realmCommand(World& w, Command& cmd) {
    if (cmd.type.rfind("realm.", 0) != 0) return false;
    const RealmRules& r = realmRules(w.content());
    RealmState s = loadRealm(w);
    Ctx x{w, s, r, w.day()};
    const EntityId actor = cmd.args.value("actor", kNone);
    // the actor's own settlement (whoever leads it acts for it), else the one on the map
    Settlement* local = nullptr;
    if (const Character* a = w.person(actor)) local = pol(x, a->status.settlement);
    for (auto& [id, p] : w.settlements())
        if (actor != kNone && p.chief == actor) local = &p;
    if (!local) local = realmLocal(w);
    auto reject = [&](const std::string& why) {
        cmd.result = "rejected:" + why;
        return true;
    };
    if (!r.loaded || !s.inited || !local) return reject("no_realm");
    const bool chief = actor != kNone && actor == local->chief;
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
            Settlement* q = pol(x, cmd.args.value("polity", kNone));
            if (!q || q->ruined || q->id == local->id || warBetween(x, local->id, q->id)) return reject("bad_target");
            startWar(x, *local, *q, "chief");
        } else if (t == "make_peace") {
            Settlement* q = pol(x, cmd.args.value("polity", kNone));
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
        if (!r.templates.count(k)) err.push_back("realm.json: layout template '" + k + "' is not in templates");
    for (const auto& [k, t] : r.templates)
        if (!t.institution.empty() && !r.institutions.count(t.institution))
            err.push_back("realm.json: template '" + k + "' institution '" + t.institution + "' is not in institutions");
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
          "arrives_village", "envoy_arrives", "leaves_village", "died_in_village", "wanderer_settles", "hoard_grows", "dragon_breath"})
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
    if (auto it = w.settlements().find(id); it != w.settlements().end()) {
        const std::string& nm = it->second.name;
        return loc.noun(lang, "realm.place." + nm + ".name", nm);
    }
    if (const Character* c = w.person(id)) return loc.personNoun(lang, c->name, c->body.sex);
    return nullptr;
}

} // namespace altera::sim
