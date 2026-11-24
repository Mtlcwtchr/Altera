// Village life (AL-39): courtship -> betrothal -> wedding with a move into the
// spouse's household, conception, pregnancy and birth into the family and the
// dynasty, the head of a household after a death, illness, quarrels, strangers.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/village_life.hpp"
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

int chronicled(const World& w, const std::string& type) {
    int n = 0;
    for (const auto& e : w.chronicleLog()) n += e.type == type;
    return n;
}

// Two unmarried adults of different households and sexes, made of age to wed.
std::pair<Character*, Character*> youngPair(World& w) {
    std::vector<Character*> singles;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.spouse == kNone && c.body.ageDays >= 3 * kDaysPerYear)
            singles.push_back(&c);
    for (Character* m : singles)
        for (Character* f : singles)
            if (m->body.sex == 'm' && f->body.sex == 'f' && m->status.family != f->status.family) {
                m->body.ageDays = 22 * kDaysPerYear;
                f->body.ageDays = 20 * kDaysPerYear;
                return {m, f};
            }
    return {nullptr, nullptr};
}

Character* marriedWoman(World& w, bool pregnant) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.sex == 'f' && c.status.spouse != kNone &&
            isPregnant(w, c) == pregnant)
            return &c;
    return nullptr;
}

} // namespace

TEST(altera_sim_village_rules_load) {
    const Content& c = realContent();
    CHECK(villageLifeRules(c).loaded);
    const auto err = checkVillageLife(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
    CHECK(villageLifeRules(c).illnesses.size() >= 2);
}

TEST(altera_sim_village_loc_keys) {
    const Loc loc = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : villageLifeLocKeys())
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
}

TEST(altera_sim_village_courtship_to_wedding) {
    World w = realWorld();
    auto [m, f] = youngPair(w);
    CHECK(m && f);
    if (!m || !f) return;
    const EntityId groomFamily = m->status.family;
    const InteractionDef* propose = w.content().interaction("propose_marriage");
    const InteractionDef* marry = w.content().interaction("marry");
    CHECK(propose && marry);
    // a refusal is remembered and goes to the chronicle
    villageAfterAct(w, *m, *propose, f, false);
    CHECK(chronicled(w, "proposal_refused") == 1);
    CHECK(f->status.pairs.count(m->id) == 0);
    // an accepted proposal betroths both sides
    villageAfterAct(w, *m, *propose, f, true);
    CHECK(m->status.hasPair(f->id, "betrothed") && f->status.hasPair(m->id, "betrothed"));
    CHECK(chronicled(w, "betrothal") == 1);
    CHECK(courtshipTarget(w, *m) == f->id);
    // the wedding: married, the bride moves into the groom's household and takes its name
    villageAfterAct(w, *m, *marry, f, true);
    CHECK_EQ(m->status.spouse, f->id);
    CHECK_EQ(f->status.spouse, m->id);
    CHECK(!m->status.hasPair(f->id, "betrothed"));
    CHECK_EQ(f->status.family, groomFamily);
    CHECK_EQ(f->home, w.families().at(groomFamily).storage);
    const auto& members = w.families().at(groomFamily).members;
    CHECK(std::find(members.begin(), members.end(), f->id) != members.end());
    CHECK(f->name.find(w.families().at(groomFamily).name) != std::string::npos);
    CHECK(chronicled(w, "wedding") == 1);
}

TEST(altera_sim_village_daily_queues_courtship) {
    // With a spark and some liking, the daily pass sends a suitor to court.
    World w = realWorld(7);
    auto [m, f] = youngPair(w);
    CHECK(m && f);
    if (!m || !f) return;
    m->relations[f->id].affinity = 0.3;
    m->relations[f->id].attraction = 0.2;
    m->relations[f->id].familiarity = 0.5;
    CHECK_EQ(courtshipTarget(w, *m), f->id);
    bool queuedAct = false;
    for (int day = 1; day <= 6 && !queuedAct; ++day) {
        w.run(kTicksPerDay);
        for (const auto& q : m->ai.queue) queuedAct |= q.target == f->id && q.source == "village_life";
        if (m->status.hasPair(f->id, "betrothed") || m->status.hasPair(f->id, "married") ||
            m->relations[f->id].affinity > 0.3)
            queuedAct = true; // already acted on it
    }
    CHECK(queuedAct);
}

TEST(altera_sim_village_conception_and_birth) {
    World w = realWorld();
    // the start already has expecting mothers (seeded), each with a birth due
    int expecting = 0;
    for (const auto& [id, c] : w.characters()) expecting += !c.isAnimal() && isPregnant(w, c);
    CHECK(expecting >= 1);
    Character* mother = marriedWoman(w, false);
    CHECK(mother != nullptr);
    if (!mother) return;
    mother->body.ageDays = 25 * kDaysPerYear;
    Character* father = w.character(mother->status.spouse);
    father->body.ageDays = 28 * kDaysPerYear;
    const InteractionDef* intimacy = w.content().interaction("intimacy");
    CHECK(intimacy != nullptr);
    for (int i = 0; i < 400 && !isPregnant(w, *mother); ++i) {
        villageAfterAct(w, *father, *intimacy, mother, true);
        w.step();
    }
    CHECK(isPregnant(w, *mother));
    CHECK(chronicled(w, "pregnancy") >= 1);
    // the birth itself (the event the pregnancy left in the queue, brought forward)
    const size_t before = w.characters().size();
    const EntityId mid = mother->id, fid = father->id;
    Event e;
    e.tick = w.now();
    e.type = "village_birth";
    e.a = mid;
    e.b = fid;
    w.schedule(e);
    w.step();
    mother = w.character(mid);
    father = w.character(fid);
    const int born = chronicled(w, "birth"), still = chronicled(w, "stillbirth");
    CHECK(born + still >= 1);
    if (born >= 1) {
        CHECK(w.characters().size() > before);
        const EntityId cid = mother->status.children.back();
        const Character* child = w.character(cid);
        CHECK(child != nullptr);
        CHECK_EQ(child->status.mother, mid);
        CHECK_EQ(child->status.father, fid);
        CHECK_EQ(child->status.family, mother->status.family);
        CHECK_EQ(child->status.dynasty, father->status.dynasty);
        CHECK(child->body.ageDays <= 1);
        CHECK(std::find(father->status.children.begin(), father->status.children.end(), cid) !=
              father->status.children.end());
        const auto& members = w.families().at(mother->status.family).members;
        CHECK(std::find(members.begin(), members.end(), cid) != members.end());
    }
}

TEST(altera_sim_village_new_head_after_death) {
    World w = realWorld();
    Character* head = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.status.role == "head" && !c.status.children.empty()) {
            head = &c;
            break;
        }
    CHECK(head != nullptr);
    if (!head) return;
    const EntityId fam = head->status.family, spouse = head->status.spouse;
    head->body.alive = false;
    head->body.causeOfDeath = "illness";
    villageDaily(w);
    CHECK(chronicled(w, "new_head") == 1);
    int heads = 0;
    for (EntityId id : w.families().at(fam).members)
        if (const Character* m = w.character(id); m && m->body.alive && m->status.role == "head") ++heads;
    CHECK_EQ(heads, 1);
    // the widow is free again and remembers
    if (const Character* s = w.character(spouse); s && s->body.alive) {
        CHECK_EQ(s->status.spouse, kNone);
        CHECK(s->hasMoodlet("grieving", w.now()));
    }
}

TEST(altera_sim_village_strangers) {
    World w = realWorld();
    const EntityId st = w.settlements().begin()->first;
    const size_t people = w.characters().size();
    const size_t families = w.families().size();
    CHECK(strangersArrive(w, st, false));
    CHECK(w.characters().size() == people + 1);
    CHECK(chronicled(w, "stranger") == 1);
    CHECK(strangersArrive(w, st, true));
    CHECK(w.families().size() == families + 1);
    CHECK(w.characters().size() >= people + 3);
    // the newcomers have a home, their household its store
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive) CHECK(c.status.family != kNone && w.object(c.home) != nullptr);
    // AL-82: ...and a hearth of its own they know, to cook the grain they brought
    for (const auto& [fid, f] : w.families()) {
        const WorldObject* h = w.object(f.hearth);
        CHECK(h != nullptr && h->type == "hearth" && h->ownerFamily == fid);
        for (EntityId m : f.members)
            if (const Character* c = w.character(m); c && c->body.alive) CHECK(c->knownObjects.count(f.hearth) == 1);
        // ...and a field and a garden of its own, with the seed for them
        int fields = 0, gardens = 0;
        for (const auto& [oid, o] : w.objects())
            if (o.ownerFamily == fid) {
                if (o.type == "field") ++fields;
                if (o.type == "garden") ++gardens;
            }
        CHECK(fields >= 1 && gardens >= 1);
    }
    const Family& newest = w.families().rbegin()->second;
    const WorldObject* store = w.object(newest.storage);
    CHECK(store && store->stock.count("seed_grain") && store->stock.at("seed_grain") >= 20);
}

TEST(altera_sim_village_lives_over_two_months) {
    // The village is no longer silent: in 60 days the chronicle gets village life.
    World w = realWorld(20261121);
    w.run(60 * kTicksPerDay);
    int village = 0;
    for (const char* t : {"courtship", "betrothal", "proposal_refused", "wedding", "pregnancy", "birth", "stillbirth",
                          "illness", "quarrel", "stranger", "new_head"})
        village += chronicled(w, t);
    std::printf("  village events in 60 days: %d (births %d, weddings %d, courtships %d, illness %d, quarrels %d, "
                "strangers %d)\n",
                village, chronicled(w, "birth"), chronicled(w, "wedding"), chronicled(w, "courtship"),
                chronicled(w, "illness"), chronicled(w, "quarrel"), chronicled(w, "stranger"));
    CHECK(village >= 3);
}


