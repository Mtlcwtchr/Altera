#include "sim/trade.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/craft/craft.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/gear.hpp"
#include "sim/social_chain.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> strs(const Json& j) {
    std::vector<std::string> v;
    if (j.is_array())
        for (const auto& s : j)
            if (s.is_string()) v.push_back(s.get<std::string>());
    return v;
}

std::map<std::string, double> nums(const Json& j) {
    std::map<std::string, double> m;
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_number()) m[k] = v.get<double>();
    return m;
}

Goods counts(const Json& j) {
    Goods g;
    if (j.is_object())
        for (const auto& [k, v] : j.items())
            if (v.is_number_integer() && v.get<int>() > 0) g[k] = v.get<int>();
    return g;
}

TradeRules parse(const Json& j) {
    TradeRules r;
    r.cfg = nums(j.value("config", Json::object()));
    r.values = nums(j.value("values", Json::object()));
    r.tagValues = nums(j.value("tag_values", Json::object()));
    const Json cats = j.value("categories", Json::object());
    for (const auto& [id, cj] : cats.items()) {
        TasteCategory t;
        t.tags = strs(cj.value("tags", Json::array()));
        t.materials = strs(cj.value("materials", Json::array()));
        t.items = strs(cj.value("items", Json::array()));
        t.minValue = cj.value("min_value", -1.0);
        t.minQuality = cj.value("min_quality", -1.0);
        r.categories[id] = t;
    }
    r.seedCategories = strs(j.value("seed_categories", Json::array()));
    r.seedLikes = j.value("seed_likes", 1);
    r.seedDislikes = j.value("seed_dislikes", 1);
    const Json tastes = j.value("trait_taste", Json::object());
    for (const auto& [trait, tj] : tastes.items()) {
        for (const auto& cat : strs(tj.value("like", Json::array()))) r.traitTaste[trait][cat] += 1.0;
        for (const auto& cat : strs(tj.value("dislike", Json::array()))) r.traitTaste[trait][cat] -= 1.0;
    }
    r.traitKeep = nums(j.value("trait_keep", Json::object()));
    const Json rolesJ = j.value("roles", Json::object());
    for (const auto& [id, rj] : rolesJ.items()) {
        TradeRole role;
        role.id = id;
        role.keep = rj.value("keep", role.keep);
        role.taste = rj.value("taste", role.taste);
        role.ignoresNeed = rj.value("ignores_need", false);
        role.sellsTags = strs(rj.value("sells_tags", Json::array()));
        role.pickTraits = strs(rj.value("pick_traits", Json::array()));
        role.stock = counts(rj.value("stock", Json::object()));
        role.restockTags = strs(rj.value("restock_tags", Json::array()));
        role.restockMax = rj.value("restock_max", 0);
        role.autoCount = rj.value("auto", 0);
        r.roles[id] = role;
    }
    const Json actsJ = j.value("acts", Json::object());
    for (const auto& [act, mode] : actsJ.items())
        if (mode.is_string()) r.acts[act] = mode.get<std::string>();
    r.autoAssign = j.value("auto_assign", false);
    const Json purse = j.value("purse", Json::array());
    if (purse.is_array() && purse.size() == 2) {
        r.purseLo = purse[0].get<int>();
        r.purseHi = purse[1].get<int>();
    }
    return r;
}

bool hasTag(const Content& c, const std::string& item, const std::string& tag) {
    auto it = c.items.find(item);
    if (it == c.items.end()) return false;
    return std::find(it->second.tags.begin(), it->second.tags.end(), tag) != it->second.tags.end();
}

bool anyTag(const Content& c, const std::string& item, const std::vector<std::string>& tags) {
    for (const auto& t : tags)
        if (hasTag(c, item, t)) return true;
    return false;
}

bool isMoney(const Content& c, const std::string& item) { return hasTag(c, item, "money"); }
bool edible(const Content& c, const std::string& item) {
    return hasTag(c, item, "food") || hasTag(c, item, "drink");
}

double roundTo(double v, double step = 0.1) { return std::round(v / step) * step; }

// Living people only (not beasts, not the dead).
bool person(const Character* c) { return c && c->body.alive && !c->isAnimal(); }

// How many of an item there are in the world: carried by the living, kept in stores.
int worldCount(const World& w, const std::string& item) {
    int n = 0;
    for (const auto& [id, c] : w.characters())
        if (c.body.alive) n += c.has(item);
    for (const auto& [id, o] : w.objects()) {
        auto it = o.stock.find(item);
        if (it != o.stock.end() && it->second > 0) n += it->second;
    }
    return n;
}

const Relation* relOf(const Character& of, EntityId to) {
    auto it = of.relations.find(to);
    return it == of.relations.end() ? nullptr : &it->second;
}

void addAxis(Character& of, EntityId to, const std::string& axis, double d) {
    Relation& r = of.relations[to];
    double* p = r.axis(axis);
    if (!p) return;
    *p = axis == "familiarity" ? clamp01(*p + d) : std::clamp(*p + d, -1.0, 1.0);
}

// The margin `responder` keeps on what it parts with when dealing with `offerer`.
double keepOf(const TradeRules& R, const World& w, const Character& responder, const Character& offerer) {
    const TradeRole* role = roleOf(w, responder);
    double k = role ? role->keep : R.c("keep", 1.1);
    for (const auto& t : responder.traits) {
        auto it = R.traitKeep.find(t);
        if (it != R.traitKeep.end()) k *= it->second;
    }
    const Relation* rel = relOf(responder, offerer.id);
    const double aff = rel ? std::clamp(rel->affinity, -1.0, 1.0) : 0.0;
    k *= 1.0 - R.c("keep_affinity_k", 0.25) * aff;
    return std::max(0.5, k);
}

// The thing a side of a deal is about: the one worth most (coin last).
std::string mainItem(const Content& c, const Goods& g) {
    std::string best;
    double bv = -1.0;
    for (const auto& [it, n] : g) {
        const double v = isMoney(c, it) ? -0.5 : baseValue(c, it) * n;
        if (v > bv) {
            bv = v;
            best = it;
        }
    }
    return best;
}

int coins(const Content& c, const Goods& g) {
    int n = 0;
    for (const auto& [it, k] : g)
        if (isMoney(c, it)) n += k;
    return n;
}

std::string coinItem(const Content& c) {
    for (const auto& [id, d] : c.items)
        if (std::find(d.tags.begin(), d.tags.end(), "money") != d.tags.end()) return id;
    return "coin";
}

// Says a line of the deal: {give} / {get} are the offerer's sides, {price} coins.
void line(World& w, Character& speaker, Character& listener, const std::string& act, const std::string& intent,
          const std::string& give, const std::string& get, int price) {
    std::map<std::string, Ref> args;
    if (!give.empty()) args["give"] = Ref{"item", kNone, give, 0.0, {}};
    if (!get.empty()) args["get"] = Ref{"item", kNone, get, 0.0, {}};
    args["price"] = Ref{"num", kNone, "", static_cast<double>(price), {}};
    say(w, speaker, listener, act, std::nullopt, Epistemic{}, "", intent, args);
}

void moveItem(World& w, Character& from, Character* to, const std::string& item, int n) {
    if (n <= 0) return;
    const int had = from.has(item);
    const int k = std::min(n, had);
    if (k <= 0) return;
    if (had - k <= 0) from.inventory.erase(item);
    else from.inventory[item] = had - k;
    // made things (with their quality) go along with the count
    auto& items = from.craft.items;
    for (int i = 0; i < k; ++i) {
        auto best = items.end();
        for (auto it = items.begin(); it != items.end(); ++it)
            if (it->form == item && (best == items.end() || it->q > best->q)) best = it;
        if (best == items.end()) break;
        if (to) to->craft.items.push_back(*best);
        items.erase(best);
    }
    pruneGear(from);
    if (to) to->inventory[item] += k;
    (void)w;
}

void eat(const Content& c, Character& who, const std::string& item) {
    auto it = c.items.find(item);
    if (it == c.items.end()) return;
    if (hasTag(c, item, "food") && it->second.nutrition > 0) who.needs["hunger"] = clamp01(who.need("hunger") + it->second.nutrition);
    if (hasTag(c, item, "drink") && who.needs.count("thirst")) who.needs["thirst"] = clamp01(who.need("thirst") + 0.35);
}

Json rounds(Character& r, EntityId with) {
    Json& all = r.deal["rounds"];
    if (!all.is_object()) all = Json::object();
    return all.value(std::to_string(with), Json::array());
}

void noteRound(const TradeRules& R, World& w, Character& r, EntityId with) {
    const Tick window = static_cast<Tick>(R.c("rounds_window_ticks", 24));
    Json kept = Json::array();
    for (const auto& t : rounds(r, with))
        if (t.is_number_integer() && w.now() - t.get<Tick>() <= window) kept.push_back(t);
    kept.push_back(w.now());
    r.deal["rounds"][std::to_string(with)] = kept;
}

int recentRounds(const TradeRules& R, const World& w, const Character& r, EntityId with) {
    if (!r.deal.is_object() || !r.deal.contains("rounds")) return 0;
    const Json& all = r.deal["rounds"];
    const std::string key = std::to_string(with);
    if (!all.is_object() || !all.contains(key)) return 0;
    const Tick window = static_cast<Tick>(R.c("rounds_window_ticks", 24));
    int n = 0;
    for (const auto& t : all[key])
        if (t.is_number_integer() && w.now() - t.get<Tick>() <= window) ++n;
    return n;
}

bool trades(const std::string& mode) { return mode == "buy" || mode == "sell" || mode == "barter" || mode == "meal"; }

std::string tasteWord(double t) { return t > 0.2 ? "like" : (t < -0.2 ? "dislike" : "neutral"); }

} // namespace

// ---------------------------------------------------------------------------

const TradeRules* tradeRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<TradeRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<TradeRules> r;
    const fs::path p = fs::path(c.dir) / "trade.json";
    if (fs::exists(p)) {
        std::ifstream f(p, std::ios::binary);
        r = std::make_unique<TradeRules>(parse(Json::parse(f)));
        for (const auto& [act, mode] : r->acts)
            if (!c.interactions.count(act)) throw std::runtime_error("trade.json: unknown act '" + act + "'");
    }
    const TradeRules* out = r.get();
    cache[c.dir] = std::move(r);
    return out;
}

std::vector<std::string> itemCategories(const Content& c, const std::string& item) {
    std::vector<std::string> out;
    const TradeRules* R = tradeRules(c);
    if (!R) return out;
    auto it = c.items.find(item);
    const ItemDef* d = it == c.items.end() ? nullptr : &it->second;
    for (const auto& [id, cat] : R->categories) {
        bool in = std::find(cat.items.begin(), cat.items.end(), item) != cat.items.end();
        if (!in && d) {
            for (const auto& t : d->tags)
                if (std::find(cat.tags.begin(), cat.tags.end(), t) != cat.tags.end()) in = true;
            if (!d->material.empty() &&
                std::find(cat.materials.begin(), cat.materials.end(), d->material) != cat.materials.end())
                in = true;
        }
        if (!in && cat.minValue > 0 && baseValue(c, item) >= cat.minValue) in = true;
        if (!in && d && cat.minQuality > 0 && d->quality >= cat.minQuality) in = true;
        if (in) out.push_back(id);
    }
    return out;
}

double baseValue(const Content& c, const std::string& item) {
    const TradeRules* R = tradeRules(c);
    auto def = c.items.find(item);
    if (def != c.items.end() && def->second.value > 0) return def->second.value; // AL-74: the item's own price
    if (!R) return 1.0;
    auto v = R->values.find(item);
    if (v != R->values.end()) return v->second;
    double best = R->c("default_value", 1.0);
    auto it = c.items.find(item);
    if (it == c.items.end()) return best;
    bool tagged = false;
    for (const auto& t : it->second.tags) {
        auto tv = R->tagValues.find(t);
        if (tv == R->tagValues.end()) continue;
        best = tagged ? std::max(best, tv->second) : tv->second;
        tagged = true;
    }
    if (hasTag(c, item, "food")) best += it->second.nutrition * R->c("nutrition_value", 4.0);
    return std::max(0.1, best);
}

double itemQuality(const Character& holder, const std::string& item) {
    double q = -1.0;
    for (const auto& it : holder.craft.items)
        if (it.form == item) q = std::max(q, it.q);
    return q < 0 ? 0.5 : q;
}

double itemWorthMult(const Content& c, const Character& holder, const std::string& item) {
    // the made thing that would change hands (moveItem gives the best one first)
    const craft::Item* best = nullptr;
    for (const auto& it : holder.craft.items)
        if (it.form == item && (!best || it.q > best->q)) best = &it;
    if (best) return craft::itemWorth(c.craft, *best);
    auto def = c.items.find(item);
    if (def == c.items.end()) return 1.0;
    return craft::tagsWorth(c.craft, def->second.tags);
}

std::map<std::string, double> tasteOf(const World& w, const Character& c) {
    std::map<std::string, double> t;
    const TradeRules* R = tradeRules(w.content());
    if (!R || c.isAnimal()) return t;
    for (const auto& tr : c.traits) {
        auto it = R->traitTaste.find(tr);
        if (it == R->traitTaste.end()) continue;
        for (const auto& [cat, v] : it->second) t[cat] += v;
    }
    // the person's own taste: fixed by the world's seed and the person, not by the tick
    std::vector<std::string> pool = R->seedCategories;
    Rng r = agentRng(w.seed(), c.id, 0, "trade.taste");
    auto pick = [&](int n, double v) {
        for (int i = 0; i < n && !pool.empty(); ++i) {
            const size_t k = static_cast<size_t>(r.next() % pool.size());
            t[pool[k]] += v;
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(k));
        }
    };
    pick(R->seedLikes, 1.0);
    pick(R->seedDislikes, -1.0);
    for (auto& [k, v] : t) v = std::clamp(v, -1.0, 1.0);
    return t;
}

double itemTaste(const World& w, const Character& c, const std::string& item) {
    const auto t = tasteOf(w, c);
    double s = 0.0;
    for (const auto& cat : itemCategories(w.content(), item)) {
        auto it = t.find(cat);
        if (it != t.end()) s += it->second;
    }
    return std::clamp(s, -1.0, 1.0);
}

double worthTo(const World& w, const Character& judge, const std::string& item, const Character& holder) {
    const Content& C = w.content();
    const TradeRules* R = tradeRules(C);
    double v = baseValue(C, item);
    if (!R || isMoney(C, item)) return v;
    v *= R->c("quality_lo", 0.6) + (R->c("quality_hi", 1.4) - R->c("quality_lo", 0.6)) * itemQuality(holder, item);
    v *= std::pow(itemWorthMult(C, holder, item), R->c("worth_exp", 1.0)); // AL-87: what it is made of
    const double common = std::max(1.0, R->c("rarity_common", 30));
    v *= 1.0 + R->c("rarity_k", 0.5) * clamp01(1.0 - worldCount(w, item) / common);
    const TradeRole* role = roleOf(w, judge);
    if (!role || !role->ignoresNeed) {
        const double full = std::max(0.05, R->c("need_full", 0.7));
        if (hasTag(C, item, "food"))
            v *= 1.0 + R->c("food_need_k", 0.8) * clamp01((full - judge.need("hunger")) / full);
        if (hasTag(C, item, "drink") && judge.needs.count("thirst"))
            v *= 1.0 + R->c("drink_need_k", 0.6) * clamp01((full - judge.need("thirst")) / full);
        const int has = judge.id == holder.id ? std::max(0, judge.has(item) - 1) : judge.has(item);
        if (has == 0) v *= 1.0 + R->c("lack_bonus", 0.15);
        else if (anyTag(C, item, {"tool", "weapon", "clothing", "armour"})) v *= R->c("tool_spare_mult", 0.6);
        const int over = has - static_cast<int>(R->c("saturation_free", 4));
        if (over > 0) v /= 1.0 + R->c("saturation_k", 0.12) * over;
    }
    const double tw = role ? role->taste : 1.0;
    v *= 1.0 + R->c("taste_k", 0.4) * tw * itemTaste(w, judge, item);
    return std::max(0.05, v);
}

int spare(const Character& c, const std::string& item) { return std::max(0, c.has(item) - wornCount(c, item)); }

const TradeRole* roleOf(const World& w, const Character& c) {
    if (c.occupation.empty()) return nullptr;
    const TradeRules* R = tradeRules(w.content());
    if (!R) return nullptr;
    auto it = R->roles.find(c.occupation);
    return it == R->roles.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------

DealEval weighDeal(const World& w, const Character& A, const Character& Rp, const Offer& o) {
    DealEval e;
    const Content& C = w.content();
    const TradeRules* R = tradeRules(C);
    auto refuse = [&](const std::string& why) {
        e.verdict = "refuse";
        e.reason = why;
        return e;
    };
    if (!R) return refuse("nothing");
    if (A.id == Rp.id) return refuse("self");
    if (!person(&Rp) || Rp.body.downUntil > w.now() || Rp.captivity.held()) return refuse("busy");
    const bool giving = o.mode == "gift" || o.mode == "treat";
    if (!giving && Rp.body.stage() == "child") return refuse("child");
    if (o.give.empty() && o.take.empty()) return refuse("nothing");
    for (const auto& [it, n] : o.give)
        if (spare(A, it) < n) return refuse("no_means");
    for (const auto& [it, n] : o.take)
        if (spare(Rp, it) < n) return refuse("no_goods");
    // what the responder gets and gives, as it sees them
    double tasteW = 0.0, tasteSum = 0.0;
    for (const auto& [it, n] : o.give) {
        DealLine l{it, n, worthTo(w, Rp, it, A), itemTaste(w, Rp, it)};
        e.gain += l.unit * n;
        tasteSum += l.taste * l.unit * n;
        tasteW += l.unit * n;
        e.gets.push_back(l);
    }
    for (const auto& [it, n] : o.take) {
        DealLine l{it, n, worthTo(w, Rp, it, Rp), itemTaste(w, Rp, it)};
        e.loss += l.unit * n;
        e.gives.push_back(l);
    }
    e.taste = tasteW > 0 ? tasteSum / tasteW : 0.0;
    const Relation* rel = relOf(Rp, A.id);
    if (rel && rel->enmity > R->c("enmity_above", 0.5)) return refuse("enmity");
    if (!giving && rel && rel->trust < R->c("distrust_below", -0.4)) return refuse("distrust");
    if (!giving && recentRounds(*R, w, Rp, A.id) >= static_cast<int>(R->c("max_rounds", 3))) return refuse("tired");
    if (o.mode == "treat") {
        for (const auto& [it, n] : o.give)
            if (!edible(C, it)) return refuse("nothing");
        bool food = false, drink = false;
        for (const auto& [it, n] : o.give) {
            food = food || hasTag(C, it, "food");
            drink = drink || hasTag(C, it, "drink");
        }
        if (food && !drink && Rp.need("hunger") >= R->c("full_hunger", 0.95)) return refuse("full");
        if (e.taste <= -0.5) return refuse("dislike");
    }
    if (o.mode == "meal")
        for (const auto& [it, n] : o.take)
            if (!edible(C, it)) return refuse("nothing");
    // room for what each side carries off (a treat and a meal are eaten; a trader or a
    // tavernkeeper keeps the wares at the stall, not in the pack)
    if (const GearRules* G = gearRules(C)) {
        if (o.mode != "treat" && !roleOf(w, Rp) && !o.give.empty() && !canTake(*G, Rp, o.give).empty())
            return refuse("no_room");
        if (o.mode != "meal" && !roleOf(w, A) && !o.take.empty() && !canTake(*G, A, o.take).empty())
            return refuse("no_room");
    }
    if (giving || o.take.empty()) {
        e.verdict = "accept";
        e.ratio = e.gain > 0 ? 99.0 : 0.0;
        return e;
    }
    e.keep = keepOf(*R, w, Rp, A);
    const double need = e.loss * e.keep;
    e.ratio = need > 0 ? e.gain / need : 99.0;
    if (e.ratio >= 1.0) {
        e.verdict = "accept";
        if (e.ratio >= R->c("generous_ratio", 1.4)) e.reason = "generous";
        return e;
    }
    // a lowball for a thing it dislikes: that is the reason it says
    const std::string low = e.ratio < R->c("insult_below", 0.3) ? "insult" : (e.taste <= -0.5 ? "dislike" : "too_little");
    if (e.ratio < R->c("counter_min", 0.6)) return refuse(low);
    // the counter: more coin from the offerer, else fewer of the goods
    const std::string coin = coinItem(C);
    const int extra = static_cast<int>(std::ceil(need - e.gain - 1e-9));
    const int has = spare(A, coin) - (o.give.count(coin) ? o.give.at(coin) : 0);
    if (extra > 0 && has >= extra) {
        e.counter = o;
        e.counter.give[coin] += extra;
        e.verdict = "counter";
        e.reason = "coin";
        return e;
    }
    Offer less = o;
    for (int guard = 0; guard < 64; ++guard) {
        // drop one of the dearest thing asked while more than one is asked
        std::string pick;
        double pv = -1.0;
        for (const auto& l : e.gives)
            if (less.take.count(l.item) && less.take.at(l.item) > 0 && l.unit > pv) {
                pv = l.unit;
                pick = l.item;
            }
        if (pick.empty()) break;
        if (--less.take[pick] <= 0) less.take.erase(pick);
        if (less.take.empty()) break;
        double loss = 0.0;
        for (const auto& l : e.gives)
            if (less.take.count(l.item)) loss += l.unit * less.take.at(l.item);
        if (e.gain >= loss * e.keep) {
            e.counter = less;
            e.verdict = "counter";
            e.reason = "less";
            return e;
        }
    }
    return refuse(low);
}

Offer defaultOffer(const World& w, const Character& A, const Character& Rp, const std::string& mode) {
    Offer o;
    o.mode = mode;
    const Content& C = w.content();
    const TradeRules* R = tradeRules(C);
    if (!R) return o;
    const std::string coin = coinItem(C);
    const double keepR = keepOf(*R, w, Rp, A);
    const TradeRole* rrole = roleOf(w, Rp);
    if (mode == "buy" || mode == "meal") {
        const bool foodOnly = mode == "meal" || A.need("hunger") < 0.6 ||
                              (rrole && !rrole->sellsTags.empty());
        std::string best;
        int bestPrice = 0;
        double bestScore = -1.0;
        for (const auto& [it, n] : Rp.inventory) {
            if (isMoney(C, it) || spare(Rp, it) <= 0) continue;
            if (mode == "meal" && !hasTag(C, it, "food")) continue;
            if (foodOnly && !edible(C, it)) continue;
            if (rrole && !rrole->sellsTags.empty() && !anyTag(C, it, rrole->sellsTags)) continue;
            const int price = std::max(1, static_cast<int>(std::ceil(worthTo(w, Rp, it, Rp) * keepR - 1e-9)));
            if (spare(A, coin) < price) continue;
            const double score = worthTo(w, A, it, Rp) * (1.0 + 0.5 * itemTaste(w, A, it)) / price;
            if (score > bestScore) {
                bestScore = score;
                best = it;
                bestPrice = price;
            }
        }
        if (!best.empty()) {
            o.give[coin] = bestPrice;
            o.take[best] = 1;
        }
        return o;
    }
    if (mode == "sell") {
        std::string best;
        double bestScore = -1.0;
        int bestPrice = 0;
        for (const auto& [it, n] : A.inventory) {
            if (isMoney(C, it) || spare(A, it) <= 0) continue;
            const int price = static_cast<int>(std::floor(worthTo(w, Rp, it, A) / keepR + 1e-9));
            if (price < 1 || spare(Rp, coin) < price) continue;
            const double score = price / worthTo(w, A, it, A);
            if (score > bestScore) {
                bestScore = score;
                best = it;
                bestPrice = price;
            }
        }
        if (!best.empty()) {
            o.give[best] = 1;
            o.take[coin] = bestPrice;
        }
        return o;
    }
    if (mode == "barter") {
        std::string want;
        double wv = -1.0;
        for (const auto& [it, n] : Rp.inventory) {
            if (isMoney(C, it) || spare(Rp, it) <= 0) continue;
            const double v = worthTo(w, A, it, Rp);
            if (v > wv) {
                wv = v;
                want = it;
            }
        }
        if (want.empty()) return o;
        const double ask = worthTo(w, Rp, want, Rp) * keepR;
        std::string give;
        double gv = 1e18, top = -1.0;
        std::string topItem;
        for (const auto& [it, n] : A.inventory) {
            if (isMoney(C, it) || spare(A, it) <= 0 || it == want) continue;
            const double v = worthTo(w, Rp, it, A);
            if (v >= ask && v < gv) {
                gv = v;
                give = it;
            }
            if (v > top) {
                top = v;
                topItem = it;
            }
        }
        if (give.empty()) give = topItem;
        if (give.empty()) return o;
        o.give[give] = 1;
        o.take[want] = 1;
        return o;
    }
    if (mode == "gift" || mode == "treat") {
        std::string best;
        double bt = -2.0, bv = -1.0;
        for (const auto& [it, n] : A.inventory) {
            if (isMoney(C, it) || spare(A, it) <= 0) continue;
            if (mode == "treat" && !edible(C, it)) continue;
            const double t = itemTaste(w, Rp, it), v = worthTo(w, Rp, it, A);
            if (t > bt + 1e-9 || (std::abs(t - bt) <= 1e-9 && v > bv)) {
                bt = t;
                bv = v;
                best = it;
            }
        }
        if (!best.empty()) o.give[best] = 1;
        return o;
    }
    return o;
}

// ---------------------------------------------------------------------------

namespace {

// Carries the deal out: the goods change hands, what is eaten is eaten, the
// relations move, the trace and the lines say so.
void carryOut(World& w, Character& A, Character& Rp, const Offer& o, const DealEval& e, bool counterTaken) {
    const Content& C = w.content();
    const TradeRules& R = *tradeRules(C);
    for (const auto& [it, n] : o.give) {
        if (o.mode == "treat") {
            moveItem(w, A, nullptr, it, n);
            for (int i = 0; i < n; ++i) eat(C, Rp, it);
        } else {
            moveItem(w, A, &Rp, it, n);
        }
    }
    for (const auto& [it, n] : o.take) {
        if (o.mode == "meal" && edible(C, it)) {
            moveItem(w, Rp, nullptr, it, n);
            for (int i = 0; i < n; ++i) eat(C, A, it);
        } else {
            moveItem(w, Rp, &A, it, n);
        }
    }
    const std::string give = mainItem(C, o.give), get = mainItem(C, o.take);
    const Json done = {{"give", give}, {"get", get}, {"gn", give.empty() ? 0 : o.give.at(give)},
                       {"tn", get.empty() ? 0 : o.take.at(get)}, {"price", coins(C, o.give) + coins(C, o.take)}};
    if (o.mode == "gift" || o.mode == "treat") {
        const std::string word = tasteWord(e.taste);
        double delta = std::min(R.c("gift_aff_max", 0.15), R.c("gift_aff_per_coin", 0.02) * e.gain);
        if (word == "like") delta *= 1.0 + e.taste;
        if (word == "dislike") delta = -R.c("dislike_aff", 0.06) * std::abs(e.taste);
        addAxis(Rp, A.id, "affinity", delta);
        if (delta > 0) addAxis(Rp, A.id, "trust", delta * R.c("gift_trust_share", 0.4));
        addAxis(Rp, A.id, "familiarity", o.mode == "treat" ? R.c("treat_familiarity", 0.03) : 0.01);
        addAxis(A, Rp.id, "familiarity", 0.01);
        const auto& axes = C.config.reputationAxes;
        if (delta > 0 && std::find(axes.begin(), axes.end(), "generosity") != axes.end())
            A.reputation["generosity"] = std::clamp(A.reputation["generosity"] + R.c("generosity_rep", 0.01), -1.0, 1.0);
        Json p = done;
        p["target"] = Rp.id;
        p["who"] = Rp.id;
        p["delta"] = roundTo(delta, 0.01);
        trace(w, A, 'E', "trade." + o.mode + "_" + word, p);
        p["target"] = A.id;
        trace(w, Rp, 'E', "trade." + o.mode + "_" + word, p);
        const std::string intent = "trade." + o.mode + "." + word;
        line(w, Rp, A, word == "dislike" ? "accept" : "thank", intent, give, get, 0);
        return;
    }
    // a trade: fair dealing builds a little trust both ways
    addAxis(Rp, A.id, "trust", R.c("fair_trust", 0.01));
    addAxis(A, Rp.id, "trust", R.c("fair_trust", 0.01));
    addAxis(Rp, A.id, "familiarity", 0.01);
    addAxis(A, Rp.id, "familiarity", 0.01);
    if (e.reason == "generous") addAxis(Rp, A.id, "affinity", R.c("generous_aff", 0.02));
    Json p = done;
    p["target"] = Rp.id;
    trace(w, A, 'E', "trade.done", p);
    p["target"] = A.id;
    trace(w, Rp, 'E', "trade.done", p);
    const int price = coins(C, o.give) + coins(C, o.take);
    if (counterTaken) {
        line(w, A, Rp, "accept", "trade.accept.counter", give, get, price);
        line(w, Rp, A, "accept", "trade.accept", give, get, price);
    } else if (o.mode == "meal") {
        line(w, Rp, A, "accept", "trade.accept.meal", give, get, price);
    } else {
        line(w, Rp, A, "accept", e.reason == "generous" ? "trade.accept.generous" : "trade.accept", give, get, price);
        line(w, A, Rp, "thank", "trade.done", give, get, price);
    }
}

// How the NPC offerer sees the counter: takes it when it gets at least what it gives.
bool npcTakesCounter(const World& w, const Character& A, const Character& Rp, const Offer& c) {
    double gain = 0.0, loss = 0.0;
    for (const auto& [it, n] : c.take) gain += worthTo(w, A, it, Rp) * n;
    for (const auto& [it, n] : c.give) loss += worthTo(w, A, it, A) * n;
    for (const auto& [it, n] : c.give)
        if (spare(A, it) < n) return false;
    return gain >= loss;
}

Json weighJson(const Character& Rp, const DealEval& e) {
    return {{"who", Rp.id},           {"gain", roundTo(e.gain)},  {"loss", roundTo(e.loss)},
            {"keep", roundTo(e.keep, 0.01)}, {"ratio", roundTo(e.ratio, 0.01)}, {"verdict", e.verdict}};
}

} // namespace

DealResult makeOffer(World& w, Character& A, Character& Rp, const Offer& o) {
    DealResult out;
    const Content& C = w.content();
    const TradeRules* R = tradeRules(C);
    if (!R) return out;
    const std::string give = mainItem(C, o.give), get = mainItem(C, o.take);
    const int price = o.mode == "sell" ? coins(C, o.take) : coins(C, o.give);
    // the offer, aloud and in the trace of both
    line(w, A, Rp, "offer", "trade.offer." + o.mode, give, get, price);
    const Json offered = {{"mode", o.mode},
                          {"give", give},
                          {"get", get},
                          {"gn", give.empty() ? 0 : o.give.at(give)},
                          {"tn", get.empty() ? 0 : o.take.at(get)},
                          {"price", price},
                          {"goods_give", goodsJson(o.give)},
                          {"goods_take", goodsJson(o.take)}};
    Json p = offered;
    p["target"] = Rp.id;
    trace(w, A, 'E', get.empty() ? "trade.offer_gift" : "trade.offer", p);
    p["target"] = A.id;
    p["from"] = A.id;
    trace(w, Rp, 'E', get.empty() ? "trade.offered_gift" : "trade.offered", p);
    out.eval = weighDeal(w, A, Rp, o);
    DealEval& e = out.eval;
    if (trades(o.mode)) {
        const Json wj = weighJson(Rp, e);
        trace(w, A, 'E', "trade.weigh", wj);
        trace(w, Rp, 'E', "trade.weigh", wj);
        noteRound(*R, w, Rp, A.id);
    }
    if (e.verdict == "accept") {
        carryOut(w, A, Rp, o, e, false);
        out.done = true;
        Rp.deal.erase("counter");
        if (Rp.deal.contains("rounds")) Rp.deal["rounds"].erase(std::to_string(A.id)); // a deal made: haggling starts over
        return out;
    }
    if (e.verdict == "counter") {
        const int extra = coins(C, e.counter.give) - coins(C, o.give);
        const std::string cget = mainItem(C, e.counter.take);
        Rp.deal["counter"] = {{"from", A.id},
                              {"mode", e.counter.mode},
                              {"give", goodsJson(e.counter.give)},
                              {"take", goodsJson(e.counter.take)},
                              {"tick", w.now()}};
        line(w, Rp, A, "bargain", e.reason == "coin" ? "trade.counter.coin" : "trade.counter.less", give, cget,
             e.reason == "coin" ? extra : coins(C, e.counter.give));
        const Json cj = {{"who", Rp.id},
                         {"target", Rp.id},
                         {"price", extra},
                         {"get", cget},
                         {"tn", cget.empty() ? 0 : e.counter.take.at(cget)},
                         {"how", e.reason}};
        trace(w, A, 'E', e.reason == "coin" ? "trade.countered_coin" : "trade.countered_less", cj);
        trace(w, Rp, 'E', e.reason == "coin" ? "trade.countered_coin" : "trade.countered_less", cj);
        if (!A.controller.isPlayer()) {
            std::string why;
            if (npcTakesCounter(w, A, Rp, e.counter) && acceptCounter(w, A, Rp, why)) {
                out.done = true;
            } else {
                line(w, A, Rp, "refuse", "trade.counter_no", give, get, 0);
                trace(w, A, 'E', "trade.counter_declined", {{"target", Rp.id}});
                Rp.deal.erase("counter");
            }
        }
        return out;
    }
    // refused: the reason aloud
    line(w, Rp, A, "refuse", "trade.refuse." + e.reason, give, get, price);
    if (e.reason == "insult") addAxis(Rp, A.id, "affinity", -R->c("insult_aff", 0.03));
    if (e.reason == "tired") addAxis(Rp, A.id, "affinity", -R->c("tired_aff", 0.02));
    const Json rj = {{"who", Rp.id}, {"target", Rp.id}, {"why", "trade_" + e.reason}};
    trace(w, A, 'E', "trade.refused", rj);
    trace(w, Rp, 'E', "trade.refused", rj);
    return out;
}

const Json* pendingCounter(const World& w, const Character& Rp, EntityId offerer) {
    if (!Rp.deal.is_object() || !Rp.deal.contains("counter")) return nullptr;
    const Json& c = Rp.deal["counter"];
    const TradeRules* R = tradeRules(w.content());
    const Tick ttl = R ? static_cast<Tick>(R->c("counter_ttl_ticks", 24)) : 24;
    if (c.value("from", kNone) != offerer) return nullptr;
    if (w.now() - c.value("tick", Tick{0}) > ttl) return nullptr;
    return &c;
}

bool acceptCounter(World& w, Character& A, Character& Rp, std::string& why) {
    const Json* pc = pendingCounter(w, Rp, A.id);
    if (!pc) return why = "trade_no_counter", false;
    Offer o;
    o.mode = pc->value("mode", std::string("barter"));
    o.give = goodsFrom(w.content(), pc->value("give", Json::object()));
    o.take = goodsFrom(w.content(), pc->value("take", Json::object()));
    Rp.deal.erase("counter");
    for (const auto& [it, n] : o.give)
        if (spare(A, it) < n) return why = "trade_no_means", false;
    for (const auto& [it, n] : o.take)
        if (spare(Rp, it) < n) return why = "trade_no_goods", false;
    DealEval e;
    e.verdict = "accept";
    for (const auto& [it, n] : o.give) e.gain += worthTo(w, Rp, it, A) * n;
    for (const auto& [it, n] : o.take) e.loss += worthTo(w, Rp, it, Rp) * n;
    carryOut(w, A, Rp, o, e, true);
    if (Rp.deal.contains("rounds")) Rp.deal["rounds"].erase(std::to_string(A.id));
    return true;
}

// ---------------------------------------------------------------------------

void setOccupation(World& w, Character& c, const std::string& role) {
    const TradeRules* R = tradeRules(w.content());
    if (role.empty()) {
        c.occupation.clear();
        return;
    }
    if (!R || !R->roles.count(role)) throw std::runtime_error("unknown trade role '" + role + "'");
    c.occupation = role;
    for (const auto& [it, n] : R->roles.at(role).stock)
        if (w.content().items.count(it)) c.inventory[it] += n;
    trace(w, c, 'E', "trade.role." + role, {{"who", c.id}});
}

std::string tradeMode(const Content& c, const std::string& act) {
    const TradeRules* R = tradeRules(c);
    if (!R) return "";
    auto it = R->acts.find(act);
    return it == R->acts.end() ? "" : it->second;
}

std::string tradeAct(const std::string& mode) { return "trade_" + mode; }

bool tradeOnAct(World& w, Character& c, const InteractionDef& d, Character* target) {
    const std::string mode = tradeMode(w.content(), d.id);
    if (mode.empty()) return false;
    Offer o;
    o.mode = mode;
    if (c.deal.is_object() && c.deal.contains("intent")) {
        const Json& in = c.deal["intent"];
        if (in.value("act", std::string()) == d.id && target && in.value("target", kNone) == target->id) {
            o.give = goodsFrom(w.content(), in.value("give", Json::object()));
            o.take = goodsFrom(w.content(), in.value("take", Json::object()));
        }
        c.deal.erase("intent");
    }
    if (!person(target)) {
        trace(w, c, 'E', "trade.nothing", {{"why", "trade_nothing"}});
        return true;
    }
    if (o.give.empty() && o.take.empty()) o = defaultOffer(w, c, *target, mode);
    if (o.give.empty() && o.take.empty()) {
        trace(w, c, 'E', "trade.nothing", {{"target", target->id}, {"why", "trade_nothing"}});
        line(w, *target, c, "refuse", "trade.refuse.nothing", "", "", 0);
        return true;
    }
    makeOffer(w, c, *target, o);
    return true;
}

bool startTrade(World& w, Character& c, Character& target, const Offer& o, bool player, std::string& why) {
    const std::string act = tradeAct(o.mode);
    const InteractionDef* d = w.content().interaction(act);
    if (!d || tradeMode(w.content(), act).empty()) return why = "unknown_mode", false;
    if (!person(&target) || target.id == c.id) return why = "wrong_target", false;
    c.deal["intent"] = {{"act", act}, {"target", target.id}, {"give", goodsJson(o.give)}, {"take", goodsJson(o.take)}};
    if (player) {
        if (c.ai.social.active() && c.ai.social.role != "follower") endSocial(w, c, "player_order");
        GoalScore gs;
        gs.goal = "player_action";
        gs.task = act;
        gs.target = target.id;
        gs.score = 0.7;
        if (!adoptGoal(w, c, gs, "player")) return why = "plan_failed:" + c.ai.lastFailReason, false;
        trace(w, c, 'C', "control.order", {{"goal", gs.goal}, {"task", gs.task}});
        c.controller.idleSince = w.now();
        return true;
    }
    const TradeRules* R = tradeRules(w.content());
    QueuedTask q{act,
                 target.id,
                 R ? R->c("npc_queue_priority", 0.75) : 0.75,
                 w.now(),
                 static_cast<Tick>(R ? R->c("npc_queue_ttl", 48) : 48),
                 "trade",
                 "trade"};
    c.ai.queue.push_back(q);
    trace(w, c, 'C', "control.queued", {{"task", act}});
    return true;
}

// ---------------------------------------------------------------------------

namespace {

bool queuedTrade(const World& w, const Character& c) {
    for (const auto& q : c.ai.queue)
        if (!tradeMode(w.content(), q.task).empty()) return true;
    return !tradeMode(w.content(), c.ai.goal.task).empty();
}

bool hasFood(const Content& C, const Character& c) {
    for (const auto& [it, n] : c.inventory)
        if (n > 0 && hasTag(C, it, "food") && spare(c, it) > 0) return true;
    return false;
}

void autoAssign(World& w, const TradeRules& R) {
    for (auto& [sid, s] : w.settlements()) {
        if (!s.full()) continue; // AL-92: beyond the map the realm counts it
        std::vector<Character*> people;
        bool anyRole = false;
        for (EntityId fid : s.families) {
            auto fit = w.families().find(fid);
            if (fit == w.families().end()) continue;
            for (EntityId id : fit->second.members) {
                Character* c = w.character(id);
                if (!person(c) || c->body.stage() == "child") continue;
                people.push_back(c);
                anyRole = anyRole || !c->occupation.empty();
            }
        }
        if (people.empty()) continue;
        std::sort(people.begin(), people.end(), [](const Character* a, const Character* b) { return a->id < b->id; });
        const bool first = !anyRole;
        for (const auto& [rid, role] : R.roles) {
            int have = 0;
            for (const Character* c : people) have += c->occupation == rid;
            for (; have < role.autoCount; ++have) {
                Character* best = nullptr;
                double bs = -1.0;
                for (Character* c : people) {
                    if (!c->occupation.empty() || c->controller.isPlayer()) continue;
                    double sc = 0.0;
                    for (const auto& t : role.pickTraits) sc += c->hasTrait(t) ? 1.0 : 0.0;
                    sc += 0.5 * agentRng(w.seed(), c->id, 0, "trade.role." + rid).uniform();
                    if (sc > bs) {
                        bs = sc;
                        best = c;
                    }
                }
                if (!best) break;
                setOccupation(w, *best, rid);
            }
        }
        if (first && R.purseHi > 0) {
            const std::string coin = coinItem(w.content());
            for (Character* c : people) {
                Rng r = agentRng(w.seed(), c->id, 0, "trade.purse");
                const int n = r.irange(R.purseLo, R.purseHi);
                if (n > 0) c->inventory[coin] += n;
            }
        }
    }
}

// The tavernkeeper fills its shelf from the household's store.
void restock(World& w, Character& c, const TradeRole& role) {
    if (role.restockTags.empty() || role.restockMax <= 0) return;
    WorldObject* store = w.object(c.home);
    if (!store) return;
    const Content& C = w.content();
    int carried = 0;
    for (const auto& [it, n] : c.inventory)
        if (anyTag(C, it, role.restockTags)) carried += n;
    int moved = 0;
    for (auto& [it, n] : store->stock) {
        if (carried >= role.restockMax) break;
        if (n <= 0 || !C.items.count(it) || !anyTag(C, it, role.restockTags)) continue;
        const int k = std::min(n, role.restockMax - carried);
        n -= k;
        c.inventory[it] += k;
        carried += k;
        moved += k;
    }
    if (moved > 0) trace(w, c, 'E', "trade.restock", {{"n", moved}, {"object", store->id}});
}

} // namespace

void tradeTick(World& w) {
    const TradeRules* R = tradeRules(w.content());
    if (!R) return;
    const Tick now = w.now();
    if (R->autoAssign && (now == 1 || (now > 0 && now % kTicksPerDay == 0))) autoAssign(w, *R);
    const Tick period = std::max<Tick>(1, static_cast<Tick>(R->c("npc_period_ticks", 6)));
    if (now == 0 || now % period != 0) return;
    const Content& C = w.content();
    const double range = R->c("range_m", 6.0);
    const std::string coin = coinItem(C);
    // the tavernkeepers by settlement
    std::vector<Character*> keepers;
    for (auto& [id, c] : w.characters())
        if (person(&c) && c.occupation == "tavernkeeper") keepers.push_back(&c);
    for (Character* k : keepers) {
        const TradeRole* role = roleOf(w, *k);
        if (!role) continue;
        if (now % kTicksPerDay == 0) restock(w, *k, *role);
        if (k->controller.isPlayer() || k->ai.social.active() || queuedTrade(w, *k)) continue;
        Rng r = w.rng(k->id, "trade.treat");
        if (!r.chance(R->c("npc_treat_chance", 0.15))) continue;
        // a friend within reach who could eat
        Character* friendP = nullptr;
        for (const auto& [oid, rel] : k->relations) {
            Character* o = w.character(oid);
            if (!person(o) || o->id == k->id || rel.affinity < R->c("npc_treat_affinity", 0.3)) continue;
            if (dist(o->pos, k->pos) > range || o->need("hunger") > 0.8) continue;
            friendP = o;
            break;
        }
        if (!friendP) continue;
        const Offer o = defaultOffer(w, *k, *friendP, "treat");
        std::string why;
        if (!o.give.empty()) startTrade(w, *k, *friendP, o, false, why);
    }
    if (keepers.empty()) return;
    // the hungry with coin and nothing to eat go to the tavern
    for (auto& [id, c] : w.characters()) {
        if (!person(&c) || c.controller.isPlayer() || c.occupation == "tavernkeeper" || c.body.stage() == "child") continue;
        if (c.need("hunger") >= R->c("npc_hungry_below", 0.45) || spare(c, coin) <= 0 || hasFood(C, c)) continue;
        if (c.ai.social.active() || queuedTrade(w, c) || c.captivity.held() || c.body.downUntil > now) continue;
        Character* best = nullptr;
        double bd = 1e18;
        for (Character* k : keepers) {
            if (k->id == c.id || k->status.settlement != c.status.settlement || !hasFood(C, *k)) continue;
            const double d = dist(k->pos, c.pos);
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        if (!best || bd > C.config.trackRadiusM) continue;
        const Offer o = defaultOffer(w, c, *best, "meal");
        std::string why;
        if (!o.take.empty()) startTrade(w, c, *best, o, false, why);
    }
}

// ---------------------------------------------------------------------------

Json goodsJson(const Goods& g) {
    Json j = Json::object();
    for (const auto& [k, n] : g)
        if (n > 0) j[k] = n;
    return j;
}

Goods goodsFrom(const Content& c, const Json& j) {
    Goods g;
    if (!j.is_object()) return g;
    for (const auto& [k, v] : j.items())
        if (v.is_number_integer() && v.get<int>() > 0 && c.items.count(k)) g[k] = v.get<int>();
    return g;
}

namespace {

int playerOfTrade(const Command& cmd) {
    if (cmd.args.contains("player")) return cmd.args["player"].get<int>();
    if (cmd.source.rfind("player:", 0) == 0) return std::stoi(cmd.source.substr(7));
    return 0;
}

Offer offerOf(const World& w, const Json& a) {
    Offer o;
    o.mode = a.value("mode", std::string("barter"));
    o.give = goodsFrom(w.content(), a.value("give", Json::object()));
    o.take = goodsFrom(w.content(), a.value("take", Json::object()));
    return o;
}

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "trade" && t != "trade_accept" && t != "trade_quote" && t != "set_occupation") return false;
    Character* c = w.character(cmd.args.at("id").get<EntityId>());
    if (!c) throw std::runtime_error("no such character");
    if (t == "set_occupation") {
        setOccupation(w, *c, cmd.args.value("role", std::string()));
        return true;
    }
    Character* target = w.character(cmd.args.value("target", kNone));
    if (!person(target)) {
        cmd.result = "rejected:wrong_target";
        return true;
    }
    const bool player = cmd.source.rfind("player:", 0) == 0 || cmd.args.contains("player");
    if (player && (!c->controller.isPlayer() || c->controller.playerId != playerOfTrade(cmd)))
        throw std::runtime_error("not controlled by player");
    if (t == "trade_quote") {
        Offer o = offerOf(w, cmd.args);
        if (o.give.empty() && o.take.empty()) o = defaultOffer(w, *c, *target, o.mode);
        const DealEval e = weighDeal(w, *c, *target, o);
        cmd.result = "ok:" + e.verdict + ":" + e.reason + ":" + std::to_string(static_cast<int>(std::lround(e.gain))) +
                     ":" + std::to_string(static_cast<int>(std::lround(e.loss * e.keep)));
        return true;
    }
    if (t == "trade_accept") {
        const TradeRules* R = tradeRules(w.content());
        if (dist(c->pos, target->pos) > (R ? R->c("range_m", 6.0) : 6.0) * 2.0) {
            cmd.result = "rejected:out_of_range";
            return true;
        }
        std::string why;
        if (!acceptCounter(w, *c, *target, why)) {
            cmd.result = "rejected:" + why;
            trace(w, *c, 'E', "trade.refused", {{"who", target->id}, {"target", target->id}, {"why", why}});
        }
        return true;
    }
    // trade: the player's person walks over and makes the offer
    const Offer o = offerOf(w, cmd.args);
    std::string why;
    if (!startTrade(w, *c, *target, o, player, why)) cmd.result = "rejected:" + why;
    return true;
}

} // namespace

void installTrade(World& w) { w.addCommandHandler(handle); }

} // namespace altera::sim
