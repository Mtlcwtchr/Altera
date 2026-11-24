// Consequences of deeds (AL-31) and the QA fixes of 21.11 (AL-23..AL-30):
// another household's things are not forbidden, the world answers instead -
// witnesses, repute, the owner told, an unseen loss found and suspected,
// rumour in talk; a player's order goes to the named target or is refused.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/consequences.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

Command cmdOf(const std::string& type, Json args, int player = 1) {
    Command c;
    c.source = "player:" + std::to_string(player);
    c.type = type;
    c.args = std::move(args);
    c.args["player"] = player;
    return c;
}

// An adult, a storage of another household, and a bystander of a third one.
struct Scene {
    Character* doer = nullptr;
    WorldObject* store = nullptr;
    Character* bystander = nullptr;
};

Scene scene(World& w) {
    Scene s;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") {
            s.doer = &c;
            break;
        }
    if (!s.doer) return s;
    for (auto& [id, o] : w.objects())
        if (o.type == "storage" && o.ownerFamily != kNone && o.ownerFamily != s.doer->status.family) {
            s.store = &o;
            break;
        }
    if (!s.store) return s;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && c.status.family != s.doer->status.family &&
            c.status.family != s.store->ownerFamily) {
            s.bystander = &c;
            break;
        }
    return s;
}

int memories(const Character& c, const std::string& type, EntityId about) {
    int n = 0;
    for (const auto& m : c.memory) n += m.type == type && !m.who.empty() && m.who[0] == about;
    return n;
}

// Everybody but `keep` goes far from `at` (nobody sees a thing).
void clearAround(World& w, Vec2 at, const std::vector<EntityId>& keep) {
    for (auto& [id, c] : w.characters())
        if (std::find(keep.begin(), keep.end(), id) == keep.end() && dist(c.pos, at) < 400.0)
            c.pos = {at.x + 900.0, at.y + 900.0};
}

} // namespace

TEST(altera_sim_consequences_foreign_variants) {
    const InteractionDef* f = realContent().interaction("take_meal@foreign");
    CHECK(f != nullptr);
    if (!f) return;
    CHECK_EQ(f->baseId, std::string("take_meal"));
    CHECK(!f->goap);
    CHECK(std::find(f->legal.begin(), f->legal.end(), "theft") != f->legal.end());
    for (const auto& k : f->pre) CHECK(k.subject != "target_owner" && k.subject != "home");
    bool fromStore = false;
    for (const auto& e : f->eff) fromStore |= e.subject == "target_stock" && e.key == "meal" && e.signedDelta() < 0;
    CHECK(fromStore);
    // using another's bed is not theft
    if (const InteractionDef* bed = realContent().interaction("sleep_in_bed@foreign"))
        CHECK(std::find(bed->legal.begin(), bed->legal.end(), "trespass") != bed->legal.end());
}

TEST(altera_sim_consequences_witnessed_theft) {
    World w = realWorld();
    Scene s = scene(w);
    CHECK(s.doer && s.store && s.bystander);
    if (!s.doer || !s.store || !s.bystander) return;
    s.store->stock["meal"] = 3;
    clearAround(w, s.store->pos, {s.doer->id, s.bystander->id});
    s.doer->pos = s.store->pos;
    s.bystander->pos = {s.store->pos.x + 5.0, s.store->pos.y};
    InteractionDef pub = *w.content().interaction("take_meal@foreign");
    pub.visibility = "public"; // seen for sure
    const double honesty0 = s.doer->reputation["reputation:honesty"];
    s.bystander->relations[s.doer->id].trust = 0.0; // strangers: the world's own past is not under test
    applyEffects(w, *s.doer, pub, s.store, nullptr);
    CHECK_EQ(s.store->stock["meal"], 2);
    CHECK_EQ(memories(*s.bystander, "saw_crime", s.doer->id), 1);
    CHECK(s.bystander->relations[s.doer->id].trust < 0.0);
    CHECK(s.doer->reputation["reputation:honesty"] < honesty0);
    // the owners were told and mean to accuse
    int told = 0;
    bool accuse = false;
    for (auto& [id, c] : w.characters())
        if (c.status.family == s.store->ownerFamily && !c.isAnimal()) {
            told += memories(c, "told_crime", s.doer->id);
            for (const auto& q : c.ai.queue) accuse |= q.task == "accuse_publicly" && q.target == s.doer->id;
        }
    CHECK(told > 0);
    CHECK(accuse);
    bool chron = false;
    for (const auto& e : w.chronicleLog()) chron |= e.type == "deed_seen";
    CHECK(chron);
    // word passes on in talk, less sure
    Character* listener = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.id != s.doer->id && c.id != s.bystander->id &&
            c.status.family != s.store->ownerFamily && !memories(c, "saw_crime", s.doer->id)) {
            listener = &c;
            break;
        }
    CHECK(listener != nullptr);
    if (!listener) return;
    listener->pos = s.bystander->pos;
    applyEffects(w, *s.bystander, *w.content().interaction("chat"), nullptr, listener);
    CHECK_EQ(memories(*listener, "rumor_crime", s.doer->id), 1);
    for (const auto& m : listener->memory)
        if (m.type == "rumor_crime") CHECK(m.confidence < 1.0 && m.source == "rumor");
}

TEST(altera_sim_consequences_unseen_loss_found) {
    World w = realWorld(11);
    Scene s = scene(w);
    CHECK(s.doer && s.store);
    if (!s.doer || !s.store) return;
    s.store->stock["meal"] = 3;
    clearAround(w, s.store->pos, {s.doer->id});
    s.doer->pos = s.store->pos;
    applyEffects(w, *s.doer, *w.content().interaction("take_meal@foreign"), s.store, nullptr);
    CHECK_EQ(s.store->stock["meal"], 2);
    // nobody saw it; within the trail's days the household finds it and suspects the only one about
    bool found = false, suspected = false;
    for (int day = 0; day < 20 && !suspected; ++day) {
        w.run(kTicksPerDay);
        for (const auto& e : w.chronicleLog()) {
            found |= e.type == "loss_noticed";
            suspected |= e.type == "suspicion";
        }
    }
    CHECK(found);
    CHECK(suspected);
    int suspects = 0;
    for (const auto& [id, c] : w.characters())
        if (c.status.family == s.store->ownerFamily) suspects += memories(c, "suspects", s.doer->id);
    CHECK(suspects > 0);
}

TEST(altera_sim_consequences_player_orders) {
    World w = realWorld();
    Scene s = scene(w);
    CHECK(s.doer != nullptr);
    if (!s.doer) return;
    std::string why;
    CHECK(switchControl(w, 1, s.doer->id, why));
    // AL-23/25: a beast is no partner for a fight, nor someone to play
    EntityId beast = kNone;
    for (const auto& [id, c] : w.characters())
        if (c.isAnimal() && c.body.alive) beast = id;
    if (beast != kNone) {
        Command r = w.applyNow(cmdOf("perform", {{"id", s.doer->id}, {"interaction", "attack"}, {"target", beast}}));
        CHECK(r.result.rfind("rejected:target:", 0) == 0);
        r = w.applyNow(cmdOf("take_control", {{"id", beast}}));
        CHECK_EQ(r.result, std::string("rejected:animal"));
    }
    // AL-25: steer is a step, not a teleport
    const Vec2 was = s.doer->pos;
    w.applyNow(cmdOf("steer", {{"id", s.doer->id}, {"x", was.x + 5000.0}, {"y", was.y}}));
    CHECK(dist(was, s.doer->pos) <= 25.0 + 1e-6);
    // AL-32: an order on another household's store is done as theft, not refused
    if (s.store) {
        s.store->stock["meal"] = 2;
        Command r = w.applyNow(cmdOf("perform", {{"id", s.doer->id}, {"interaction", "take_meal"}, {"target", s.store->id}}));
        CHECK(r.result.empty() || r.result == "ok");
        CHECK_EQ(s.doer->ai.goal.task, std::string("take_meal@foreign"));
    }
}

TEST(altera_sim_consequences_gear_only_for_orders) {
    // AL-24: under free will the player's person lives like anyone else
    World w = realWorld();
    Scene s = scene(w);
    if (!s.doer) return;
    std::string why;
    switchControl(w, 1, s.doer->id, why);
    WorldObject* home = w.object(s.doer->home);
    CHECK(home != nullptr);
    if (!home) return;
    home->stock["meal"] = 3;
    s.doer->inventory["stone"] = 400; // far past what a body carries
    const InteractionDef* d = w.content().interaction("take_meal");
    s.doer->ai.goal.source = "utility";
    CHECK(canPerform(w, *s.doer, *d, home, &why, false));
    s.doer->ai.goal.source = "player";
    CHECK(!canPerform(w, *s.doer, *d, home, &why, false));
    CHECK_EQ(why, std::string("gear:too_heavy"));
}

TEST(altera_sim_consequences_skill_gain_curve) {
    // AL-30: practice gives less the better one is, and nothing to the small
    World w = realWorld();
    Character* adult = nullptr;
    Character* small = nullptr;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive) continue;
        if (!adult && c.body.stage() == "adult") adult = &c;
    }
    CHECK(adult != nullptr);
    if (!adult) return;
    Character kid = *adult;
    kid.body.ageDays = 3 * kDaysPerYear;
    small = &kid;
    Effect e;
    e.subject = "skill";
    e.key = "cooking";
    e.op = '+';
    e.value = 0.1;
    adult->skills["cooking"] = 0.0;
    applyEffectList(w, *adult, {e}, nullptr);
    const double low = adult->skills["cooking"];
    CHECK(low > 0.0 && low < 0.1);
    adult->skills["cooking"] = 0.8;
    applyEffectList(w, *adult, {e}, nullptr);
    CHECK(adult->skills["cooking"] - 0.8 < low * 0.2);
    small->skills["cooking"] = 0.0;
    applyEffectList(w, *small, {e}, nullptr);
    CHECK_EQ(small->skills["cooking"], 0.0);
    // a child (6..14) learns, but slower than a grown-up from the same start
    kid.body.ageDays = 10 * kDaysPerYear;
    kid.skills["cooking"] = 0.0;
    applyEffectList(w, kid, {e}, nullptr);
    CHECK(kid.skills["cooking"] > 0.0 && kid.skills["cooking"] < low * 0.5);
    // above one's talent (at most 0.95) practice gives next to nothing
    adult->skills["cooking"] = 0.97;
    applyEffectList(w, *adult, {e}, nullptr);
    CHECK(adult->skills["cooking"] - 0.97 < 0.1 * 0.3 * 0.03 * 0.03 * 0.7);
}
