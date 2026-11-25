// Household crafts on the real content: livestock (animals.cpp domestic
// species), the kitchen garden (a second crop type in farming.cpp), and the
// chores the AI does with them.

#include <algorithm>
#include <vector>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

Json worldCfg(const char* season) {
    Json j = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    j["season"] = season;
    return j;
}

std::vector<Character*> herds(World& w, const char* species) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (c.species == species && c.body.alive) out.push_back(&c);
    return out;
}

int inv(const Character& c, const char* item) {
    auto it = c.inventory.find(item);
    return it == c.inventory.end() ? 0 : it->second;
}

int stockOf(const WorldObject& o, const char* k) {
    auto it = o.stock.find(k);
    return it == o.stock.end() ? 0 : it->second;
}

const Character* personOf(const World& w, EntityId family) {
    for (const auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.status.family == family && c.body.alive) return &c;
    return nullptr;
}

} // namespace

TEST(lr_sim_livestock_at_start) {
    // Households keep their beasts by the house; another household's herd is
    // not found as a target (it would be theft, not a chore).
    World w = makeTestWorld(real(), worldCfg("summer"));
    const auto hens = herds(w, "hens");
    CHECK(!hens.empty());
    for (const Character* h : hens) {
        CHECK(h->status.family != kNone);
        const WorldObject* store = w.object(w.families().at(h->status.family).storage);
        CHECK(store && dist(store->pos, h->den) < 20.0);
        CHECK(h->group >= 3);
    }
    const Character* h = hens.front();
    EntityId other = kNone;
    for (const auto& [fid, f] : w.families())
        if (fid != h->status.family) other = fid;
    CHECK_EQ(findCreature(w, "hens", h->pos, 500.0, h->status.family), h->id);
    if (other != kNone) CHECK(findCreature(w, "hens", h->pos, 5.0, other) != h->id);
}

TEST(lr_sim_livestock_products_and_trough) {
    World w = makeTestWorld(real(), worldCfg("summer"));
    w.setStartDay(120); // laying and milking season
    for (int d = 0; d < 4; ++d) animalsDaily(w);
    int eggs = 0, milk = 0;
    for (Character* h : herds(w, "hens")) eggs += inv(*h, "eggs");
    for (Character* g : herds(w, "goats")) milk += inv(*g, "milk");
    CHECK(eggs > 0);
    if (!herds(w, "goats").empty()) CHECK(milk > 0);
    // Eggs and milk are a chore of the owner's household (herd:work).
    const Character* hen = herds(w, "hens").front();
    const Character* owner = personOf(w, hen->status.family);
    CHECK(owner != nullptr);
    if (owner) {
        CHECK(herdValue(w, *owner, hen, "eggs") > 0.0);
        CHECK(herdValue(w, *owner, nullptr, "work") >= 0.6);
    }
    // Winter: no grass, the trough feeds them; empty, it is the first chore.
    w.setStartDay(300);
    for (Character* g : herds(w, "goats")) {
        g->inventory["hay"] = 10;
        const double before = g->need("hunger");
        g->needs["hunger"] = 0.5;
        animalsDaily(w);
        CHECK(inv(*g, "hay") < 10);
        CHECK(g->need("hunger") > 0.5);
        g->inventory.erase("hay");
        g->needs["hunger"] = before;
        const Character* o = personOf(w, g->status.family);
        if (!o) continue;
        CHECK(herdValue(w, *o, g, "trough") > 0.0);
        CHECK(herdValue(w, *o, nullptr, "hay_need") > 0.0);
        if (WorldObject* store = w.object(o->home)) {
            store->stock["hay"] = 30;
            CHECK(herdValue(w, *o, nullptr, "work") >= 0.9);
        }
        break;
    }
    // Slaughter season: pigs over the one kept for the winter are culled.
    w.setStartDay(250);
    for (Character* p : herds(w, "pig")) {
        p->group = 4;
        const Character* o = personOf(w, p->status.family);
        if (o) CHECK_EQ(static_cast<int>(herdValue(w, *o, p, "cull")), 3);
        break;
    }
}

TEST(lr_sim_garden_ripens) {
    // The kitchen garden is a crop of its own (garden:<key>), apart from the
    // field: it ripens in 70 days into baskets of vegetables.
    World w = makeTestWorld(real(), worldCfg("spring"));
    std::vector<WorldObject*> gardens;
    for (EntityId id : w.objectsOfType("garden"))
        if (WorldObject* o = w.object(id)) gardens.push_back(o);
    CHECK_EQ(gardens.size(), w.families().size());
    if (gardens.empty()) return;
    WorldObject* g = gardens.front();
    CHECK(stockOf(*g, "strips") > 0);
    const Character* owner = personOf(w, g->ownerFamily);
    CHECK(owner != nullptr);
    if (!owner) return;
    // The field's sums do not see the garden and the other way round.
    CHECK(farmValue(w, *owner, nullptr, "strips", "garden") == stockOf(*g, "strips"));
    g->stock["sown"] = g->stock["strips"];
    g->stock["grown"] = 0;
    g->stock["ripe"] = 0;
    for (int d = 0; d < 75 && stockOf(*g, "ripe") == 0; ++d) {
        w.run(kTicksPerDay);
        cropsDaily(w);
    }
    CHECK(stockOf(*g, "ripe") > stockOf(*g, "strips") * 5);
    CHECK(farmValue(w, *owner, nullptr, "work", "garden") >= 1.0);
}

TEST(lr_sim_crafts_chores_run) {
    // Twenty days of summer with the AI: eggs and milk come in, the garden is
    // worked, and nobody dies of it.
    World w = makeTestWorld(real(), worldCfg("summer"));
    installAi(w);
    int dairy = 0; // eggs, milk and cheese seen in the stores along the way
    for (int k = 0; k < 10; ++k) {
        w.run(2 * kTicksPerDay);
        for (const auto& [fid, f] : w.families())
            if (const WorldObject* s = w.object(f.storage))
                dairy += stockOf(*s, "eggs") + stockOf(*s, "cheese") + stockOf(*s, "milk");
    }
    int alive = 0, people = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        ++people;
        alive += c.body.alive ? 1 : 0;
    }
    CHECK_EQ(alive, people);
    CHECK(dairy > 0);
    // The hens are not left full of eggs nobody collects.
    int eggsLeft = 0, cap = 0;
    for (Character* h : herds(w, "hens")) {
        eggsLeft += inv(*h, "eggs");
        cap += 4 * h->group;
    }
    CHECK(eggsLeft < cap);
}
