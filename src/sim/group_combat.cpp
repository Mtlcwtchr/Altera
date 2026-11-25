#include "sim/group_combat.hpp"

#include <algorithm>
#include <cmath>

#include "sim/aftermath.hpp"
#include "sim/ai.hpp"
#include "sim/character.hpp"
#include "sim/combat_bridge.hpp"
#include "sim/world.hpp"

namespace lr::sim {

GroupRules loadGroupRules(const Json& j, const Content& c, std::vector<std::string>& err) {
    GroupRules r;
    r.enabled = j.value("enabled", true);
    r.joinRadiusM = j.value("join_radius_m", r.joinRadiusM);
    r.maxSide = j.value("max_side", r.maxSide);
    r.packHeadsMax = j.value("pack_heads_max", r.packHeadsMax);
    r.kinJoin = j.value("kin_join", r.kinJoin);
    r.friendAffinity = j.value("friend_affinity", r.friendAffinity);
    r.villageVsBeasts = j.value("village_vs_beasts", r.villageVsBeasts);
    r.joinActs = j.value("join_acts", std::vector<std::string>{});
    r.beastAttack = j.value("beast_attack", r.beastAttack);
    r.beastAct = j.value("beast_act", r.beastAct);
    r.winSamples = j.value("win_samples", r.winSamples);
    r.chronicle = j.value("chronicle", r.chronicle);
    if (r.maxSide < 1) err.push_back("group.max_side: at least 1");
    if (r.packHeadsMax < 1) err.push_back("group.pack_heads_max: at least 1");
    if (r.winSamples < 1) err.push_back("group.win_samples: at least 1");
    for (const auto& a : r.joinActs) {
        const InteractionDef* d = c.interaction(a);
        if (!d || !d->combat.set) err.push_back("group.join_acts: '" + a + "' is not an act with a combat block");
    }
    const InteractionDef* ba = c.interaction(r.beastAct);
    if (!ba || !ba->combat.set) err.push_back("group.beast_act: '" + r.beastAct + "' is not an act with a combat block");
    return r;
}

namespace {

bool predator(const World& w, const Character& c) {
    if (!c.isAnimal()) return false;
    const SpeciesDef* sp = w.content().speciesOf(c.species);
    return sp && sp->diet == "prey";
}

bool kinOf(const Character& a, const Character& b) {
    return (a.status.family != kNone && a.status.family == b.status.family) ||
           (a.status.dynasty != kNone && a.status.dynasty == b.status.dynasty);
}

double affinity(const Character& from, const Character& to) {
    auto it = from.relations.find(to.id);
    return it == from.relations.end() ? 0.0 : it->second.affinity;
}

int headsOf(const GroupRules& R, const Character& c) {
    return c.isAnimal() ? std::clamp(c.group, 1, R.packHeadsMax) : 1;
}

void die(World& w, Character& who, const std::string& cause) {
    if (!who.body.alive) return;
    who.body.alive = false;
    who.body.health = 0.0;
    who.body.causeOfDeath = cause;
    w.chronicle("death", {who.id}, "chronicle.death", {{"who", who.id}, {"cause", cause}});
}

std::string mirror(const std::string& o) {
    if (o == "win") return "lose";
    if (o == "lose" || o == "flee") return "win";
    return "draw";
}

} // namespace

Sides gatherSides(World& w, Character& actor, const InteractionDef& d, Character& target) {
    Sides s;
    s.a.push_back(&actor);
    s.b.push_back(&target);
    const CombatBridge* b = combatBridge(w.content());
    if (!b || !b->group.enabled) {
        s.headsA = s.headsB = 1;
        return s;
    }
    const GroupRules& R = b->group;
    s.headsA = headsOf(R, actor);
    s.headsB = headsOf(R, target);
    const bool beasts = actor.isAnimal() || target.isAnimal();
    const bool joinAct = std::find(R.joinActs.begin(), R.joinActs.end(), d.id) != R.joinActs.end();
    if (!beasts && !joinAct) return s;
    const Tick now = w.now();
    // The side a bystander takes: kin or a friend of one side's people (the
    // stronger tie wins), the village against beasts that threaten one of its own.
    auto tie = [&](const Character& c, const std::vector<Character*>& side) {
        double t = 0.0;
        for (const Character* m : side) {
            if (m->isAnimal()) continue;
            if (R.kinJoin && kinOf(c, *m)) t = std::max(t, 1.0 + affinity(c, *m));
            const double a = affinity(c, *m);
            if (a >= R.friendAffinity) t = std::max(t, a);
            if (R.villageVsBeasts && c.status.settlement != kNone && c.status.settlement == m->status.settlement) {
                const auto& other = &side == &s.a ? s.b : s.a;
                const bool threat = std::any_of(other.begin(), other.end(), [&](const Character* o) { return predator(w, *o); });
                if (threat) t = std::max(t, 0.5);
            }
        }
        return t;
    };
    std::vector<std::pair<double, Character*>> cands;
    for (auto& [id, c] : w.characters()) {
        if (&c == &actor || &c == &target || !c.body.alive || c.isAnimal()) continue;
        if (c.body.downUntil > now || c.captivity.held() || c.body.stage() == "child") continue;
        if (c.ai.fsm.state == "Fight") continue; // busy in a fight of its own
        const double dd = std::min(dist(c.pos, actor.pos), dist(c.pos, target.pos));
        if (dd > R.joinRadiusM) continue;
        cands.push_back({dd, &c});
    }
    std::sort(cands.begin(), cands.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first < y.first : x.second->id < y.second->id;
    });
    for (const auto& [dd, c] : cands) {
        const double ta = tie(*c, s.a), tb = tie(*c, s.b);
        if (ta <= 0.0 && tb <= 0.0) continue;
        if (ta >= tb && s.headsA < R.maxSide) {
            s.a.push_back(c);
            ++s.headsA;
        } else if (tb > ta && s.headsB < R.maxSide) {
            s.b.push_back(c);
            ++s.headsB;
        }
    }
    return s;
}

GroupResolution resolveGroupCombat(const World& w, const Sides& s, const InteractionDef& d, std::uint64_t salt) {
    GroupResolution r;
    const CombatBridge* b = combatBridge(w.content());
    if (!b) return r;
    const combat::CombatData& cd = *b->data;
    std::uint64_t key = 0x9e3779b97f4a7c15ull ^ salt;
    for (const Character* c : s.a) key = key * 1099511628211ull ^ static_cast<std::uint64_t>(c->id);
    for (const Character* c : s.b) key = key * 1099511628211ull ^ (static_cast<std::uint64_t>(c->id) << 1);
    combat::Engine e(cd, combat::combatSeed(w.seed(), key, static_cast<std::uint64_t>(w.now())));
    std::vector<combat::Fighter> A, B;
    std::vector<GroupFighterResult> meta;
    std::vector<std::vector<double>> hpStart;
    auto add = [&](std::vector<combat::Fighter>& v, Character* c, int side) {
        const int heads = headsOf(b->group, *c);
        const std::string& skill = side == 0 ? d.combat.attackSkill : d.combat.defenseSkill;
        for (int h = 0; h < heads; ++h) {
            combat::Fighter f = e.make(fighterOf(*b, *c, skill), static_cast<std::int64_t>(c->id) * 16 + h, side);
            if (h == 0) carryWounds(cd, b->recovery, *c, f);
            hpStart.push_back(f.hp);
            v.push_back(std::move(f));
            GroupFighterResult m;
            m.who = c;
            m.head = h;
            m.side = side;
            meta.push_back(m);
        }
    };
    for (Character* c : s.a) add(A, c, 0);
    const std::size_t nA = meta.size();
    for (Character* c : s.b) add(B, c, 1);
    const combat::BattleResult br = e.battle(A, B);
    r.ran = true;
    r.rounds = br.rounds;
    for (std::size_t i = 0; i < meta.size(); ++i) {
        const combat::Fighter& f = i < nA ? A[i] : B[i - nA];
        GroupFighterResult& m = meta[i];
        m.state = combat::stateId(f.state);
        double lost = 0.0, total = 0.0;
        for (std::size_t z = 0; z < cd.zoneHp.size(); ++z) {
            total += cd.zoneHp[z];
            lost += std::max(0.0, std::min(hpStart[i][z], hpStart[i][z] - f.hp[z]));
        }
        m.health = total > 0 ? 1.0 - lost / total : 1.0;
        m.blood = f.blood;
        m.hits = woundHits(f);
        const bool fallen = f.state == combat::State::Dead || f.state == combat::State::Ko;
        if (f.fighting()) (m.side == 0 ? r.aliveA : r.aliveB)++;
        if (fallen) (m.side == 0 ? r.fallenA : r.fallenB)++;
        r.fighters.push_back(m);
    }
    const bool actorFled = !r.fighters.empty() && r.fighters[0].state == "flee";
    if (r.aliveA > 0 && r.aliveB == 0) r.outcome = "win";
    else if (actorFled) r.outcome = "flee";
    else if (r.aliveA == 0 && r.aliveB > 0) r.outcome = "lose";
    else r.outcome = "draw";
    return r;
}

void applyGroupResult(World& w, const Sides& s, const InteractionDef& d, const GroupResolution& r,
                      WorldObject* target, const std::vector<EntityId>& witnesses) {
    const CombatBridge& b = *combatBridge(w.content());
    Character& actor = *s.a.front();
    Character& foe = *s.b.front();
    // People: health, wounds, deaths. Packs: the heads that fell.
    std::map<Character*, int> headsLost;
    for (const auto& m : r.fighters) {
        Character& who = *m.who;
        if (!who.body.alive) continue;
        const bool dead = m.state == "dead";
        if (who.isAnimal()) {
            if (dead) ++headsLost[&who];
            continue;
        }
        if (dead && d.combat.lethal && !who.controller.isPlayer()) {
            die(w, who, "combat");
            continue;
        }
        const double before = who.body.health;
        who.body.health = std::max(b.healthMinAfter, who.body.health * m.health);
        if (dead) who.body.health = b.healthMinAfter;
        addCombatWounds(w, who, m.hits, dead ? std::max(m.blood, 1.0) : m.blood, before - who.body.health);
    }
    for (auto& [herd, n] : headsLost) loseHeads(w, *herd, std::min(n, herd->group), "combat");
    // The act's own outcome effects for the one who started it.
    auto out = d.combat.outcomes.find(r.outcome);
    if (out != d.combat.outcomes.end() && actor.body.alive) applyEffectList(w, actor, out->second, target, &foe, &witnesses);
    // Every person remembers how it went for their side.
    std::map<EntityId, bool> remembered;
    for (const auto& m : r.fighters) {
        Character& who = *m.who;
        if (who.isAnimal() || !who.body.alive || remembered[who.id]) continue;
        remembered[who.id] = true;
        std::string o = m.side == 0 ? r.outcome : mirror(r.outcome);
        if (m.state == "flee") o = "flee";
        MemoryEntry e;
        e.type = b.memory.at(o);
        const auto& other = m.side == 0 ? s.b : s.a;
        for (const Character* c : other) e.who.push_back(c->id);
        auto v = b.memoryValence.find(o);
        e.valence = v == b.memoryValence.end() ? 0.0 : v->second;
        w.remember(who, std::move(e));
        if (m.side == 1 && foe.isAnimal() == false && actor.isAnimal()) {
            MemoryEntry bt;
            bt.type = "attacked_by_beast";
            bt.who = {actor.id};
            bt.valence = -0.7;
            w.remember(who, std::move(bt));
        }
    }
    Json sideA = Json::array(), sideB = Json::array();
    for (const Character* c : s.a) sideA.push_back(c->id);
    for (const Character* c : s.b) sideB.push_back(c->id);
    std::vector<EntityId> about;
    for (const Character* c : s.a) about.push_back(c->id);
    for (const Character* c : s.b) about.push_back(c->id);
    w.chronicle("combat_group", about, b.group.chronicle,
                {{"actor", actor.id}, {"target", foe.id}, {"act", d.id}, {"outcome", r.outcome}, {"rounds", r.rounds},
                 {"side_a", sideA}, {"side_b", sideB}, {"heads_a", s.headsA}, {"heads_b", s.headsB},
                 {"fallen_a", r.fallenA}, {"fallen_b", r.fallenB}});
    // The winning side's leader decides over each fallen person of the other.
    if (r.outcome != "win" && r.outcome != "lose") return;
    const int winSide = r.outcome == "win" ? 0 : 1;
    Character* leader = nullptr;
    for (const auto& m : r.fighters)
        if (m.side == winSide && m.state == "fight" && m.who->body.alive) {
            leader = m.who;
            break;
        }
    if (!leader) return;
    std::map<EntityId, bool> decided;
    for (const auto& m : r.fighters) {
        Character& loser = *m.who;
        if (m.side == winSide || loser.isAnimal() || !loser.body.alive || decided[loser.id]) continue;
        std::string state = m.state;
        if (state == "dead") state = "ko"; // survived a non-lethal fall: down, not dead
        if (state != "ko" && state != "yield") continue;
        decided[loser.id] = true;
        finishFight(w, *leader, loser, d, state, witnesses);
    }
}

double winChance(World& w, Character& actor, const InteractionDef& d, Character& target) {
    const CombatBridge* b = combatBridge(w.content());
    if (!b || !d.combat.set) return 0.5;
    const Sides s = gatherSides(w, actor, d, target);
    if (!s.group()) {
        if (b->lod) return b->lod->winShare(fighterOf(*b, actor, d.combat.attackSkill), fighterOf(*b, target, d.combat.defenseSkill));
    }
    int wins = 0;
    const int n = b->group.winSamples;
    for (int i = 0; i < n; ++i) {
        const GroupResolution r = resolveGroupCombat(w, s, d, 0x57a7u + static_cast<std::uint64_t>(i));
        if (r.outcome == "win") ++wins;
    }
    return static_cast<double>(wins) / static_cast<double>(std::max(1, n));
}

} // namespace lr::sim
