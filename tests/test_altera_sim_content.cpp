// Altera life-sim core on the real content (content/ai, parts B and C) and the
// schema v0.3 extensions: witnesses (w.), conditional law (legal_if /
// legal_unless), status pairs, rights, verdicts, chance inputs, seasons,
// start knowledge coverage and name declension.

#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& real() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

const Json& realWorldCfg() {
    static const Json j = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return j;
}

// Two adults next to each other in an empty world.
struct Pair {
    World w;
    EntityId a = kNone, b = kNone;
};

Pair pairWorld(const Content& c) {
    Pair p{World(c, 7)};
    Character x;
    x.name = "A";
    x.body.ageDays = 30 * kDaysPerYear;
    x.body.sex = 'm';
    Character y = x;
    y.name = "B";
    y.body.sex = 'f';
    y.pos = {2.0, 0.0};
    p.a = p.w.addCharacter(x).id;
    p.b = p.w.addCharacter(y).id;
    return p;
}

} // namespace

TEST(altera_sim_real_content_loads) {
    const Content& c = real();
    CHECK(c.interactions.size() >= 140);
    CHECK(c.goals.size() >= 18);
    CHECK(c.knowledge.count("foraging") == 1);
    CHECK(!c.householdMust.empty());
    CHECK(c.dialogueRules.contains("dialog_plans"));
    CHECK(c.traits.at("honest").weightInPool > 0.0);
    CHECK(c.need("warmth")->shelteredMult < 1.0);
    CHECK(c.objects.at("shrine").perSettlement == 1);
    const InteractionDef* marry = c.interaction("marry");
    CHECK(marry != nullptr);
    if (!(marry != nullptr)) return;
    CHECK_EQ(marry->legalIf.size(), size_t{1});
    CHECK(c.interaction("start_dialog")->consent == "target");
    const ValidationReport lv = validateLoc(c, realLoc());
    for (const auto& e : lv.errors) std::cerr << "    " << e << "\n";
    CHECK(lv.ok());
}

TEST(altera_sim_real_world_survives_three_days) {
    World w = makeTestWorld(real(), realWorldCfg());
    installAi(w);
    w.run(3 * kTicksPerDay);
    int alive = 0, people = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.isAnimal()) continue;
        ++people;
        alive += c.body.alive ? 1 : 0;
    }
    CHECK(people > 0);
    CHECK_EQ(alive, people);
}

TEST(altera_sim_start_knowledge_covers_chains) {
    const World w = makeTestWorld(real(), realWorldCfg());
    // Every household knows household_must, the settlement knows settlement_must.
    for (const auto& [fid, f] : w.families())
        for (const auto& k : real().householdMust) {
            bool covered = false;
            for (EntityId m : f.members) covered = covered || w.character(m)->knowledge.count(k) > 0;
            CHECK(covered);
        }
    for (const auto& k : real().settlementMust) {
        bool covered = false;
        for (const auto& [id, c] : w.characters()) covered = covered || c.knowledge.count(k) > 0;
        CHECK(covered);
    }
    // Shared objects exist once per settlement.
    int shrines = 0;
    for (const auto& [id, o] : w.objects()) shrines += o.type == "shrine" ? 1 : 0;
    CHECK_EQ(shrines, 1);
}

TEST(altera_sim_seasons_gate_gathering) {
    Json cfg = realWorldCfg();
    cfg["season"] = "winter";
    const World w = makeTestWorld(real(), cfg);
    CHECK(w.season() == "winter");
    bool sawBush = false;
    for (const auto& [id, o] : w.objects())
        if (o.type == "berry_bush") {
            sawBush = true;
            CHECK(!w.inSeason(o));
        }
    CHECK(sawBush);
    cfg["season"] = "summer";
    const World s = makeTestWorld(real(), cfg);
    for (const auto& [id, o] : s.objects())
        if (o.type == "berry_bush") CHECK(s.inSeason(o));
}

TEST(altera_sim_v03_legal_if_and_unless) {
    const Content& c = real();
    Pair p = pairWorld(c);
    Character& a = *p.w.character(p.a);
    Character& b = *p.w.character(p.b);
    auto has = [](const std::vector<LegalTag>& v, const std::string& t) {
        for (const auto& x : v)
            if (x.tag == t) return true;
        return false;
    };
    // intimacy is adultery only when one of them is married to somebody else.
    const InteractionDef& intimacy = *c.interaction("intimacy");
    CHECK(!has(legalTags(p.w, a, intimacy, nullptr, &b), "adultery"));
    a.status.spouse = 999; // married elsewhere
    CHECK(has(legalTags(p.w, a, intimacy, nullptr, &b), "adultery"));
    a.status.spouse = b.id; // married to each other
    CHECK(!has(legalTags(p.w, a, intimacy, nullptr, &b), "adultery"));
    // execute is murder unless the target was sentenced; exile is tyranny without the right.
    const InteractionDef& execute = *c.interaction("execute");
    CHECK(has(legalTags(p.w, a, execute, nullptr, &b), "murder"));
    b.status.verdicts.insert("execute");
    CHECK(!has(legalTags(p.w, a, execute, nullptr, &b), "murder"));
    const InteractionDef& exile = *c.interaction("exile");
    CHECK(has(legalTags(p.w, a, exile, nullptr, &b), "tyranny"));
    a.status.rights.insert("exile");
    CHECK(!has(legalTags(p.w, a, exile, nullptr, &b), "tyranny"));
    // Price: an honest character pays more for the same norm.
    Character honest = a;
    honest.traits = {"honest"};
    CHECK(legalPrice(p.w, honest, "theft") > legalPrice(p.w, a, "theft"));
}

TEST(altera_sim_v03_status_pairs_and_witnesses) {
    const Json items = Json::parse(R"({"items": [{"id": "meal"}]})");
    const Json needs = Json::parse(R"({"needs": [{"id": "hunger"}], "moodlets": [{"id": "shocked", "value": -0.2}]})");
    const Json inter = Json::parse(R"({"interactions": [
        {"id": "wed", "category": "social", "target": "character", "eff": ["status_pair:married", "w.memory:saw_wedding",
         "w.rel:respect+0.2"], "visibility": "public"},
        {"id": "split", "category": "social", "target": "character", "eff": ["status_pair:married=0"]},
        {"id": "slap", "category": "social", "target": "character", "legal": ["assault"], "visibility": "public",
         "eff": ["w.moodlet:shocked"]},
        {"id": "whisper", "category": "social", "target": "character", "visibility": "private", "eff": ["w.memory:heard"]},
        {"id": "court", "category": "social", "target": "character", "eff": [],
         "chance": {"base": 0.1, "add": [{"input": "rel:attraction", "k": 0.5}], "min": 0.0, "max": 0.95}}]})");
    const Json config = Json::parse(R"({"memory_types": {"saw_wedding": 0.4, "heard": 0.2}})");
    const Content c = Content::fromJson(items, Json::object(), needs, Json::object(), Json::object(), Json::object(),
                                        inter, Json::object(), config);
    const ValidationReport r = validate(c);
    for (const auto& e : r.errors) std::cerr << "    " << e << "\n";
    CHECK(r.ok());
    Pair p = pairWorld(c);
    Character bystander;
    bystander.name = "C";
    bystander.pos = {5.0, 0.0};
    bystander.body.ageDays = 30 * kDaysPerYear;
    const EntityId cId = p.w.addCharacter(bystander).id;
    Character far = bystander;
    far.id = kNone;
    far.pos = {5000.0, 0.0};
    const EntityId farId = p.w.addCharacter(far).id;
    Character& a = *p.w.character(p.a);
    Character& b = *p.w.character(p.b);

    applyEffects(p.w, a, *c.interaction("wed"), nullptr, &b);
    CHECK(a.status.hasPair(b.id, "married"));
    CHECK(b.status.hasPair(a.id, "married"));
    CHECK_EQ(a.status.spouse, b.id);
    const Character& seen = *p.w.character(cId);
    bool remembered = false;
    for (const auto& m : seen.memory) remembered = remembered || (m.type == "saw_wedding" && m.who.front() == a.id);
    CHECK(remembered);
    CHECK(seen.relations.at(a.id).respect > 0.19);
    CHECK(p.w.character(farId)->memory.empty()); // out of sight
    applyEffects(p.w, a, *c.interaction("split"), nullptr, &b);
    CHECK(!a.status.hasPairAny("married"));
    CHECK_EQ(b.status.spouse, kNone);

    // A public crime: witnesses get the w. moodlet and a saw_crime memory.
    applyEffects(p.w, a, *c.interaction("slap"), nullptr, &b);
    CHECK(p.w.character(cId)->hasMoodlet("shocked", p.w.now()));
    bool sawCrime = false;
    for (const auto& m : p.w.character(cId)->memory) sawCrime = sawCrime || m.type == "saw_crime";
    CHECK(sawCrime);
    // Private: nobody but the two.
    CHECK(witnessesOf(p.w, a, *c.interaction("whisper"), &b).empty());

    // chance.add: attraction raises the chance.
    const InteractionDef& court = *c.interaction("court");
    const double cold = chanceOf(p.w, a, court, &b);
    a.relations[b.id].attraction = 0.8;
    CHECK(std::abs(cold - 0.1) < 1e-9);
    CHECK(std::abs(chanceOf(p.w, a, court, &b) - 0.5) < 1e-9);
}

TEST(altera_sim_v03_validator_rejects_bad_witness_effects) {
    const Json inter = Json::parse(R"({"interactions": [
        {"id": "x", "category": "social", "target": "character", "eff": ["w.has:meal+1"]},
        {"id": "y", "category": "social", "target": "character", "eff": [],
         "legal_if": [{"tag": "t", "if": ["right:no_such_right"]}]}]})");
    const Json items = Json::parse(R"({"items": [{"id": "meal"}]})");
    const Content c = Content::fromJson(items, Json::object(), Json::object(), Json::object(), Json::object(),
                                        Json::object(), inter, Json::object(), Json::object());
    const ValidationReport r = validate(c);
    auto mentions = [&](const std::string& s) {
        for (const auto& e : r.errors)
            if (e.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(mentions("not a witness effect"));
    CHECK(mentions("unknown right"));
}

TEST(altera_sim_v03_name_declension_ru) {
    const Loc& l = realLoc();
    CHECK(l.grammar("ru") != nullptr);
    if (!(l.grammar("ru") != nullptr)) return;
    // u8 literals keep the test independent of the compiler's source charset.
    const Json masha = l.personNoun("ru", reinterpret_cast<const char*>(u8"\u041C\u0430\u0448\u0430"), 'f');
    CHECK(masha["gen"].get<std::string>() == reinterpret_cast<const char*>(u8"\u041C\u0430\u0448\u0438")); // after a hushing letter
    CHECK(masha["dat"].get<std::string>() == reinterpret_cast<const char*>(u8"\u041C\u0430\u0448\u0435"));
    const Json ivan = l.personNoun("ru", reinterpret_cast<const char*>(u8"\u0418\u0432\u0430\u043D"), 'm');
    CHECK(ivan["gen"].get<std::string>() == reinterpret_cast<const char*>(u8"\u0418\u0432\u0430\u043D\u0430"));
    CHECK(ivan["ins"].get<std::string>() == reinterpret_cast<const char*>(u8"\u0418\u0432\u0430\u043D\u043E\u043C"));
    CHECK(ivan["g"].get<std::string>() == "m");
    // Latin (generated) names stay as they are.
    const Json latin = l.personNoun("ru", "Ralemmund Mossbarrow", 'm');
    CHECK(latin["dat"].get<std::string>() == "Ralemmund Mossbarrow");
    // A trace with a character argument assembles with the declined form.
    CHECK(formatLoc("{who:dat}", Json{{"who", ivan}}, "ru") == reinterpret_cast<const char*>(u8"\u0418\u0432\u0430\u043D\u0443"));
}

// ---- part H: animals, hunting, fishing, stores ------------------------------

namespace {

EntityId addAnimal(World& w, const std::string& species, Vec2 pos, int group) {
    Character a;
    a.species = species;
    a.name = species;
    a.group = group;
    a.pos = pos;
    a.den = pos;
    a.needs["hunger"] = 0.9;
    return w.addCharacter(std::move(a)).id;
}

} // namespace

TEST(altera_sim_animals_spawn_and_flee) {
    World w = makeTestWorld(real(), realWorldCfg());
    int deer = 0, fish = 0, people = 0;
    for (const auto& [id, c] : w.characters()) {
        if (c.species == "deer") ++deer;
        if (c.species == "fish_shoal") {
            ++fish;
            CHECK(w.map().waterAt(c.pos));
        }
        if (!c.isAnimal()) ++people;
    }
    CHECK(deer > 10);
    CHECK(fish > 3);
    // A person walking up to a herd (not stalking it) sends it running.
    Pair p = pairWorld(real());
    const EntityId herd = addAnimal(p.w, "deer", {20.0, 0.0}, 5);
    installAi(p.w);
    const double before = dist(p.w.character(herd)->pos, p.w.character(p.a)->pos);
    p.w.run(3);
    CHECK(dist(p.w.character(herd)->pos, p.w.character(p.a)->pos) > before + 50.0);
    CHECK(people > 0);
}

TEST(altera_sim_hunting_takes_a_head) {
    Pair p = pairWorld(real());
    Character& hunter = *p.w.character(p.a);
    hunter.knowledge.insert("hunting");
    hunter.skills["hunting"] = 1.0;
    hunter.inventory["spear"] = 1;
    const EntityId herd = addAnimal(p.w, "deer", {30.0, 0.0}, 3);
    const InteractionDef& hunt = *real().interaction("hunt_game");
    Character& deer = *p.w.character(herd);
    std::string why;
    CHECK(canPerform(p.w, hunter, hunt, nullptr, &why, true, &deer));
    int got = 0;
    for (int i = 0; i < 20 && deer.body.alive; ++i) {
        p.w.step();
        applyEffects(p.w, hunter, hunt, nullptr, &deer);
        got = hunter.has("carcass");
    }
    CHECK(got >= 1);
    CHECK(deer.group < 3);
    // The last head taken: the group is gone.
    while (deer.body.alive) {
        p.w.step();
        applyEffects(p.w, hunter, hunt, nullptr, &deer);
    }
    CHECK_EQ(deer.group, 0);
}

TEST(altera_sim_fish_shoal_keeps_spawning_stock) {
    Pair p = pairWorld(real());
    Character& fisher = *p.w.character(p.a);
    fisher.knowledge.insert("fishing");
    fisher.inventory["fishing_line"] = 1;
    const EntityId shoal = addAnimal(p.w, "fish_shoal", {10.0, 0.0}, 5);
    const InteractionDef& line = *real().interaction("fish_with_line");
    std::string why;
    // 5 fish, a catch takes 3 and more than 2 must stay: depleted.
    CHECK(!canPerform(p.w, fisher, line, nullptr, &why, true, p.w.character(shoal)));
    CHECK(why.rfind("depleted:", 0) == 0);
}

TEST(altera_sim_food_spoils_by_half_life) {
    Pair p = pairWorld(real());
    Character& a = *p.w.character(p.a);
    a.inventory["fish"] = 100;       // half-life 2 days
    a.inventory["grain"] = 100;      // half-life 600 days
    for (int d = 0; d < 2; ++d) spoilFood(p.w);
    CHECK(a.has("fish") > 35 && a.has("fish") < 65);
    CHECK(a.has("grain") >= 99);
}

TEST(altera_sim_last_year_stores_follow_the_season) {
    auto food = [&](const char* season) {
        Json cfg = realWorldCfg();
        cfg["season"] = season;
        const World w = makeTestWorld(real(), cfg);
        double units = 0.0;
        for (const auto& [fid, f] : w.families())
            for (const auto& [item, n] : w.object(f.storage)->stock)
                units += real().items.at(item).nutrition * n;
        return units;
    };
    const double autumn = food("autumn"), summer = food("summer");
    CHECK(autumn > summer * 3.0); // after the harvest vs the hungry gap
    CHECK(summer > 0.0);
}

TEST(altera_sim_pickpocket_is_an_act_on_carried_things) {
    const InteractionDef& pp = *real().interaction("pickpocket");
    CHECK(pp.targetsCarried());
    CHECK(!pp.targetsCharacter());
    CHECK(pp.consent.empty());
    CHECK(!pp.speech.empty()); // others can be asked or ordered to do it
}

TEST(altera_sim_child_asks_and_parent_feeds) {
    World w = makeTestWorld(real(), realWorldCfg());
    installAi(w);
    // A hungry child next to a parent with grain at home and firewood.
    Character* child = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.stage() == "child") child = &c;
    if (!child) return; // the generator gave no child for this seed
    child->needs["hunger"] = 0.12;
    child->knowledge.erase("cooking");
    const EntityId kid = child->id;
    w.run(kTicksPerDay);
    const Character& after = *w.character(kid);
    CHECK(after.body.alive);
    CHECK(after.need("hunger") > 0.12);
}
