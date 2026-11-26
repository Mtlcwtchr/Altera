// LR-159: riding and draught beasts, vehicles (sim/transport.hpp, content/ai/transport.json):
// the data hangs together, households start with beasts and carts, a rider goes faster and
// the beast with him, a team pulls a vehicle and its load slows it, NPCs ride a long way
// on their own beast and get down on arrival, the harness survives a save.

#include <cmath>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/transport.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& trContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World trWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(trContent(), cfg, seed);
    installAi(w);
    return w;
}

Character* adult(World& w, EntityId not_ = kNone) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && id != not_ && c.status.family != kNone) return &c;
    return nullptr;
}

Command cmdOf(const std::string& type, Json args) {
    Command c;
    c.source = "player:1";
    c.type = type;
    c.args = std::move(args);
    return c;
}

Character& horseFor(World& w, const Character& c, Vec2 at, const std::string& sp = "horse") {
    Character a;
    a.species = sp;
    a.name = sp;
    a.group = 1;
    a.pos = a.den = at;
    a.status.family = c.status.family;
    a.needs["hunger"] = 0.8;
    a.body.ageDays = 5 * kDaysPerYear;
    return w.addCharacter(std::move(a));
}

void lead(World& w, Character& c) {
    CHECK_EQ(w.applyNow(cmdOf("take_control", {{"id", c.id}})).result, std::string("ok"));
}

} // namespace

TEST(lr_sim_transport_data) {
    const TransportRules* R = transportRules(trContent());
    CHECK(R != nullptr);
    if (!R) return;
    CHECK(R->mounts.size() >= 6);
    CHECK(R->vehicles.size() >= 5);
    for (const auto& [id, m] : R->mounts) {
        const SpeciesDef* s = trContent().speciesOf(id);
        CHECK(s != nullptr);
        if (s) CHECK(s->domestic);
        CHECK(m.ride > 0.0 || m.draft > 0.0);
    }
    for (const auto& [id, v] : R->vehicles) {
        CHECK(trContent().objects.count(id) == 1);
        CHECK(v.capacityKg > 0.0);
        for (const auto& b : v.draft) CHECK(R->mount(b) != nullptr);
    }
    CHECK(R->mount("ox") && R->mount("ox")->ride == 0.0);   // oxen pull, nobody rides them
}

TEST(lr_sim_transport_households_start_with_beasts_and_carts) {
    int beasts = 0, carts = 0;
    for (std::uint64_t seed : {3, 7, 11}) {
        World w = trWorld(seed);
        const TransportRules* R = transportRules(w.content());
        for (const auto& [id, c] : w.characters())
            if (c.isAnimal() && R->mount(c.species) && c.status.family != kNone) ++beasts;
        for (const auto& [id, o] : w.objects())
            if (isVehicle(w, o)) {
                ++carts;
                CHECK(o.ownerFamily != kNone);
            }
    }
    CHECK(beasts > 0);
    CHECK(carts > 0);
}

TEST(lr_sim_transport_rider_goes_faster_and_the_horse_with_him) {
    World w = trWorld();
    Character* c = adult(w);
    CHECK(c != nullptr);
    if (!c) return;
    lead(w, *c);
    Character& h = horseFor(w, *c, {c->pos.x + 1.0, c->pos.y});
    const EntityId hid = h.id;
    CHECK_EQ(transportSpeedMult(w, *c), 1.0);
    const Json acts = transportActs(w, *c, hid);
    CHECK(!acts.empty() && acts[0]["id"] == "transport.mount" && acts[0]["can"] == true);
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.mount"}, {"target", hid}})).result,
             std::string("ok"));
    CHECK_EQ(rideOf(w, c->id), hid);
    CHECK(transportSpeedMult(w, *c) > 2.0);
    CHECK(transportTireMult(w, *c) < 0.5);
    // a ride of 40 m: the steer takes it whole (on foot it is cut to 25 m), the horse keeps under him
    const Vec2 to{c->pos.x + 40.0, c->pos.y};
    CHECK_EQ(w.applyNow(cmdOf("steer", {{"id", c->id}, {"x", to.x}, {"y", to.y}})).result, std::string("ok"));
    CHECK(std::abs(c->pos.x - to.x) < 1e-6);
    w.step();
    CHECK(dist(w.character(hid)->pos, c->pos) < 1e-6);
    CHECK(w.character(hid)->body.fatigue > 0.0);   // it did the walking
    // nobody else rides a ridden horse
    Character* o = adult(w, c->id);
    if (o) CHECK(mountBeast(w, *o, *w.character(hid)) == "rejected:beast_busy");
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.dismount"}})).result, std::string("ok"));
    CHECK_EQ(rideOf(w, c->id), kNone);
    CHECK(!harnessed(w, hid));
}

TEST(lr_sim_transport_who_may_ride_what) {
    World w = trWorld();
    Character* c = adult(w);
    if (!c) return;
    Character& ox = horseFor(w, *c, c->pos, "ox");
    CHECK(mountBeast(w, *c, ox) == "rejected:not_ridden");
    Character& wolf = horseFor(w, *c, c->pos, "war_wolf");
    if (c->race.empty() || c->race == "human") CHECK(mountBeast(w, *c, wolf) == "rejected:wrong_kind");
    Character& foreign = horseFor(w, *c, c->pos);
    foreign.status.family = c->status.family + 100000;
    CHECK(mountBeast(w, *c, foreign) == "rejected:not_yours");
}

TEST(lr_sim_transport_herd_gives_one_head) {
    World w = trWorld();
    Character* c = adult(w);
    if (!c) return;
    Character& herd = horseFor(w, *c, c->pos);
    herd.group = 3;
    const EntityId herdId = herd.id;
    CHECK(mountBeast(w, *c, herd).empty());
    CHECK_EQ(w.character(herdId)->group, 2);
    const EntityId one = rideOf(w, c->id);
    CHECK(one != herdId);
    CHECK_EQ(w.character(one)->group, 1);
}

TEST(lr_sim_transport_team_pulls_the_cart_and_the_load_slows_it) {
    World w = trWorld();
    Character* c = adult(w);
    if (!c) return;
    lead(w, *c);
    const EntityId v = spawnRig(w, c->status.family, {c->pos.x + 1.0, c->pos.y}, "cart", "horse", 1);
    CHECK(v != kNone);
    CHECK_EQ(teamOf(w, v).size(), std::size_t(1));
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.drive"}, {"target", v}})).result,
             std::string("ok"));
    CHECK_EQ(driverOf(w, v), c->id);
    const double empty = transportSpeedMult(w, *c);
    CHECK(empty > 1.0);
    c->inventory["log"] += 8;
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.load"}, {"target", v}})).result,
             std::string("ok"));
    CHECK(c->inventory.count("log") == 0);
    CHECK(vehicleCargoKg(w, *w.object(v)) > 50.0);
    CHECK(vehicleCargoKg(w, *w.object(v)) <= vehicleCapacityKg(w, *w.object(v)));
    CHECK(transportSpeedMult(w, *c) < empty);
    // drive 20 m east: the cart under the driver, the horse ahead of it
    const Vec2 to{c->pos.x + 20.0, c->pos.y};
    w.applyNow(cmdOf("steer", {{"id", c->id}, {"x", to.x}, {"y", to.y}}));
    w.step();
    const WorldObject* o = w.object(v);
    CHECK(dist(o->pos, to) < 1e-6);
    const Character* h = w.character(teamOf(w, v)[0]);
    CHECK(h->pos.x > o->pos.x + 1.0);
    // a cart with nothing in harness does not go
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.alight"}})).result, std::string("ok"));
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.unhitch"}, {"target", v}})).result,
             std::string("ok"));
    CHECK(teamOf(w, v).empty());
    CHECK(drive(w, *c, *w.object(v)) == "rejected:no_team");
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.hitch"}, {"target", v}})).result,
             std::string("ok"));
    CHECK_EQ(teamOf(w, v).size(), std::size_t(1));
    CHECK_EQ(w.applyNow(cmdOf("perform", {{"id", c->id}, {"interaction", "transport.unload"}, {"target", v}})).result,
             std::string("ok"));
    CHECK(c->inventory.count("log") == 1);
}

TEST(lr_sim_transport_npc_rides_far_and_gets_down) {
    World w = trWorld();
    Character* c = adult(w);
    if (!c) return;
    const EntityId cid = c->id;
    Character& h = horseFor(w, *c, {c->pos.x + 2.0, c->pos.y});
    const EntityId hid = h.id;
    const Vec2 far{std::min(w.map().sizeM - 5.0, c->pos.x + 400.0), c->pos.y};
    autoRideStart(w, *w.character(cid), far);
    CHECK_EQ(rideOf(w, cid), hid);
    autoRideEnd(w, *w.character(cid));
    CHECK_EQ(rideOf(w, cid), kNone);
    // a short walk: no horse
    autoRideStart(w, *w.character(cid), {c->pos.x + 30.0, c->pos.y});
    CHECK_EQ(rideOf(w, cid), kNone);
}

TEST(lr_sim_transport_survives_a_save) {
    World w = trWorld();
    Character* c = adult(w);
    if (!c) return;
    Character& h = horseFor(w, *c, c->pos);
    const EntityId cid = c->id, hid = h.id;
    CHECK(mountBeast(w, *c, h).empty());
    const EntityId v = spawnRig(w, c->status.family, c->pos, "wagon", "ox", 2);
    World w2(trContent(), 7);
    installAi(w2);
    w2.loadJson(w.toJson());
    CHECK_EQ(rideOf(w2, cid), hid);
    CHECK_EQ(teamOf(w2, v).size(), std::size_t(2));
    // a harnessed beast does not wander off to graze
    w2.step();
    const Vec2 at = w2.character(teamOf(w2, v)[0])->pos;
    for (int i = 0; i < 30; ++i) w2.step();
    CHECK(dist(w2.character(teamOf(w2, v)[0])->pos, at) < 1e-6);
}

TEST(lr_sim_transport_npcs_ride_in_a_lived_day) {
    // walks of a few hundred metres take a tick or two (800 m a tick); an NPC near his own horse rides
    // them and is seen arriving on horseback, then gets down
    World w = trWorld();
    std::set<EntityId> riders;
    for (Tick t = 0; t < kTicksPerDay; ++t) {
        w.step();
        if (const Json* s = static_cast<const World&>(w).moduleState("transport"); s && s->contains("ride"))
            for (auto it = (*s)["ride"].begin(); it != (*s)["ride"].end(); ++it) riders.insert(std::stoull(it.key()));
    }
    CHECK(!riders.empty());
}
