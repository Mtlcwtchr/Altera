// Care (care.json): who depends on others - infants, children, the old, the
// sick, the crippled - and who looks after them. A dependent is a category
// with a weight and the needs others have to see to; a carer weighs every
// dependent in reach by how badly a need has run down, the category's weight
// and how close they are (household, kin, neighbour). The utility input
// `care` is the most pressing such case; conditions `care:<need>` read that
// ward's shortfall in one need, and the goal's target is the ward, so the
// care acts (feed_ward, warm_ward, ...) go to them.

#include <algorithm>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

bool matches(const DependentDef& d, const Character& c) {
    const int years = c.body.ageDays / kDaysPerYear;
    bool any = false;
    if (d.ageLt >= 0) {
        if (years >= d.ageLt) return false;
        any = true;
    }
    if (d.ageGe >= 0) {
        if (years < d.ageGe) return false;
        any = true;
    }
    if (d.healthLt >= 0.0) {
        if (c.body.health >= d.healthLt) return false;
        any = true;
    }
    if (!d.trait.empty()) {
        if (!c.hasTrait(d.trait)) return false;
        any = true;
    }
    return any;
}

double deficit(const CareDef& cd, const Character& ward, const std::string& need) {
    if (need == "hunger" && ward.has("meal") > 0) return 0.0; // has food in hand
    const double t = std::max(0.05, cd.threshold);
    return std::clamp((t - ward.need(need)) / t, 0.0, 1.0);
}

double circleOf(const World& w, const Character& carer, const Character& ward) {
    const CareDef& cd = w.content().care;
    auto weight = [&](const char* k, double def) {
        auto it = cd.circles.find(k);
        return it == cd.circles.end() ? def : it->second;
    };
    if (carer.status.family != kNone && carer.status.family == ward.status.family) return weight("household", 1.0);
    if (!carer.relations.count(ward.id)) return 0.0; // strangers are not seen to
    if (carer.status.dynasty != kNone && carer.status.dynasty == ward.status.dynasty) return weight("kin", 0.6);
    const double aff = carer.relations.at(ward.id).affinity;
    return weight("neighbour", 0.3) * std::clamp(1.0 + aff, 0.0, 2.0);
}

} // namespace

const DependentDef* dependencyOf(const World& w, const Character& c) {
    const DependentDef* best = nullptr;
    if (c.isAnimal() || !c.body.alive) return nullptr;
    for (const auto& d : w.content().care.dependents)
        if (matches(d, c) && (!best || d.weight > best->weight)) best = &d;
    return best;
}

CareChoice bestWard(const World& w, const Character& carer) {
    CareChoice out;
    const CareDef& cd = w.content().care;
    if (cd.dependents.empty() || dependencyOf(w, carer)) return out; // a dependent does not look after others
    const double reach = w.content().config.trackRadiusM;
    for (const auto& [id, o] : w.characters()) {
        if (id == carer.id || o.isAnimal() || !o.body.alive) continue;
        const DependentDef* dep = dependencyOf(w, o);
        if (!dep) continue;
        if (dist(o.pos, carer.pos) > reach) continue;
        const double circle = circleOf(w, carer, o);
        if (circle <= 0.0) continue;
        for (const auto& need : dep->needs) {
            const double u = circle * dep->weight * deficit(cd, o, need);
            if (u > out.urgency + 1e-9 || (u > 0.0 && std::abs(u - out.urgency) <= 1e-9 && id < out.ward)) {
                out.urgency = u;
                out.ward = id;
                out.need = need;
            }
        }
    }
    return out;
}

namespace {

EntityId settlementOfFamily(const World& w, EntityId family) {
    auto f = w.families().find(family);
    return f == w.families().end() ? kNone : f->second.settlement;
}

// How short another household is: 1 with an empty store, 0 from
// charity_days of food per member up.
double shortfall(const World& w, const Family& fam, double days) {
    for (EntityId m : fam.members)
        if (const Character* p = w.character(m); p && p->body.alive)
            return std::clamp(1.0 - homeFoodDays(w, *p) / std::max(1.0, days), 0.0, 1.0);
    return 0.0;
}

// AL-82: a neighbour is a household of the same village, or one living close
// by whatever the law says (an exile still lives next door).
bool neighbours(const World& w, const Character& giver, EntityId family) {
    if (family == kNone || family == giver.status.family) return false;
    auto f = w.families().find(family);
    if (f == w.families().end()) return false;
    const EntityId here = settlementOfFamily(w, giver.status.family);
    if (here != kNone && f->second.settlement == here) return true;
    const WorldObject* mine = w.object(giver.home);
    if (!mine) return false;
    for (EntityId m : f->second.members)
        if (const Character* p = w.character(m); p && p->body.alive)
            if (const WorldObject* theirs = w.object(p->home)) return dist(mine->pos, theirs->pos) < 800.0;
    return false;
}

} // namespace

double charityValue(const World& w, const Character& giver, const Character* target, const std::string& key) {
    if (giver.status.family == kNone || giver.isAnimal()) return 0.0;
    const CareDef& cd = w.content().care;
    if (target) {
        // charity:target - a member of another household of the village that is running out.
        if (key != "target" || target->isAnimal() || !neighbours(w, giver, target->status.family)) return 0.0;
        auto f = w.families().find(target->status.family);
        return f != w.families().end() && shortfall(w, f->second, cd.charityDays) > 0.0 ? 1.0 : 0.0;
    }
    // charity:need - the neediest neighbour's shortfall, as far as the
    // giver's own store can spare it (nothing below charity_spare days).
    const double own = homeFoodDays(w, giver);
    const double spare = std::clamp((own - cd.charitySpare) / std::max(1.0, cd.charitySpare), 0.0, 1.0);
    if (spare <= 0.0) return 0.0;
    double worst = 0.0;
    for (const auto& [fid, fam] : w.families()) {
        if (!neighbours(w, giver, fid)) continue;
        double s = shortfall(w, fam, cd.charityDays);
        // Small children in the house weigh more.
        for (EntityId m : fam.members)
            if (const Character* p = w.character(m); p && p->body.alive && p->body.stage() == "child") s *= 1.15;
        worst = std::max(worst, std::min(1.0, s));
    }
    return worst * spare;
}

double careValue(const World& w, const Character& carer, const std::string& key) {
    const CareChoice ch = bestWard(w, carer);
    if (key.empty() || key == "need") return ch.urgency;
    const Character* ward = w.character(ch.ward);
    if (!ward) return 0.0;
    return deficit(w.content().care, *ward, key);
}

} // namespace altera::sim
