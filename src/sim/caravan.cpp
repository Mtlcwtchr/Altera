// LR-154: caravans, peddlers and supply trains - see caravan.hpp.

#include "sim/caravan.hpp"
#include "sim/regional.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>

#include "sim/content.hpp"
#include "sim/data_fs.hpp"
#include "sim/expr.hpp"
#include "sim/realm.hpp"
#include "sim/trade.hpp"
#include "sim/world.hpp"

namespace fs = std::filesystem;

namespace lr::sim {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Venture, id, leader, members, guards, beasts, home, from, to, then, depart,
                                                arrive, state, marketUntil, kind, bought, sold, legs, stops)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CaravanState, on, nextId, ventures, markets, deposits, rest, stats)

CaravanState loadCaravans(const World& w) {
    CaravanState s;
    if (const Json* j = w.moduleState("caravans"); j && j->is_object()) s = j->get<CaravanState>();
    return s;
}
void saveCaravans(World& w, const CaravanState& s) { w.moduleState("caravans") = Json(s); }

namespace {

constexpr const char* kSource = "caravan";

struct Good {
    std::string id;
    bool food = false;
    double price = 1.0, bulk = 1.0, deposit = 0.0, hi = -1.0;
    Expr make, use;
};

struct Kind {
    std::string id;
    Expr when;
};

struct Rules {
    bool loaded = false, enabled = false;
    std::map<std::string, double> cfg;
    Expr traders, capacity, speed;
    std::vector<Kind> kinds;
    std::map<std::string, Good> goods;
    std::vector<std::string> errors;
    double c(const std::string& k, double d = 0.0) const {
        auto it = cfg.find(k);
        return it == cfg.end() ? d : it->second;
    }
    std::array<double, 2> pair(const Json& j, const char* k, std::array<double, 2> d) const {
        if (j.contains(k) && j[k].is_array() && j[k].size() == 2) return {j[k][0].get<double>(), j[k][1].get<double>()};
        return d;
    }
    std::array<double, 2> cooldown{4, 12}, traderYears{18, 55}, guardYears{17, 45}, loot{0.4, 0.9}, marketDays{2, 4};
};

Expr compileOr(const Json& j, const char* k, const std::string& dflt, std::vector<std::string>& errs, const std::string& where) {
    const std::string src = j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : dflt;
    std::string err;
    Expr e = Expr::compile(src, &err);
    if (!err.empty()) errs.push_back(where + "." + k + ": " + err);
    return e;
}

Rules parse(const Json& j) {
    Rules r;
    r.loaded = true;
    r.enabled = j.value("enabled", true);
    if (j.contains("config"))
        for (const auto& [k, v] : j["config"].items())
            if (v.is_number()) r.cfg[k] = v.get<double>();
    const Json& cf = j.value("config", Json::object());
    r.cooldown = r.pair(cf, "cooldown_days", r.cooldown);
    r.traderYears = r.pair(cf, "trader_years", r.traderYears);
    r.guardYears = r.pair(cf, "guard_years", r.guardYears);
    r.loot = r.pair(cf, "loot_share", r.loot);
    r.marketDays = r.pair(cf, "market_days", r.marketDays);
    r.traders = compileOr(j, "traders", "1", r.errors, "caravans");
    r.capacity = compileOr(j, "capacity", "carriers * 35", r.errors, "caravans");
    r.speed = compileOr(j, "speed", "25", r.errors, "caravans");
    for (const auto& k : j.value("kinds", Json::array())) {
        Kind kd;
        kd.id = k.value("id", "");
        kd.when = compileOr(k, "when", "1", r.errors, "kinds." + kd.id);
        if (!kd.id.empty()) r.kinds.push_back(std::move(kd));
    }
    const Json goods = j.value("goods", Json::object());
    for (const auto& [id, g] : goods.items()) {
        Good d;
        d.id = id;
        d.food = g.value("food", false);
        d.price = g.value("price", 1.0);
        d.bulk = std::max(0.01, g.value("bulk", 1.0));
        d.deposit = g.value("deposit", 0.0);
        d.hi = g.value("price_hi", -1.0);
        d.make = compileOr(g, "make", "0", r.errors, "goods." + id);
        d.use = compileOr(g, "use", "0", r.errors, "goods." + id);
        r.goods[id] = std::move(d);
    }
    return r;
}

const Rules& rules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<Rules>> cache;
    static const Rules kOff;
    if (c.dir.empty()) return kOff;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(data::cacheKey(c.dir));
    if (it != cache.end()) return *it->second;
    auto r = std::make_unique<Rules>();
    const fs::path p = fs::path(c.dir) / "caravans.json";
    if (data::exists(p)) {
        data::InFile in(p);
        *r = parse(Json::parse(in));
    }
    return *cache.emplace(data::cacheKey(c.dir), std::move(r)).first->second;
}

// --- context ------------------------------------------------------------------------

struct Ctx {
    World& w;
    RealmState& rs;
    CaravanState& s;
    const Rules& r;
    int day;
};

std::string key(EntityId id) { return std::to_string(id); }
double dkm(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
bool alive(const Character* c) { return c && c->body.alive && !c->isAnimal(); }
bool onMap(const World& w, const Character& c) { return w.character(c.id) == &c; }
int years(const Character& c) { return c.body.lifeYears(); }

Settlement* pol(World& w, EntityId id) {
    auto it = w.settlements().find(id);
    return it == w.settlements().end() ? nullptr : &it->second;
}
const Settlement* pol(const World& w, EntityId id) {
    auto it = w.settlements().find(id);
    return it == w.settlements().end() ? nullptr : &it->second;
}

void log(Ctx& x, const std::string& code, Json params, std::vector<EntityId> who = {}) {
    ++x.s.stats[code];
    x.w.chronicle("caravan", std::move(who), "chronicle.caravan." + code, std::move(params));
}

bool hasDeposit(const CaravanState& s, const Settlement& p, const std::string& good) {
    auto it = s.deposits.find(key(p.id));
    return it != s.deposits.end() && std::find(it->second.begin(), it->second.end(), good) != it->second.end();
}

// What a settlement's formulas read: its numbers, its tags and size, its peoples.
struct PolCtx : ExprCtx {
    const Settlement& p;
    const Rules& r;
    double deposit = 0.0;
    std::string belt;   // LR-168: the belt of the settlement's culture (regional.hpp)
    PolCtx(const Content& c, const Settlement& p_, const Rules& r_) : p(p_), r(r_), belt(regionalBelt(c, p_.culture)) {}
    double var(const std::string& n) const override {
        const double heads = std::max(1.0, p.pop);
        if (n == "pop") return p.pop;
        if (n == "land") return p.land;
        if (n == "crafts") return p.crafts;
        if (n == "fort") return p.fort;
        if (n == "fields") return p.fields;
        if (n == "wealth_head") return std::max(0.0, p.wealth) / heads;
        if (n == "food_days") return p.food / heads;
        if (n == "deposit") return deposit;
        if (n.rfind("cfg.", 0) == 0) return r.c(n.substr(4));
        if (n.rfind("belt.", 0) == 0) return n.compare(5, std::string::npos, belt) == 0 && !belt.empty() ? 1.0 : 0.0;
        if (n.rfind("culture.", 0) == 0) return n.compare(8, std::string::npos, p.culture) == 0 && !p.culture.empty() ? 1.0 : 0.0;
        if (n.rfind("people.", 0) == 0) {
            const std::string k = n.substr(7);
            double sum = 0.0, mine = 0.0;
            for (const auto& [id, v] : p.peoples) sum += v, mine += id == k ? v : 0.0;
            return sum > 0.0 ? mine / sum : 0.0;
        }
        return 0.0;
    }
    double sym(const std::string& fn, const std::string& arg) const override {
        if (fn == "tag") return p.tagged(arg) || p.size == arg ? 1.0 : 0.0;
        return 0.0;
    }
};

// A party's numbers for its kind, capacity and speed.
struct PartyCtx : ExprCtx {
    const Rules& r;
    std::map<std::string, double> v;
    explicit PartyCtx(const Rules& r_) : r(r_) {}
    double var(const std::string& n) const override {
        if (n.rfind("cfg.", 0) == 0) return r.c(n.substr(4));
        auto it = v.find(n);
        return it == v.end() ? 0.0 : it->second;
    }
    double sym(const std::string&, const std::string&) const override { return 0.0; }
};

double nutritionOf(const Content& c, const std::string& item) {
    auto it = c.items.find(item);
    return it == c.items.end() ? 0.0 : std::max(0.05, it->second.nutrition);
}

// --- markets ----------------------------------------------------------------------------

double perDay(const Expr& e, const PolCtx& pc) { return std::max(0.0, e.eval(pc, 0.0)) * std::max(0.0, pc.p.pop) / kDaysPerYear; }

// The households' stores and pockets of the map's village.
double fullStock(const World& w, const Settlement& p, const std::string& item) {
    double n = 0.0;
    for (EntityId fid : p.families) {
        auto f = w.families().find(fid);
        if (f == w.families().end()) continue;
        if (const WorldObject* o = w.object(f->second.storage))
            if (auto it = o->stock.find(item); it != o->stock.end()) n += std::max(0, it->second);
        for (EntityId m : f->second.members)
            if (const Character* c = w.character(m); alive(c)) n += spare(*c, item);
    }
    return n;
}

double stockOf(const World& w, const CaravanState& s, const Rules& r, const Settlement& p, const Good& g) {
    if (g.food) return std::max(0.0, p.food) / nutritionOf(w.content(), g.id);  // person-days in units
    if (p.full()) return fullStock(w, p, g.id);
    auto m = s.markets.find(key(p.id));
    if (m == s.markets.end()) return 0.0;
    auto it = m->second.find(g.id);
    return it == m->second.end() ? 0.0 : it->second;
    (void)r;
}

// What the settlement wants to hold: its need over target_days (food: its people's days).
double targetOf(const World& w, const CaravanState& s, const Rules& r, const Settlement& p, const Good& g) {
    if (g.food) return std::max(1.0, p.pop) * r.c("target_days", 240) / nutritionOf(w.content(), g.id);
    PolCtx pc(w.content(), p, r);
    pc.deposit = hasDeposit(s, p, g.id) ? 1.0 : 0.0;
    return perDay(g.use, pc) * r.c("target_days", 240);
}

double priceOf(const World& w, const CaravanState& s, const Rules& r, const Settlement& p, const Good& g, double stockShift = 0.0) {
    const double stock = std::max(0.0, stockOf(w, s, r, p, g) + stockShift);
    const double target = targetOf(w, s, r, p, g);
    const double ratio = (target + 1.0) / (stock + 1.0);
    return g.price * std::clamp(std::pow(ratio, r.c("elasticity", 0.6)), r.c("price_lo", 0.35), r.c("price_hi", 3.0));
}

// How much of it the settlement can part with (above what it keeps) and how much it would take.
double spareAt(const World& w, const CaravanState& s, const Rules& r, const Settlement& p, const Good& g) {
    const double keep = targetOf(w, s, r, p, g) * r.c("keep_days", 120) / std::max(1.0, r.c("target_days", 240));
    return std::max(0.0, stockOf(w, s, r, p, g) - keep);
}
double wantAt(const World& w, const CaravanState& s, const Rules& r, const Settlement& p, const Good& g) {
    return std::max(0.0, targetOf(w, s, r, p, g) - stockOf(w, s, r, p, g));
}

void addStock(Ctx& x, Settlement& p, const Good& g, double n) {
    if (g.food) {
        p.food = std::max(0.0, p.food + n * nutritionOf(x.w.content(), g.id));
        return;
    }
    if (p.full()) return; // on the map things change hands item by item
    double& st = x.s.markets[key(p.id)][g.id];
    st = std::max(0.0, st + n);
}

void rollDeposits(Ctx& x, const Settlement& p) {
    if (x.s.deposits.count(key(p.id))) return;
    auto& d = x.s.deposits[key(p.id)];
    for (const auto& [id, g] : x.r.goods)
        if (g.deposit > 0.0 && x.w.rng(p.id, "caravan:deposit:" + id).chance(g.deposit)) d.push_back(id);
    // the market's first stock: a part of a year's making
    if (!p.full()) {
        PolCtx pc(x.w.content(), p, x.r);
        auto& m = x.s.markets[key(p.id)];
        for (const auto& [id, g] : x.r.goods) {
            if (g.food) continue;
            pc.deposit = hasDeposit(x.s, p, id) ? 1.0 : 0.0;
            m[id] = perDay(g.make, pc) * kDaysPerYear * x.r.c("start_stock_years", 0.4);
        }
    }
}

void marketsDaily(Ctx& x) {
    for (auto& [id, p] : x.w.settlements()) {
        if (p.ruined) continue;
        rollDeposits(x, p);
        if (p.full()) continue;
        PolCtx pc(x.w.content(), p, x.r);
        auto& m = x.s.markets[key(p.id)];
        for (const auto& [gid, g] : x.r.goods) {
            if (g.food) continue;
            pc.deposit = hasDeposit(x.s, p, gid) ? 1.0 : 0.0;
            const double make = perDay(g.make, pc), use = perDay(g.use, pc);
            double& st = m[gid];
            st = std::max(0.0, st + make - use);
            st = std::min(st, std::max(1.0, (make + use) * kDaysPerYear * x.r.c("stock_cap_years", 1.5)));
        }
    }
}

// --- people -------------------------------------------------------------------------------

double purseOf(const World& w, const Character& c) { return c.standing.purse + (onMap(w, c) ? c.has("coin") : 0); }
void pay(World& w, Character& c, double v) {
    if (onMap(w, c)) {
        // on the map coin is the item; a fraction stays in the purse
        const double total = c.standing.purse + c.has("coin") + v;
        const int coin = std::max(0, static_cast<int>(std::floor(total)));
        c.inventory["coin"] = coin;
        if (coin == 0) c.inventory.erase("coin");
        c.standing.purse = total - coin;
    } else {
        c.standing.purse = std::max(0.0, c.standing.purse + v);
    }
}

bool inVenture(const CaravanState& s, EntityId id) {
    for (const auto& v : s.ventures)
        if (v.state != "done" && v.state != "lost" && std::find(v.members.begin(), v.members.end(), id) != v.members.end()) return true;
    return false;
}

// A free person of the settlement offstage: at home, of no Standing role, in no venture.
bool freeAtHome(const Ctx& x, const Character& c, EntityId p) {
    return alive(&c) && !onMap(x.w, c) && c.status.settlement == p && c.standing.at == p && c.standing.life == "settled" &&
           c.standing.role.empty() && c.standing.captor == kNone && c.standing.travelTo == kNone && !inVenture(x.s, c.id);
}

std::vector<Character*> tradersOf(Ctx& x, EntityId p) {
    std::vector<Character*> out;
    for (auto& [id, c] : x.w.offstage())
        if (c.occupation == "trader" && alive(&c) && c.status.settlement == p && c.standing.role.empty()) out.push_back(&c);
    return out;
}

// New traders where the settlement's numbers call for them (caravans.json traders).
void recruit(Ctx& x, Settlement& p) {
    PolCtx pc(x.w.content(), p, x.r);
    const int want = std::min(static_cast<int>(x.r.c("max_traders", 6)), static_cast<int>(std::floor(x.r.traders.eval(pc, 0.0) + 0.5)));
    int have = static_cast<int>(tradersOf(x, p.id).size());
    if (have >= want) return;
    const TradeRules* tr = tradeRules(x.w.content());
    std::vector<std::string> picks;
    if (tr)
        if (auto it = tr->roles.find("trader"); it != tr->roles.end()) picks = it->second.pickTraits;
    std::vector<std::pair<double, Character*>> pool;
    for (auto& [id, c] : x.w.offstage()) {
        if (!freeAtHome(x, c, p.id) || !c.occupation.empty()) continue;
        if (years(c) < x.r.traderYears[0] || years(c) > x.r.traderYears[1]) continue;
        double sc = 0.0;
        for (const auto& t : picks) sc += c.hasTrait(t) ? 1.0 : 0.0;
        sc += x.w.rng(c.id, "caravan:trader").uniform() * 0.8 + c.standing.greed * 0.5;
        pool.push_back({sc, &c});
    }
    std::sort(pool.begin(), pool.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second->id < b.second->id); });
    for (auto& [sc, c] : pool) {
        if (have >= want) break;
        c->occupation = "trader";
        // the stake: the settlement's coin behind him (kin, partners, the lord's money)
        const double stake = std::clamp(std::max(0.0, p.wealth) * x.r.c("stake_share", 0.04), x.r.c("stake_min", 8), x.r.c("stake_max", 120));
        const double take = std::min(stake, std::max(0.0, p.wealth));
        p.wealth -= take;
        c->standing.purse += take;
        ++have;
        ++x.s.stats["trader_new"];
    }
}

// --- routes -------------------------------------------------------------------------------------

bool atWar(const RealmState& rs, EntityId a, EntityId b) {
    for (const auto& wr : rs.wars)
        if (!wr.over && ((wr.attacker == a && wr.defender == b) || (wr.attacker == b && wr.defender == a))) return true;
    return false;
}

double grudge(RealmState& rs, EntityId a, EntityId b) { return rs.view(a, b).grudge(); }

// Outlaw camps whose band could reach the road a..b.
double roadRisk(const Ctx& x, Vec2 a, Vec2 b) {
    double risk = 0.0;
    for (const auto& [id, p] : x.w.settlements()) {
        if (p.ruined || !p.tagged("outlaw")) continue;
        const Vec2 v{b.x - a.x, b.y - a.y};
        const double len2 = std::max(1e-6, v.x * v.x + v.y * v.y);
        const double t = std::clamp(((p.km.x - a.x) * v.x + (p.km.y - a.y) * v.y) / len2, 0.0, 1.0);
        const Vec2 c{a.x + v.x * t, a.y + v.y * t};
        if (dkm(c, p.km) <= x.r.c("ambush_km", 22)) risk += p.militia / (p.militia + 3.0);
    }
    return risk;
}

struct RoutePlan {
    EntityId to = kNone;
    std::map<std::string, int> buy;
    double cost = 0.0, value = 0.0, profit = -1e9;
    int guards = 0, carriers = 0, beasts = 0, days = 1;
};

double capacityFor(const Rules& r, int carriers, int guards, int beasts) {
    PartyCtx pc(r);
    pc.v = {{"carriers", carriers}, {"guards", guards}, {"beasts", beasts}, {"party", carriers + guards}};
    return std::max(1.0, r.capacity.eval(pc, 35.0 * carriers));
}
double speedFor(const Rules& r, int party, int beasts) {
    PartyCtx pc(r);
    pc.v = {{"party", party}, {"beasts", beasts}};
    return std::max(1.0, r.speed.eval(pc, 25.0));
}

// The best load for the road home -> to: goods by margin per bulk while the purse and the backs allow.
RoutePlan planRoute(const Ctx& x, const Character& trader, const Settlement& home, const Settlement& to, int menFree) {
    RoutePlan pl;
    pl.to = to.id;
    const double purse = purseOf(x.w, trader);
    struct Line {
        const Good* g;
        double margin, buyAt, spare, want;
    };
    std::vector<Line> lines;
    for (const auto& [id, g] : x.r.goods) {
        const double spare = spareAt(x.w, x.s, x.r, home, g);
        const double want = wantAt(x.w, x.s, x.r, to, g);
        if (spare < 1.0 || want < 1.0) continue;
        const double buyAt = priceOf(x.w, x.s, x.r, home, g), sellAt = priceOf(x.w, x.s, x.r, to, g);
        if (sellAt <= buyAt * 1.05) continue;
        lines.push_back({&g, (sellAt - buyAt) / g.bulk, buyAt, spare, want});
    }
    if (lines.empty()) return pl;
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.margin > b.margin || (a.margin == b.margin && a.g->id < b.g->id); });
    // beasts of burden the purse can pay for (the transport layer replaces them with real ones)
    const int beasts = std::min(static_cast<int>(x.r.c("beasts_max", 4)), static_cast<int>(purse * 0.25 / std::max(1.0, x.r.c("beast_price", 6))));
    const int carriers = 1 + std::min({static_cast<int>(x.r.c("max_carriers", 3)) - 1, std::max(0, menFree),
                                       static_cast<int>(purse / std::max(1.0, x.r.c("carriers_per_value", 220)))});
    double room = capacityFor(x.r, carriers, 0, beasts);
    double left = purse;
    // greedy by what a line would earn now with the room and coin left (the purse or the backs bind)
    std::vector<bool> used(lines.size(), false);
    for (;;) {
        int bi = -1, bn = 0;
        double bg = 0.0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (used[i]) continue;
            const Line& l = lines[i];
            const int n = static_cast<int>(std::floor(std::min({l.spare, l.want, room / l.g->bulk, left / std::max(0.01, l.buyAt)})));
            const double gain = n * l.margin * l.g->bulk;
            if (n > 0 && gain > bg) bg = gain, bi = static_cast<int>(i), bn = n;
        }
        if (bi < 0) break;
        used[static_cast<size_t>(bi)] = true;
        const Line& l = lines[static_cast<size_t>(bi)];
        pl.buy[l.g->id] += bn;
        pl.cost += bn * l.buyAt;
        pl.value += bn * (l.buyAt + l.margin * l.g->bulk);
        room -= bn * l.g->bulk;
        left -= bn * l.buyAt;
        if (room < 0.5 || left < 0.5) break;
    }
    if (pl.buy.empty()) return pl;
    const double km = dkm(home.km, to.km);
    // guards when the load is worth it and the road is bad
    const double risk = roadRisk(x, home.km, to.km);
    int guards = 0;
    if (pl.value >= x.r.c("guard_from_value", 40))
        guards = std::min({static_cast<int>(x.r.c("max_guards", 6)), std::max(0, menFree - (carriers - 1)),
                           static_cast<int>(std::ceil(pl.value / x.r.c("value_per_guard", 45) * (0.5 + risk)))});
    const int party = carriers + guards;
    pl.days = std::max(1, static_cast<int>(std::ceil(km / speedFor(x.r, party, beasts))));
    const double road = pl.days * 2.0 * (party * x.r.c("upkeep_per_day", 0.05) + guards * x.r.c("guard_wage_per_day", 0.15));
    const double ambush = std::min(0.9, risk * x.r.c("ambush_per_day", 0.06) * pl.days) / (1.0 + guards * 0.5);
    pl.profit = pl.value - pl.cost - road - ambush * pl.value * 0.6;
    pl.guards = guards;
    pl.carriers = carriers;
    pl.beasts = beasts;
    return pl;
}

// --- the party --------------------------------------------------------------------------------

double cargoValue(const Ctx& x, const Character& leader, const Settlement* at) {
    double v = 0.0;
    for (const auto& [id, g] : x.r.goods)
        if (int n = leader.has(id); n > 0) v += n * (at ? priceOf(x.w, x.s, x.r, *at, g) : g.price);
    return v;
}

void classify(Ctx& x, Venture& v) {
    const Character* l = x.w.person(v.leader);
    if (!l) return;
    double food = 0.0, all = 0.0;
    for (const auto& [id, g] : x.r.goods)
        if (int n = l->has(id); n > 0) (g.food ? food : all) += n * g.bulk;
    all += food;
    PartyCtx pc(x.r);
    pc.v = {{"party", static_cast<double>(v.members.size())},
            {"guards", static_cast<double>(v.guards)},
            {"beasts", static_cast<double>(v.beasts)},
            {"value", cargoValue(x, *l, nullptr)},
            {"bulk", all},
            {"food_share", all > 0 ? food / all : 0.0}};
    for (const auto& k : x.r.kinds)
        if (k.when.eval(pc, 0.0) != 0.0) {
            v.kind = k.id;
            return;
        }
    v.kind = x.r.kinds.empty() ? "peddler" : x.r.kinds.back().id;
}

void buyAt(Ctx& x, Character& leader, Settlement& p, const std::map<std::string, int>& what, Venture& v) {
    for (const auto& [id, n] : what) {
        auto g = x.r.goods.find(id);
        if (g == x.r.goods.end() || n <= 0) continue;
        const double price = priceOf(x.w, x.s, x.r, p, g->second, -0.5 * n);
        const double cost = price * n;
        if (purseOf(x.w, leader) < cost) continue;
        pay(x.w, leader, -cost);
        leader.inventory[id] += n;
        addStock(x, p, g->second, -n);
        p.wealth += cost;
        p.income["trade"] += cost * 0.5;
        v.bought += cost;
    }
}

// Sells what the settlement wants of the load, as far as its coin goes.
void sellAt(Ctx& x, Character& leader, Settlement& p, Venture& v, bool everything) {
    for (const auto& [id, g] : x.r.goods) {
        int have = leader.has(id);
        if (have <= 0) continue;
        int n = everything ? have : std::min(have, static_cast<int>(std::ceil(wantAt(x.w, x.s, x.r, p, g))));
        if (n <= 0) continue;
        const double price = priceOf(x.w, x.s, x.r, p, g, 0.5 * n);
        n = std::min(n, static_cast<int>(std::max(0.0, p.wealth) / std::max(0.01, price)));
        if (n <= 0) continue;
        leader.inventory[id] -= n;
        if (leader.inventory[id] <= 0) leader.inventory.erase(id);
        addStock(x, p, g, n);
        p.wealth -= n * price;
        pay(x.w, leader, n * price);
        v.sold += n * price;
    }
}

void hireGuard(Ctx& x, Character& leader, Character& g, int days) {
    const double wage = days * x.r.c("guard_wage_per_day", 0.15);
    const double paid = std::min(wage, purseOf(x.w, leader));
    pay(x.w, leader, -paid);
    g.standing.purse += paid;
}

void toRoad(Ctx& x, Venture& v, EntityId from, EntityId to, int days) {
    v.from = from;
    v.to = to;
    v.depart = x.day;
    v.arrive = x.day + std::max(1, days);
    v.state = "road";
    ++v.legs;
    for (EntityId id : v.members)
        if (Character* c = x.w.person(id)) {
            c->standing.life = "road"; // away: not counted at home
            c->standing.at = from;
        }
}

int legDays(const Ctx& x, const Venture& v, EntityId a, EntityId b) {
    const Settlement* pa = pol(x.w, a);
    const Settlement* pb = pol(x.w, b);
    if (!pa || !pb) return 1;
    return std::max(1, static_cast<int>(std::ceil(dkm(pa->km, pb->km) / speedFor(x.r, static_cast<int>(v.members.size()), v.beasts))));
}

// Whether the road a..b passes the map's village close enough for a market day there.
bool passesVillage(Ctx& x, const Venture& v, Vec2 a, Vec2 b) {
    const Settlement* l = realmLocal(x.w);
    if (!l || l->ruined || v.stops > 0) return false; // one market day on the map a venture
    const Vec2 d{b.x - a.x, b.y - a.y};
    const double len2 = d.x * d.x + d.y * d.y;
    if (len2 < 1e-6) return false;
    const double t = std::clamp(((l->km.x - a.x) * d.x + (l->km.y - a.y) * d.y) / len2, 0.0, 1.0);
    const Vec2 c{a.x + d.x * t, a.y + d.y * t};
    if (dkm(c, l->km) > x.r.c("stop_radius_km", 12) || t <= 0.05 || t >= 0.95) return false;
    return x.w.rng(static_cast<EntityId>(v.id), "caravan:stop").chance(x.r.c("stop_chance", 0.5));
}

// Sets the party on the road to `to`, by the village when the road passes it.
void travel(Ctx& x, Venture& v, EntityId from, EntityId to) {
    const Settlement* a = pol(x.w, from);
    const Settlement* b = pol(x.w, to);
    const Settlement* l = realmLocal(x.w);
    if (a && b && l && l->id != to && l->id != from && passesVillage(x, v, a->km, b->km)) {
        v.then = to;
        toRoad(x, v, from, l->id, legDays(x, v, from, l->id));
        return;
    }
    v.then = kNone;
    toRoad(x, v, from, to, legDays(x, v, from, to));
}

int setOut(Ctx& x, Character& leader, Settlement& home, EntityId forced) {
    if (static_cast<int>(x.s.ventures.size()) >= static_cast<int>(x.r.c("max_ventures", 40))) return 0;
    // the free men of the settlement who could carry and guard
    std::vector<Character*> men;
    for (auto& [id, c] : x.w.offstage())
        if (c.id != leader.id && freeAtHome(x, c, home.id) && c.occupation.empty() && years(c) >= x.r.guardYears[0] &&
            years(c) <= x.r.guardYears[1])
            men.push_back(&c);
    std::sort(men.begin(), men.end(), [](const Character* a, const Character* b) {
        return a->standing.martial > b->standing.martial || (a->standing.martial == b->standing.martial && a->id < b->id);
    });
    RoutePlan best;
    for (auto& [id, p] : x.w.settlements()) {
        if (p.id == home.id || p.ruined || p.tagged("outlaw")) continue;
        if (forced != kNone && p.id != forced) continue;
        if (dkm(home.km, p.km) > x.r.c("reach_km", 90) && forced == kNone) continue;
        if (atWar(x.rs, home.id, p.id) || grudge(x.rs, p.id, home.id) > 0.6) continue;
        RoutePlan pl = planRoute(x, leader, home, p, static_cast<int>(men.size()));
        if (pl.profit > best.profit) best = pl;
    }
    bool carries = false; // wares of his own (a peddler's pack): a forced road goes with them
    for (const auto& [id, g] : x.r.goods) carries |= leader.has(id) > 0;
    if (forced != kNone && best.buy.empty() && carries) {
        if (const Settlement* to = pol(x.w, forced)) {
            best.to = forced;
            best.carriers = 1;
            best.days = std::max(1, static_cast<int>(std::ceil(dkm(home.km, to->km) / speedFor(x.r, 1, 0))));
            best.profit = 0.0;
        }
    }
    if (best.to == kNone || (best.buy.empty() && !(forced != kNone && carries))) return 0;
    if (forced == kNone && (best.profit < x.r.c("min_profit", 4.0) || best.profit < best.cost * x.r.c("min_profit_share", 0.12))) return 0;
    Venture v;
    v.id = x.s.nextId++;
    v.leader = leader.id;
    v.home = home.id;
    v.members.push_back(leader.id);
    v.beasts = best.beasts;
    const int helpers = std::min<int>(static_cast<int>(men.size()), best.carriers - 1 + best.guards);
    for (int i = 0; i < helpers; ++i) {
        Character& m = *men[static_cast<size_t>(i)];
        v.members.push_back(m.id);
        if (i >= best.carriers - 1) {
            ++v.guards;
            m.occupation = "caravan_guard";
            hireGuard(x, leader, m, best.days * 2);
        } else {
            m.occupation = "caravan_hand";
        }
    }
    pay(x.w, leader, -best.beasts * x.r.c("beast_price", 6) * 0.2); // their keep for the road
    buyAt(x, leader, home, best.buy, v);
    if (v.bought <= 0.0 && !(forced != kNone && carries)) {
        for (EntityId id : v.members)
            if (Character* c = x.w.person(id); c && c->id != leader.id) c->occupation.clear();
        return 0;
    }
    travel(x, v, home.id, best.to);
    classify(x, v);
    ++x.s.stats["set_out"];
    ++x.s.stats["set_out_" + v.kind];
    x.s.ventures.push_back(v);
    return v.id;
}

void plansDaily(Ctx& x) {
    const int every = std::max(1, static_cast<int>(x.r.c("plan_every_days", 6)));
    for (auto& [id, p] : x.w.settlements()) {
        if (p.ruined || p.full() || p.tagged("outlaw")) continue;
        if ((x.day + static_cast<int>(p.id)) % every == 0) recruit(x, p);
        for (Character* t : tradersOf(x, p.id)) {
            if (!freeAtHome(x, *t, p.id)) continue;
            if ((x.day + static_cast<int>(t->id)) % every != 0) continue;
            if (auto r = x.s.rest.find(key(t->id)); r != x.s.rest.end() && x.day < r->second) continue;
            setOut(x, *t, p, kNone);
        }
    }
}

// --- the map --------------------------------------------------------------------------------

Vec2 edgePoint(Ctx& x, const Settlement& local, Vec2 fromKm, EntityId who) {
    Rng rng = x.w.rng(who, "caravan:arrive");
    Vec2 c = local.center;
    const Vec2 dk{fromKm.x - local.km.x, fromKm.y - local.km.y};
    const double len = std::hypot(dk.x, dk.y);
    const Vec2 dir = len > 1e-6 ? Vec2{dk.x / len, dk.y / len} : Vec2{1.0, 0.0};
    const double d = rng.range(140.0, 220.0);
    Vec2 p{c.x + dir.x * d + rng.range(-6.0, 6.0), c.y + dir.y * d + rng.range(-6.0, 6.0)};
    const double sz = x.w.map().sizeM;
    if (sz > 0) p.x = std::clamp(p.x, 5.0, sz - 5.0), p.y = std::clamp(p.y, 5.0, sz - 5.0);
    return p;
}

// The party walks onto the map: the same people, at the edge on the side they came from.
bool walkOn(Ctx& x, Venture& v, Settlement& at) {
    const Settlement* from = pol(x.w, v.from);
    Vec2 base{0, 0};
    bool first = true;
    for (EntityId id : v.members) {
        Character* c = x.w.toStage(id);
        if (!c) continue;
        c->body.downUntil = 0;
        c->body.health = std::clamp(c->body.health, 0.3, 1.0);
        c->ai = AiState{};
        c->ai.nextThink = x.w.now();
        for (const auto& n : x.w.content().needs)
            if (c->need(n.id) < 0.5) c->needs[n.id] = 0.65;
        pay(x.w, *c, 0.0); // the purse into coin
        if (first) base = edgePoint(x, at, from ? from->km : Vec2{1, 0}, v.leader), first = false;
        Rng rng = x.w.rng(id, "caravan:place");
        c->pos = {base.x + rng.range(-3.0, 3.0), base.y + rng.range(-3.0, 3.0)};
        for (const auto& [oid, o] : x.w.objects())
            if (!o.ownerFamily && dist(o.pos, c->pos) <= 1500.0) c->knownObjects.insert(oid);
        c->standing.at = at.id;
        for (auto& [oid, o] : x.w.characters()) {
            if (!alive(&o) || oid == c->id || o.status.settlement != at.id) continue;
            Relation& rel = o.relations[c->id];
            rel.familiarity = std::max(rel.familiarity, 0.03);
        }
    }
    return !first;
}

bool canLeave(const World& w, const Character& c) {
    if (!c.body.alive) return true;
    return !(c.captivity.held() || c.body.downUntil > w.now() || c.ai.social.active());
}

void walkOff(Ctx& x, Venture& v) {
    for (EntityId id : v.members) {
        Character* c = x.w.character(id);
        if (!c || !c->body.alive) continue;
        const int coin = c->has("coin");
        if (coin > 0) c->inventory.erase("coin"), c->standing.purse += coin;
        c->ai.queue.clear();
        c->ai.goal = GoalInstance{};
        c->ai.plan = Plan{};
        c->ai.social = SocialLink{};
        c->ai.followAfter = SocialLink{};
        x.w.toOffstage(id);
        for (auto& [oid, o] : x.w.characters()) {
            auto& q = o.ai.queue;
            q.erase(std::remove_if(q.begin(), q.end(), [&](const QueuedTask& t) { return t.target == id; }), q.end());
        }
    }
}

// Market days on the map: the village comes to buy and to sell, by the ordinary trade rules.
void marketDay(Ctx& x, Venture& v) {
    Character* m = x.w.character(v.leader);
    if (!alive(m) || m->ai.social.active()) return;
    std::vector<Character*> folk;
    for (auto& [id, c] : x.w.characters())
        if (alive(&c) && !c.controller.isPlayer() && c.body.stage() != "child" && c.status.settlement == v.to &&
            !inVenture(x.s, c.id) && !c.captivity.held())
            folk.push_back(&c);
    Rng rng = x.w.rng(v.leader, "caravan:market");
    for (size_t i = folk.size(); i > 1; --i) std::swap(folk[i - 1], folk[static_cast<size_t>(rng.irange(0, static_cast<int>(i) - 1))]);
    int buyers = static_cast<int>(x.r.c("village_buyers_per_day", 4)), sellers = static_cast<int>(x.r.c("village_sellers_per_day", 2));
    for (Character* c : folk) {
        if (buyers <= 0 && sellers <= 0) break;
        std::string why;
        if (buyers > 0 && spare(*c, "coin") > 0) {
            Offer o = defaultOffer(x.w, *c, *m, "buy");
            bool fromCargo = false;
            for (const auto& [it, n] : o.take) fromCargo |= x.r.goods.count(it) > 0;
            if (fromCargo && startTrade(x.w, *c, *m, o, false, why)) {
                --buyers;
                ++x.s.stats["village_buy"];
                continue;
            }
        }
        if (sellers > 0) {
            // the trader asks for what he can sell on (goods of the caravans' list)
            Offer o = defaultOffer(x.w, *m, *c, "buy");
            bool good = !o.take.empty();
            for (const auto& [it, n] : o.take) good &= x.r.goods.count(it) > 0;
            if (good) {
                const DealResult res = makeOffer(x.w, *m, *c, o);
                if (res.done) --sellers, ++x.s.stats["village_sell"];
            }
        }
    }
}

// --- the road ---------------------------------------------------------------------------------

void loseMember(Ctx& x, Venture& v, Character& c, const std::string& cause) {
    c.body.alive = false;
    c.body.causeOfDeath = cause;
    c.standing.deathDay = x.day;
    if (Character* sp = x.w.person(c.status.spouse)) sp->status.spouse = kNone;
    c.occupation.clear();
    v.members.erase(std::remove(v.members.begin(), v.members.end(), c.id), v.members.end());
    ++x.s.stats["died_" + cause];
}

bool ambush(Ctx& x, Venture& v, Settlement& camp) {
    Character* l = x.w.person(v.leader);
    if (!l) return false;
    Rng rng = x.w.rng(static_cast<EntityId>(v.id), "caravan:ambush");
    const double band = std::min(camp.militia, x.r.c("ambush_band", 6));
    if (band < 1.0) return false;
    const double def = v.guards * x.r.c("guard_power", 1.0) + (v.members.size() - v.guards) * x.r.c("carrier_power", 0.35);
    const double win = def / (def + band);
    const Settlement* home = pol(x.w, v.home);
    if (rng.chance(win)) {
        if (rng.chance(x.r.c("fought_off_kill", 0.4))) camp.militia = std::max(0.0, camp.militia - 1.0);
        log(x, "fought_off", {{"who", v.leader}, {"pol", v.home}, {"pol2", camp.id}});
        return false;
    }
    const double share = rng.range(x.r.loot[0], x.r.loot[1]);
    double taken = 0.0;
    for (const auto& [id, g] : x.r.goods) {
        const int n = l->has(id);
        const int k = static_cast<int>(std::round(n * share));
        if (k <= 0) continue;
        l->inventory[id] -= k;
        if (l->inventory[id] <= 0) l->inventory.erase(id);
        taken += k * g.price;
    }
    const double coin = purseOf(x.w, *l) * share;
    pay(x.w, *l, -coin);
    taken += coin;
    camp.wealth += taken;
    camp.income["raids"] += taken;
    std::vector<EntityId> who = v.members;
    for (EntityId id : who)
        if (Character* c = x.w.person(id); c && rng.chance(x.r.c("die_share", 0.25) * (id == v.leader ? 0.6 : 1.0))) loseMember(x, v, *c, "robbers");
    if (home) {
        RealmGrievance g;
        g.cause = "raid";
        g.weight = x.r.c("grievance_robbed", 0.25);
        g.day = x.day;
        x.rs.view(v.home, camp.id).grievances.push_back(g);
    }
    log(x, "robbed", {{"who", v.leader}, {"pol", v.home}, {"pol2", camp.id}, {"n", static_cast<int>(std::round(taken))}});
    return true;
}

void roadDangers(Ctx& x, Venture& v) {
    const Vec2 at = ventureKm(x.w, v);
    for (auto& [id, p] : x.w.settlements()) {
        if (p.ruined || !p.tagged("outlaw") || dkm(at, p.km) > x.r.c("ambush_km", 22)) continue;
        Rng rng = x.w.rng(p.id, "caravan:band:" + std::to_string(v.id));
        if (rng.chance(x.r.c("ambush_per_day", 0.06) * p.militia / (p.militia + 3.0))) {
            ambush(x, v, p);
            return;
        }
    }
}

void finish(Ctx& x, Venture& v, const std::string& state) {
    v.state = state;
    for (EntityId id : v.members)
        if (Character* c = x.w.person(id)) {
            c->standing.life = "settled";
            if (c->occupation != "trader") c->occupation.clear();
        }
    if (Character* l = x.w.person(v.leader)) {
        Rng rng = x.w.rng(l->id, "caravan:rest");
        x.s.rest[key(l->id)] = x.day + rng.irange(static_cast<int>(x.r.cooldown[0]), static_cast<int>(x.r.cooldown[1]));
    }
    ++x.s.stats[state == "done" ? "home" : "lost"];
}

void arrive(Ctx& x, Venture& v) {
    Settlement* p = pol(x.w, v.to);
    Character* l = x.w.person(v.leader);
    if (!l || !alive(l)) {
        // the trader is dead: the others go home with what is left
        for (EntityId id : v.members)
            if (Character* c = x.w.person(id)) c->standing.at = c->status.settlement;
        finish(x, v, "lost");
        return;
    }
    for (EntityId id : v.members)
        if (Character* c = x.w.person(id)) c->standing.at = v.to;
    if (!p || p->ruined) {
        if (v.to == v.home) return finish(x, v, "lost");
        travel(x, v, v.to, v.home);
        return;
    }
    if (v.to == v.home) {
        // home: what is left of the load sells here, a share of the gain to the settlement
        sellAt(x, *l, *p, v, false);
        const double gain = v.sold - v.bought;
        if (gain > 0.0) {
            const double tax = gain * x.r.c("home_tax", 0.1);
            pay(x.w, *l, -std::min(tax, purseOf(x.w, *l)));
            p->wealth += tax;
            p->income["trade"] += gain;
        }
        x.s.stats["gain"] += static_cast<int>(std::round(gain));
        return finish(x, v, "done");
    }
    if (atWar(x.rs, v.home, p->id)) { // the road went bad on the way: back
        log(x, "turned_back", {{"who", v.leader}, {"pol", v.home}, {"pol2", p->id}});
        travel(x, v, v.to, v.home);
        return;
    }
    if (p->full()) {
            if (!walkOn(x, v, *p)) return finish(x, v, "lost");
        Rng rng = x.w.rng(v.leader, "caravan:stay");
        v.state = "market";
        ++v.stops;
        v.marketUntil = x.day + rng.irange(static_cast<int>(x.r.marketDays[0]), static_cast<int>(x.r.marketDays[1]));
        classify(x, v);
        log(x, v.kind + ".arrives_village", {{"who", v.leader}, {"pol", v.home}, {"n", static_cast<int>(v.members.size())}}, {v.leader});
        return;
    }
    // a market beyond the map: sell, buy for the way back (or on), go
    sellAt(x, *l, *p, v, false);
    ++x.s.stats["markets"];
    const EntityId next = v.then != kNone ? v.then : v.home;
    const Settlement* home = pol(x.w, v.home);
    if (home && next == v.home) {
        RoutePlan back = planRoute(x, *l, *p, *home, 0);
        if (!back.buy.empty() && back.profit > 0.0) buyAt(x, *l, *p, back.buy, v);
    }
    travel(x, v, v.to, next);
    classify(x, v);
}

void venturesDaily(Ctx& x) {
    for (auto& v : x.s.ventures) {
        if (v.state == "done" || v.state == "lost") continue;
        // the dead fall out of the party
        v.members.erase(std::remove_if(v.members.begin(), v.members.end(), [&](EntityId id) { return !alive(x.w.person(id)); }), v.members.end());
        if (v.state == "market") {
            const bool any = std::any_of(v.members.begin(), v.members.end(), [&](EntityId id) { return x.w.character(id) != nullptr; });
            if (!any) { // gone from the map by other means
                finish(x, v, "lost");
                continue;
            }
            if (x.day < v.marketUntil) {
                marketDay(x, v);
                continue;
            }
            bool ready = true;
            for (EntityId id : v.members)
                if (const Character* c = x.w.character(id); c && !canLeave(x.w, *c)) ready = false;
            if (!ready) continue;
            log(x, "leaves_village", {{"who", v.leader}}, {v.leader});
            walkOff(x, v);
            // the party left on the map stays there; the rest goes on
            v.members.erase(std::remove_if(v.members.begin(), v.members.end(), [&](EntityId id) { return x.w.character(id) != nullptr; }), v.members.end());
            if (v.members.empty() || !alive(x.w.person(v.leader))) {
                finish(x, v, "lost");
                continue;
            }
            const EntityId next = v.then != kNone ? v.then : v.home;
            v.state = "road";
            travel(x, v, v.to, next);
            classify(x, v);
            continue;
        }
        if (x.day >= v.arrive) {
            arrive(x, v);
            continue;
        }
        roadDangers(x, v);
        if (v.members.empty()) finish(x, v, "lost");
    }
    // the finished ones are forgotten after a while
    x.s.ventures.erase(std::remove_if(x.s.ventures.begin(), x.s.ventures.end(),
                                      [&](const Venture& v) { return (v.state == "done" || v.state == "lost") && x.day - v.arrive > 30; }),
                       x.s.ventures.end());
}

void daily(World& w, RealmState& rs) {
    const Rules& r = rules(w.content());
    if (!r.loaded || !r.enabled) return;
    CaravanState s = loadCaravans(w);
    if (!s.on) return;
    Ctx x{w, rs, s, r, w.day()};
    marketsDaily(x);
    venturesDaily(x);
    plansDaily(x);
    saveCaravans(w, s);
}

bool command(World& w, Command& cmd) {
    if (cmd.type.rfind("caravan.", 0) != 0) return false;
    if (cmd.type == "caravan.list") {
        cmd.result = "ok:" + caravansJson(w).dump();
        return true;
    }
    cmd.result = "rejected:unknown";
    return true;
}

} // namespace

// --- public -------------------------------------------------------------------------------------

bool caravansOn(const World& w) {
    const Rules& r = rules(w.content());
    if (!r.loaded || !r.enabled || !realmLoaded(w.content())) return false;
    const Json* j = w.moduleState("caravans");
    return j && j->is_object() && j->value("on", false);
}

std::vector<std::string> checkCaravans(const Content& c) {
    const Rules& r = rules(c);
    std::vector<std::string> out = r.errors;
    if (!r.loaded) return out;
    for (const auto& [id, g] : r.goods)
        if (!c.items.count(id)) out.push_back("caravans.json: unknown item '" + id + "'");
    if (r.kinds.empty()) out.push_back("caravans.json: no kinds");
    return out;
}

std::vector<std::string> caravanLocKeys() {
    std::vector<std::string> k;
    for (const char* c : {"robbed", "fought_off", "turned_back", "leaves_village"}) k.push_back(std::string("chronicle.caravan.") + c);
    for (const char* kind : {"peddler", "caravan", "supply_train"}) {
        k.push_back(std::string("chronicle.caravan.") + kind + ".arrives_village");
        k.push_back(std::string("caravan.kind.") + kind + ".name");
    }
    for (const char* o : {"caravan_guard", "caravan_hand"}) {
        k.push_back(std::string("trade.role.") + o);
        k.push_back(std::string("ui2d.sheet.occ.") + o);
        k.push_back(std::string("news.trade.") + o + ".m");
        k.push_back(std::string("news.trade.") + o + ".f");
    }
    return k;
}

double caravanPrice(const World& w, const Settlement& p, const std::string& good) {
    const Rules& r = rules(w.content());
    auto it = r.goods.find(good);
    if (it == r.goods.end()) return 0.0;
    const CaravanState s = loadCaravans(w);
    return priceOf(w, s, r, p, it->second);
}
double caravanStock(const World& w, const Settlement& p, const std::string& good) {
    const Rules& r = rules(w.content());
    auto it = r.goods.find(good);
    if (it == r.goods.end()) return 0.0;
    const CaravanState s = loadCaravans(w);
    return stockOf(w, s, r, p, it->second);
}

void caravansDaily(World& w) {
    RealmState rs = loadRealm(w);
    daily(w, rs);
    saveRealm(w, rs);
}

Vec2 ventureKm(const World& w, const Venture& v) {
    const Settlement* a = pol(w, v.from);
    const Settlement* b = pol(w, v.to);
    if (!a || !b) return a ? a->km : (b ? b->km : Vec2{0, 0});
    if (v.state != "road") return b->km;
    const double t = std::clamp(static_cast<double>(w.day() - v.depart) / std::max(1, v.arrive - v.depart), 0.0, 1.0);
    return {a->km.x + (b->km.x - a->km.x) * t, a->km.y + (b->km.y - a->km.y) * t};
}

Json caravansJson(const World& w) {
    const CaravanState s = loadCaravans(w);
    Json out = Json::array();
    for (const auto& v : s.ventures) {
        Json j = v;
        const Vec2 km = ventureKm(w, v);
        j["km"] = Json::array({km.x, km.y});
        Json cargo = Json::object();
        if (const Character* l = w.person(v.leader))
            for (const auto& [id, n] : l->inventory)
                if (n > 0 && rules(w.content()).goods.count(id)) cargo[id] = n;
        j["cargo"] = cargo;
        out.push_back(j);
    }
    return out;
}

bool caravanAmbush(World& w, int id, EntityId campId) {
    const Rules& r = rules(w.content());
    CaravanState s = loadCaravans(w);
    RealmState rs = loadRealm(w);
    Ctx x{w, rs, s, r, w.day()};
    Settlement* camp = pol(w, campId);
    bool robbed = false;
    for (auto& v : s.ventures)
        if (v.id == id && camp) robbed = ambush(x, v, *camp);
    saveCaravans(w, s);
    saveRealm(w, rs);
    return robbed;
}

int caravanSetOut(World& w, EntityId leader, EntityId to) {
    const Rules& r = rules(w.content());
    CaravanState s = loadCaravans(w);
    RealmState rs = loadRealm(w);
    Ctx x{w, rs, s, r, w.day()};
    Character* l = w.person(leader);
    Settlement* home = l ? pol(w, l->status.settlement) : nullptr;
    int id = 0;
    if (l && home && !onMap(w, *l)) {
        for (auto& [sid, p] : w.settlements()) rollDeposits(x, p);
        l->occupation = "trader";
        id = setOut(x, *l, *home, to);
    }
    saveCaravans(w, s);
    saveRealm(w, rs);
    return id;
}

void installCaravans(World& w) {
    w.addCommandHandler(command);
    Json& j = w.moduleState("caravans");
    if (!j.is_object() || !j.value("on", false)) {
        CaravanState s = loadCaravans(w);
        s.on = true;
        saveCaravans(w, s);
    }
    static const bool once = [] {
        addRealmHook(daily);
        return true;
    }();
    (void)once;
}

} // namespace lr::sim
