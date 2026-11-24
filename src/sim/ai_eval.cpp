// Condition evaluation and the interaction executor shared by every layer and
// by every controller: one function decides whether anybody can do a thing and
// one applies it (CORE_SPEC §2.2 - the player gets nothing an NPC lacks).

#include <algorithm>
#include <cmath>

#include "sim/dialogue_say.hpp"
#include "sim/ai.hpp"
#include "sim/building.hpp"
#include "sim/captivity.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/consequences.hpp"
#include "sim/dialogue_memory.hpp"
#include "sim/gear.hpp"
#include "sim/group_combat.hpp"
#include "sim/tones.hpp"
#include "sim/village_life.hpp"

namespace altera::sim {

namespace {

std::string ownerRelation(const Character& c, const WorldObject* o, const Character* beast = nullptr) {
    // Livestock is owned like a thing: by the household that keeps it.
    const EntityId owner = o ? o->ownerFamily : (beast && beast->isAnimal() ? beast->status.family : kNone);
    if (owner == kNone) return "none";
    return owner == c.status.family ? "family" : "other";
}

const WorldObject* homeStorage(const World& w, const Character& c) { return w.object(c.home); }

// AL-30: how far a person can get at a skill by practice alone, the same on
// every machine (FNV-1a of the id and the skill's name, not std::hash).
double skillTalent(const AiConfig& cf, EntityId id, const std::string& skill) {
    if (cf.skillTalentMax >= 1.0 && cf.skillTalentMin >= 1.0) return 1.0;
    std::uint64_t h = 1469598103934665603ull;
    auto mixByte = [&](std::uint8_t b) { h = (h ^ b) * 1099511628211ull; };
    for (int i = 0; i < 8; ++i) mixByte(std::uint8_t(std::uint64_t(id) >> (8 * i)));
    for (char ch : skill) mixByte(std::uint8_t(ch));
    const double u = double(h >> 11) / double(1ull << 53);
    return cf.skillTalentMin + (cf.skillTalentMax - cf.skillTalentMin) * u;
}

} // namespace

bool goapFact(const Condition& k) { return (k.subject == "has" && !k.onTarget) || k.subject == "at"; }

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
    if (s == "herd") return herdValue(x.w, c, x.targetChar && x.targetChar->isAnimal() ? x.targetChar : nullptr, k.key);
    if (const std::string crop = cropSubjectType(x.w.content(), s); !crop.empty())
        return farmValue(x.w, c, x.target, k.key, crop);
    if (s == "care") return careValue(x.w, c, k.key);
    if (s == "site") return siteValue(x.w, c, x.target, k.key);
    if (s == "build") return buildValue(x.w, c, k.key);
    if (s == "charity") return charityValue(x.w, c, x.targetChar, k.key);
    if (s == "target_owner") {
        const std::string r = ownerRelation(c, x.target, x.targetChar);
        return (r == k.key || (k.key == "self" && r == "family")) ? 1.0 : 0.0;
    }
    if ((s == "known" || s == "at") && x.w.content().speciesOf(k.key)) {
        // Animals move: known = a live group within the tracking radius.
        const double radius = s == "known" ? x.w.content().config.trackRadiusM : x.w.content().config.interactionRangeM * 4;
        int n = 0;
        const bool tame = x.w.content().speciesOf(k.key)->domestic;
        for (const auto& [id, o] : x.w.characters())
            if (o.body.alive && o.species == k.key && dist(o.pos, c.pos) <= radius &&
                (!tame || o.status.family == c.status.family)) // livestock: only the household's own
                ++n;
        return n;
    }
    if (s == "known" || s == "at") {
        const double range = x.w.content().config.interactionRangeM;
        int n = 0;
        for (EntityId id : x.w.objectsOfType(k.key)) {
            if (!c.knownObjects.count(id)) continue;
            const WorldObject* o = x.w.object(id);
            if (!o) continue;
            if (o->reservedFor(c.id, x.w.now())) continue;
            if (!x.w.inSeason(*o)) continue;
            if (s == "at" && dist(o->pos, c.pos) > range) continue;
            ++n;
        }
        return n;
    }
    if (s == "status_pair") {
        if (!x.targetChar) return 0.0;
        return x.c.status.hasPair(x.targetChar->id, k.key) ? 1.0 : 0.0;
    }
    if (s == "status_pair_any") return c.status.hasPairAny(k.key) ? 1.0 : 0.0;
    if (s == "right") return c.status.rights.count(k.key) ? 1.0 : 0.0;
    if (s == "verdict") {
        // A sentence is always read from the target (the one being judged);
        // without a target character it is the actor's own.
        const Character& who = x.targetChar ? *x.targetChar : c;
        return who.status.verdicts.count(k.key) ? 1.0 : 0.0;
    }
    if (s == "rel") {
        if (!x.targetChar) return 0.0;
        auto it = x.c.relations.find(x.targetChar->id);
        return it == x.c.relations.end() ? 0.0 : it->second.get(k.key);
    }
    return 0.0;
}

bool evalCondition(const EvalCtx& x, const Condition& k) { return k.test(evalSubject(x, k)); }

EntityId findPerson(const World& w, const Character& c, const InteractionDef& d, EntityId prefer, std::string* why,
                    bool strict) {
    auto passes = [&](const Character& o) {
        const EvalCtx x{w, c, nullptr, &o};
        for (const auto& k : d.pre)
            if (!evalCondition(x, k)) {
                if (why) *why = "person:" + k.text;
                return false;
            }
        return true;
    };
    if (prefer != kNone)
        if (const Character* o = w.character(prefer); o && o->body.alive && !o->isAnimal() && o->id != c.id && passes(*o))
            return prefer;
    // The player named this person: a refusal with the reason, never a stand-in (AL-23).
    if (strict && prefer != kNone) {
        const Character* o = w.character(prefer);
        if (why && (why->empty() || why->rfind("person:", 0) != 0))
            *why = !o ? "no_person" : !o->body.alive ? "person:dead" : o->isAnimal() ? "person:animal"
                 : o->id == c.id ? "person:self" : *why;
        return kNone;
    }
    // Gifts go to the needy: eff gives the target something.
    bool gift = false;
    for (const auto& e : d.eff)
        if (e.onTarget && (e.subject == "has" || e.subject == "need" || e.subject == "home") && e.signedDelta() > 0.0)
            gift = true;
    EntityId best = kNone;
    double bestScore = 1e18;
    for (const auto& [id, o] : w.characters()) {
        if (id == c.id || o.isAnimal() || !o.body.alive) continue;
        if (!c.relations.count(id)) continue; // strangers are not sought out
        const double d0 = dist(o.pos, c.pos);
        if (d0 > w.content().config.trackRadiusM) continue;
        if (!passes(o)) continue;
        double score = d0 / 100.0;
        if (o.status.family == c.status.family) score -= 3.0;
        if (gift) score -= 4.0 * (1.0 - o.need("hunger"));
        score -= 2.0 * c.relations.at(id).affinity;
        if (score < bestScore || (score == bestScore && id < best)) {
            bestScore = score;
            best = id;
        }
    }
    if (best == kNone && why && why->empty()) *why = "no_person";
    return best;
}

EntityId pickedTarget(const World& w, const Character& c, const InteractionDef& d) {
    if (c.ai.goal.source != "player" || c.ai.goal.target == kNone) return kNone;
    const EntityId t = c.ai.goal.target;
    if (d.targetsObject()) {
        const WorldObject* o = w.object(t);
        return o && o->type == d.targetType ? t : kNone;
    }
    if (d.targetsCreature()) {
        const Character* b = w.character(t);
        return b && b->body.alive && b->species == d.targetType ? t : kNone;
    }
    return kNone;
}

std::string orderTargetProblem(const World& w, const Character& c, const InteractionDef& d, EntityId target,
                               std::string& why) {
    if (d.targetsCharacter()) {
        if (findPerson(w, c, d, target, &why, true) != kNone) return "";
        return "target:" + (why.empty() ? std::string("no_person") : why);
    }
    if (d.targetsObject()) {
        const WorldObject* o = w.object(target);
        if (!o) return "target:no_target";
        if (o->type != d.targetType) return "target:wrong_target:" + d.targetType;
        return "";
    }
    if (d.targetsCreature()) {
        const Character* b = w.character(target);
        if (!b || !b->body.alive) return "target:no_target";
        if (b->species != d.targetType) return "target:wrong_target:" + d.targetType;
        return "";
    }
    return ""; // self acts: the target is a hint the act does not use
}

const InteractionDef* foreignVariant(const World& w, const Character& c, const InteractionDef& d, EntityId target) {
    if (!d.baseId.empty() || target == kNone) return nullptr;
    const WorldObject* o = d.targetsObject() ? w.object(target) : nullptr;
    const Character* b = d.targetsCreature() ? w.character(target) : nullptr;
    if (!o && !b) return nullptr;
    if (ownerRelation(c, o, b) != "other") return nullptr;
    return w.content().interaction(d.id + "@foreign");
}

double consentChance(const World& w, const Character& actor, const Character& target, const InteractionDef& d,
                     const std::string& tone) {
    if (d.consent != "target") return 1.0;
    if (!target.body.alive) return 0.0;
    auto rel = target.relations.find(actor.id);
    const double aff = rel == target.relations.end() ? 0.0 : rel->second.affinity;
    const double trust = rel == target.relations.end() ? 0.0 : rel->second.trust;
    double p = 0.45 + 0.5 * aff + 0.3 * trust;
    if (target.status.family == actor.status.family) p += 0.35;
    // AL-38: what the target knows the actor did (theft from her household above all)
    if (const double wary = deedWariness(w, target, actor.id); wary > 0.0)
        p -= recallRules(w.content()).consentWariness * wary;
    // Asked to give away food while hungry yourself: unlikely.
    for (const auto& e : d.eff)
        if (e.onTarget && e.subject == "has" && e.signedDelta() < 0.0) {
            if (target.need("hunger") < 0.35) p -= 0.5;
            // ...or out of a thin store: another household's need is weighed
            // against the days your own family has left.
            if (target.status.family != actor.status.family)
                p -= 0.6 * (1.0 - clamp01(homeFoodDays(w, target) / 90.0));
        }
    // The player's tone (tones.json) moves the answer.
    if (const ToneRules* tr = toneRules(w.content()); tr && !tone.empty()) p += toneShift(*tr, tone, d, actor, target);
    return std::clamp(p, 0.02, 0.98);
}

bool consents(const World& w, const Character& actor, const Character& target, const InteractionDef& d) {
    if (d.consent != "target") return true;
    if (!target.body.alive) return false;
    Rng r = agentRng(w.seed(), target.id, w.now(), "consent:" + d.id + ":" + std::to_string(actor.id));
    return r.uniform() < consentChance(w, actor, target, d, toneOf(w, actor, &target));
}

void runConversation(World& w, Character& c, Character& other, const Json& params) {
    const std::string intent = params.value("intent", std::string("small_talk"));
    const Content& ct = w.content();
    // Every conversation is also company.
    if (const InteractionDef* chat = ct.interaction("chat")) {
        applyEffectList(w, c, chat->eff, nullptr, &other, nullptr);
        Effect social;
        social.onTarget = true;
        social.subject = "need";
        social.key = "social";
        social.op = '+';
        social.value = 0.2;
        social.text = "t.need:social+0.2";
        applyEffectList(w, c, {social}, nullptr, &other, nullptr);
    }
    if (intent == "ask_favor" && params.value("want", std::string()) == "meal") {
        // The listener's mind decides; a yes is a promise to bring food.
        InteractionDef ask;
        ask.id = "ask_favor";
        ask.consent = "target";
        Effect take;
        take.onTarget = true;
        take.subject = "has";
        take.key = "meal";
        take.op = '-';
        take.value = 1.0;
        ask.eff = {take};
        if (consents(w, c, other, ask) && ct.task("share_food_task")) {
            QueuedTask q;
            q.task = "share_food_task";
            q.target = c.id;
            q.priority = other.status.family == c.status.family ? 0.85 : 0.6;
            q.created = w.now();
            q.ttl = 72;
            q.reason = "asked_for_food";
            q.source = "dialogue:" + std::to_string(c.id);
            auto& queue = other.ai.queue;
            if (std::none_of(queue.begin(), queue.end(), [&](const QueuedTask& t) {
                    return t.task == q.task && t.target == q.target;
                }))
                queue.push_back(q);
            trace(w, c, 'C', "dialog.accepted", {{"intent", intent}, {"listener", other.id}});
        } else {
            w.remember(c, MemoryEntry{0, "was_refused", {other.id}, kNone, -0.2, 0.0, "seen", 1.0});
            Json why = {{"intent", intent}, {"listener", other.id}};
            // AL-38: the deed the listener holds against the asker, for the line
            // "Нет. Не после того, как ты обокрал мельника" (T6 renders it)
            for (const auto& rc : recall(w, other, c.id))
                if (rc.kind == "deed") {
                    why["deed"] = rc.prop.pred;
                    why["topic"] = rc.topic;
                    break;
                }
            trace(w, c, 'C', "dialog.refused", why);
            sayRefusal(w, other, c, "consent", "task.share_food_task.name"); // AL-36: the line itself (AL-53)
        }
    }
}

double reachOf(const World& w, const InteractionDef& d) {
    return d.rangeM > 0.0 ? d.rangeM : w.content().config.interactionRangeM;
}

EntityId findCreature(const World& w, const std::string& species, Vec2 from, double radius, EntityId family) {
    EntityId best = kNone;
    double bd = radius;
    const SpeciesDef* tame = w.content().speciesOf(species);
    const bool own = tame && tame->domestic;
    for (const auto& [id, o] : w.characters()) {
        if (!o.body.alive || o.species != species || o.group <= 0) continue;
        if (own && o.status.family != family) continue; // the household's own livestock
        if (const SpeciesDef* sp = w.content().speciesOf(species); sp && sp->habitat == "water" && o.group <= 4) continue;
        const double d = dist(o.pos, from);
        if (d <= bd) {
            bd = d;
            best = id;
        }
    }
    return best;
}

bool targetPos(const World& w, EntityId id, Vec2& out) {
    if (const WorldObject* o = w.object(id)) {
        out = o->pos;
        return true;
    }
    if (const Character* ch = w.character(id)) {
        out = ch->pos;
        return true;
    }
    return false;
}

double homeFoodDays(const World& w, const Character& c) {
    const WorldObject* h = w.object(c.home);
    if (!h) return 0.0;
    double food = 0.0;
    for (const auto& [item, n] : h->stock) {
        auto d = w.content().items.find(item);
        if (d != w.content().items.end()) food += d->second.nutrition * n;
    }
    int members = 1;
    if (const Family* f = w.familyOf(c)) members = std::max<int>(1, static_cast<int>(f->members.size()));
    const NeedDef* hunger = w.content().need("hunger");
    const double perDay = hunger ? hunger->decayPerDay : 0.5;
    return food / (members * std::max(0.05, perDay));
}

void spoilFood(World& w) {
    const Content& ct = w.content();
    auto rot = [&](std::map<std::string, int>& stock, EntityId owner, const char* salt) {
        for (auto it = stock.begin(); it != stock.end();) {
            auto d = ct.items.find(it->first);
            if (d == ct.items.end() || d->second.spoilDays <= 0.0 || it->second <= 0) {
                ++it;
                continue;
            }
            // spoil_days is a half-life: half of a stack is gone after that many days.
            const double expect = it->second * (0.6931471805599453 / d->second.spoilDays);
            int lost = static_cast<int>(expect);
            Rng r = w.rng(owner, std::string(salt) + it->first);
            if (r.uniform() < expect - lost) ++lost;
            it->second -= std::min(lost, it->second);
            if (it->second <= 0 && salt[0] == 'p') it = stock.erase(it);
            else ++it;
        }
    };
    // AL-30: every store rots, the village's own too; only what still grows
    // (bushes, patches: their stock is the plant's) is left to regrow.
    for (auto& [id, o] : w.objects()) {
        if (o.stock.empty()) continue;
        const auto def = ct.objects.find(o.type);
        const bool growing = def != ct.objects.end() && (!def->second.maxStock.empty() || !def->second.regrow.empty());
        if (o.ownerFamily != kNone || !growing) rot(o.stock, id, "s:");
    }
    for (auto& [id, c] : w.characters())
        if (c.body.alive && !c.isAnimal()) rot(c.inventory, id, "p:");
}

bool canPerform(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                std::string* why, bool checkRange, const Character* targetChar) {
    auto fail = [&](std::string r) {
        if (why) *why = std::move(r);
        return false;
    };
    if (!c.body.alive) return fail("dead");
    if (d.targetsObject()) {
        if (!target) return fail("no_target:" + d.targetType);
        if (target->type != d.targetType) return fail("wrong_target:" + d.targetType);
        if (target->reservedFor(c.id, w.now())) return fail("reserved:" + d.targetType);
        if (!w.inSeason(*target)) return fail("season:" + d.targetType);
        if (checkRange) {   // AL-78: one stands beside a thing (stand_m), so it is in reach from there
            const auto od = w.content().objects.find(target->type);
            const double stand = od != w.content().objects.end() ? od->second.standM : 0.0;
            if (dist(target->pos, c.pos) > std::max(reachOf(w, d), stand + 0.25)) return fail("out_of_range:" + d.targetType);
        }
    }
    if (d.targetsCharacter() && targetChar) { // without a person only the actor's side is checked
        if (!targetChar->body.alive || targetChar->isAnimal()) return fail("no_person");
        if (checkRange && dist(targetChar->pos, c.pos) > reachOf(w, d)) return fail("out_of_range:person");
    }
    if (d.targetsCreature()) {
        if (!targetChar || !targetChar->body.alive || targetChar->species != d.targetType)
            return fail("no_target:" + d.targetType);
        if (checkRange && dist(targetChar->pos, c.pos) > reachOf(w, d)) return fail("out_of_range:" + d.targetType);
        // A shoal fished down to its spawning stock gives nothing more for now.
        int heads = 0;
        for (const auto& e : d.eff)
            if (e.onTarget && e.subject == "group") heads += static_cast<int>(-e.signedDelta());
        const SpeciesDef* sp = w.content().speciesOf(targetChar->species);
        if (sp && sp->habitat == "water" && targetChar->group <= heads + 2) return fail("depleted:" + d.targetType);
    }
    const EvalCtx x{w, c, target, targetChar};
    for (const auto& k : d.pre)
        if (!evalCondition(x, k)) return fail("pre:" + k.text);
    // The player's person carries as a body can (gear.json): what an act
    // gives must not be too heavy or find no room in the hands and bags.
    // Only for the player's own orders (AL-24): the person's own life under
    // free will lives by the same rules as a neighbour's, and food goes to
    // the mouth, not the bag - only its weight counts.
    if (c.controller.isPlayer() && c.ai.goal.source == "player")
        if (const GearRules* R = gearRules(w.content())) {
            std::map<std::string, int> gains, bagged;
            for (const auto& e : d.eff)
                if (!e.onTarget && !e.onWitnesses && e.subject == "has" && e.op == '+' && !e.isParam()) {
                    gains[e.key] += static_cast<int>(e.value);
                    const auto it = w.content().items.find(e.key);
                    if (it == w.content().items.end() || it->second.nutrition <= 0.0) bagged[e.key] += static_cast<int>(e.value);
                }
            if (!gains.empty()) {
                const std::string heavy = canTake(*R, c, gains);
                if (heavy == "too_heavy") return fail("gear:too_heavy");
                // AL-73: what does not fit is left where it was (takeWhatFits); only
                // an act of which not one thing finds room is refused
                if (!bagged.empty()) {
                    bool any = false;
                    for (const auto& [k, n] : bagged)
                        if (canTake(*R, c, {{k, 1}}).empty()) any = true;
                    if (!any) return fail("gear:" + canTake(*R, c, bagged));
                }
            }
        }
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

// Applies a list of effects. Subjects of the social layer that are parsed and
// validated but not interpreted yet (status_pair, household_*, commitment:
// stage 2, see AI_DATA_SCHEMA.md §1.4) are skipped here.
} // namespace
void applyEffectList(World& w, Character& actor, const std::vector<Effect>& list, WorldObject* target,
                     Character* targetChar, const std::vector<EntityId>* witnesses);
namespace {

void setPair(World& w, Character& a, Character& b, const std::string& id, bool on) {
    auto one = [&](Character& x, Character& y) {
        if (on) x.status.pairs[y.id].insert(id);
        else {
            auto it = x.status.pairs.find(y.id);
            if (it != x.status.pairs.end()) {
                it->second.erase(id);
                if (it->second.empty()) x.status.pairs.erase(it);
            }
        }
        if (id == "married") x.status.spouse = on ? y.id : (x.status.spouse == y.id ? kNone : x.status.spouse);
    };
    one(a, b);
    one(b, a);
    w.chronicle(on ? "status_pair" : "status_pair_end", {a.id, b.id},
                on ? "chronicle.status_pair" : "chronicle.status_pair_end", {{"a", a.id}, {"b", b.id}, {"pair", id}});
}

void applyWitnessEffect(World& w, Character& actor, const Effect& e, const std::vector<EntityId>& witnesses) {
    for (EntityId id : witnesses) {
        Character* wc = w.character(id);
        if (!wc || !wc->body.alive) continue;
        if (e.subject == "rel") {
            addRel(*wc, actor.id, e);
        } else if (e.subject == "moodlet") {
            Effect m = e;
            m.onWitnesses = false;
            applyEffectList(w, *wc, {m}, nullptr, nullptr, nullptr);
        } else if (e.subject == "memory") {
            MemoryEntry m;
            m.type = e.key;
            m.who = {actor.id};
            m.source = "seen";
            w.remember(*wc, std::move(m));
        }
    }
}

} // namespace

std::vector<EntityId> witnessesOf(const World& w, const Character& actor, const InteractionDef& d,
                                  const Character* targetChar) {
    std::vector<EntityId> out;
    if (d.visibility == "private") return out; // participants only: actor and target are not witnesses
    const AiConfig& cfg = w.content().config;
    if (d.targetsCreature()) return out; // the herd is not a witness
    for (const auto& [id, o] : w.characters()) {
        if (o.isAnimal()) continue;
        if (id == actor.id || !o.body.alive) continue;
        if (targetChar && id == targetChar->id) {
            // The robbed person may feel the hand in the purse (secret acts).
            if (d.targetsCarried()) {
                Rng r = agentRng(w.seed(), id, w.now(), "notice:" + d.id + ":" + std::to_string(actor.id));
                if (r.uniform() < cfg.noticeChance * 1.5) out.push_back(id);
            }
            continue;
        }
        if (dist(o.pos, actor.pos) > cfg.perceptionRadiusM) continue;
        if (d.visibility == "secret") {
            Rng r = agentRng(w.seed(), id, w.now(), "notice:" + d.id + ":" + std::to_string(actor.id));
            if (r.uniform() >= cfg.noticeChance) continue;
        }
        out.push_back(id);
    }
    return out;
}

double chanceOf(const World& w, const Character& c, const InteractionDef& d, const Character* targetChar) {
    if (!d.chance.set) return 1.0;
    double p = d.chance.base;
    if (!d.chance.skill.empty()) {
        auto it = c.skills.find(d.chance.skill);
        p += d.chance.perSkill * (it == c.skills.end() ? 0.0 : it->second);
    }
    const EvalCtx x{w, c, nullptr, targetChar};
    for (const auto& in : d.chance.add) p += in.k * evalSubject(x, in.input);
    return std::clamp(p, d.chance.min, d.chance.max);
}

std::vector<LegalTag> legalTags(const World& w, const Character& c, const InteractionDef& d, const WorldObject* target,
                                const Character* targetChar) {
    const EvalCtx x{w, c, target, targetChar};
    auto holds = [&](const LegalRule& r) {
        for (const auto& any : r.all) {
            bool one = false;
            for (const auto& k : any)
                if (evalCondition(x, k)) {
                    one = true;
                    break;
                }
            if (!one) return false;
        }
        return true;
    };
    std::vector<LegalTag> tags;
    for (const auto& t : d.legal) tags.push_back({t, 1.0});
    for (const auto& r : d.legalIf)
        if (holds(r) && std::none_of(tags.begin(), tags.end(), [&](const LegalTag& g) { return g.tag == r.tag; }))
            tags.push_back({r.tag, 1.0});
    for (const auto& r : d.legalUnless) {
        if (!holds(r)) continue;
        for (auto it = tags.begin(); it != tags.end();) {
            if (it->tag != r.tag) {
                ++it;
                continue;
            }
            if (r.mult < 0.0) it = tags.erase(it);
            else {
                it->mult *= r.mult;
                ++it;
            }
        }
    }
    return tags;
}

double legalPrice(const World& w, const Character& c, const std::string& tag, double mult) {
    const Content& ct = w.content();
    double m = mult;
    for (const auto& t : c.traits) {
        auto td = ct.traits.find(t);
        if (td == ct.traits.end()) continue;
        auto lp = td->second.legalPenalty.find(tag);
        if (lp != td->second.legalPenalty.end()) m *= lp->second;
    }
    // Desperation lowers the price of breaking a norm (AI_ARCHITECTURE §3.4).
    const double desperation = c.need("hunger") < 0.2 ? 0.35 : 1.0;
    return ct.config.legalBase * m * desperation;
}

void applyEffectList(World& w, Character& actor, const std::vector<Effect>& list, WorldObject* target,
                     Character* targetChar, const std::vector<EntityId>* witnesses) {
    for (const auto& e : list) {
        if (e.onWitnesses) {
            if (witnesses) applyWitnessEffect(w, actor, e, *witnesses);
            continue;
        }
        if (e.onTarget && !targetChar) continue;
        Character& c = e.onTarget ? *targetChar : actor;
        const double dv = e.signedDelta();
        const std::string& s = e.subject;
        if (s == "has") {
            int& n = c.inventory[e.key];
            n = e.op == '=' ? static_cast<int>(e.value) : n + static_cast<int>(dv);
            if (n <= 0) c.inventory.erase(e.key);
            if (!c.worn.empty() || !c.gearPins.empty()) pruneGear(c);
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
            double gain = dv;
            if (e.op != '=' && gain > 0.0) { // AL-30: diminishing practice, none for the small
                const AiConfig& cf = w.content().config;
                gain = c.body.ageDays < cf.skillMinAgeYears * kDaysPerYear
                           ? 0.0
                           : gain * cf.skillGainMult * std::pow(std::max(0.0, 1.0 - v), cf.skillGainCurve);
                if (c.body.ageDays < cf.skillChildUntilYears * kDaysPerYear) gain *= cf.skillChildMult;
                const double talent = skillTalent(cf, c.id, e.key);
                if (v > talent) gain *= std::exp(-(v - talent) * cf.skillOverTalentFalloff);
            }
            v = clamp01(e.op == '=' ? e.value : v + gain);
        } else if (s == "body") {
            double& v = e.key == "health" ? c.body.health : c.body.fatigue;
            // a heavy load makes every tiring act tire more (gear.json)
            double d = dv;
            if (e.key == "fatigue" && e.op == '+')
                if (const GearRules* R = gearRules(w.content())) d *= loadFatigueMult(*R, c);
            v = clamp01(e.op == '=' ? e.value : v + d);
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
            if (e.onTarget) m.who = {actor.id}; // t.memory: the target remembers the actor
            else if (targetChar) m.who = {targetChar->id};
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
        } else if (s == "group") {
            if (targetChar && targetChar->isAnimal()) {
                const int n = static_cast<int>(e.op == '=' ? targetChar->group - e.value : -dv);
                if (n > 0) loseHeads(w, *targetChar, std::min(n, targetChar->group), "hunted");
            }
        } else if (s == "status_pair") {
            if (targetChar) setPair(w, actor, *targetChar, e.key, e.op == 0 || e.value != 0.0);
        } else if (s == "rel") {
            if (targetChar) addRel(actor, targetChar->id, e);
        } else if (s == "rel_back") {
            if (targetChar) addRel(*targetChar, actor.id, e);
        } else if (s == "death") {
            kill(w, c, e.key);
        } else if (s == "site") {
            if (target) siteEffect(w, c, *target, e.key);
        } else if (s == "captivity") {
            // captivity:take - the actor holds the target (captivity.json); captivity:release - lets go.
            if (targetChar && e.key == "take") takeCaptive(w, actor, *targetChar);
            else if (targetChar && e.key == "release") freeCaptive(w, *targetChar, "released");
        }
    }
}

namespace {

std::string mirrorOutcome(const std::string& o, const std::string& targetState) {
    if (targetState == "flee") return "flee";
    if (o == "win") return "lose";
    if (o == "lose" || o == "flee") return "win";
    return "draw";
}

void applyCombatResult(World& w, Character& c, Character& t, const InteractionDef& d, const CombatResolution& cr,
                       WorldObject* target, const std::vector<EntityId>& wit) {
    const CombatBridge& b = *combatBridge(w.content());
    wearMadeWeapon(c, cr.actorWeaponWear);
    wearMadeWeapon(t, cr.targetWeaponWear);
    auto hurt = [&](Character& who, double mult, bool dead, const std::vector<WoundHit>& hits, double blood) {
        // A player is never killed in a fight: defeat means an off-screen rescue
        // (rescue.json); real death comes only from special acts (execution).
        if (dead && d.combat.lethal && !who.controller.isPlayer()) return kill(w, who, "combat");
        const double before = who.body.health;
        who.body.health = std::max(b.healthMinAfter, who.body.health * mult);
        if (dead) who.body.health = b.healthMinAfter; // a non-lethal fight leaves the body down, not dead
        // The wounds stay in the profile and heal in the ordinary ticks (wounds.cpp).
        addCombatWounds(w, who, hits, dead ? std::max(blood, 1.0) : blood, before - who.body.health);
    };
    hurt(c, cr.actorHealth, cr.actorDead, cr.actorHits, cr.actorBlood);
    hurt(t, cr.targetHealth, cr.targetDead, cr.targetHits, cr.targetBlood);
    auto out = d.combat.outcomes.find(cr.outcome);
    if (out != d.combat.outcomes.end()) applyEffectList(w, c, out->second, target, &t, &wit);
    auto remember = [&](Character& who, EntityId other, const std::string& o) {
        if (!who.body.alive) return;
        MemoryEntry m;
        m.type = b.memory.at(o);
        m.who = {other};
        auto v = b.memoryValence.find(o);
        m.valence = v == b.memoryValence.end() ? 0.0 : v->second;
        w.remember(who, std::move(m));
    };
    remember(c, t.id, cr.outcome);
    remember(t, c.id, mirrorOutcome(cr.outcome, cr.targetState));
    w.chronicle("combat", {c.id, t.id}, b.chronicle,
                {{"actor", c.id}, {"target", t.id}, {"act", d.id}, {"outcome", cr.outcome}, {"rounds", cr.rounds},
                 {"actor_state", cr.actorState}, {"target_state", cr.targetState}, {"mode", cr.mode}});
    // The winner decides over a knocked-out or yielding loser (aftermath.json);
    // a body that survived a non-lethal "death" is down, i.e. knocked out.
    if (cr.outcome == "win" || cr.outcome == "lose") {
        Character& winner = cr.outcome == "win" ? c : t;
        Character& loser = cr.outcome == "win" ? t : c;
        std::string state = cr.outcome == "win" ? cr.targetState : cr.actorState;
        if (state == "dead" && loser.body.alive) state = "ko";
        finishFight(w, winner, loser, d, state, wit);
    }
}

} // namespace

// AL-73: the player's person takes of an act's yield what the hands and bags
// hold; the rest stays where it was (the tree keeps its logs), said in the
// trace "gear.left" so the game shows it. Food goes to the mouth and others'
// lives are not counted (canPerform, AL-24).
static std::vector<Effect> takeWhatFits(World& w, Character& c, const InteractionDef& d, const std::vector<Effect>& eff) {
    if (!c.controller.isPlayer() || c.ai.goal.source != "player") return eff;
    const GearRules* R = gearRules(w.content());
    if (!R) return eff;
    std::vector<Effect> out = eff;
    std::map<std::string, int> took;
    for (auto& e : out) {
        if (e.onTarget || e.onWitnesses || e.subject != "has" || e.op != '+' || e.isParam()) continue;
        const auto it = w.content().items.find(e.key);
        if (it != w.content().items.end() && it->second.nutrition > 0.0) continue;
        const int want = static_cast<int>(e.value);
        int n = want;
        while (n > 0 && !canTake(*R, c, {{e.key, n}}).empty()) --n;
        if (n == want) continue;
        e.value = n;
        e.text = "has:" + e.key + "+" + std::to_string(n);
        took[e.key] = want - n;
    }
    if (took.empty()) return out;
    for (auto& e : out)
        if (e.subject == "target_stock" && e.op == '-' && took.count(e.key)) {
            e.value = std::max(0.0, e.value - took[e.key]);
            e.text = "target_stock:" + e.key + "-" + std::to_string(static_cast<int>(e.value));
        }
    for (const auto& [k, n] : took) trace(w, c, 'F', "gear.left", {{"action", d.id}, {"item", k}, {"n", n}});
    return out;
}

void applyEffects(World& w, Character& c, const InteractionDef& d, WorldObject* target, Character* targetChar) {
    // chance: one deterministic roll per (actor, tick, interaction); a failed
    // roll applies on_fail instead of eff. Risks roll independently after.
    bool success = true;
    // The player's tone (tones.json): it moves the chance roll, makes the
    // target's reception of a social act a roll of its own, and adds effects.
    const ToneRules* tr = toneRules(w.content());
    const std::string tone = d.continuous ? std::string() : toneOf(w, c, targetChar);
    bool received = true;
    if (d.chance.set) {
        double p = chanceOf(w, c, d, targetChar);
        if (tr && !tone.empty() && targetChar)
            p = std::clamp(p + toneShift(*tr, tone, d, c, *targetChar), d.chance.min, d.chance.max);
        Rng r = w.rng(c.id, "chance:" + d.id);
        success = r.uniform() < p;
    } else if (tr && !tone.empty() && targetChar && receptionRolls(*tr, d)) {
        Rng r = w.rng(c.id, "reception:" + d.id);
        received = success = r.uniform() < receptionChance(w, *tr, c, d, *targetChar, tone);
    }
    const std::vector<EntityId> wit = witnessesOf(w, c, d, targetChar);
    // a crafting act's eff is what the planner expects; the resolver decides
    // what really comes of it (craftAct, called by the FSM with the step's params)
    if (d.craft.empty()) applyEffectList(w, c, success ? takeWhatFits(w, c, d, d.eff) : d.onFail, target, targetChar, &wit);
    if (!tone.empty() && tr) {
        const ToneDef& td = tr->tones.at(tone);
        applyEffectList(w, c, success ? td.eff : td.onFail, target, targetChar, &wit);
        if (!received) applyEffectList(w, c, tr->receptionOnFail, target, targetChar, &wit);
    }
    // Fight (FSM state of an act with a combat block): the combat core plays it
    // out; the outcome's effects, health, deaths, memory and chronicle follow.
    // More than two fighters (a pack, helpers who join) make a group fight,
    // resolved at once by the battle loop (group.json).
    if (success && d.combat.set && targetChar && targetChar->body.alive && c.body.alive) {
        const Sides sides = gatherSides(w, c, d, *targetChar);
        if (sides.group()) {
            const GroupResolution gr = resolveGroupCombat(w, sides, d);
            if (gr.ran) applyGroupResult(w, sides, d, gr, target, wit);
        } else {
            const CombatResolution cr = resolveCombat(w, c, d, *targetChar);
            if (cr.ran) applyCombatResult(w, c, *targetChar, d, cr, target, wit);
        }
    }
    for (const auto& rk : d.risk) {
        Rng r = w.rng(c.id, "risk:" + d.id + ":" + rk.id);
        if (r.uniform() < rk.p) applyEffectList(w, c, rk.eff, target, targetChar, &wit);
    }
    // A norm broken in front of witnesses is remembered by them (rumours, courts).
    const auto tags = legalTags(w, c, d, target, targetChar);
    if (!wit.empty() && !tags.empty())
        for (EntityId id : wit) {
            Character* wc = w.character(id);
            if (!wc) continue;
            MemoryEntry m;
            m.type = "saw_crime";
            m.who = {c.id};
            if (targetChar && !targetChar->isAnimal()) m.who.push_back(targetChar->id);
            m.object = target ? target->id : (targetChar && targetChar->isAnimal() ? targetChar->id : kNone);
            m.goods = deedGoods(d);
            m.valence = -0.3 * tags.front().mult;
            w.remember(*wc, std::move(m));
        }
    // AL-82: at the household's own larder (a store_* act), what else of food
    // keeps is put away too, but for a couple for the road: otherwise each
    // smoke-and-store leaves a fish in the satchel and the store runs dry while
    // one member carries fifty.
    if (success && target && target->type == "storage" && c.status.family != kNone &&
        target->ownerFamily == c.status.family && d.id.rfind("store_", 0) == 0) {
        constexpr int kKeep = 2;
        for (auto it = c.inventory.begin(); it != c.inventory.end(); ++it) {
            auto def = w.content().items.find(it->first);
            if (def == w.content().items.end() || def->second.nutrition <= 0.0) continue;
            if (def->second.spoilDays > 0.0 && def->second.spoilDays < 30.0) continue; // fresh food is eaten, not kept
            if (it->second <= kKeep) continue;
            target->stock[it->first] += it->second - kKeep;
            it->second = kKeep;
        }
    }
    // ...and the world answers: repute, the owners told, a trail, rumour, the outcome (AL-31).
    afterDeed(w, c, d, target, targetChar, success, tags, wit);
    villageAfterAct(w, c, d, targetChar, success); // AL-39: courtship, wedding, conception, quarrels
}

EntityId findTarget(const World& w, const Character& c, const InteractionDef& d, Vec2 from, std::string* why,
                    bool ignoreReservations) {
    if (d.targetsCreature()) {
        const EntityId t = findCreature(w, d.targetType, from, w.content().config.trackRadiusM, c.status.family);
        if (t == kNone && why) *why = "unknown:" + d.targetType;
        return t;
    }
    if (!d.targetsObject()) return kNone;
    EntityId best = kNone;
    double bestD = 1e18;
    bool anyOfType = false, sawReserved = false;
    std::string lastWhy;
    for (EntityId id : w.objectsOfType(d.targetType)) {
        if (!c.knownObjects.count(id)) continue;
        const WorldObject* o = w.object(id);
        if (!o) continue;
        anyOfType = true;
        if (!w.inSeason(*o)) {
            lastWhy = "season:" + d.targetType;
            continue;
        }
        if (!ignoreReservations && o->reservedFor(c.id, w.now())) {
            lastWhy = "reserved:" + d.targetType;
            sawReserved = true;
            continue;
        }
        // Target-side conditions only; actor conditions (has:, knows:) are the
        // planner's business.
        const EvalCtx x{w, c, o, nullptr};
        bool ok = true;
        for (const auto& k : d.pre) {
            if (k.subject != "target_stock" && k.subject != "target_owner" && k.subject != "farm" && k.subject != "site")
                continue;
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
    // Held by someone else is the reason worth reporting: it passes (the caller waits).
    if (best == kNone && why)
        *why = sawReserved ? "reserved:" + d.targetType : (anyOfType ? lastWhy : "unknown:" + d.targetType);
    return best;
}

} // namespace altera::sim
