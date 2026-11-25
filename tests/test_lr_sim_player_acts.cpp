// The player's acts, seen tick by tick (AL-73): an order walks the person to
// the target, the act runs in a visible state, the world changes; a talk
// draws the other in - a free person talks back, a busy one answers over the
// work, which goes on, an unwilling one says no aloud.
//
// LR_PROBE=1 prints every tick and the traces (LR_PROBE_ACT names
// the talk act of the probe test).

#include <cstdio>
#include <cstdlib>
#include <string>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const std::string kOk = "ok";

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld() {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 7);
    installAi(w);
    w.run(7 * kTicksPerDay / 24);
    return w;
}

Command cmdOf(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = 1;
    return c;
}

bool verbose() { return std::getenv("LR_PROBE") != nullptr; }

void dump(const World& w, const Character& c, const char* who) {
    if (!verbose()) return;
    std::printf("t%lld %s#%llu %s/%s anim=%s goal=%s pos=%.1f,%.1f social=%s:%s:%s%s\n", (long long)w.now(), who,
                (unsigned long long)c.id, c.ai.fsm.state.c_str(), c.ai.fsm.interaction.c_str(), c.ai.fsm.anim.c_str(),
                c.ai.goal.valid() ? c.ai.goal.task.c_str() : "-", c.pos.x, c.pos.y, c.ai.social.chain.c_str(),
                c.ai.social.role.c_str(), c.ai.social.phase.c_str(), c.ai.social.aside ? " aside" : "");
}

void dumpTrace(const Character& c, Tick from) {
    if (!verbose()) return;
    for (const auto& e : c.ai.trace) {
        if (e.tick < from) continue;
        if (e.code == "say.line") std::printf("   [%lld] say %s\n", (long long)e.tick, e.params.value("key", std::string()).c_str());
        else std::printf("   [%lld %c] %s %s\n", (long long)e.tick, e.layer, e.code.c_str(), e.params.dump().substr(0, 200).c_str());
    }
}

// The lines `c` said since `from` (the keys), in order.
std::vector<std::string> saidBy(const Character& c, Tick from) {
    std::vector<std::string> out;
    for (const auto& e : c.ai.trace)
        if (e.tick >= from && e.code == "say.line" && e.params.value("speaker", kNone) == c.id)
            out.push_back(e.params.value("key", std::string()));
    return out;
}

bool hasTrace(const Character& c, Tick from, const std::string& code) {
    for (const auto& e : c.ai.trace)
        if (e.tick >= from && e.code == code) return true;
    return false;
}

// Two grown people of different households some way apart.
std::pair<Character*, Character*> twoPeople(World& w) {
    Character* me = nullptr;
    Character* other = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        if (!me) me = &c;
        else if (!other && dist(c.pos, me->pos) > 8.0) other = &c;
    }
    return {me, other};
}

// Holds a person still (resting) for a while: free to talk.
void holdResting(World& w, Character& c) {
    dropGoal(w, c, "test_hold", false);
    c.ai.fsm = FsmState{"Rest", "", kNone, c.pos, 200, w.now()};
    c.ai.nextThink = w.now() + 200;
}

Tick talkTo(World& w, Character& me, Character& other, const std::string& act, int ticks = 20) {
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me.id}})).result, kOk);
    const Tick t0 = w.now();
    const Command r = w.applyNow(cmdOf("perform", {{"id", me.id}, {"interaction", act}, {"target", other.id}}));
    CHECK_EQ(r.result, kOk);
    if (verbose()) std::printf("perform %s -> %s, dist %.1f\n", act.c_str(), r.result.c_str(), dist(me.pos, other.pos));
    for (int i = 0; i < ticks; ++i) {
        dump(w, me, "me");
        dump(w, other, "npc");
        w.step();
    }
    dumpTrace(me, t0);
    dumpTrace(other, t0);
    return t0;
}

} // namespace

TEST(lr_sim_player_talk_probe) {
    if (!verbose()) return;
    World w = realWorld();
    auto [me, other] = twoPeople(w);
    CHECK(me && other);
    if (!me || !other) return;
    const char* act = std::getenv("LR_PROBE_ACT");
    talkTo(w, *me, *other, act ? act : "chat", 30);
}

// A person at work hears a chat over the work: the work goes on, the answers
// are said aloud, and the player's person says theirs.
TEST(lr_sim_talk_busy_person_works_on) {
    World w = realWorld();
    auto [me, other] = twoPeople(w);
    CHECK(me && other);
    if (!me || !other) return;
    other->relations[me->id].affinity = 0.8; // a friend: says yes
    CHECK(other->ai.fsm.state == "Work");
    const std::string work = other->ai.fsm.interaction;
    bool aside = false, worked = true;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me->id}})).result, kOk);
    const Tick t0 = w.now();
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", me->id}, {"interaction", "chat"}, {"target", other->id}})).result, kOk);
    for (int i = 0; i < 6; ++i) {
        w.step();
        aside = aside || other->ai.social.aside;
        if (other->ai.social.active() && other->ai.fsm.state == "Talk") worked = false;
    }
    CHECK(aside);
    CHECK(worked);
    // the talk breaks no work (a change of mind of their own once it is over is theirs)
    bool broke = false;
    for (const auto& e : other->ai.trace)
        if (e.tick >= t0 && e.code == "fsm.abort" && e.params.value("why", std::string()).rfind("switched_to:", 0) != 0)
            broke = true;
    CHECK(!broke);
    const auto mine = saidBy(*me, t0), theirs = saidBy(*other, t0);
    CHECK(mine.size() >= 3);
    CHECK(theirs.size() >= 3);
    bool busyLine = false;
    for (const auto& k : theirs) busyLine = busyLine || k.find("_busy") != std::string::npos;
    CHECK(busyLine);
    CHECK(hasTrace(*me, t0, "social.done"));
}

// A free person turns to the speaker and talks back; a joke (no chain of its
// own before AL-73) draws the other in as well.
TEST(lr_sim_talk_free_person_talks_back) {
    for (const char* act : {"chat", "joke", "tell_story"}) {
        World w = realWorld();
        auto [me, other] = twoPeople(w);
        CHECK(me && other);
        if (!me || !other) return;
        other->relations[me->id].affinity = 0.8;
        holdResting(w, *other);
        CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me->id}})).result, kOk);
        const Tick t0 = w.now();
        CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", me->id}, {"interaction", act}, {"target", other->id}})).result, kOk);
        bool listened = false, near = false;
        for (int i = 0; i < 8; ++i) {
            w.step();
            if (other->ai.social.active() && other->ai.fsm.state == "Talk") listened = true;
            if (me->ai.fsm.state == "Talk" && dist(me->pos, other->pos) < 2.5) near = true;
        }
        CHECK(listened);
        CHECK(near);
        CHECK(!saidBy(*me, t0).empty());
        CHECK(!saidBy(*other, t0).empty());
        CHECK(hasTrace(*me, t0, "social.done"));
    }
}

// One who wants no talk says so aloud, and the order ends with the reason.
TEST(lr_sim_talk_unwilling_person_says_no) {
    World w = realWorld();
    auto [me, other] = twoPeople(w);
    CHECK(me && other);
    if (!me || !other) return;
    Relation& r = other->relations[me->id];
    r.affinity = -1.0;
    r.enmity = 1.0;
    r.fear = 1.0;
    // the yes is a roll against a floor of the chance: a lucky yes is talked out and asked again
    Tick t0 = 0;
    for (int k = 0; k < 4; ++k) {
        holdResting(w, *other);
        t0 = talkTo(w, *me, *other, "chat", 6);
        if (hasTrace(*me, t0, "social.refused")) break;
        for (int i = 0; i < 40 && (me->ai.social.active() || me->ai.goal.valid()); ++i) w.step();
    }
    CHECK(hasTrace(*me, t0, "social.refused"));
    const auto theirs = saidBy(*other, t0);
    CHECK(!theirs.empty());
    CHECK(!me->ai.goal.valid());
}

// Chopping with the hands nearly full: the person goes to the tree, works
// there, takes the logs that fit; the rest stays on the tree, and is said.
TEST(lr_sim_player_chop_takes_what_fits) {
    World w = realWorld();
    Character* me = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && c.knowledge.count("woodcutting")) {
            me = &c;
            break;
        }
    CHECK(me != nullptr);
    if (!me) return;
    const WorldObject* tree = nullptr;
    double bd = 1e18;
    for (const auto& [id, o] : w.objects())
        if (o.type == "tree" && o.stock.count("log") && o.stock.at("log") >= 2 && dist(o.pos, me->pos) < bd)
            bd = dist(o.pos, me->pos), tree = &o;
    CHECK(tree != nullptr);
    if (!tree) return;
    me->inventory["axe"] = std::max(me->inventory["axe"], 1);
    me->attrs["strength"] = 0.5; // LR-141: an average back (the stats are rolled for everyone now)
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me->id}})).result, kOk);
    const Tick t0 = w.now();
    const int logs0 = me->inventory["log"], stock0 = tree->stock.at("log");
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", me->id}, {"interaction", "chop_tree"}, {"target", tree->id}})).result,
             kOk);
    bool worked = false;
    for (int i = 0; i < 16; ++i) {
        dump(w, *me, "me");
        w.step();
        worked = worked || (me->ai.fsm.state == "Work" && me->ai.fsm.interaction == "chop_tree");
    }
    dumpTrace(*me, t0);
    if (verbose())
        for (const auto& [k, n] : me->inventory) std::printf("   has %s x%d\n", k.c_str(), n);
    CHECK(worked);
    const int got = me->inventory["log"] - logs0, gone = stock0 - w.object(tree->id)->stock.at("log");
    CHECK(got >= 1);
    CHECK_EQ(got, gone);
    if (got < 2) CHECK(hasTrace(*me, t0, "gear.left"));
    CHECK(hasTrace(*me, t0, "fsm.done"));
}

// An order that cannot be done as asked is refused at once, with the reason.
TEST(lr_sim_player_order_fails_fast) {
    World w = realWorld();
    Character* me = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") {
            me = &c;
            break;
        }
    CHECK(me != nullptr);
    if (!me) return;
    const WorldObject* tree = nullptr;
    for (const auto& [id, o] : w.objects())
        if (o.type == "tree" && o.stock.count("log") && o.stock.at("log") >= 2) {
            tree = &o;
            break;
        }
    CHECK(tree != nullptr);
    if (!tree) return;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me->id}})).result, kOk);
    me->inventory.erase("axe");
    const Command r = w.applyNow(cmdOf("perform", {{"id", me->id}, {"interaction", "chop_tree"}, {"target", tree->id}}));
    // refused at the order (no axe) or within a couple of ticks - never minutes of standing
    if (r.result == kOk)
        for (int i = 0; i < 4 && me->ai.goal.valid(); ++i) w.step();
    if (verbose()) std::printf("order -> %s, goal %s\n", r.result.c_str(), me->ai.goal.id.c_str());
    dumpTrace(*me, 0);
    CHECK(!me->ai.goal.valid() || me->ai.goal.id != "player_action");
}

// AL-78: the game's forest replaces the generated trees in one pass; the
// index by type and the rest of the world stay as they were.
TEST(lr_sim_remove_objects_in_one_pass) {
    World w = realWorld();
    const std::size_t all = w.objects().size(), trees = w.objectsOfType("tree").size();
    CHECK(trees > 10);
    std::vector<EntityId> some(w.objectsOfType("tree").begin(), w.objectsOfType("tree").begin() + 10);
    CHECK_EQ(w.removeObjects(some), std::size_t(10));
    CHECK_EQ(w.objects().size(), all - 10);
    CHECK_EQ(w.objectsOfType("tree").size(), trees - 10);
    for (EntityId id : some) CHECK(w.object(id) == nullptr);
    w.run(6);   // the world goes on without them
    CHECK(!w.objectsOfType("house").empty());
}

// AL-78: one works at a thing from beside it - the well's rim, not its middle.
TEST(lr_sim_work_beside_the_thing) {
    World w = realWorld();
    const WorldObject* well = nullptr;
    for (const auto& [id, o] : w.objects())
        if (o.type == "well") well = &o;
    Character* me = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && well && dist(c.pos, well->pos) > 5.0) {
            me = &c;
            break;
        }
    CHECK(well && me);
    if (!well || !me) return;
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", me->id}})).result, kOk);
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", me->id}, {"interaction", "drink_from_well"}, {"target", well->id}})).result,
             kOk);
    bool at = false;
    for (int i = 0; i < 8; ++i) {
        w.step();
        if (me->ai.fsm.interaction == "drink_from_well") {
            at = true;
            CHECK(dist(me->pos, well->pos) > 1.0);
            CHECK(dist(me->pos, well->pos) < 1.6);
        }
    }
    dumpTrace(*me, 0);
    CHECK(at);
}
