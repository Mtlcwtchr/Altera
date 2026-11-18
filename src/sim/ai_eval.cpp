// Condition evaluation and the interaction executor shared by every layer and
// by every controller: one function decides whether anybody can do a thing and
// one applies it (CORE_SPEC §2.2 - the player gets nothing an NPC lacks).

#include <algorithm>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

std::string ownerRelation(const Character& c, const WorldObject* o) {
    if (!o || o->ownerFamily == kNone) return "none";
    return o->ownerFamily == c.status.family ? "family" : "other";
}

const WorldObject* homeStorage(const World& w, const Character& c) { return w.object(c.home); }

} // namespace

bool goapFact(const Condition& k) { return k.subject == "has" || k.subject == "at"; }

double evalSubject(const EvalCtx& x, const Condition& k) {
    // t.<subject>: the same subjects, read from the target character.
    if (k.onTarget && !x.targetChar) return 0.0;
    const Character& c = k.onTarget ? *x.targetChar : x.c;
    const std::string& s = k.subject;
    if (s == "moodlet") return c.hasMoodlet(k.key, x.w.now()) ? 1.0 : 0.0;
    if (s == "reputation") {
        auto it = c.reputation.find(k.key);
        return it == c.reputation.end() ? 0.0 : it->second;
    }
    if (s == "has") return c.has(k.key);
    if (s == "need") return c.need(k.key);
    if (s == "knows") return c.knowledge.count(k.key) ? 1.0 : 0.0;
    if (s == "skill") {
        auto it = c.skills.find(k.key);
        return it == c.skills.end() ? 0.0 : it->second;
    }
    if (s == "trait") return c.hasTrait(k.key) ? 1.0 : 0.0;
    if (s == "age") return c.body.stage() == k.key ? 1.0 : 0.0;
    if (s == "body") return k.key == "health" ? c.body.health : (k.key == "fatigue" ? c.body.fatigue : 0.0);
    if (s == "time") return (k.key == "night") == x.w.isNight() ? 1.0 : 0.0;
    if (s == "home") {
        const WorldObject* h = homeStorage(x.w, c);
        if (!h) return 0.0;
        auto it = h->stock.find(k.key);
        return it == h->stock.end() ? 0.0 : it->second;
    }
    if (s == "target_stock") {
        if (!x.target) return 0.0;
        auto it = x.target->stock.find(k.key);
        return it == x.target->stock.end() ? 0.0 : it->second;
    }
    if (s == "target_owner") {
        const std::string r = ownerRelation(c, x.target);
        return (r == k.key || (k.key == "self" && r == "family")) ? 1.0 : 0.0;
    }
    if (s == "known" || s == "at") {
        const double range = x.w.content().config.interactionRangeM;
        int n = 0;
        for (EntityId id : c.knownObjects) {
            const WorldObject* o = x.w.object(id);
            if (!o || o->type != k.key) continue;
            if (o->reservedFor(c.id, x.w.now())) continue;
            if (s == "at" && dist(o->pos, c.pos) > range) continue;
            ++n;
        }
        return n;
    }
    if (s == "rel") {
        if (!x.targetChar) return 0.0;
        auto it = x.c.relations.find(x.targetChar->id);
        return it == x.c.relations.end() ? 0.0 : it->second.get(k.key);
    }
    return 0.0;
}

bool evalCondition(const EvalCtx& x, const Condition& k) { return k.test(evalSubject(x, k)); }

bool canPerform(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                std::string* why, bool checkRange) {
    auto fail = [&](std::string r) {
        if (why) *why = std::move(r);
        return false;
    };
    if (!c.body.alive) return fail("dead");
    if (d.targetsObject()) {
        if (!target) return fail("no_target:" + d.targetType);
        if (target->type != d.targetType) return fail("wrong_target:" + d.targetType);
        if (target->reservedFor(c.id, w.now())) return fail("reserved:" + d.targetType);
        if (checkRange && dist(target->pos, c.pos) > w.content().config.interactionRangeM)
            return fail("out_of_range:" + d.targetType);
    }
    const EvalCtx x{w, c, target, nullptr};
    for (const auto& k : d.pre)
        if (!evalCondition(x, k)) return fail("pre:" + k.text);
    return true;
}

namespace {

double clampAxis(const std::string& axis, double v) {
    if (axis == "familiarity") return clamp01(v);
    if (axis == "debt") return v;
    return std::clamp(v, -1.0, 1.0);
}

void addRel(Character& from, EntityId to, const Effect& e) {
    Relation& r = from.relations[to];
    if (double* p = r.axis(e.key)) *p = clampAxis(e.key, e.op == '=' ? e.value : *p + e.signedDelta());
}

void kill(World& w, Character& who, const std::string& cause) {
    if (!who.body.alive) return;
    who.body.alive = false;
    who.body.health = 0.0;
    who.body.causeOfDeath = cause;
    w.chronicle("death", {who.id}, "chronicle.death", {{"who", who.id}, {"cause", cause}});
}

} // namespace

// Applies a list of effects. Subjects of the social layer that are parsed and
// validated but not interpreted yet (status_pair, household_*, commitment:
// stage 2, see AI_DATA_SCHEMA.md §1.4) are skipped here.
void applyEffectList(World& w, Character& actor, const std::vector<Effect>& list, WorldObject* target,
                     Character* targetChar) {
    for (const auto& e : list) {
        if (e.onTarget && !targetChar) continue;
        Character& c = e.onTarget ? *targetChar : actor;
        const double dv = e.signedDelta();
        const std::string& s = e.subject;
        if (s == "has") {
            int& n = c.inventory[e.key];
            n = e.op == '=' ? static_cast<int>(e.value) : n + static_cast<int>(dv);
            if (n <= 0) c.inventory.erase(e.key);
        } else if (s == "need" || (s == "target_need" && targetChar)) {
            Character& who = s == "target_need" ? *targetChar : c;
            double& v = who.needs[e.key];
            v = clamp01(e.op == '=' ? e.value : v + dv);
        } else if (s == "target_stock") {
            if (!target) continue;
            int& n = target->stock[e.key];
            n = std::max(0, e.op == '=' ? static_cast<int>(e.value) : n + static_cast<int>(dv));
        } else if (s == "home") {
            WorldObject* h = w.object(c.home);
            if (!h) continue;
            int& n = h->stock[e.key];
            n = std::max(0, e.op == '=' ? static_cast<int>(e.value) : n + static_cast<int>(dv));
        } else if (s == "skill") {
            double& v = c.skills[e.key];
            v = clamp01(e.op == '=' ? e.value : v + dv);
        } else if (s == "body") {
            double& v = e.key == "health" ? c.body.health : c.body.fatigue;
            v = clamp01(e.op == '=' ? e.value : v + dv);
        } else if (s == "reputation") {
            double& v = c.reputation[e.key];
            v = std::clamp(e.op == '=' ? e.value : v + dv, -1.0, 1.0);
        } else if (s == "moodlet") {
            auto md = w.content().moodlets.find(e.key);
            const Tick until = w.now() + (md == w.content().moodlets.end() ? 72 : md->second.ttl);
            const double value = md == w.content().moodlets.end() ? 0.0 : md->second.value;
            auto it = std::find_if(c.moodlets.begin(), c.moodlets.end(), [&](const Moodlet& m) { return m.id == e.key; });
            if (it != c.moodlets.end()) it->until = until;
            else c.moodlets.push_back({e.key, value, until});
        } else if (s == "memory") {
            MemoryEntry m;
            m.type = e.key;
            m.object = target ? target->id : kNone;
            if (targetChar) m.who = {targetChar->id};
            if (target && target->ownerFamily != kNone && target->ownerFamily != c.status.family) {
                auto f = w.families().find(target->ownerFamily);
                if (f != w.families().end()) m.who = f->second.members;
            }
            w.remember(c, std::move(m));
        } else if (s == "spawn") {
            WorldObject o;
            o.type = e.key;
            o.pos = c.pos;
            o.ownerFamily = c.status.family;
            auto def = w.content().objects.find(e.key);
            if (def != w.content().objects.end()) o.stock = def->second.stock;
            const EntityId id = w.addObject(o).id;
            c.knownObjects.insert(id);
        } else if (s == "rel") {
            if (targetChar) addRel(actor, targetChar->id, e);
        } else if (s == "rel_back") {
            if (targetChar) addRel(*targetChar, actor.id, e);
        } else if (s == "death") {
            kill(w, c, e.key);
        }
    }
}

void applyEffects(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar) {
    // chance: one deterministic roll per (actor, tick, interaction); a failed
    // roll applies on_fail instead of eff. Risks roll independently after.
    bool success = true;
    if (d.chance.set) {
        double p = d.chance.base;
        if (!d.chance.skill.empty()) {
            auto it = c.skills.find(d.chance.skill);
            p += d.chance.perSkill * (it == c.skills.end() ? 0.0 : it->second);
        }
        p = std::clamp(p, d.chance.min, d.chance.max);
        Rng r = w.rng(c.id, "chance:" + d.id);
        success = r.uniform() < p;
    }
    applyEffectList(w, c, success ? d.eff : d.onFail, target, targetChar);
    for (const auto& rk : d.risk) {
        Rng r = w.rng(c.id, "risk:" + d.id + ":" + rk.id);
        if (r.uniform() < rk.p) applyEffectList(w, c, rk.eff, target, targetChar);
    }
}

EntityId findTarget(const World& w, const Character& c, const InteractionDef& d, Vec2 from, std::string* why,
                    bool ignoreReservations) {
    if (!d.targetsObject()) return kNone;
    EntityId best = kNone;
    double bestD = 1e18;
    bool anyOfType = false;
    std::string lastWhy;
    for (EntityId id : c.knownObjects) {
        const WorldObject* o = w.object(id);
        if (!o || o->type != d.targetType) continue;
        anyOfType = true;
        if (!ignoreReservations && o->reservedFor(c.id, w.now())) {
            lastWhy = "reserved:" + d.targetType;
            continue;
        }
        // Target-side conditions only; actor conditions (has:, knows:) are the
        // planner's business.
        const EvalCtx x{w, c, o, nullptr};
        bool ok = true;
        for (const auto& k : d.pre) {
            if (k.subject != "target_stock" && k.subject != "target_owner") continue;
            if (!evalCondition(x, k)) {
                ok = false;
                lastWhy = "target:" + k.text;
                break;
            }
        }
        if (!ok) continue;
        const double dd = dist(o->pos, from);
        if (dd < bestD || (dd == bestD && id < best)) {
            bestD = dd;
            best = id;
        }
    }
    if (best == kNone && why) *why = anyOfType ? lastWhy : "unknown:" + d.targetType;
    return best;
}

} // namespace altera::sim
