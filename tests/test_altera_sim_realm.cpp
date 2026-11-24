// The realm (AL-85): the region beyond the map - polities, figures, politics,
// wars, wanderers and quests - and what of it reaches the real village.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

// The village without the per-tick minds (fast): the realm's daily pass by hand.
World quietWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
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

RealmFigure* aWanderer(RealmState& s) {
    for (auto& f : s.figures)
        if (f.alive && f.life == "road" && !f.present && f.travelTo == kNone && f.captor == kNone) return &f;
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

TEST(altera_sim_realm_rules_and_loc) {
    const Content& c = realContent();
    const auto err = checkRealm(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
    CHECK(realmQuestOn(c, "beast_bounty"));
    CHECK(!realmQuestOn(c, "haunting")); // tagged magic: off until magic comes
    const Loc loc = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : realmLocKeys())
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
}

TEST(altera_sim_realm_genesis_is_deterministic) {
    World a = quietWorld(7), b = quietWorld(7);
    days(a, 1);
    days(b, 1);
    const RealmState sa = loadRealm(a);
    CHECK(sa.inited);
    CHECK(sa.polities.size() >= 6);
    CHECK(sa.local() != nullptr);
    CHECK_EQ(sa.local()->settlement, a.settlements().begin()->first);
    for (const auto& p : sa.polities) {
        if (p.settlement != kNone) continue;
        const RealmFigure* r = sa.figure(p.ruler);
        CHECK(r && r->alive);
    }
    CHECK(Json(sa).dump() == Json(loadRealm(b)).dump());
    // the chronicle renders with names, not ids
    const Loc loc = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& e : a.chronicleLog())
        if (e.code.rfind("chronicle.realm.", 0) == 0) {
            const std::string t = loc.text("ru", e.code, locArgs(loc, "ru", a, e.params));
            CHECK(t.find('{') == std::string::npos && t.find('[') == std::string::npos);
        }
}

TEST(altera_sim_realm_saves_and_loads) {
    World w = quietWorld(7);
    days(w, 3);
    const std::string before = Json(loadRealm(w)).dump();
    World v(realContent(), 1);
    v.loadJson(w.toJson());
    CHECK(Json(loadRealm(v)).dump() == before);
    CHECK_EQ(v.stateHash(), w.stateHash());
}

TEST(altera_sim_realm_raid_takes_real_grain) {
    World w = quietWorld(7);
    days(w, 1);
    RealmState s = loadRealm(w);
    RealmPolity* local = s.local();
    RealmPolity* camp = nullptr;
    for (auto& p : s.polities)
        if (p.kind == "bandit_camp") camp = &p;
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

TEST(altera_sim_realm_wanderer_comes_as_himself) {
    World w = quietWorld(7);
    days(w, 1);
    RealmState s = loadRealm(w);
    RealmFigure* f = aWanderer(s);
    CHECK(f != nullptr);
    if (!f) return;
    const EntityId id = f->id;
    f->travelTo = s.local()->id;
    f->arrive = w.day() + 1;
    saveRealm(w, s);
    days(w, 1);
    Character* c = w.character(id);
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c->name == loadRealm(w).figure(id)->name);
    MemoryEntry m;
    m.type = "good_chat";
    m.salience = 0.9;
    w.remember(*c, m);
    // he leaves when his days are up: parked, with what he lived through
    for (int i = 0; i < 12 && w.character(id); ++i) days(w, 1);
    CHECK(w.character(id) == nullptr);
    s = loadRealm(w);
    CHECK(s.figure(id)->parked.is_object());
    // and comes back the same
    RealmFigure* g = s.figure(id);
    g->travelTo = s.local()->id;
    g->arrive = w.day() + 1;
    saveRealm(w, s);
    days(w, 2);
    c = w.character(id);
    CHECK(c != nullptr);
    if (!c) return;
    bool remembers = false;
    for (const auto& e : c->memory) remembers |= e.type == "good_chat";
    CHECK(remembers);
}

TEST(altera_sim_realm_beast_bounty) {
    World w = quietWorld(7);
    days(w, 1);
    Character* pack = nullptr;
    for (auto& [id, c] : w.characters())
        if (c.isAnimal() && c.species == "wolf" && c.body.alive) pack = &c;
    CHECK(pack != nullptr);
    if (!pack) return;
    const EntityId packId = pack->id;
    RealmState s = loadRealm(w);
    const EntityId localId = s.local()->id;
    // a villager was mauled
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement != kNone) {
            w.remember(c, MemoryEntry{0, "attacked_by_beast", {packId}, kNone, -0.7, 0.0, "seen", 1.0});
            break;
        }
    // the chief has something to pay with
    if (const Character* chief = w.character(s.local()->ruler))
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
    RealmFigure* f = aWanderer(s);
    for (auto& ff : s.figures)
        if (!f && ff.alive && ff.life == "road" && !ff.present && ff.captor == kNone) f = &ff;
    CHECK(f != nullptr);
    if (!f) return;
    const EntityId fid = f->id;
    const double wealth = f->wealth;
    q->takers.push_back(fid);
    f->quest = qid;
    f->travelTo = localId;
    f->arrive = w.day() + 1;
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
    CHECK(s.figure(fid)->wealth > wealth || w.character(fid)->has("coin") > 0);
}

TEST(altera_sim_realm_commands) {
    World w = quietWorld(7);
    days(w, 1);
    RealmState s = loadRealm(w);
    EntityId target = kNone;
    for (const auto& p : s.polities)
        if (p.settlement == kNone && p.kind == "village") target = p.id;
    Command no;
    no.source = "player:1";
    no.type = "realm.declare_war";
    no.args = {{"actor", 999999}, {"polity", target}};
    CHECK(w.applyNow(no).result.rfind("rejected", 0) == 0);
    Command yes = no;
    yes.args["actor"] = s.local()->ruler;
    CHECK_EQ(w.applyNow(yes).result, std::string("ok"));
    s = loadRealm(w);
    bool war = false;
    for (const auto& wr : s.wars) war |= !wr.over && wr.attacker == s.local()->id && wr.defender == target;
    CHECK(war);
}

TEST(altera_sim_realm_lives_three_years) {
    World w = quietWorld(11);
    days(w, 3 * kDaysPerYear);
    const RealmState s = loadRealm(w);
    int alive = 0;
    for (const auto& f : s.figures) alive += f.alive;
    std::printf("  realm 3y: %d figures alive of %zu, wars %d, peace %d, quests taken %d done %d, successions %d, raids %d\n",
                alive, s.figures.size(), stat(s, "war_declared"),
                stat(s, "peace_white") + stat(s, "peace_tribute") + stat(s, "peace_vassal") + stat(s, "peace_reparation"),
                stat(s, "quest_taken"), stat(s, "quest_done"), stat(s, "succession") + stat(s, "succession_crisis"),
                stat(s, "raid") + stat(s, "raid_village"));
    CHECK(alive > 10 && alive <= 140);
    CHECK(stat(s, "quest_taken") > 0);
    CHECK(stat(s, "war_declared") + stat(s, "offer") > 0);
}
