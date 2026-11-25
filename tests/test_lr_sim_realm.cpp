// The realm (AL-85, AL-92): the region beyond the map and the village in it, one
// kind of settlement and one kind of person - politics, wars, wanderers, quests,
// what a settlement is (classified, not set) and people crossing onto the map.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

// The village without the per-tick minds (fast): the realm's daily pass by hand.
World quietWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installRealm(w);
    return w;
}

void days(World& w, int n) {
    for (int i = 0; i < n; ++i) {
        for (auto& [id, c] : w.characters())
            if (!c.isAnimal() && c.body.alive) c.needs["hunger"] = 1.0, c.needs["thirst"] = 1.0;
        w.run(kTicksPerDay);
        realmDaily(w);
    }
}

int stat(const RealmState& s, const std::string& k) {
    auto it = s.stats.find(k);
    return it == s.stats.end() ? 0 : it->second;
}

// Someone on the road, offstage, free and going nowhere (or, with `any`, going anywhere).
Character* aWanderer(World& w, bool any = false) {
    for (auto& [id, f] : w.offstage())
        if (f.body.alive && f.standing.life == "road" && (any || f.standing.travelTo == kNone) && f.standing.captor == kNone) return &f;
    return nullptr;
}

Settlement* firstWith(World& w, const char* tag) {
    for (auto& [id, p] : w.settlements())
        if (!p.full() && p.tagged(tag)) return &p;
    return nullptr;
}

int stock(World& w, const std::string& item) {
    int n = 0;
    for (const auto& [fid, f] : w.families())
        if (const WorldObject* o = w.object(f.storage))
            if (auto it = o->stock.find(item); it != o->stock.end()) n += it->second;
    return n;
}

} // namespace

TEST(lr_sim_realm_rules_and_loc) {
    const Content& c = realContent();
    const auto err = checkRealm(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
    CHECK(realmQuestOn(c, "beast_bounty"));
    CHECK(realmQuestOn(c, "haunting")); // AL-118: magic is on, its quests too
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : realmLocKeys())
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
}

TEST(lr_sim_realm_genesis_is_deterministic) {
    World a = quietWorld(7), b = quietWorld(7);
    days(a, 1);
    days(b, 1);
    const RealmState sa = loadRealm(a);
    CHECK(sa.inited);
    CHECK(a.settlements().size() >= 7);
    CHECK(realmLocal(a) != nullptr);
    CHECK_EQ(realmLocal(a)->id, a.settlements().begin()->first);
    // every settlement is classified the same way, the map's village too
    for (const auto& [id, p] : a.settlements()) {
        CHECK(!p.size.empty());
        if (p.full()) continue;
        const Character* r = a.person(p.chief);
        CHECK(r && r->body.alive);
    }
    CHECK(realmLocal(a)->tagged("farming"));
    CHECK(Json(sa).dump() == Json(loadRealm(b)).dump());
    CHECK(a.toJson().dump() == b.toJson().dump());
    // the chronicle renders with names, not ids
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& e : a.chronicleLog())
        if (e.code.rfind("chronicle.realm.", 0) == 0) {
            const std::string t = loc.text("ru", e.code, locArgs(loc, "ru", a, e.params));
            CHECK(t.find('{') == std::string::npos && t.find('[') == std::string::npos);
        }
}

TEST(lr_sim_realm_saves_and_loads) {
    World w = quietWorld(7);
    days(w, 3);
    const std::string before = Json(loadRealm(w)).dump();
    World v(realContent(), 1);
    v.loadJson(w.toJson());
    CHECK(Json(loadRealm(v)).dump() == before);
    CHECK_EQ(v.stateHash(), w.stateHash());
    CHECK_EQ(v.offstage().size(), w.offstage().size());
    CHECK(v.toJson().dump() == w.toJson().dump());
}

TEST(lr_sim_realm_raid_takes_real_grain) {
    World w = quietWorld(7);
    days(w, 1);
    RealmState s = loadRealm(w);
    Settlement* local = realmLocal(w);
    Settlement* camp = firstWith(w, "raiders");
    CHECK(local && camp);
    if (!local || !camp) return;
    camp->militia = 200; // a horde: the raid gets through
    const int grain = stock(w, "grain");
    const double taken = realmRaid(w, s, camp->id, local->id);
    CHECK(taken > 0.0);
    CHECK(stock(w, "grain") < grain);
    int raided = 0;
    for (const auto& [id, c] : w.characters())
        for (const auto& m : c.memory) raided += m.type == "raided";
    CHECK(raided > 0);
    // the village chief puts a price on the band
    bool bounty = false;
    for (const auto& q : s.quests) bounty |= q.kind == "bandit_bounty" && q.polity == local->id && q.target == camp->id;
    CHECK(bounty);
}

TEST(lr_sim_realm_wanderer_comes_as_himself) {
    World w = quietWorld(7);
    days(w, 1);
    Character* f = aWanderer(w);
    CHECK(f != nullptr);
    if (!f) return;
    const EntityId id = f->id;
    const Character* self = f; // one object: moved onto the map and off it, never copied
    const std::string name = f->name;
    f->standing.travelTo = realmLocal(w)->id;
    f->standing.arrive = w.day() + 1;
    days(w, 1);
    Character* c = w.character(id);
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c == self);
    CHECK(c->name == name);
    CHECK(!w.offstage().count(id));
    MemoryEntry m;
    m.type = "good_chat";
    m.salience = 0.9;
    w.remember(*c, m);
    // he leaves when his days are up: offstage, with what he lived through
    for (int i = 0; i < 12 && w.character(id); ++i) days(w, 1);
    CHECK(w.character(id) == nullptr);
    CHECK(w.offstage().count(id) == 1);
    Character* g = w.person(id);
    CHECK(g == self);
    // and comes back the same
    g->standing.travelTo = realmLocal(w)->id;
    g->standing.arrive = w.day() + 1;
    // checked on the day he is in (a short stay may already be over a day later)
    for (int i = 0; i < 3 && !w.character(id); ++i) days(w, 1);
    c = w.character(id);
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c == self);
    bool remembers = false;
    for (const auto& e : c->memory) remembers |= e.type == "good_chat";
    CHECK(remembers);
}

// What a settlement is comes from its numbers and history: a band that settles and
// ploughs stops being a robbers' nest; a village that grows is a town.
TEST(lr_sim_realm_classified_not_set) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* camp = firstWith(w, "outlaw");
    CHECK(camp != nullptr);
    if (!camp) return;
    CHECK(camp->tagged("raiders"));
    camp->land = 1.2;
    camp->income.clear();
    camp->income["fields"] = 40.0;
    camp->lastRaid = -9999;
    camp->pop = 300;
    realmClassify(w, *camp);
    CHECK(!camp->tagged("raiders"));
    CHECK(!camp->tagged("outlaw"));
    CHECK(camp->tagged("farming"));
    CHECK_EQ(camp->size, std::string("town"));
    // and a farming village that lives by raids turns into raiders, then outlaws
    Settlement* v = nullptr;
    for (auto& [id, p] : w.settlements())
        if (!p.full() && p.tagged("farming") && !p.tagged("cult") && p.liege == kNone && p.id != camp->id) v = &p;
    CHECK(v != nullptr);
    if (!v) return;
    v->income.clear();
    v->income["raids"] = 60.0;
    v->income["fields"] = 20.0;
    v->lastRaid = w.day();
    realmClassify(w, *v);
    CHECK(v->tagged("raiders"));
    CHECK(v->tagged("outlaw"));
}

TEST(lr_sim_realm_beast_bounty) {
    World w = quietWorld(7);
    days(w, 1);
    Character* pack = nullptr;
    for (auto& [id, c] : w.characters())
        if (c.isAnimal() && c.species == "wolf" && c.body.alive) pack = &c;
    CHECK(pack != nullptr);
    if (!pack) return;
    const EntityId packId = pack->id;
    RealmState s = loadRealm(w);
    const EntityId localId = realmLocal(w)->id;
    // a villager was mauled
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement != kNone) {
            w.remember(c, MemoryEntry{0, "attacked_by_beast", {packId}, kNone, -0.7, 0.0, "seen", 1.0});
            break;
        }
    // the chief has something to pay with
    if (const Character* chief = w.character(realmLocal(w)->chief))
        if (auto f = w.families().find(chief->status.family); f != w.families().end())
            w.object(f->second.storage)->stock["coin"] += 40;
    days(w, 1);
    s = loadRealm(w);
    RealmQuest* q = nullptr;
    for (auto& qq : s.quests)
        if (qq.kind == "beast_bounty" && qq.polity == localId && qq.target == packId) q = &qq;
    CHECK(q != nullptr);
    if (!q) return;
    const EntityId qid = q->id;
    // a wanderer takes it and comes (one on his way elsewhere turns back: the trip is set below)
    Character* f = aWanderer(w);
    if (!f) f = aWanderer(w, true);
    CHECK(f != nullptr);
    if (!f) return;
    const EntityId fid = f->id;
    const double wealth = f->standing.purse;
    q->takers.push_back(fid);
    f->standing.quest = qid;
    f->standing.travelTo = localId;
    f->standing.arrive = w.day() + 1;
    saveRealm(w, s);
    days(w, 1);
    Character* hunter = w.character(fid);
    CHECK(hunter != nullptr);
    if (!hunter) return;
    bool hunting = false;
    for (const auto& t : hunter->ai.queue) hunting |= t.task == "attack" && t.target == packId;
    CHECK(hunting);
    // the pack falls: the bounty is paid
    w.character(packId)->body.alive = false;
    days(w, 1);
    s = loadRealm(w);
    for (const auto& qq : s.quests)
        if (qq.id == qid) CHECK(qq.status == "done");
    const Character* paid = w.person(fid);
    CHECK(paid && (paid->standing.purse > wealth || paid->has("coin") > 0));
}

TEST(lr_sim_realm_commands) {
    World w = quietWorld(7);
    days(w, 1);
    EntityId target = kNone;
    for (const auto& [id, p] : w.settlements())
        if (!p.full() && p.size == "village" && !p.tagged("outlaw") && !p.tagged("cult")) target = p.id;
    CHECK(target != kNone);
    Command no;
    no.source = "player:1";
    no.type = "realm.declare_war";
    no.args = {{"actor", 999999}, {"polity", target}};
    CHECK(w.applyNow(no).result.rfind("rejected", 0) == 0);
    Command yes = no;
    yes.args["actor"] = realmLocal(w)->chief;
    CHECK_EQ(w.applyNow(yes).result, std::string("ok"));
    const RealmState s = loadRealm(w);
    bool war = false;
    for (const auto& wr : s.wars) war |= !wr.over && wr.attacker == realmLocal(w)->id && wr.defender == target;
    CHECK(war);
}

// The people of a settlement off the map are people (AL-92 stage 2): its numbers are
// their count, they live by the village's rules (wed, bear, fall ill, die), and when
// hunger drives them out they reach the map as themselves.
TEST(lr_sim_realm_people_offstage) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* v = nullptr;
    for (auto& [id, p] : w.settlements())
        if (!p.full() && p.tagged("farming") && !p.tagged("cult") && (!v || p.pop > v->pop)) v = &p;
    CHECK(v != nullptr);
    if (!v) return;
    const EntityId vid = v->id;
    int n = 0, couples = 0, kids = 0;
    for (const auto& [id, c] : w.offstage())
        if (c.body.alive && c.status.settlement == vid && c.standing.life != "road") {
            ++n;
            couples += c.status.spouse != kNone && c.body.sex == 'f';
            kids += c.body.ageDays < 16 * kDaysPerYear;
        }
    CHECK_EQ(static_cast<int>(v->pop), n);
    CHECK(n >= 20);
    CHECK(couples > 0 && kids > 0);
    // a famine there: hunger kills the weak, and the hungry flee to the map as themselves
    for (int i = 0; i < 120; ++i) {
        Settlement& p = w.settlements().at(vid);
        p.food = 0.0, p.unrest = 0.0;
        days(w, 1);
    }
    const RealmState s = loadRealm(w);
    const int left = static_cast<int>(w.settlements().at(vid).pop);
    bool fled = false;
    for (const auto& [id, c] : w.characters())
        fled |= c.standing.at == vid && c.status.settlement == realmLocal(w)->id; // came from there, the same person
    std::printf("  famine 120d: %d -> %d people, hunger deaths %d, refugees %d, fled %d\n", n, left, stat(s, "died_hunger"),
                stat(s, "refugees"), fled ? 1 : 0);
    CHECK(stat(s, "died_hunger") > 0);
    CHECK(left < n);
    if (stat(s, "refugees") > 0) CHECK(fled);
}

TEST(lr_sim_realm_lives_three_years) {
    World w = quietWorld(11);
    days(w, 3 * kDaysPerYear);
    const RealmState s = loadRealm(w);
    int alive = 0, people = 0;
    for (const auto& [id, f] : w.offstage()) {
        alive += f.body.alive && !f.isAnimal() && !f.standing.role.empty();
        people += f.body.alive && !f.isAnimal();
    }
    // a settlement's numbers are its people
    for (const auto& [id, p] : w.settlements()) {
        if (p.full()) continue;
        int n = 0;
        for (const auto& [cid, c] : w.offstage())
            n += c.body.alive && c.status.settlement == id && c.standing.life != "road" && c.standing.captor == kNone;
        CHECK_EQ(static_cast<int>(p.pop), n);
    }
    std::printf("  realm 3y: %d people offstage (born %d, weddings %d, died ill %d old %d hunger %d raid/war %d), %d of Standing, "
                "wars %d, peace %d, quests taken %d done %d, successions %d, raids %d\n",
                people, stat(s, "born"), stat(s, "wedding"), stat(s, "died_illness"), stat(s, "died_old_age"), stat(s, "died_hunger"),
                stat(s, "died_raid") + stat(s, "died_battle") + stat(s, "died_war") + stat(s, "died_revolt"), alive,
                stat(s, "war_declared"),
                stat(s, "peace_white") + stat(s, "peace_tribute") + stat(s, "peace_vassal") + stat(s, "peace_reparation"),
                stat(s, "quest_taken"), stat(s, "quest_done"), stat(s, "succession") + stat(s, "succession_crisis"),
                stat(s, "raid") + stat(s, "raid_village"));
    CHECK(alive > 10 && alive <= 140);
    CHECK(people > 150);
    CHECK(stat(s, "born") > 0 && stat(s, "wedding") > 0);
    CHECK(stat(s, "quest_taken") > 0);
    CHECK(stat(s, "war_declared") + stat(s, "offer") > 0);
}
