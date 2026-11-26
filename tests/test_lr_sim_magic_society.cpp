// Magic in society (AL-118, LivelyRealms §5.9, §5.10): norms by culture, witnesses
// and witch trials, paid services, magic at work, the undead following their
// master or going for the living, magic quests in the realm.

#include <algorithm>
#include <cstdio>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/magic.hpp"
#include "sim/magic_society.hpp"
#include "sim/realm.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

World realWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, seed);
    installAi(w);
    return w;
}

std::vector<Character*> adults(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && !c.controller.isPlayer()) out.push_back(&c);
    return out;
}

int chronicled(const World& w, const std::string& prefix) {
    int n = 0;
    for (const auto& e : w.chronicleLog()) n += e.code.rfind(prefix, 0) == 0;
    return n;
}

Spell spell(const std::string& aspect, char vec, const std::string& form, int power = 1) {
    Spell s;
    s.parts.push_back({aspect, vec});
    s.form = form;
    if (power > 1) s.mods["power"] = power;
    return s;
}

void know(World& w, Character& c, const std::string& aspect, double k, double mana = 50.0) {
    MagicSelf m = magicOf(w, c);
    m.rolled = true;
    m.aspects[aspect] = k;
    m.mana = mana;
    m.manaAt = w.now();
    setMagicOf(w, c, m);
    c.skills["magic"] = 0.6;
}

// One who is neither the chief nor kin of the chief.
Character* commoner(World& w) {
    for (Character* c : adults(w)) {
        const auto s = w.settlements().find(c->status.settlement);
        if (s == w.settlements().end() || s->second.chief == c->id) continue;
        const Character* ch = w.character(s->second.chief);
        if (ch && ch->status.family == c->status.family) continue;
        return c;
    }
    return nullptr;
}

std::vector<Character*> neighbours(World& w, const Character& c, int n) {
    std::vector<Character*> out;
    for (Character* o : adults(w))
        if (o->id != c.id && o->status.settlement == c.status.settlement && static_cast<int>(out.size()) < n) out.push_back(o);
    return out;
}

} // namespace

TEST(lr_sim_magic_society_rules_load) {
    const Content& c = realContent();
    const SocietyRules& r = societyRules(c);
    CHECK(r.loaded);
    CHECK(r.options.size() >= 5);
    CHECK(r.work.size() >= 4);
    const auto err = checkSociety(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : societyLocKeys(c))
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
    // magic quests are on in the realm now
    CHECK(realmQuestOn(c, "haunting"));
    CHECK(realmQuestOn(c, "dragon_slay"));
}

TEST(lr_sim_magic_society_norms_by_culture) {
    World w = realWorld();
    CHECK_EQ(magicClass(w.content(), "death"), std::string("necromancy"));
    CHECK_EQ(magicClass(w.content(), "blood"), std::string("witchcraft"));
    CHECK_EQ(magicClass(w.content(), "fire"), std::string("magic"));
    Character* c = commoner(w);
    CHECK(c != nullptr);
    const EntityId sid = c->status.settlement;
    CHECK(!settlementCulture(w, sid).empty());
    // LR-166: the hellenes of the start only suspect necromancy (the oracle of the dead)
    CHECK(settlementNorm(w, sid, "necromancy") > settlementNorm(w, sid, "witchcraft"));
    // a Slavic village: necromancy a crime, witchcraft suspect, magic allowed
    w.settlements().at(sid).culture = "slav_village";
    CHECK(settlementNorm(w, sid, "necromancy") >= 0.7);
    CHECK(settlementNorm(w, sid, "witchcraft") > 0.2);
    CHECK(settlementNorm(w, sid, "magic") < 1e-9);
}

TEST(lr_sim_magic_society_healer_not_accused_necromancer_judged) {
    World w = realWorld();
    Character* mage = commoner(w);
    CHECK(mage != nullptr);
    auto near = neighbours(w, *mage, 4);
    CHECK(near.size() >= 2);
    for (Character* o : near) o->pos = {mage->pos.x + 3.0, mage->pos.y + 1.0};
    // Life in front of the neighbours: seen, but nobody minds
    know(w, *mage, "life", 0.8);
    CastTarget self;
    self.who = mage->id;
    for (int i = 0; i < 3; ++i) {
        know(w, *mage, "life", 0.8);
        castSpell(w, *mage, spell("life", '+', "touch"), self);
        w.run(1);
    }
    bool sawMagic = false;
    for (const auto& m : near[0]->memory) sawMagic |= m.type == "saw_magic" && m.goods.count("aspect:life");
    CHECK(sawMagic);
    CHECK(accusationWeight(w, *mage) < 1e-9);
    // Death in front of them: necromancy, a crime here
    for (int i = 0; i < 4; ++i) {
        know(w, *mage, "death", 0.7);
        castSpell(w, *mage, spell("death", '-', "touch"), self);
        w.run(1);
    }
    bool sawNecro = false;
    for (Character* o : near)
        for (const auto& m : o->memory) sawNecro |= m.type == "saw_necromancy";
    CHECK(sawNecro);
    CHECK(accusationWeight(w, *mage) >= societyRules(w.content()).accuseAt);
    const std::string v = holdTrial(w, *mage);
    std::printf("  verdict: %s\n", v.c_str());
    CHECK(v == "exile" || v == "burn" || v == "shame");
    CHECK(chronicled(w, "chronicle.magic.trial.") >= 1);
    if (v == "burn") CHECK(!mage->body.alive);
    if (v == "exile") CHECK_EQ(mage->status.settlement, kNone);
}

TEST(lr_sim_magic_society_trial_happens_by_itself) {
    World w = realWorld(11);
    Character* mage = commoner(w);
    CHECK(mage != nullptr);
    auto near = neighbours(w, *mage, 5);
    for (Character* o : near) o->pos = {mage->pos.x + 2.0, mage->pos.y};
    CastTarget self;
    self.who = mage->id;
    for (int i = 0; i < 5; ++i) {
        know(w, *mage, "death", 0.7);
        castSpell(w, *mage, spell("death", '-', "touch"), self);
        w.run(1);
    }
    for (int d = 0; d < 3 && !chronicled(w, "chronicle.magic.trial."); ++d) w.run(kTicksPerDay);
    CHECK(chronicled(w, "chronicle.magic.trial.") >= 1);
}

TEST(lr_sim_magic_society_healer_is_paid) {
    World w = realWorld();
    Character* healer = commoner(w);
    CHECK(healer != nullptr);
    Character* patient = nullptr;
    for (Character* o : adults(w))
        if (o->status.family != healer->status.family && o->status.settlement == healer->status.settlement) patient = o;
    CHECK(patient != nullptr);
    patient->body.health = 0.4;
    WorldObject* store = w.object(w.families().at(patient->status.family).storage);
    CHECK(store != nullptr);
    store->stock["coin"] = 20;
    patient->inventory["coin"] = 0;
    const int before = healer->inventory["coin"];
    for (int d = 0; d < 6 && !chronicled(w, "chronicle.magic.society.paid"); ++d) {
        know(w, *healer, "life", 0.9);
        patient->body.health = std::min(patient->body.health, 0.4);
        w.run(kTicksPerDay);
    }
    CHECK(chronicled(w, "chronicle.magic.heal") >= 1);
    CHECK(chronicled(w, "chronicle.magic.society.paid") >= 1);
    CHECK(healer->inventory["coin"] > before || store->stock["coin"] < 20);
}

TEST(lr_sim_magic_society_work_spell_on_a_hungry_field) {
    World w = realWorld();
    Character* farmer = nullptr;
    WorldObject* field = nullptr;
    for (Character* c : adults(w)) {
        for (EntityId id : w.objectsOfType("field"))
            if (WorldObject* o = w.object(id); o && o->ownerFamily == c->status.family) field = o;
        if (field) {
            farmer = c;
            break;
        }
    }
    CHECK(farmer != nullptr && field != nullptr);
    field->stock["sown"] = std::max(1, field->stock["sown"]);
    field->stock["fertility"] = 250;
    know(w, *farmer, "life", 0.8);
    const WorkSpell* ws = societyRules(w.content()).workSpell("cast_grow");
    CHECK(ws != nullptr);
    const double v = workSpellValue(w, *farmer, *ws, *field);
    std::printf("  value %.3f\n", v);
    CHECK(v > societyRules(w.content()).workMin);
    // queued as an ordinary act; done at the field, the spell is cast there
    Command cmd;
    cmd.type = "work_spell";
    cmd.args = {{"id", farmer->id}, {"spell", "cast_grow"}, {"at", field->id}};
    cmd = w.applyNow(cmd);
    CHECK_EQ(cmd.result, std::string("ok"));
    for (int i = 0; i < 6 * 24 && field->stock["fertility"] <= 250; ++i) {
        know(w, *farmer, "life", 0.8);
        w.run(6);
    }
    std::printf("  fertility %d\n", field->stock["fertility"]);
    CHECK(field->stock["fertility"] > 250);
    const Json* st = static_cast<const World&>(w).moduleState("magic_society");
    CHECK(st && (*st)["people"].contains(std::to_string(farmer->id)) &&
          (*st)["people"][std::to_string(farmer->id)].value("work", 0) >= 1);
    // nothing to do on a fertile field
    field->stock["fertility"] = 900;
    CHECK(workSpellValue(w, *farmer, *ws, *field) <= 0.0);
}

TEST(lr_sim_magic_society_mindless_undead_follow_and_unbound_bite) {
    World w = realWorld();
    auto ps = adults(w);
    Character& necro = *ps[0];
    Character& dead = *ps[1];
    dead.body.alive = false;
    dead.body.health = 0.0;
    dead.body.causeOfDeath = "illness";
    dead.pos = {necro.pos.x + 1.0, necro.pos.y};
    w.run(kTicksPerDay - w.now() % kTicksPerDay); // the daily pass sees the death
    bool raised = false;
    for (int i = 0; i < 20 && !raised; ++i) {
        know(w, necro, "death", 0.7);
        dead.pos = {necro.pos.x + 1.0, necro.pos.y};
        CastTarget t;
        t.who = dead.id;
        raised = castSpell(w, necro, spell("death", '~', "touch", 1), t).raised == dead.id;
        w.run(1);
    }
    CHECK(raised);
    CHECK(dead.hasTrait("mindless"));
    CHECK(undeadHeld(w, dead));
    // the master walks off; the body keeps a few steps behind
    necro.pos = {necro.pos.x + 40.0, necro.pos.y + 10.0};
    w.run(3);
    know(w, necro, "death", 0.7);
    const double dd = dist(dead.pos, necro.pos);
    std::printf("  behind %.2f m\n", dd);
    CHECK(dd <= societyRules(w.content()).followM + 2.0);
    // the master dies: unbound, the body goes for the living
    necro.body.alive = false;
    w.run(kTicksPerDay * 3);
    CHECK(undeadMaster(w, dead) == kNone || !dead.body.alive);
    if (dead.body.alive) {
        bool bitten = false;
        for (int i = 0; i < kTicksPerDay && !bitten && dead.body.alive; ++i) {
            w.run(1);
            for (const auto& [id, o] : w.characters())
                for (const auto& m : o.memory) bitten |= m.type == "attacked_by_undead" && !m.who.empty() && m.who[0] == dead.id;
        }
        CHECK(bitten || !dead.body.alive);
    }
}

TEST(lr_sim_magic_society_attacking_undead_is_no_crime) {
    World w = realWorld();
    auto ps = adults(w);
    Character& a = *ps[0];
    Character& u = *ps[1];
    const InteractionDef* atk = w.content().interaction("attack");
    CHECK(atk != nullptr);
    CHECK(!legalTags(w, a, *atk, nullptr, &u).empty());
    u.traits.push_back("undead");
    CHECK(legalTags(w, a, *atk, nullptr, &u).empty());
}

TEST(lr_sim_magic_society_haunting_quest_for_unbound_dead) {
    World w = realWorld();
    w.run(kTicksPerDay + 1); // the realm is laid out
    auto ps = adults(w);
    Character& dead = *ps[1];
    dead.traits.push_back("undead");
    dead.traits.push_back("mindless");
    Json& ms = w.moduleState("magic");
    if (!ms.is_object()) ms = Json::object();
    ms["undead"][std::to_string(dead.id)] = {{"master", kNone}, {"since", w.day()}, {"mind", "mindless"}, {"orphan_days", 0}};
    bool posted = false;
    for (int d = 0; d < 25 && !posted; ++d) {
        w.run(kTicksPerDay);
        for (const auto& q : loadRealm(w).quests) posted |= q.kind == "haunting" && q.target == dead.id;
        if (!dead.body.alive) break;
    }
    CHECK(posted || !dead.body.alive);
}

TEST(lr_sim_magic_society_state_saves) {
    World w = realWorld();
    Character* mage = commoner(w);
    auto near = neighbours(w, *mage, 3);
    for (Character* o : near) o->pos = {mage->pos.x + 2.0, mage->pos.y};
    CastTarget self;
    self.who = mage->id;
    for (int i = 0; i < 4; ++i) {
        know(w, *mage, "death", 0.7);
        castSpell(w, *mage, spell("death", '-', "touch"), self);
        w.run(1);
    }
    holdTrial(w, *mage);
    const Json j = w.toJson();
    World back = realWorld();
    back.loadJson(j);
    const Json* s = static_cast<const World&>(back).moduleState("magic_society");
    CHECK(s != nullptr && s->contains("trials") && (*s)["trials"].contains(std::to_string(mage->id)));
}
