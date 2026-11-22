// Social chains (AL-40): see social_chain.hpp.

#include "sim/social_chain.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "sim/dialogue_say.hpp"
#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/content.hpp"
#include "sim/tones.hpp"
#include "sim/world.hpp"

namespace altera::sim {

namespace fs = std::filesystem;

namespace {

std::vector<Effect> effects(const Json& j, const char* key) {
    std::vector<Effect> out;
    for (const auto& e : j.value(key, std::vector<std::string>{})) out.push_back(parseEffect(e));
    return out;
}

SocialChainDef parseChain(const std::string& id, const Json& j) {
    SocialChainDef sc;
    sc.id = id;
    const Json a = j.value("accept", Json::object());
    sc.accept.base = a.value("base", sc.accept.base);
    sc.accept.kin = a.value("kin", 0.0);
    sc.accept.spouse = a.value("spouse", 0.0);
    sc.accept.mood = a.value("mood", 0.0);
    sc.accept.min = a.value("min", sc.accept.min);
    sc.accept.max = a.value("max", sc.accept.max);
    sc.accept.rel = a.value("rel", std::map<std::string, double>{});
    sc.accept.needs = a.value("needs", std::map<std::string, double>{});
    sc.accept.traits = a.value("traits", std::map<std::string, double>{});
    sc.forbid = j.value("forbid", std::vector<std::string>{});
    sc.minFamiliarity = j.value("min_familiarity", -1.0);
    sc.place = j.value("place", std::string());
    sc.onRefuse = effects(j, "on_refuse");
    sc.onAccept = effects(j, "on_accept");
    for (const auto& s : j.value("stages", Json::array())) {
        SocialStage st;
        st.state = s.value("state", st.state);
        st.anim = s.value("anim", std::string());
        st.actorAnim = s.value("actor_anim", st.anim);
        st.targetAnim = s.value("target_anim", st.anim);
        st.ticks = s.value("ticks", Tick{0});
        st.eff = effects(s, "eff");
        sc.stages.push_back(std::move(st));
    }
    if (j.contains("follow")) {
        const Json f = j["follow"];
        sc.follow = true;
        sc.followTicks = f.value("ticks", sc.followTicks);
        sc.playerLeaderTicks = f.value("player_leader_ticks", sc.playerLeaderTicks);
        sc.joinMoodlet = f.value("join_moodlet", std::string());
    }
    if (!sc.follow && sc.stages.empty()) throw std::runtime_error("social_chains.json: chain '" + id + "' has no stages");
    return sc;
}

Character* partnerOf(World& w, const Character& c) {
    Character* p = w.character(c.ai.social.partner);
    return p && p->id != c.id ? p : nullptr;
}

bool unable(const World& w, const Character& c) {
    return !c.body.alive || c.body.downUntil > w.now() || c.captivity.held() || c.ai.fsm.state == "Fight";
}

bool closeKin(const Character& a, const Character& b) {
    if (a.status.mother == b.id || a.status.father == b.id || b.status.mother == a.id || b.status.father == a.id)
        return true;
    const bool sameMother = a.status.mother != kNone && a.status.mother == b.status.mother;
    const bool sameFather = a.status.father != kNone && a.status.father == b.status.father;
    return sameMother || sameFather;
}

void show(Character& c, const std::string& state, const std::string& act, EntityId target, const std::string& anim,
          Tick ticks, Tick now) {
    c.ai.fsm.state = state;
    c.ai.fsm.interaction = act;
    c.ai.fsm.target = target;
    c.ai.fsm.dest = c.pos;
    c.ai.fsm.timer = ticks;
    c.ai.fsm.started = now;
    c.ai.fsm.anim = anim;
}

// A critical need (any goal's interrupt) breaks a long wait or a follow.
bool criticalNeed(const World& w, const Character& c) {
    const EvalCtx x{w, c, nullptr, nullptr};
    for (const auto& g : w.content().goals)
        if (g.hasInterrupt && evalCondition(x, g.interrupt)) return true;
    return false;
}

void stepTowards(Character& c, Vec2 to, double maxStep) {
    const double d = dist(c.pos, to);
    if (d <= maxStep || d < 1e-9) c.pos = to;
    else {
        c.pos.x += (to.x - c.pos.x) * maxStep / d;
        c.pos.y += (to.y - c.pos.y) * maxStep / d;
    }
}

// The point `gap` metres short of `to` on the way from `from`.
Vec2 shortOf(Vec2 from, Vec2 to, double gap) {
    const double d = dist(from, to);
    if (d <= gap || d < 1e-9) return from;
    return {to.x + (from.x - to.x) * gap / d, to.y + (from.y - to.y) * gap / d};
}

// Clears one side: the FSM back to Idle, a follow put aside comes back.
void release(World& w, Character& c) {
    AiState& ai = c.ai;
    if (WorldObject* o = w.object(ai.social.place); o && o->reservedBy == c.id) o->reservedBy = kNone;
    ai.social = SocialLink{};
    ai.fsm = FsmState{};
    ai.nextThink = w.now();
    if (ai.followAfter.active()) {
        ai.social = ai.followAfter;
        ai.followAfter = SocialLink{};
    }
}

Tick stageTicks(const SocialStage& s, const InteractionDef& d) {
    return std::max<Tick>(1, s.ticks > 0 ? s.ticks : d.duration);
}

void startStage(World& w, Character& a, Character& t, const SocialRules& R, const SocialChainDef& sc,
                const InteractionDef& d, int i) {
    const SocialStage& s = sc.stages[static_cast<size_t>(i)];
    const Tick ticks = stageTicks(s, d);
    a.ai.social.phase = t.ai.social.phase = "stage";
    a.ai.social.stage = t.ai.social.stage = i;
    a.ai.social.until = t.ai.social.until = w.now() + ticks;
    // Side by side for the act (they are within reach already).
    if (i == 0 && dist(a.pos, t.pos) > R.togetherM) a.pos = shortOf(a.pos, t.pos, R.togetherM * 0.5);
    show(a, s.state, sc.id, t.id, s.actorAnim, ticks, w.now());
    show(t, s.state, sc.id, a.id, s.targetAnim, ticks, w.now());
    trace(w, a, 'F', "social.stage", {{"action", sc.id}, {"stage", i}, {"who", t.id}, {"anim", s.actorAnim}});
}

// The place the pair goes to: of the actor's or the target's household,
// free, nearest to the actor.
EntityId findPlace(const World& w, const Character& a, const Character& t, const SocialRules& R,
                   const std::string& type) {
    EntityId best = kNone;
    double bd = R.placeRadiusM;
    for (const auto& [id, o] : w.objects()) {
        if (o.type != type) continue;
        if (o.ownerFamily != kNone && o.ownerFamily != a.status.family && o.ownerFamily != t.status.family) continue;
        if (o.reservedFor(a.id, w.now()) || o.reservedFor(t.id, w.now())) continue;
        const double d = dist(a.pos, o.pos);
        if (d < bd) {
            bd = d;
            best = id;
        }
    }
    return best;
}

// The actor's side ends; the target (and a place) are let go too.
void closePair(World& w, Character& a, Character* t) {
    if (t && t->ai.social.active() && t->ai.social.partner == a.id && t->ai.social.role == "target") release(w, *t);
    release(w, a);
}

void decide(World& w, Character& a, Character& t, const SocialRules& R, const SocialChainDef& sc,
            const InteractionDef& d, bool accept) {
    SocialLink& al = a.ai.social;
    SocialLink& tl = t.ai.social;
    if (!accept) {
        applyEffectList(w, a, sc.onRefuse, nullptr, &t, nullptr);
        w.remember(a, MemoryEntry{0, "was_refused", {t.id}, kNone, -0.2, 0.0, "seen", 1.0});
        al.phase = "refused";
        tl.phase = "refused";
        al.until = tl.until = w.now() + R.refuseTicks;
        show(a, "Talk", sc.id, t.id, R.rejectedAnim, R.refuseTicks, w.now());
        show(t, "Talk", sc.id, a.id, R.refuseAnim, R.refuseTicks, w.now());
        trace(w, a, 'C', "social.refused", {{"action", sc.id}, {"who", t.id}});
        trace(w, t, 'C', "social.said_no", {{"action", sc.id}, {"who", a.id}});
        sayRefusal(w, t, a, "consent", "interaction." + d.id + ".name"); // AL-36: aloud, with the reason
        return;
    }
    applyEffectList(w, a, sc.onAccept, nullptr, &t, nullptr);
    trace(w, a, 'C', "social.accepted", {{"action", sc.id}, {"who", t.id}});
    trace(w, t, 'C', "social.said_yes", {{"action", sc.id}, {"who", a.id}});
    if (sc.follow) {
        // The target follows the actor from now on; the actor's act is done.
        tl = SocialLink{};
        tl.chain = sc.id;
        tl.role = "follower";
        tl.partner = a.id;
        tl.phase = "follow";
        tl.since = w.now();
        tl.expires = w.now() + (a.controller.isPlayer() ? sc.playerLeaderTicks : sc.followTicks);
        t.ai.fsm = FsmState{};
        al.phase = "done";
        return;
    }
    if (!sc.place.empty()) {
        const EntityId p = findPlace(w, a, t, R, sc.place);
        if (p == kNone) {
            al.phase = "broken";
            al.result = "no_place:" + sc.place;
            return;
        }
        WorldObject* o = w.object(p);
        o->reservedBy = a.id;
        o->reservedUntil = w.now() + R.approachMaxTicks + 24;
        al.place = tl.place = p;
        al.phase = tl.phase = "approach";
        al.until = tl.until = w.now() + R.approachMaxTicks;
        show(a, "Move", sc.id, p, "", R.approachMaxTicks, w.now());
        show(t, "Move", sc.id, p, "", R.approachMaxTicks, w.now());
        trace(w, a, 'F', "social.approach", {{"action", sc.id}, {"who", t.id}, {"object", p}});
        return;
    }
    startStage(w, a, t, R, sc, d, 0);
}

} // namespace

const SocialRules* socialRules(const Content& c) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<SocialRules>> cache;
    if (c.dir.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(c.dir);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<SocialRules> r;
    const fs::path p = fs::path(c.dir) / "social_chains.json";
    if (fs::exists(p)) {
        r = std::make_unique<SocialRules>();
        std::ifstream f(p, std::ios::binary);
        const Json j = Json::parse(f);
        const Json cfg = j.value("config", Json::object());
        r->askTicks = cfg.value("ask_ticks", r->askTicks);
        r->refuseTicks = cfg.value("refuse_ticks", r->refuseTicks);
        r->answerTicks = cfg.value("answer_ticks", r->answerTicks);
        r->approachMaxTicks = cfg.value("approach_max_ticks", r->approachMaxTicks);
        r->refusedCooldown = cfg.value("refused_cooldown_ticks", r->refusedCooldown);
        r->placeRadiusM = cfg.value("place_radius_m", r->placeRadiusM);
        r->togetherM = cfg.value("together_m", r->togetherM);
        r->followM = cfg.value("follow_m", r->followM);
        r->followLostM = cfg.value("follow_lost_m", r->followLostM);
        r->followSpeedMult = cfg.value("follow_speed_mult", r->followSpeedMult);
        r->joinRadiusM = cfg.value("join_radius_m", r->joinRadiusM);
        r->askAnim = cfg.value("ask_anim", r->askAnim);
        r->listenAnim = cfg.value("listen_anim", r->listenAnim);
        r->refuseAnim = cfg.value("refuse_anim", r->refuseAnim);
        r->rejectedAnim = cfg.value("rejected_anim", r->rejectedAnim);
        const Json chains = j.value("chains", Json::object());
        for (auto ci = chains.begin(); ci != chains.end(); ++ci) {
            const InteractionDef* d = c.interaction(ci.key());
            if (!d) throw std::runtime_error("social_chains.json: unknown act '" + ci.key() + "'");
            if (!d->targetsCharacter())
                throw std::runtime_error("social_chains.json: act '" + ci.key() + "' does not target a person");
            SocialChainDef sc = parseChain(ci.key(), ci.value());
            if (!sc.place.empty() && !c.objects.count(sc.place))
                throw std::runtime_error("social_chains.json: chain '" + ci.key() + "' names unknown place '" +
                                         sc.place + "'");
            r->chains[ci.key()] = std::move(sc);
        }
    }
    return (cache[c.dir] = std::move(r)).get();
}

const SocialChainDef* socialChain(const Content& c, const std::string& act) {
    const SocialRules* R = socialRules(c);
    if (!R) return nullptr;
    auto it = R->chains.find(act);
    return it == R->chains.end() ? nullptr : &it->second;
}

std::string socialBlock(const World& w, const Character& actor, const Character& target, const SocialChainDef& sc) {
    if (&actor == &target) return "self";
    if (target.isAnimal()) return "not_a_person";
    for (const auto& f : sc.forbid) {
        if (f == "close_kin" && closeKin(actor, target)) return "forbid:close_kin";
        if (f == "child" && (actor.body.stage() == "child" || target.body.stage() == "child")) return "forbid:child";
    }
    if (sc.minFamiliarity >= 0.0) {
        auto rel = target.relations.find(actor.id);
        const double fam = rel == target.relations.end() ? 0.0 : rel->second.familiarity;
        if (fam < sc.minFamiliarity && target.status.family != actor.status.family) return "stranger";
    }
    if (unable(w, target)) return "busy:unable";
    const SocialLink& tl = target.ai.social;
    // Following the very actor is no obstacle: the follow waits for the act.
    if (tl.active() && !(tl.role == "follower" && tl.partner == actor.id && tl.phase == "follow")) return "busy:engaged";
    const std::string& st = target.ai.fsm.state;
    if (st != "Idle" && !w.content().fsmInterruptible(st)) return "busy:" + st;
    return "";
}

double socialAcceptChance(const World& w, const Character& actor, const Character& target, const InteractionDef& d,
                          const SocialChainDef& sc, const std::string& tone) {
    if (!target.body.alive) return 0.0;
    const SocialAccept& A = sc.accept;
    double p = A.base;
    auto rel = target.relations.find(actor.id);
    if (rel != target.relations.end())
        for (const auto& [axis, k] : A.rel) p += k * rel->second.get(axis);
    if (target.status.family != kNone && target.status.family == actor.status.family) p += A.kin;
    if (target.status.spouse == actor.id) p += A.spouse;
    p += A.mood * target.mood(w.now());
    for (const auto& [need, k] : A.needs) p += k * (1.0 - target.need(need));
    for (const auto& [trait, k] : A.traits)
        if (target.hasTrait(trait)) p += k;
    if (const ToneRules* tr = toneRules(w.content()); tr && !tone.empty()) p += toneShift(*tr, tone, d, actor, target);
    return std::clamp(p, A.min, A.max);
}

bool beginSocial(World& w, Character& actor, Character& target, const InteractionDef& d, std::string& why) {
    const SocialRules* R = socialRules(w.content());
    const SocialChainDef* sc = socialChain(w.content(), d.id);
    if (!R || !sc) return why = "social.no_chain", false;
    if (const std::string b = socialBlock(w, actor, target, *sc); !b.empty()) {
        why = "refused:" + d.id + ":" + b;
        trace(w, actor, 'C', "social.blocked", {{"action", sc->id}, {"who", target.id}, {"why", b}});
        return false;
    }
    // An NPC does not pester somebody who just said no.
    if (!actor.controller.isPlayer())
        for (const auto& m : actor.memory)
            if (m.type == "was_refused" && !m.who.empty() && m.who[0] == target.id &&
                w.now() - m.tick < R->refusedCooldown) {
                why = "refused:" + d.id + ":recently";
                return false;
            }
    if (actor.ai.social.active()) actor.ai.followAfter = actor.ai.social; // a player follower acting
    if (target.ai.social.active()) target.ai.followAfter = target.ai.social; // follows this actor (socialBlock)
    // The target stops what it is doing; its plan keeps its place.
    if (target.ai.fsm.state != "Idle") abortAction(w, target, "social:" + d.id);
    releaseReservations(w, target);
    SocialLink al;
    al.chain = sc->id;
    al.role = "actor";
    al.partner = target.id;
    al.phase = "ask";
    al.since = w.now();
    al.until = w.now() + R->askTicks;
    al.tone = toneOf(w, actor, &target);
    SocialLink tl = al;
    tl.role = "target";
    tl.partner = actor.id;
    actor.ai.social = al;
    target.ai.social = tl;
    show(actor, "Talk", sc->id, target.id, R->askAnim, R->askTicks, w.now());
    show(target, "Talk", sc->id, actor.id, R->listenAnim, R->askTicks, w.now());
    trace(w, actor, 'C', "social.ask", {{"action", sc->id}, {"who", target.id}});
    trace(w, target, 'C', "social.asked", {{"action", sc->id}, {"who", actor.id}});
    return true;
}

namespace {

// The follower's tick. None leaves a player follower to the player.
SocialStep tickFollower(World& w, Character& f, const SocialRules& R, std::string& why) {
    SocialLink& fl = f.ai.social;
    Character* leader = partnerOf(w, f);
    std::string end;
    if (!leader || unable(w, *leader)) end = "leader_gone";
    else if (w.now() >= fl.expires) end = "invitation_over";
    else if (leader->ai.fsm.state == "Sleep") end = "leader_sleeps";
    else if (dist(f.pos, leader->pos) > R.followLostM) end = "lost";
    else if (!f.controller.isPlayer() && fl.phase == "follow" && criticalNeed(w, f)) end = "own_need";
    if (!end.empty()) {
        trace(w, f, 'C', "social.follow_end", {{"who", fl.partner}, {"why", end}});
        release(w, f);
        why = end;
        return f.controller.isPlayer() ? SocialStep::None : SocialStep::Running;
    }
    const AiConfig& cfg = w.content().config;
    if (fl.phase == "join") {
        const InteractionDef* d = w.content().interaction(f.ai.fsm.interaction);
        WorldObject* o = w.object(f.ai.fsm.target);
        if (d && d->continuous) applyEffects(w, f, *d, o, nullptr);
        if (w.now() < fl.until) return SocialStep::Running;
        if (d && !d->continuous) applyEffects(w, f, *d, o, nullptr);
        if (o && o->reservedBy == f.id) o->reservedBy = kNone;
        trace(w, f, 'F', "social.joined_done", {{"action", d ? d->id : std::string()}, {"who", leader->id}});
        fl.phase = "follow";
        f.ai.fsm = FsmState{};
        return SocialStep::Running;
    }
    // A player follower walks on its own once the player gives orders or steers.
    if (f.controller.isPlayer() && (f.ai.goal.valid() || w.now() - f.controller.idleSince < 2)) return SocialStep::None;
    // Join what the leader does (not a person-to-person act: those are chains of their own).
    const FsmState& lf = leader->ai.fsm;
    const InteractionDef* ld = w.content().interaction(lf.interaction);
    if (ld && lf.state != "Idle" && lf.state != "Move" && lf.state != "Wait" && !leader->ai.social.active() &&
        !ld->targetsCharacter() && !ld->targetsCreature() && !ld->targetsCarried()) {
        WorldObject* pick = nullptr;
        bool can = false;
        if (ld->targetsObject()) {
            WorldObject* lo = w.object(lf.target);
            if (lo && !lo->reservedFor(f.id, w.now()) && canPerform(w, f, *ld, lo, nullptr, false)) pick = lo;
            else {
                double bd = R.joinRadiusM;
                for (auto& [id, o] : w.objects()) {
                    if (o.type != ld->targetType || o.reservedFor(f.id, w.now())) continue;
                    const double d = dist(o.pos, leader->pos);
                    if (d < bd && canPerform(w, f, *ld, &o, nullptr, false)) {
                        bd = d;
                        pick = &o;
                    }
                }
            }
            can = pick != nullptr;
        } else {
            can = canPerform(w, f, *ld, nullptr, nullptr, false);
        }
        if (can) {
            const Vec2 at = pick ? pick->pos : leader->pos;
            f.pos = shortOf(f.pos, at, std::min(R.followM, reachOf(w, *ld) * 0.5));
            if (pick && ld->reserve) {
                pick->reservedBy = f.id;
                pick->reservedUntil = w.now() + ld->duration + 2;
            }
            fl.phase = "join";
            fl.until = w.now() + std::max<Tick>(1, ld->duration);
            show(f, ld->fsm, ld->id, pick ? pick->id : kNone, "", ld->duration, w.now());
            if (const SocialChainDef* sc = socialChain(w.content(), fl.chain);
                sc && !sc->joinMoodlet.empty() && w.content().moodlets.count(sc->joinMoodlet)) {
                const std::vector<Effect> m{parseEffect("moodlet:" + sc->joinMoodlet),
                                            parseEffect("t.moodlet:" + sc->joinMoodlet)};
                applyEffectList(w, f, m, nullptr, leader, nullptr);
            }
            trace(w, f, 'F', "social.join", {{"action", ld->id}, {"who", leader->id}});
            return SocialStep::Running;
        }
    }
    // Keep up with the leader, a step behind.
    const double d = dist(f.pos, leader->pos);
    if (d > R.followM) {
        const Vec2 to = shortOf(f.pos, leader->pos, R.followM);
        stepTowards(f, to, cfg.walkMPerTick * R.followSpeedMult);
        show(f, "Move", fl.chain, leader->id, "", 1, w.now());
    } else if (f.ai.fsm.state != "Idle") {
        f.ai.fsm = FsmState{};
    }
    return SocialStep::Running;
}

} // namespace

SocialStep tickSocial(World& w, Character& c, std::string& why) {
    SocialLink& l = c.ai.social;
    if (!l.active()) return SocialStep::None;
    const SocialRules* R = socialRules(w.content());
    const SocialChainDef* sc = socialChain(w.content(), l.chain);
    const InteractionDef* d = w.content().interaction(l.chain);
    if (!R || !sc || !d) {
        why = "social.no_chain";
        const bool actor = l.role == "actor";
        release(w, c);
        return actor ? SocialStep::Failed : SocialStep::None;
    }
    if (l.role == "follower") return tickFollower(w, c, *R, why);
    if (l.role == "target") {
        // Held by the actor's side; a target left alone (a load, a lost
        // actor) lets go.
        Character* a = partnerOf(w, c);
        if (!a || !a->ai.social.active() || a->ai.social.partner != c.id) {
            release(w, c);
            return SocialStep::None;
        }
        // A critical need or harm breaks a long wait for the answer or the walk.
        if (unable(w, c) || ((l.phase == "approach") && !c.controller.isPlayer() && criticalNeed(w, c))) {
            endSocial(w, c, "target_interrupted");
            return SocialStep::None;
        }
        return SocialStep::Running;
    }
    // ---- the actor's side drives the pair
    if (l.phase == "broken") {
        why = l.result.empty() ? "social.broken" : l.result;
        if (l.result.rfind("no_place", 0) == 0) trace(w, c, 'C', "social.no_place", {{"action", l.chain}, {"object", sc->place}});
        Character* t = partnerOf(w, c);
        closePair(w, c, t);
        return SocialStep::Failed;
    }
    if (l.phase == "done") {
        Character* t = partnerOf(w, c);
        if (t) applyEffects(w, c, *d, nullptr, t);
        release(w, c);
        return SocialStep::Done;
    }
    Character* t = partnerOf(w, c);
    if (!t || unable(w, *t) || t->ai.social.partner != c.id || t->ai.social.role != "target") {
        why = "social.partner_gone";
        closePair(w, c, t);
        return SocialStep::Failed;
    }
    if (unable(w, c)) {
        why = "social.actor_unable";
        closePair(w, c, t);
        return SocialStep::Failed;
    }
    const AiConfig& cfg = w.content().config;
    if (l.phase == "ask") {
        if (w.now() < l.until) return SocialStep::Running;
        if (t->controller.isPlayer()) {
            // The player answers (answer_social); silence = the character's own mind.
            l.phase = t->ai.social.phase = "answer";
            l.until = t->ai.social.until = w.now() + R->answerTicks;
            trace(w, *t, 'C', "social.await_answer", {{"action", l.chain}, {"who", c.id}});
            return SocialStep::Running;
        }
        Rng r = agentRng(w.seed(), t->id, w.now(), "social:" + l.chain + ":" + std::to_string(c.id));
        decide(w, c, *t, *R, *sc, *d, r.uniform() < socialAcceptChance(w, c, *t, *d, *sc, l.tone));
        return l.phase == "done" || l.phase == "broken" ? tickSocial(w, c, why) : SocialStep::Running;
    }
    if (l.phase == "answer") {
        if (w.now() < l.until) return SocialStep::Running;
        Rng r = agentRng(w.seed(), t->id, w.now(), "social:" + l.chain + ":" + std::to_string(c.id));
        decide(w, c, *t, *R, *sc, *d, r.uniform() < socialAcceptChance(w, c, *t, *d, *sc, l.tone));
        return l.phase == "done" || l.phase == "broken" ? tickSocial(w, c, why) : SocialStep::Running;
    }
    if (l.phase == "refused") {
        if (w.now() < l.until) return SocialStep::Running;
        why = "refused:" + l.chain;
        closePair(w, c, t);
        return SocialStep::Failed;
    }
    if (l.phase == "approach") {
        WorldObject* o = w.object(l.place);
        if (!o) {
            why = "social.place_gone";
            closePair(w, c, t);
            return SocialStep::Failed;
        }
        stepTowards(c, o->pos, cfg.walkMPerTick);
        stepTowards(*t, shortOf(t->pos, o->pos, R->togetherM * 0.5), cfg.walkMPerTick);
        c.ai.fsm.dest = o->pos;
        t->ai.fsm.dest = o->pos;
        if (dist(c.pos, o->pos) <= R->togetherM && dist(t->pos, o->pos) <= R->togetherM) {
            trace(w, c, 'F', "social.arrived", {{"action", l.chain}, {"object", l.place}});
            startStage(w, c, *t, *R, *sc, *d, 0);
            return SocialStep::Running;
        }
        if (w.now() >= l.until) {
            why = "social.approach_timeout";
            closePair(w, c, t);
            return SocialStep::Failed;
        }
        return SocialStep::Running;
    }
    if (l.phase == "stage") {
        if (w.now() < l.until) return SocialStep::Running;
        const SocialStage& s = sc->stages[static_cast<size_t>(l.stage)];
        applyEffectList(w, c, s.eff, w.object(l.place), t, nullptr);
        if (l.stage + 1 < static_cast<int>(sc->stages.size())) {
            startStage(w, c, *t, *R, *sc, *d, l.stage + 1);
            return SocialStep::Running;
        }
        // The act itself: its catalog effects, chance, risks, witnesses.
        applyEffects(w, c, *d, nullptr, t);
        trace(w, c, 'F', "social.done", {{"action", l.chain}, {"who", t->id}});
        closePair(w, c, t);
        return SocialStep::Done;
    }
    why = "social.bad_phase:" + l.phase;
    closePair(w, c, t);
    return SocialStep::Failed;
}

void endSocial(World& w, Character& c, const std::string& why) {
    SocialLink& l = c.ai.social;
    if (!l.active()) return;
    trace(w, c, 'C', "social.broken", {{"action", l.chain}, {"why", why}});
    if (l.role == "follower") {
        release(w, c);
        return;
    }
    Character* p = partnerOf(w, c);
    if (l.role == "actor") {
        closePair(w, c, p);
        return;
    }
    // The target breaks it: the actor fails at its next tick.
    if (p && p->ai.social.active() && p->ai.social.partner == c.id && p->ai.social.role == "actor") {
        p->ai.social.phase = "broken";
        p->ai.social.result = "social.broken:" + why;
        if (WorldObject* o = w.object(p->ai.social.place); o && o->reservedBy == p->id) o->reservedBy = kNone;
        p->ai.fsm = FsmState{};
    }
    release(w, c);
}

bool answerSocial(World& w, Character& t, bool accept, std::string& why) {
    SocialLink& tl = t.ai.social;
    if (!tl.active() || tl.role != "target" || tl.phase != "answer") return why = "nothing_to_answer", false;
    Character* a = partnerOf(w, t);
    const SocialRules* R = socialRules(w.content());
    const SocialChainDef* sc = socialChain(w.content(), tl.chain);
    const InteractionDef* d = w.content().interaction(tl.chain);
    if (!a || !R || !sc || !d || a->ai.social.partner != t.id) return why = "asker_gone", false;
    trace(w, t, 'C', "social.player_answer", {{"action", tl.chain}, {"accept", accept}});
    decide(w, *a, t, *R, *sc, *d, accept);
    return true;
}

std::vector<EntityId> followersOf(const World& w, EntityId leader) {
    std::vector<EntityId> out;
    for (const auto& [id, c] : w.characters()) {
        const SocialLink& l = c.ai.social.role == "follower" ? c.ai.social : c.ai.followAfter;
        if (l.active() && l.role == "follower" && l.partner == leader && c.body.alive) out.push_back(id);
    }
    return out;
}

void pullFollowers(World& w, const Character& leader) {
    const SocialRules* R = socialRules(w.content());
    if (!R) return;
    for (auto& [id, f] : w.characters()) {
        SocialLink& l = f.ai.social;
        if (!l.active() || l.role != "follower" || l.partner != leader.id || l.phase != "follow") continue;
        if (unable(w, f) || (f.controller.isPlayer() && f.ai.goal.valid())) continue;
        if (dist(f.pos, leader.pos) > R->followM && dist(f.pos, leader.pos) <= R->followLostM) {
            f.pos = shortOf(f.pos, leader.pos, R->followM);
            f.ai.fsm.dest = f.pos;
        }
    }
}

namespace {

int playerOfCmd(const Command& cmd) {
    if (cmd.args.contains("player")) return cmd.args["player"].get<int>();
    if (cmd.source.rfind("player:", 0) == 0) return std::stoi(cmd.source.substr(7));
    return 0;
}

bool handle(World& w, Command& cmd) {
    const std::string& t = cmd.type;
    if (t != "answer_social" && t != "dismiss_follower") return false;
    Character* c = w.character(cmd.args.at("id").get<EntityId>());
    if (!c) throw std::runtime_error("no such character");
    if (!c->controller.isPlayer() || c->controller.playerId != playerOfCmd(cmd))
        throw std::runtime_error("not controlled by player");
    std::string why;
    if (t == "answer_social") {
        if (!answerSocial(w, *c, cmd.args.at("accept").get<bool>(), why)) cmd.result = "rejected:" + why;
        return true;
    }
    // dismiss_follower: the leader lets one (or every) follower go; a
    // follower calling it on itself stops following.
    const EntityId only = cmd.args.value("follower", kNone);
    int n = 0;
    if (c->ai.social.role == "follower" && only == kNone) {
        endSocial(w, *c, "stopped_following");
        ++n;
    }
    for (EntityId id : followersOf(w, c->id)) {
        if (only != kNone && id != only) continue;
        Character* f = w.character(id);
        if (f->ai.social.role == "follower") endSocial(w, *f, "dismissed");
        else f->ai.followAfter = SocialLink{};
        ++n;
    }
    if (n == 0) cmd.result = "rejected:no_followers";
    return true;
}

} // namespace

void installSocial(World& w) { w.addCommandHandler(handle); }

} // namespace altera::sim
