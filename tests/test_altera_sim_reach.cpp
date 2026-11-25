// AL-108: an act is done at its target, facing it. Nobody pets the goats or
// talks to a neighbour from across the yard: one walks up to the act's reach
// (workReachOf: a step away for an act done by hand, the range for a hunt),
// keeps by a target that moves, and looks at it (Character::facing).

#include <cmath>
#include <cstdio>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& reachContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World reachWorld() {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(reachContent(), cfg, 7);
    installAi(w);
    return w;
}

Command playerCmd(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = 1;
    return c;
}

// The act a person is doing to a target right now (null: walking, idle, alone).
const InteractionDef* actAtTarget(const World& w, const Character& c) {
    const FsmState& f = c.ai.fsm;
    if (f.state == "Idle" || f.state == "Move" || f.state == "Wait" || f.interaction == "goto") return nullptr;
    if (f.target == kNone || f.target == c.id) return nullptr;
    const InteractionDef* d = w.content().interaction(f.interaction);
    if (!d || !(d->targetsObject() || d->targetsCreature() || d->targetsCharacter())) return nullptr;
    return d;
}

struct ReachStats {
    int checked = 0, far = 0, away = 0;
};

// Every person at an act: within the act's reach of the target, facing it.
void checkReach(const World& w, ReachStats& s, int report = 6) {
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive) continue;
        const InteractionDef* d = actAtTarget(w, c);
        if (!d) continue;
        Vec2 tp;
        if (!targetPos(w, c.ai.fsm.target, tp)) continue;
        ++s.checked;
        const double dd = dist(c.pos, tp);
        // a joint act at a place (a bed): both stand by the place, within together_m of it
        const double slack = c.ai.social.place != kNone ? 2.0 : 0.05;
        const double reach = workReachOf(w, *d, w.object(c.ai.fsm.target)) + slack;
        if (dd > reach) {
            if (s.far++ < report)
                std::printf("  far: t%lld #%llu %s/%s at %.1f m (reach %.1f)\n", (long long)w.now(), (unsigned long long)id,
                            c.ai.fsm.state.c_str(), d->id.c_str(), dd, reach);
        }
        if (dd > 0.3) {
            const double dot = (c.facing.x * (tp.x - c.pos.x) + c.facing.y * (tp.y - c.pos.y)) / dd;
            if (dot < 0.7 && s.away++ < report)
                std::printf("  looks away: t%lld #%llu %s/%s dot %.2f\n", (long long)w.now(), (unsigned long long)id,
                            c.ai.fsm.state.c_str(), d->id.c_str(), dot);
        }
    }
}

} // namespace

// Two days of village life, tick by tick: every act at a target is done in its
// reach and facing it.
TEST(altera_sim_acts_in_reach_and_facing) {
    World w = reachWorld();
    ReachStats s;
    for (Tick i = 0; i < 2 * kTicksPerDay; ++i) {
        w.step();
        checkReach(w, s);
    }
    std::printf("  acts checked %d, out of reach %d, looking away %d\n", s.checked, s.far, s.away);
    CHECK(s.checked > 200);
    CHECK_EQ(s.far, 0);
    CHECK_EQ(s.away, 0);
}

// The player's order on a herd far off: the person walks up to it first and
// pets it from a step away, facing it; a person the same.
TEST(altera_sim_player_act_walks_up_close) {
    World w = reachWorld();
    w.run(8 * kTicksPerDay / 24);
    Character* me = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") {
            me = &c;
            break;
        }
    CHECK(me);
    if (!me) return;
    CHECK_EQ(w.applyNow(playerCmd("take_control", {{"id", me->id}})).result, std::string("ok"));
    int done = 0;
    for (const char* species : {"hens", "goats", "pig"}) {
        const Character* herd = nullptr;
        for (const auto& [id, c] : w.characters())
            if (c.species == species && c.body.alive && c.group > 0 && (!herd || dist(c.pos, me->pos) < dist(herd->pos, me->pos)))
                herd = &c;
        if (!herd) continue;
        const std::string act = std::string("pet_") + species;
        // stand off a good way, then order
        me->pos = {herd->pos.x + 12.0, herd->pos.y};
        const Command r = w.applyNow(playerCmd("perform", {{"id", me->id}, {"interaction", act}, {"target", herd->id}}));
        if (r.result != "ok") {
            std::printf("  %s: %s\n", act.c_str(), r.result.c_str());
            continue;
        }
        bool acted = false;
        for (int i = 0; i < 8 && !acted; ++i) {
            w.step();
            if (me->ai.fsm.interaction == act) {
                acted = true;
                const double dd = dist(me->pos, herd->pos);
                CHECK(dd <= workReachOf(w, *w.content().interaction(act)) + 0.05);
                if (dd > 0.3)
                    CHECK((me->facing.x * (herd->pos.x - me->pos.x) + me->facing.y * (herd->pos.y - me->pos.y)) / dd > 0.7);
            }
        }
        CHECK(acted);
        ++done;
    }
    CHECK(done > 0);
}
