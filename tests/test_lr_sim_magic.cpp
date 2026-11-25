// Magic of aspects (AL-116, LivelyRealms §5): data, casting, knowledge and the
// gift, mana, the world sink, potions, raising the dead and their upkeep, and
// the paths out of grief (coping.json) down to necromancy.

#include <algorithm>
#include <cstdio>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/magic.hpp"
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

std::vector<Character*> people(World& w) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") out.push_back(&c);
    return out;
}

int chronicled(const World& w, const std::string& code) {
    int n = 0;
    for (const auto& e : w.chronicleLog()) n += e.code == code;
    return n;
}

Spell spell(const std::string& aspect, char vec, const std::string& form, int power = 1) {
    Spell s;
    s.parts.push_back({aspect, vec});
    s.form = form;
    if (power > 1) s.mods["power"] = power;
    return s;
}

void know(World& w, Character& c, const std::string& aspect, double k) {
    MagicSelf m = magicOf(w, c);
    m.rolled = true;
    m.aspects[aspect] = k;
    m.mana = 50.0;
    m.manaAt = w.now();
    setMagicOf(w, c, m);
    c.skills["magic"] = 0.6;
}

void fillMana(World& w, Character& c) {
    MagicSelf m = magicOf(w, c);
    m.mana = 50.0;
    m.manaAt = w.now();
    setMagicOf(w, c, m);
}

// The best of several casts: the tier is a roll, the test wants the spell to land.
CastOutcome castUntil(World& w, Character& c, const Spell& s, const CastTarget& t, int tries = 12) {
    CastOutcome o;
    for (int i = 0; i < tries; ++i) {
        fillMana(w, c);
        o = castSpell(w, c, s, t);
        if (!o.cast || (o.power > 0.0 && !o.backfire)) return o;
        w.run(1); // another tick, another roll
    }
    return o;
}

} // namespace

TEST(lr_sim_magic_rules_load) {
    const Content& c = realContent();
    const MagicRules& r = magicRules(c);
    CHECK(r.loaded);
    CHECK(r.aspects.size() >= 20);
    CHECK(r.forms.count("bolt") && r.forms.count("touch") && r.forms.count("aura"));
    CHECK(r.troubles.count("kin_death"));
    const auto err = checkMagic(c);
    for (const auto& e : err) std::printf("  %s\n", e.c_str());
    CHECK(err.empty());
}

TEST(lr_sim_magic_loc_keys) {
    const Loc loc = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    for (const auto& k : magicLocKeys(realContent()))
        for (const char* lang : {"ru", "en"}) {
            const bool has = loc.entry(lang, k) != nullptr;
            if (!has) std::printf("  missing %s %s\n", lang, k.c_str());
            CHECK(has);
        }
}

TEST(lr_sim_magic_spell_json_and_key) {
    Spell a;
    a.parts = {{"fire", '+'}, {"air", '+'}};
    a.form = "wave";
    a.mods["power"] = 2;
    a.name = "Вихрь";
    const Spell b = spellFromJson(spellToJson(a));
    CHECK_EQ(b.parts.size(), size_t(2));
    CHECK_EQ(b.form, std::string("wave"));
    CHECK_EQ(b.mod("power"), 2);
    CHECK_EQ(b.name, std::string("Вихрь"));
    Spell c = a;
    std::swap(c.parts[0], c.parts[1]);
    c.name = "other";
    CHECK_EQ(c.key(), a.key()); // the same spell whatever the order and the name
}

TEST(lr_sim_magic_knowledge_gates) {
    World w = realWorld();
    auto ps = people(w);
    CHECK(ps.size() >= 2);
    Character& mage = *ps[0];
    Character& other = *ps[1];
    other.pos = {mage.pos.x + 1.0, mage.pos.y};
    CastTarget t;
    t.who = other.id;
    // unknown aspect: nothing happens
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "touch"), t).reason, std::string("not_known"));
    // a novice holds only touch and aura without extra power
    know(w, mage, "fire", 0.3);
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "bolt"), t).reason, std::string("novice_form"));
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "touch", 2), t).reason, std::string("novice_form"));
    CHECK(castSpell(w, mage, spell("fire", '+', "touch"), t).cast);
    // more than three aspects cannot be held; unknown forms are refused
    Spell four = spell("fire", '+', "touch");
    four.parts = {{"fire", '+'}, {"air", '+'}, {"water", '-'}, {"earth", '+'}};
    CHECK_EQ(castSpell(w, mage, four, t).reason, std::string("too_many_aspects"));
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "nope"), t).reason, std::string("unknown_form"));
    // time is prepared but not in the world
    know(w, mage, "time", 0.9);
    CHECK_EQ(castSpell(w, mage, spell("time", '+', "touch"), t).reason, std::string("disabled_aspect"));
}

TEST(lr_sim_magic_cast_bolt_mana_and_learning) {
    World w = realWorld();
    auto ps = people(w);
    Character& mage = *ps[0];
    Character& foe = *ps[1];
    foe.pos = {mage.pos.x + 10.0, mage.pos.y};
    mage.traits.push_back("gifted"); // above the plain ceiling only with the gift
    know(w, mage, "fire", 0.7);
    const SpellPreview pv = previewSpell(w, mage, spell("fire", '+', "bolt"));
    CHECK(pv.ok);
    CHECK(pv.cost > 0.0);
    double total = 0.0;
    for (const auto& [tier, share] : pv.tierChance) total += share;
    CHECK(std::abs(total - 1.0) < 1e-6);
    CastTarget t;
    t.who = foe.id;
    const double hp = foe.body.health;
    const CastOutcome o = castUntil(w, mage, spell("fire", '+', "bolt"), t);
    CHECK(o.cast);
    CHECK(o.power > 0.0);
    CHECK(foe.body.health < hp);
    CHECK(manaNow(w, mage) < 50.0);
    CHECK(aspectKnowledge(w, mage, "fire") > 0.7);
    // out of reach
    foe.pos = {mage.pos.x + 200.0, mage.pos.y};
    fillMana(w, mage);
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "bolt"), t).reason, std::string("out_of_reach"));
    // no mana
    MagicSelf m = magicOf(w, mage);
    m.mana = 0.0;
    m.manaAt = w.now();
    setMagicOf(w, mage, m);
    foe.pos = {mage.pos.x + 5.0, mage.pos.y};
    CHECK_EQ(castSpell(w, mage, spell("fire", '+', "bolt"), t).reason, std::string("no_mana"));
}

TEST(lr_sim_magic_gift_is_talent_everybody_learns) {
    World w = realWorld();
    auto ps = people(w);
    Character& gifted = *ps[0];
    Character& plain = *ps[1];
    gifted.traits.push_back("gifted");
    plain.traits.erase(std::remove(plain.traits.begin(), plain.traits.end(), "gifted"), plain.traits.end());
    const double g = learnAspect(w, gifted, "water", 0.1, "command");
    const double p = learnAspect(w, plain, "water", 0.1, "command");
    CHECK(p > 0.0);     // everybody can learn
    CHECK(g > p * 2.0); // the gift is a talent
    for (int i = 0; i < 200; ++i) {
        learnAspect(w, gifted, "water", 0.1, "command");
        learnAspect(w, plain, "water", 0.1, "command");
    }
    const MagicRules& r = magicRules(w.content());
    CHECK(aspectKnowledge(w, plain, "water") <= r.ceilPlain + 1e-9);
    CHECK(aspectKnowledge(w, gifted, "water") > r.ceilPlain);
    CHECK(manaMax(w, gifted) > manaMax(w, plain) - 1e-9);
}

TEST(lr_sim_magic_world_sink_and_potion) {
    World w = realWorld();
    auto ps = people(w);
    Character& mage = *ps[0];
    Character& sick = *ps[1];
    std::vector<std::string> fields;
    setMagicWorldSink([&](World&, Vec2, double, const Json& ch, double) {
        for (auto it = ch.begin(); it != ch.end(); ++it) fields.push_back(it.key());
    });
    know(w, mage, "fire", 0.8);
    CastTarget at;
    at.hasPos = true;
    at.at = {mage.pos.x + 5.0, mage.pos.y};
    castUntil(w, mage, spell("fire", '+', "area"), at);
    setMagicWorldSink(nullptr);
    CHECK(std::find(fields.begin(), fields.end(), "heat") != fields.end());
    CHECK(std::find(fields.begin(), fields.end(), "aether") != fields.end());
    // a potion of Life heals without mana or knowledge
    sick.body.health = 0.4;
    applyAspectsTo(w, sick, {{"life", '+', 2.0}}, kNone);
    CHECK(sick.body.health > 0.55);
    CHECK(sick.hasMoodlet("healed_by_magic", w.now()));
}

TEST(lr_sim_magic_raise_and_upkeep) {
    World w = realWorld();
    auto ps = people(w);
    Character& necro = *ps[0];
    Character& dead = *ps[1];
    dead.body.alive = false;
    dead.body.health = 0.0;
    dead.body.causeOfDeath = "illness";
    dead.pos = {necro.pos.x + 1.0, necro.pos.y};
    magicDaily(w); // the death is seen
    know(w, necro, "death", 0.7);
    CastTarget t;
    t.who = dead.id;
    const CastOutcome o = castUntil(w, necro, spell("death", '~', "touch", 2), t, 20);
    CHECK_EQ(o.raised, dead.id);
    CHECK(dead.body.alive);
    CHECK(isUndead(dead));
    CHECK_EQ(undeadMaster(w, dead), necro.id);
    CHECK(chronicled(w, "chronicle.magic.raise") == 1);
    // the undead's needs stand still
    const double hunger = dead.need("hunger");
    w.run(kTicksPerDay / 2);
    CHECK(dead.need("hunger") >= hunger - 1e-9);
    // Light and Life burn the dead; Death~ again on the living does nothing
    const double hp = dead.body.health;
    applyAspectsTo(w, dead, {{"light", '+', 1.0}}, necro.id);
    CHECK(dead.body.health < hp);
    // the master dies: the body is unbound, rots and falls
    necro.body.alive = false;
    for (int d = 0; d < 40 && dead.body.alive; ++d) w.run(kTicksPerDay);
    CHECK(chronicled(w, "chronicle.magic.unbound") >= 1);
    CHECK(!dead.body.alive);
}

TEST(lr_sim_magic_grief_starts_a_path) {
    World w = realWorld();
    Character* widow = nullptr;
    Character* husband = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.spouse != kNone && c.body.sex == 'f') {
            widow = &c;
            husband = w.character(c.status.spouse);
            break;
        }
    CHECK(widow && husband);
    if (!widow || !husband) return;
    husband->body.alive = false;
    husband->body.causeOfDeath = "illness";
    w.run(kTicksPerDay + 1);
    const Json* s = static_cast<const World&>(w).moduleState("magic");
    CHECK(s && (*s)["coping"].contains(std::to_string(widow->id)));
    CHECK(chronicled(w, "chronicle.magic.coping.kin_death.grieve") >= 1);
}

TEST(lr_sim_magic_path_of_the_necromancer) {
    World w = realWorld(11);
    Character* mourner = nullptr;
    Character* lost = nullptr;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.spouse != kNone) {
            mourner = &c;
            lost = w.character(c.status.spouse);
            break;
        }
    CHECK(mourner && lost);
    if (!mourner || !lost) return;
    mourner->traits = {"stubborn", "unscrupulous", "gifted"};
    mourner->standing.honor = 0.1;
    lost->body.alive = false;
    lost->body.causeOfDeath = "fight";
    lost->pos = mourner->pos;
    w.run(kTicksPerDay + 1);
    // straight to forbidden study: the stage learns Death until a first spell can be held
    Json& p = w.moduleState("magic")["coping"][std::to_string(mourner->id)];
    p["stage"] = "study_forbidden";
    p["since"] = static_cast<int>(w.now() / kTicksPerDay);
    const MagicRules& r = magicRules(w.content());
    for (int d = 0; d < 60 && aspectKnowledge(w, *mourner, "death") < r.useMin; ++d) w.run(kTicksPerDay);
    CHECK(aspectKnowledge(w, *mourner, "death") >= r.useMin);
    // the raising itself: the stage casts Death~ on the lost one until it answers
    Json& p2 = w.moduleState("magic")["coping"][std::to_string(mourner->id)];
    p2["stage"] = "raise";
    know(w, *mourner, "death", 0.6);
    for (int d = 0; d < 30 && !lost->body.alive; ++d) {
        fillMana(w, *mourner);
        lost->pos = mourner->pos;
        w.run(kTicksPerDay);
    }
    CHECK(lost->body.alive);
    CHECK(isUndead(*lost));
    CHECK_EQ(undeadMaster(w, *lost), mourner->id);
}

TEST(lr_sim_magic_save_and_teach) {
    World w = realWorld();
    auto ps = people(w);
    Character& teacher = *ps[0];
    Character& pupil = *ps[1];
    know(w, teacher, "life", 0.8);
    Spell s = spell("life", '+', "touch");
    s.name = "Тёплая ладонь";
    const int i = saveSpell(w, teacher, s);
    CHECK_EQ(saveSpell(w, teacher, s), i);
    CHECK_EQ(magicOf(w, teacher).book.size(), size_t(1));
    CHECK(teachAspect(w, teacher, pupil, "life"));
    CHECK(aspectKnowledge(w, pupil, "life") > 0.0);
    CHECK(!teachAspect(w, pupil, teacher, "life")); // the pupil cannot teach the master
    // the same through commands
    Command c;
    c.type = "magic_learn";
    c.args = {{"id", pupil.id}, {"aspect", "earth"}, {"amount", 0.3}};
    w.applyNow(c);
    CHECK(aspectKnowledge(w, pupil, "earth") > 0.0);
}

TEST(lr_sim_magic_state_saves) {
    World w = realWorld();
    auto ps = people(w);
    know(w, *ps[0], "stone", 0.5);
    const Json j = w.toJson();
    World w2 = realWorld();
    w2.loadJson(j);
    CHECK(std::abs(aspectKnowledge(w2, *w2.character(ps[0]->id), "stone") - 0.5) < 1e-9);
}
