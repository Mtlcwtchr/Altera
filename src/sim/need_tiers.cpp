// LR-140: derived needs and the tiers of needs (see need_tiers.hpp).

#include "sim/need_tiers.hpp"

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace lr::sim {

namespace {

double logistic(double k, double x0, double x) { return 1.0 / (1.0 + std::exp(-k * (x - x0))); }

// -1..1 axes to 0..1 (familiarity and debt are 0..1 already).
double axis01(const std::string& axis, double v) {
    if (axis == "familiarity" || axis == "debt") return clamp01(v);
    return clamp01(0.5 + 0.5 * v);
}

// What one pass of the derived needs works out once for everyone.
struct PassCtx {
    // others' relations toward a person, from those of its own settlement:
    // axis -> (sum of axis01 * weight, sum of weights)
    std::map<EntityId, std::map<std::string, std::pair<double, double>>> back;
    std::map<EntityId, double> wealthRank;
    std::vector<const Character*> beasts; // animal groups that may dare a person
    const std::map<EntityId, int>* built = nullptr;
};

double wealthOf(const World& w, const Character& c) {
    const Content& ct = w.content();
    auto value = [&](const std::map<std::string, int>& m) {
        double s = 0.0;
        for (const auto& [item, n] : m) {
            auto it = ct.items.find(item);
            if (it != ct.items.end()) s += it->second.value * n;
        }
        return s;
    };
    double s = value(c.inventory) + c.standing.purse;
    if (const Family* f = w.familyOf(c)) {
        int members = 0;
        for (EntityId m : f->members)
            if (const Character* mc = w.character(m); mc && mc->body.alive) ++members;
        if (const WorldObject* st = w.object(f->storage); st && members > 0) s += value(st->stock) / members;
    }
    return s;
}

const std::vector<std::string>& usedAxes() {
    static const std::vector<std::string> a = relAxes();
    return a;
}

PassCtx makeCtx(const World& w) {
    PassCtx x;
    // Others' relations toward each person (rel_back), within one settlement
    // (or among the settled-nowhere, all who know them).
    for (const auto& [pid, p] : w.characters()) {
        if (!p.body.alive || p.isAnimal()) continue;
        for (const auto& [oid, rel] : p.relations) {
            const Character* o = w.character(oid);
            if (!o || !o->body.alive || o->isAnimal()) continue;
            if (o->status.settlement != kNone && p.status.settlement != o->status.settlement) continue;
            const double wt = 0.2 + clamp01(rel.familiarity);
            auto& acc = x.back[oid];
            for (const auto& a : usedAxes()) {
                auto& [s, n] = acc[a];
                s += axis01(a, rel.get(a)) * wt;
                n += wt;
            }
        }
    }
    // Wealth rank within the settlement.
    std::map<EntityId, std::vector<std::pair<double, EntityId>>> bySettlement;
    for (const auto& [id, c] : w.characters())
        if (c.body.alive && !c.isAnimal()) bySettlement[c.status.settlement].push_back({wealthOf(w, c), id});
    for (auto& [s, v] : bySettlement) {
        const double n = static_cast<double>(v.size());
        for (const auto& [wv, id] : v) {
            double below = 0.0, eq = 0.0;
            for (const auto& [ov, oid] : v) {
                if (ov < wv) below += 1.0;
                else if (ov == wv) eq += 1.0;
            }
            x.wealthRank[id] = n <= 1.0 ? 0.5 : clamp01((below + 0.5 * (eq - 1.0)) / (n - 1.0));
        }
    }
    for (const auto& [id, c] : w.characters()) {
        if (!c.body.alive || !c.isAnimal()) continue;
        const SpeciesDef* sd = w.content().speciesOf(c.species);
        if (sd && sd->aggression > 0.0) x.beasts.push_back(&c);
    }
    return x;
}

double homeStock(const World& w, const Character& c, const std::string& item) {
    const WorldObject* h = w.object(c.home);
    if (!h) return 0.0;
    auto it = h->stock.find(item);
    return it == h->stock.end() ? 0.0 : it->second;
}

double inputOf(const World& w, const Character& c, const DerivedNeedDef& d, const DerivedInput& in, const PassCtx* x) {
    std::string input = in.input;
    bool inv = false;
    if (input.rfind("inv:", 0) == 0) {
        inv = true;
        input = input.substr(4);
    }
    const auto colon = input.find(':');
    const std::string subj = input.substr(0, colon);
    const std::string key = colon == std::string::npos ? "" : input.substr(colon + 1);
    double norm = in.norm > 0.0 ? in.norm : 1.0;
    if (auto it = d.seasonNorm.find(w.season()); it != d.seasonNorm.end()) norm *= it->second;
    const Content& ct = w.content();
    double v = 0.5;
    if (subj == "body") v = key == "health" ? c.body.health : 1.0 - c.body.fatigue;
    else if (subj == "need") v = needPull(ct, c, key);
    else if (subj == "has") v = clamp01(c.has(key) / norm);
    else if (subj == "home") v = clamp01(homeStock(w, c, key) / norm);
    else if (subj == "home_food_days") v = clamp01(homeFoodDays(w, c) / norm);
    else if (subj == "moodlet") v = c.hasMoodlet(key, w.now()) ? 1.0 : 0.0;
    else if (subj == "trait") v = c.hasTrait(key) ? 1.0 : 0.0;
    else if (subj == "reputation") {
        auto it = c.reputation.find(key);
        v = clamp01(0.5 + 0.5 * (it == c.reputation.end() ? 0.0 : it->second));
    } else if (subj == "has_shelter") {
        // a roof over the household's home (its storage), or over the person now
        const WorldObject* h = w.object(c.home);
        const double r = std::max(6.0, ct.config.interactionRangeM);
        v = 0.0;
        for (const Vec2& p : w.roofs())
            if (dist(p, c.pos) <= ct.config.interactionRangeM || (h && dist(p, h->pos) <= r)) v = 1.0;
    } else if (subj == "threat_near") {
        // beasts that may dare a person and people it fears or hates, within `norm` m
        const double R = in.norm > 0.0 ? in.norm : 40.0;
        double t = 0.0;
        if (x)
            for (const Character* b : x->beasts) {
                const double dd = dist(b->pos, c.pos);
                if (dd >= R) continue;
                const SpeciesDef* sd = ct.speciesOf(b->species);
                t = std::max(t, clamp01(sd ? sd->aggression * 2.0 : 0.5) * (1.0 - dd / R));
            }
        for (const auto& [oid, rel] : c.relations) {
            const double f = std::max(rel.fear, rel.enmity);
            if (f <= 0.2) continue;
            const Character* o = w.character(oid);
            if (!o || !o->body.alive) continue;
            const double dd = dist(o->pos, c.pos);
            if (dd < R) t = std::max(t, clamp01(f) * (1.0 - dd / R));
        }
        v = clamp01(t);
    } else if (subj == "household_avg" || subj == "has_household") {
        double s = 0.0, n = 0.0;
        if (const Family* f = w.familyOf(c))
            for (EntityId m : f->members) {
                if (m == c.id) continue;
                const Character* mc = w.character(m);
                if (!mc || !mc->body.alive) continue;
                n += 1.0;
                if (subj == "household_avg") {
                    const std::string a = key.rfind("rel.", 0) == 0 ? key.substr(4) : key;
                    auto it = c.relations.find(m);
                    s += axis01(a, it == c.relations.end() ? 0.0 : it->second.get(a));
                }
            }
        if (subj == "has_household") v = n > 0.0 ? 1.0 : 0.0;
        else v = n > 0.0 ? s / n : 0.5;
    } else if (subj == "community_avg") {
        const std::string a = key.rfind("rel_back.", 0) == 0 ? key.substr(9) : key;
        v = 0.5;
        if (x)
            if (auto it = x->back.find(c.id); it != x->back.end())
                if (auto ia = it->second.find(a); ia != it->second.end() && ia->second.second > 0.0)
                    v = ia->second.first / ia->second.second;
    } else if (subj == "wealth_rank") {
        v = 0.5;
        if (x)
            if (auto it = x->wealthRank.find(c.id); it != x->wealthRank.end()) v = it->second;
    } else if (subj == "has_office") {
        bool chief = false;
        for (const auto& [sid, s] : w.settlements())
            if (s.chief == c.id) chief = true;
        v = chief || !c.status.rights.empty() ? 1.0 : 0.0;
    } else if (subj == "has_heir") {
        v = 0.0;
        for (EntityId k : c.status.children)
            if (const Character* kc = w.character(k); kc && kc->body.alive) v = 1.0;
    } else if (subj == "built_count") {
        int n = 0;
        if (x && x->built)
            if (auto it = x->built->find(c.id); it != x->built->end()) n = it->second;
        v = clamp01(n / norm);
    }
    return inv ? 1.0 - v : v;
}

double targetWith(const World& w, const Character& c, const DerivedNeedDef& d, const PassCtx* x) {
    double s = 0.0, n = 0.0;
    for (const auto& in : d.inputs) {
        s += inputOf(w, c, d, in, x) * in.w;
        n += in.w;
    }
    return n > 0.0 ? clamp01(s / n) : 1.0;
}

} // namespace

double needCare(const Content& ct, const Character& c, const std::string& need) {
    const DerivedNeedDef* d = ct.derivedNeed(need);
    if (!d || !d->weighted) return 1.0;
    double care = d->careBase;
    if (!d->careReputation.empty())
        if (auto it = c.reputation.find(d->careReputation); it != c.reputation.end()) care += 0.5 * it->second;
    for (const auto& [t, v] : d->careTraits)
        if (c.hasTrait(t)) care = std::max(care, v);
    return clamp01(care);
}

double needPull(const Content& ct, const Character& c, const std::string& need) {
    const double v = c.need(need);
    const DerivedNeedDef* d = ct.derivedNeed(need);
    if (!d || !d->weighted) return v;
    return 1.0 - needCare(ct, c, need) * (1.0 - v);
}

double derivedTarget(const World& w, const Character& c, const DerivedNeedDef& d) {
    const PassCtx x = makeCtx(w);
    std::map<EntityId, int> built;
    for (const auto& e : w.chronicleLog())
        if (e.type == "build" && !e.who.empty()) ++built[e.who.front()];
    PassCtx xx = x;
    xx.built = &built;
    return targetWith(w, c, d, &xx);
}

double tierSat(const World& w, const Character& c, const NeedTierDef& t) {
    double sat = 1.0;
    for (const auto& g : t.gate) {
        if (g == "body:health") sat = std::min(sat, c.body.health);
        else if (c.needs.count(g)) sat = std::min(sat, needPull(w.content(), c, g));
    }
    return sat;
}

double tierGate(const World& w, const Character& c, const std::string& goal) {
    const Content& ct = w.content();
    const TierGateDef& g = ct.tierGate;
    if (!g.on || c.isAnimal()) return 1.0;
    auto it = g.goalTier.find(goal);
    if (it == g.goalTier.end()) return 1.0;
    int order = 0;
    for (const auto& t : ct.needTiers)
        if (t.id == it->second) order = t.order;
    if (order <= 1) return 1.0;
    double m = 1.0;
    for (const auto& t : ct.needTiers) {
        if (t.order < 1 || t.order >= order) continue;
        const double sat = tierSat(w, c, t);
        m *= g.floor + (1.0 - g.floor) * logistic(g.k, g.x0, sat);
    }
    return m;
}

void derivedNeedsTick(World& w) {
    const Content& ct = w.content();
    if (ct.derivedNeeds.empty()) return;
    PassCtx x = makeCtx(w);
    auto& cache = w.needCache;
    const auto& log = w.chronicleLog();
    if (cache.scanned > log.size()) {
        cache.scanned = 0;
        cache.built.clear();
    }
    for (; cache.scanned < log.size(); ++cache.scanned) {
        const auto& e = log[cache.scanned];
        if (e.type == "build" && !e.who.empty()) ++cache.built[e.who.front()];
    }
    x.built = &cache.built;
    const double step = 1.0 - std::pow(1.0 - clamp01(ct.tierGate.ratePerDay),
                                       static_cast<double>(ct.tierGate.stepTicks) / kTicksPerDay);
    // Work all targets out first, then move: one person's change does not
    // reach another's target within the same pass (order-free).
    std::vector<std::pair<double*, double>> moves;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal()) continue;
        for (const auto& d : ct.derivedNeeds) {
            if (d.adultOnly && c.body.stage() == "child") continue;
            const double target = targetWith(w, c, d, &x);
            auto it = c.needs.find(d.id);
            if (it == c.needs.end()) moves.push_back({nullptr, target}); // placeholder, set below
            else moves.push_back({&it->second, target});
        }
    }
    std::size_t i = 0;
    for (auto& [id, c] : w.characters()) {
        if (!c.body.alive || c.isAnimal()) continue;
        for (const auto& d : ct.derivedNeeds) {
            if (d.adultOnly && c.body.stage() == "child") continue;
            auto& [ptr, target] = moves[i++];
            if (!ptr) c.needs[d.id] = target; // first time: start where the world puts it
            else *ptr = clamp01(*ptr + (target - *ptr) * step);
        }
    }
}

std::string needTier(const Content& ct, const std::string& need) {
    const NeedTierDef* t = ct.tierOfNeed(need);
    return t ? t->id : std::string();
}

std::vector<TierView> needTiersOf(const World& w, const Character& c) {
    const Content& ct = w.content();
    std::vector<TierView> out;
    std::vector<const NeedTierDef*> order;
    for (const auto& t : ct.needTiers) order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const NeedTierDef* a, const NeedTierDef* b) {
        const int ka = a->order < 1 ? 1000 : a->order, kb = b->order < 1 ? 1000 : b->order;
        return ka < kb;
    });
    for (const NeedTierDef* t : order) {
        TierView tv;
        tv.id = t->id;
        tv.order = t->order;
        tv.sat = tierSat(w, c, *t);
        for (const auto& n : t->needs) {
            auto it = c.needs.find(n);
            if (it == c.needs.end()) continue;
            NeedView nv;
            nv.id = n;
            nv.v = it->second;
            nv.derived = ct.derivedNeed(n) != nullptr;
            nv.care = needCare(ct, c, n);
            if (const NeedDef* nd = ct.need(n)) nv.crit = nd->critical;
            tv.needs.push_back(nv);
        }
        if (!tv.needs.empty()) out.push_back(tv);
    }
    return out;
}

} // namespace lr::sim
