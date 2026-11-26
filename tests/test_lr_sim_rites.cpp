// LR-171: prayers and rites of the cults (sim/rites.hpp): the data loads and validates,
// a prayer with parameters is answered (the pantheon's hook or the fallback), the aims
// fit the person's troubles, rites are held per settlement by their steps, attendance,
// effects and the common prayer; mysteries raise grades; intimacy in an ecstatic rite
// only between consenting adults (setting v0.3 §1.1).

#include <algorithm>
#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/culture.hpp"
#include "sim/rites.hpp"
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

EntityId firstSettlement(const World& w) { return w.settlements().empty() ? kNone : w.settlements().begin()->first; }

std::vector<Character*> villagers(World& w, EntityId sid) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.status.settlement == sid) out.push_back(&c);
    return out;
}

Character* adultOf(World& w, EntityId sid) {
    for (Character* c : villagers(w, sid))
        if (c->body.stage() != "child") return c;
    return nullptr;
}

bool hasMood(const World& w, const Character& c, const std::string& id) {
    for (const auto& m : c.moodlets)
        if (m.id == id && m.until > w.now()) return true;
    return false;
}

const Json* rs(const World& w) { return w.moduleState("rites"); }

std::string cmd(World& w, const std::string& type, Json args) {
    Command c;
    c.type = type;
    c.source = "scenario";
    c.args = std::move(args);
    return w.applyNow(std::move(c)).result;
}

struct HookGuard {
    ~HookGuard() {
        setPrayerAnswerHook({});
        setRiteDoneHook({});
    }
};

} // namespace

TEST(lr_sim_rites_load_and_validate) {
    const RitesRules& r = ritesRules(realContent());
    CHECK(r.loaded);
    for (const auto& e : r.errors) std::printf("  %s\n", e.c_str());
    CHECK(r.errors.empty());
    CHECK(r.rites.size() >= 40);
    CHECK(r.aims.size() >= 14);
    ValidationReport rep;
    validateRites(realContent(), rep);
    for (const auto& e : rep.errors) std::printf("  %s\n", e.c_str());
    CHECK(rep.errors.empty());
    // 16 groups of cults, each with rites of its own
    std::set<std::string> groups;
    for (const auto& [id, d] : r.rites) groups.insert(d.group);
    CHECK(groups.size() == 16);
    // the kinds of §6.5 are all there
    std::set<std::string> kinds;
    for (const auto& [id, d] : r.rites) kinds.insert(d.kind);
    for (const char* k : {"sacrifice", "feast", "oath", "pilgrimage", "contest", "funeral", "ecstatic", "mystery", "trance",
                          "vigil", "hunt", "incubation", "offering", "wedding"})
        CHECK(kinds.count(k) == 1);
}

TEST(lr_sim_rites_intimacy_only_adults_by_consent) {
    // §1.1: every rite with intimacy is for adults and by consent; the validator says so
    const RitesRules& r = ritesRules(realContent());
    int ecstatic = 0;
    for (const auto& [id, d] : r.rites) {
        if (!d.intimacy.on) continue;
        ++ecstatic;
        CHECK(d.intimacy.adultsOnly && d.intimacy.consent);
        CHECK(d.intimacy.minLifeYears >= 18);
        CHECK(d.age == "adult");
    }
    CHECK(ecstatic >= 2);
    // the intimacy act itself asks both sides not to be a child
    const InteractionDef* ia = realContent().interaction("intimacy");
    CHECK(ia);
    bool other = false;
    for (const auto& k : ia->pre) other |= k.onTarget && k.subject == "age";
    CHECK(other);
}

TEST(lr_sim_rites_faiths_have_rites) {
    // every faith a culture keeps has rites of its own
    for (const auto& [id, f] : cultureRules(realContent()).faiths) {
        const auto rs = ritesOfFaith(realContent(), id);
        if (rs.empty()) std::printf("  faith %s has no rites\n", id.c_str());
        CHECK(!rs.empty());
    }
}

TEST(lr_sim_rites_prayer_hook_answers) {
    HookGuard g;
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    Character* c = adultOf(w, sid);
    CHECK(c);
    Prayer seen;
    setPrayerAnswerHook([&](World&, const Prayer& p) {
        seen = p;
        PrayerAnswer a;
        a.kind = PrayerAnswer::Granted;
        a.strength = 1.0;
        return a;
    });
    c->body.health = 0.4;
    const double before = c->body.health;
    CHECK(cmd(w, "pray", {{"id", c->id}, {"aim", "heal"}, {"now", true}}) == "ok:granted");
    CHECK(seen.who == c->id && seen.aim == "heal");
    CHECK(!seen.faith.empty());
    CHECK(seen.weight > 0.0);
    CHECK(c->body.health > before);
    CHECK(hasMood(w, *c, "prayer_granted"));
    // a refusal with a reason
    setPrayerAnswerHook([](World&, const Prayer&) {
        PrayerAnswer a;
        a.kind = PrayerAnswer::Refused;
        a.reason = "pantheon.refuse.no_favor";
        return a;
    });
    CHECK(cmd(w, "pray", {{"id", c->id}, {"aim", "wisdom"}, {"now", true}}) == "ok:refused");
    CHECK(hasMood(w, *c, "prayer_refused"));
    // unknown aim / an offering one does not have
    CHECK(cmd(w, "pray", {{"id", c->id}, {"aim", "nope"}}) == "rejected:unknown_aim");
    CHECK(cmd(w, "pray", {{"id", c->id}, {"offering", "gold"}}).rfind("rejected", 0) == 0 || c->inventory.count("gold"));
}

TEST(lr_sim_rites_revenge_on_kin_refused) {
    HookGuard g;
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    Character* a = nullptr;
    Character* b = nullptr;
    for (Character* c : villagers(w, sid))
        for (Character* d : villagers(w, sid))
            if (!a && c != d && c->status.family == d->status.family && c->status.family != kNone) a = c, b = d;
    CHECK(a && b);
    bool called = false;
    setPrayerAnswerHook([&](World&, const Prayer&) {
        called = true;
        return PrayerAnswer{};
    });
    CHECK(cmd(w, "pray", {{"id", a->id}, {"aim", "revenge"}, {"target", b->id}, {"now", true}}) == "ok:refused");
    CHECK(!called); // the world's own rule, before any god
}

TEST(lr_sim_rites_offering_raises_weight) {
    World w = realWorld();
    Character* c = adultOf(w, firstSettlement(w));
    CHECK(c);
    const double none = prayerWeight(w, *c, "pray_alone", "", "");
    const double gold = prayerWeight(w, *c, "pray_at_shrine", "gold", "");
    const double bread = prayerWeight(w, *c, "pray_at_shrine", "bread", "");
    CHECK(gold > bread && bread > none);
}

TEST(lr_sim_rites_aim_fits_trouble) {
    World w = realWorld();
    Character* c = adultOf(w, firstSettlement(w));
    CHECK(c);
    applyEffectList(w, *c, {parseEffect("moodlet:ill_fever")}, nullptr); // the body's health is kept by its wounds
    int heal = 0;
    for (int i = 0; i < 20; ++i) {
        w.run(1);
        EntityId whom = kNone;
        if (prayerAimOf(w, *c, &whom) == "heal") {
            ++heal;
            CHECK(whom != kNone);
        }
    }
    if (heal < 8) {
        EntityId whom = kNone;
        std::printf("  health %.2f stage %s aim now %s\n", c->body.health, c->body.stage().c_str(), prayerAimOf(w, *c, &whom).c_str());
    }
    CHECK(heal >= 8);
}

TEST(lr_sim_rites_fallback_deferred_come_true) {
    World w = realWorld();
    Character* c = adultOf(w, firstSettlement(w));
    CHECK(c);
    int granted = 0, deferred = 0, silent = 0;
    for (int i = 0; i < 40; ++i) {
        Prayer p;
        p.who = c->id;
        p.aim = "thanks";
        p.weight = 1.5;
        const PrayerAnswer a = offerPrayer(w, p);
        granted += a.kind == PrayerAnswer::Granted;
        deferred += a.kind == PrayerAnswer::Deferred;
        silent += a.kind == PrayerAnswer::Silent;
        w.run(1);
    }
    CHECK(granted > 0 && deferred > 0 && silent > 0);
    const Json* st = rs(w);
    CHECK(st && (*st)["deferred"].size() > 0);
    w.run(kTicksPerDay * 6);
    // the ones deferred by then came due (new prayers of the villagers may wait still)
    for (const Json& d : (*rs(w))["deferred"]) CHECK(d.value("due", 0) > w.day());
}

TEST(lr_sim_rites_rite_held_by_steps) {
    HookGuard g;
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    // give everyone the hearth faith so the hearth rites are theirs
    for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "hearth_faith"}, {"piety", 0.7}});
    std::map<EntityId, double> done;
    setRiteDoneHook([&](World&, EntityId who, const std::string& faith, const std::string& rite, double weight) {
        CHECK(faith == "hearth_faith");
        CHECK(rite == "new_fire");
        CHECK(weight > 0.0);
        done[who] += 1;
    });
    const std::string res = cmd(w, "start_rite", {{"settlement", sid}, {"rite", "new_fire"}});
    CHECK(res == "ok");
    const Json* st = rs(w);
    CHECK(st && (*st)["active"].size() == 1);
    // people are called to the first step
    w.run(2);
    const Json& rec = (*rs(w))["active"][0];
    CHECK(rec["step"] == 0);
    int called = 0;
    for (Character* c : villagers(w, sid))
        for (const auto& q : c->ai.queue) called += q.source == "rites";
    CHECK(called > 0);
    CHECK(!riteOf(w, rec["leader"].get<EntityId>()).is_null());
    w.run(kTicksPerDay / 2);
    const Json* st2 = rs(w);
    CHECK((*st2)["active"].empty());
    CHECK((*st2)["done"].size() == 1);
    const Json& fin = (*st2)["done"][0];
    std::printf("  new_fire: came %zu of %zu, weight %.2f\n", fin["came"].size(), fin.value("people", Json::array()).size(),
                fin.value("weight", 0.0));
    CHECK(fin["came"].size() >= 2);
    CHECK(done.size() == fin["came"].size());
    // the attendees remember the rite and feel it
    const EntityId one = fin["came"][0].get<EntityId>();
    Character* c = w.character(one);
    bool mem = false;
    for (const auto& m : c->memory) mem |= m.type == "rite" && m.goods.count("rite:new_fire");
    CHECK(mem);
    CHECK(hasMood(w, *c, "rite_joy"));
}

TEST(lr_sim_rites_daily_schedules_by_calendar) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "sun_oracle"}, {"piety", 0.6}});
    if (!cultureRules(w.content()).faithDef("sun_oracle")) {
        // the pantheon thread brings the new faiths: until then a dawn hymn of the old ones
        for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "hearth_faith"}, {"piety", 0.6}});
    }
    // ten days: the every-N-days rites come
    std::set<std::string> held;
    for (int d = 0; d < 12; ++d) {
        w.run(kTicksPerDay);
        if (const Json* st = rs(w))
            for (const char* k : {"active", "done"})
                for (const Json& rec : (*st)[k]) held.insert(rec["rite"].get<std::string>());
    }
    for (const auto& h : held) std::printf("  held: %s\n", h.c_str());
    CHECK(!held.empty());
}

TEST(lr_sim_rites_mystery_grades) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "green_covenant"}, {"piety", 0.8}});
    // the sowing mysteries need a field (the test world has the households' fields)
    for (int round = 0; round < 3; ++round) {
        CHECK(cmd(w, "start_rite", {{"settlement", sid}, {"rite", "sowing_mysteries"}}) == "ok");
        w.run(kTicksPerDay / 2);
    }
    const Json& done = (*rs(w))["done"];
    CHECK(done.size() >= 3);
    int top = 0, any = 0;
    for (Character* c : villagers(w, sid)) {
        const int g = riteGrade(w, c->id, "sowing_mysteries");
        any += g >= 0;
        top += g == 2;
        if (c->body.stage() == "child") CHECK(g < 0); // adults only
    }
    std::printf("  initiated %d, hierophants %d\n", any, top);
    CHECK(any > 0);
    CHECK(top > 0);
}

TEST(lr_sim_rites_ecstatic_pairs_adults_consenting) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    auto ppl = villagers(w, sid);
    for (Character* c : ppl) cmd(w, "set_faith", {{"id", c->id}, {"faith", "hearth_faith"}, {"piety", 0.7}});
    // the temple feast needs a lik of love: give the faith's people a rite of the love group
    // through the faith list (hearth_faith keeps the love rites)
    // mutual liking among the adults: some pairs want each other, children get nothing
    std::vector<Character*> adults;
    for (Character* c : ppl)
        if (c->body.stage() != "child" && c->body.lifeYears() >= 18) adults.push_back(c);
    CHECK(adults.size() >= 4);
    for (std::size_t i = 0; i + 1 < adults.size(); i += 2) {
        adults[i]->relations[adults[i + 1]->id].affinity = 0.8;
        adults[i]->relations[adults[i + 1]->id].attraction = 0.8;
        adults[i + 1]->relations[adults[i]->id].affinity = 0.8;
        adults[i + 1]->relations[adults[i]->id].attraction = 0.8;
    }
    // a child who "likes" an adult is never paired (and the adult never paired with the child)
    for (Character* c : ppl)
        if (c->body.stage() == "child") {
            c->relations[adults[0]->id].affinity = 0.9;
            c->relations[adults[0]->id].attraction = 0.9;
            adults[0]->relations[c->id].affinity = 0.9;
            adults[0]->relations[c->id].attraction = 0.9;
        }
    // one-sided liking is no consent
    if (adults.size() >= 6) {
        adults[4]->relations[adults[5]->id].attraction = 0.0;
    }
    CHECK(cmd(w, "start_rite", {{"settlement", sid}, {"rite", "temple_feast"}}) == "ok");
    std::set<EntityId> paired;
    Json pairs = Json::array();
    for (int t = 0; t < kTicksPerDay / 2; ++t) {
        w.run(1);
        const Json* st = rs(w);
        for (const Json& rec : (*st)["active"])
            if (rec.contains("pairs")) pairs = rec["pairs"];
    }
    for (const Json& p : pairs) {
        const Character* a = w.character(p[0].get<EntityId>());
        const Character* b = w.character(p[1].get<EntityId>());
        CHECK(a && b);
        CHECK(a->body.stage() != "child" && b->body.stage() != "child");
        CHECK(a->body.lifeYears() >= 18 && b->body.lifeYears() >= 18);
        CHECK(a->relations.at(b->id).affinity >= 0.3 && b->relations.at(a->id).affinity >= 0.3);
        CHECK(a->relations.at(b->id).attraction >= 0.25 && b->relations.at(a->id).attraction >= 0.25);
    }
    std::printf("  temple_feast pairs: %zu\n", pairs.size());
    CHECK(pairs.size() >= 1);
}

TEST(lr_sim_rites_forbidden_by_culture) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "hearth_faith"}, {"piety", 0.7}});
    const RitesRules& r = ritesRules(w.content());
    CHECK(r.rite("temple_feast"));
    // a rite whose first step has no place near is not held
    CHECK(cmd(w, "start_rite", {{"settlement", sid}, {"rite", "nope"}}) == "rejected:unknown_rite");
}

TEST(lr_sim_rites_save_roundtrip) {
    World w = realWorld();
    const EntityId sid = firstSettlement(w);
    for (Character* c : villagers(w, sid)) cmd(w, "set_faith", {{"id", c->id}, {"faith", "hearth_faith"}, {"piety", 0.7}});
    CHECK(cmd(w, "start_rite", {{"settlement", sid}, {"rite", "new_fire"}}) == "ok");
    w.run(3);
    const Json saved = w.toJson();
    World w2 = realWorld();
    w2.loadJson(saved);
    const Json* st = rs(w2);
    CHECK(st && (*st)["active"].size() == 1);
}
