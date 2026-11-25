// LR-140: derived needs (safety, provision, kinship, acceptance, standing, legacy,
// grace) worked out from the world and eased toward it, the tiers of needs and
// their gate in Utility, and the tiers in the 2D client's card and sheet.
#include <algorithm>
#include <set>
#include <string>

#include "client2d/sheet2d.hpp"
#include "client2d/view2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/need_tiers.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

const Content& tierContent() {
    static const Content c = Content::load(kRoot + "/content/ai");
    return c;
}

World tierVillage() {
    Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
    cfg["village_plan"] = true;
    World w = makeTestWorld(tierContent(), cfg, 5);
    installAi(w);
    return w;
}

const std::set<std::string> kDerived{"safety", "provision", "kinship", "acceptance", "standing", "legacy", "grace"};

Character* firstAdult(World& w) {
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() != "child") return &c;
    return nullptr;
}

} // namespace

TEST(lr_need_tiers_content) {
    const Content& ct = tierContent();
    CHECK_EQ(ct.derivedNeeds.size(), kDerived.size());
    for (const auto& d : ct.derivedNeeds) {
        CHECK(kDerived.count(d.id) == 1);
        CHECK(ct.tierOfNeed(d.id) != nullptr);
    }
    // every need, base or derived, sits in one tier
    for (const auto& n : ct.needs) CHECK(!needTier(ct, n.id).empty());
    std::set<std::string> tiers;
    for (const auto& t : ct.needTiers) tiers.insert(t.id);
    CHECK((tiers == std::set<std::string>{"body", "safety", "belonging", "esteem", "purpose", "spirit"}));
    for (const auto& [goal, tier] : ct.tierGate.goalTier) {
        CHECK(ct.goal(goal) != nullptr);
        CHECK(tiers.count(tier) == 1);
    }
}

TEST(lr_need_tiers_derived_from_world) {
    World w = tierVillage();
    w.step(); // tick 0: the first pass puts everyone where the world says
    int people = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive) continue;
        ++people;
        for (const auto& d : kDerived) {
            if (d == "legacy" && c.body.stage() == "child") {
                CHECK(c.needs.count(d) == 0);
                continue;
            }
            CHECK(c.needs.count(d) == 1);
            const double v = c.need(d);
            CHECK(v >= 0.0 && v <= 1.0);
        }
    }
    CHECK(people > 0);
    // a household with an empty store feels its provision lower than one with a full store
    Character* a = firstAdult(w);
    CHECK(a != nullptr);
    WorldObject* home = w.object(a->home);
    CHECK(home != nullptr);
    const DerivedNeedDef* prov = w.content().derivedNeed("provision");
    auto saved = home->stock;
    for (auto& [item, n] : home->stock) n = 0;
    const double empty = derivedTarget(w, *a, *prov);
    home->stock = saved;
    home->stock["firewood"] = 40;
    home->stock["dried_meat"] += 200;
    const double full = derivedTarget(w, *a, *prov);
    CHECK(full > empty);
}

TEST(lr_need_tiers_ease_smoothly) {
    World w = tierVillage();
    w.step();
    Character* a = firstAdult(w);
    CHECK(a != nullptr);
    const EntityId id = a->id;
    const double target = derivedTarget(w, *a, *w.content().derivedNeed("safety"));
    // far below what the world says: it climbs back slowly, not in one jump
    a->needs["safety"] = 0.0;
    for (int i = 0; i < int(kTicksPerDay); ++i) w.step();
    const double day1 = w.character(id)->need("safety");
    CHECK(day1 > 0.0);
    CHECK(day1 < target * 0.1 + 0.02); // ~5% of the gap a day
}

TEST(lr_need_tiers_gate_in_utility) {
    World w = tierVillage();
    w.step();
    Character* a = firstAdult(w);
    CHECK(a != nullptr);
    for (const char* n : {"hunger", "thirst", "energy", "warmth"}) a->needs[n] = 1.0;
    a->body.health = 1.0;
    a->needs["safety"] = a->needs["provision"] = 1.0;
    const double fed = tierGate(w, *a, "socialize");
    a->needs["hunger"] = 0.05;
    const double starving = tierGate(w, *a, "socialize");
    CHECK(starving < fed);
    CHECK(starving >= w.content().tierGate.floor * w.content().tierGate.floor - 1e-9); // never zero
    CHECK_EQ(tierGate(w, *a, "eat"), 1.0);   // body goals are not gated
    CHECK_EQ(tierGate(w, *a, "pray"), 1.0);  // spirit is beside the pyramid
    // the gate shows in the why of the goal's score
    bool shown = false;
    for (const GoalScore& gs : scoreGoals(w, *a))
        if (gs.goal == "socialize" || gs.goal == "stock_food") shown |= gs.why.contains("tier");
    CHECK(shown);
}

TEST(lr_need_tiers_care_weights_grace) {
    World w = tierVillage();
    w.step();
    Character* a = firstAdult(w);
    CHECK(a != nullptr);
    const Content& ct = w.content();
    a->traits.erase(std::remove(a->traits.begin(), a->traits.end(), "pious"), a->traits.end());
    a->reputation["piety"] = 0.0;
    a->needs["grace"] = 0.0;
    const double careless = needPull(ct, *a, "grace");
    a->traits.push_back("pious");
    const double pious = needPull(ct, *a, "grace");
    CHECK(pious < careless);          // the pious one is pulled to pray harder
    CHECK(needCare(ct, *a, "grace") > 0.9);
    CHECK_EQ(needPull(ct, *a, "hunger"), a->need("hunger")); // base needs are as they are
}

TEST(lr_need_tiers_in_card_and_sheet) {
    World w = tierVillage();
    for (int i = 0; i < 13; ++i) w.step();
    Character* a = firstAdult(w);
    CHECK(a != nullptr);
    c2::ViewText t;
    for (const Json& d : {c2::personJson(w, t, a->id), c2::sheetJson(w, t, a->id)["overview"]}) {
        CHECK(d["tiers"].is_array());
        CHECK_EQ(d["tiers"].size(), std::size_t(6));
        CHECK_EQ(d["tiers"][0]["id"].get<std::string>(), std::string("body"));
        CHECK_EQ(d["tiers"].back()["id"].get<std::string>(), std::string("spirit"));
        std::set<std::string> ids;
        for (const auto& n : d["needs"]) {
            ids.insert(n["id"].get<std::string>());
            CHECK(!n["tier"].get<std::string>().empty());
        }
        for (const auto& k : kDerived) CHECK(ids.count(k) == 1);
        for (const auto& tr : d["tiers"])
            for (const auto& n : tr["needs"]) CHECK(ids.count(n.get<std::string>()) == 1);
    }
}
