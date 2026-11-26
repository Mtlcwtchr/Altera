// LR-154: caravans, peddlers and supply trains - ordinary people of a settlement who
// carry goods where they fetch more; what the party is comes from its numbers.

#include <algorithm>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/caravan.hpp"
#include "sim/loc.hpp"
#include "sim/realm.hpp"
#include "sim/trade.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World quietWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installRealm(w);
    installCaravans(w);
    installTrade(w);
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

int stat(const CaravanState& s, const std::string& k) {
    auto it = s.stats.find(k);
    return it == s.stats.end() ? 0 : it->second;
}

const Venture* ventureOf(const World& w, int id) {
    static CaravanState keep;
    keep = loadCaravans(w);
    for (const auto& v : keep.ventures)
        if (v.id == id) return &v;
    return nullptr;
}

// A free adult at home in a stat settlement, of no Standing role.
Character* aResident(World& w, EntityId p, EntityId not1 = kNone) {
    for (auto& [id, c] : w.offstage())
        if (c.body.alive && !c.isAnimal() && c.status.settlement == p && c.standing.at == p && c.standing.role.empty() &&
            c.standing.life == "settled" && c.body.lifeYears() >= 20 && c.body.lifeYears() <= 45 && id != not1 && c.occupation.empty())
            return &c;
    return nullptr;
}

Settlement* aStat(World& w, bool outlaw = false) {
    for (auto& [id, p] : w.settlements())
        if (!p.full() && !p.ruined && p.tagged("outlaw") == outlaw && p.pop > 10) return &p;
    return nullptr;
}

} // namespace

TEST(lr_sim_caravan_rules_and_loc) {
    const Content& c = realContent();
    const auto err = checkCaravans(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : caravanLocKeys())
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
}

// Prices follow stock against need: a settlement starving pays more for grain than a full one.
TEST(lr_sim_caravan_prices_follow_need) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* p = aStat(w);
    CHECK(p != nullptr);
    if (!p) return;
    const double food = p->food;
    p->food = p->pop * 400;
    const double full = caravanPrice(w, *p, "grain");
    p->food = p->pop * 5;
    const double hungry = caravanPrice(w, *p, "grain");
    p->food = food;
    CHECK(hungry > full * 2.0);
}

// Traders appear and set out by themselves; the parties are classified, the region trades.
TEST(lr_sim_caravan_region_trades) {
    World w = quietWorld(7);
    days(w, 90);
    const CaravanState s = loadCaravans(w);
    std::printf("  set_out %d home %d markets %d robbed %d gain %d traders %d\n", stat(s, "set_out"), stat(s, "home"),
                stat(s, "markets"), stat(s, "robbed"), stat(s, "gain"), stat(s, "trader_new"));
    CHECK(stat(s, "trader_new") > 0);
    CHECK(stat(s, "set_out") > 3);
    CHECK(stat(s, "home") > 0);
    for (const auto& v : s.ventures) CHECK(!v.kind.empty());
    // the traders are the settlements' own people, offstage or on the map, never copies
    for (const auto& v : s.ventures)
        if (v.state == "road" || v.state == "market")
            for (EntityId id : v.members) CHECK(w.person(id) != nullptr);
    const std::string list = w.applyNow(Command{0, 0, "system", "caravan.list", Json::object(), ""}).result;
    CHECK(list.rfind("ok:[", 0) == 0);
}

TEST(lr_sim_caravan_deterministic_and_saved) {
    World a = quietWorld(11), b = quietWorld(11);
    days(a, 40);
    days(b, 40);
    CHECK(Json(loadCaravans(a)).dump() == Json(loadCaravans(b)).dump());
    World v(realContent(), 1);
    v.loadJson(a.toJson());
    CHECK(Json(loadCaravans(v)).dump() == Json(loadCaravans(a)).dump());
}

// A caravan sent to the map's village walks on as the same people, trades by the
// ordinary rules and walks off with what it has.
TEST(lr_sim_caravan_comes_to_the_village) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* home = aStat(w);
    Settlement* local = realmLocal(w);
    CHECK(home && local);
    if (!home || !local) return;
    Character* t = aResident(w, home->id);
    CHECK(t != nullptr);
    if (!t) return;
    const Character* self = t;
    t->standing.purse = 200;
    // the village lacks what the home makes plenty of
    int id = caravanSetOut(w, t->id, local->id);
    if (!id) { // nothing the village wants at a profit: give him a load the village surely lacks
        t->inventory["cloth"] += 20;
        id = caravanSetOut(w, t->id, local->id);
    }
    CHECK(id > 0);
    if (!id) return;
    bool came = false;
    for (int i = 0; i < 20 && !came; ++i) {
        days(w, 1);
        came = w.character(t->id) != nullptr;
    }
    CHECK(came);
    if (!came) return;
    Character* m = w.character(t->id);
    CHECK(m == self);
    CHECK(m->occupation == "trader");
    const Venture* v = ventureOf(w, id);
    CHECK(v && v->state == "market");
    // a villager buys from him by the ordinary trade
    Character* buyer = nullptr;
    for (auto& [cid, c] : w.characters())
        if (c.body.alive && !c.isAnimal() && c.status.settlement == local->id && c.body.stage() == "adult") buyer = &c;
    CHECK(buyer != nullptr);
    if (buyer) {
        buyer->inventory["coin"] += 50;
        buyer->pos = m->pos;
        std::string item;
        for (const auto& [it, n] : m->inventory)
            if (n > 0 && it != "coin" && spare(*m, it) > 0) item = it;
        CHECK(!item.empty());
        Offer o;
        o.mode = "buy";
        o.take[item] = 1;
        o.give["coin"] = 40;
        const int before = m->has("coin");
        const DealResult r = makeOffer(w, *buyer, *m, o);
        CHECK(r.done);
        CHECK(m->has("coin") > before);
    }
    // the market days end: the party walks off, the same people
    bool gone = false;
    for (int i = 0; i < 12 && !gone; ++i) {
        days(w, 1);
        gone = w.character(t->id) == nullptr;
    }
    CHECK(gone);
    CHECK(w.person(t->id) == self);
    CHECK(w.offstage().count(t->id) == 1);
}

// Robbers near the road fall on a party without guards and take a share of the load.
TEST(lr_sim_caravan_robbed_on_the_road) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* home = aStat(w);
    Settlement* camp = aStat(w, true);
    if (!camp) { // a camp for the test: a settlement of people who live by raids
        for (auto& [sid, p] : w.settlements())
            if (!p.full() && &p != home) camp = &p;
        if (camp) camp->tags.insert("outlaw");
    }
    CHECK(home && camp);
    if (!home || !camp) return;
    camp->militia = 40;
    Character* t = aResident(w, home->id);
    CHECK(t != nullptr);
    if (!t) return;
    t->standing.purse = 30;
    t->inventory["cloth"] += 30;
    int id = caravanSetOut(w, t->id, realmLocal(w)->id);
    CHECK(id > 0);
    if (!id) return;
    { // a lone peddler: no guards to fight for him
        CaravanState s = loadCaravans(w);
        for (auto& v : s.ventures)
            if (v.id == id) {
                for (EntityId m : v.members)
                    if (Character* c = w.person(m); c && m != t->id) c->occupation.clear(), c->standing.life = "settled";
                v.members = {t->id};
                v.guards = 0;
            }
        saveCaravans(w, s);
    }
    const double before = camp->wealth;
    bool robbed = false;
    for (int i = 0; i < 6 && !robbed; ++i) {
        robbed = caravanAmbush(w, id, camp->id);
        if (!robbed) w.run(1); // another hour, another roll
    }
    CHECK(robbed);
    CHECK(camp->wealth > before);
    const CaravanState s = loadCaravans(w);
    CHECK(stat(s, "robbed") >= 1);
    // the home holds it against the camp
    const RealmState rs = loadRealm(w);
    auto it = rs.views.find(std::to_string(home->id) + ">" + std::to_string(camp->id));
    CHECK(it != rs.views.end() && !it->second.grievances.empty());
}

// Grain goes by supply trains: the hungry settlement's food rises only when the train arrives.
TEST(lr_sim_caravan_supply_train) {
    World w = quietWorld(7);
    days(w, 1);
    Settlement* a = nullptr;
    Settlement* b = nullptr;
    for (auto& [sid, p] : w.settlements()) {
        if (p.full() || p.ruined || p.tagged("outlaw") || p.pop < 10) continue;
        if (!a) a = &p;
        else if (!b) b = &p;
    }
    CHECK(a && b);
    if (!a || !b) return;
    a->food = a->pop * 500; // a rich harvest
    b->food = b->pop * 3;   // hunger
    b->wealth = std::max(b->wealth, 400.0);
    Character* t = aResident(w, a->id);
    CHECK(t != nullptr);
    if (!t) return;
    t->standing.purse = 60;
    const int id = caravanSetOut(w, t->id, b->id);
    CHECK(id > 0);
    if (!id) return;
    const Venture* v = ventureOf(w, id);
    CHECK(v != nullptr);
    if (!v) return;
    CHECK(t->has("grain") > 0);
    CHECK_EQ(v->kind, std::string("supply_train"));
    const double food0 = b->food;
    int arrive = v->arrive;
    bool sold = false;
    for (int d = w.day(); d <= arrive + 3 && !sold; ++d) {
        b->food = std::min(b->food, food0); // eaten as it comes, but not below what it was
        days(w, 1);
        const Venture* now = ventureOf(w, id);
        sold = now && now->sold > 0.0;
    }
    CHECK(sold);
}
