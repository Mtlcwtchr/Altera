// LivelyRealms life-sim core: localization (LOC_FORMAT.md) and the mini-language
// extensions of AI_DATA_SCHEMA.md v0.2 (t. prefix, params, chance/risk,
// combat, terms, social effects, reputation, rel axes, dialogue.json).

#include <set>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/loc.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& content() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/tests/data/lr_mvp/ai");
    return c;
}

const Loc& loc() {
    static const Loc l = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/tests/data/lr_mvp/loc");
    return l;
}

World aiWorld(std::uint64_t seed = 0) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/tests/data/lr_mvp/test_world.json");
    World w = makeTestWorld(content(), cfg, seed);
    installAi(w);
    return w;
}

bool mentions(const std::vector<std::string>& list, const std::string& s) {
    for (const auto& e : list)
        if (e.find(s) != std::string::npos) return true;
    return false;
}

std::string head(const std::string& reason) { return reason.substr(0, reason.find(':')); }

// Minimal content around one interaction list (items/objects/needs/moodlets fixed).
Content miniContent(const Json& interactions, const Json& dialogue = Json::object(), const Json& config = Json::object()) {
    const Json items = Json::parse(R"({"items": [{"id": "meal"}, {"id": "berries"}]})");
    const Json objects = Json::parse(R"({"objects": [{"id": "storage", "stock": {"meal": 2}}]})");
    const Json needs = Json::parse(R"({"needs": [{"id": "hunger"}], "moodlets": [{"id": "glad"}]})");
    const Json goals = Json::parse(R"({"goals": [{"id": "eat", "task": "t",
        "considerations": [{"input": "need:hunger", "curve": {"type": "inverse"}}]}]})");
    const Json htn = Json::parse(R"({"tasks": [{"id": "t", "methods": [{"id": "m", "steps": [{"do": "eat"}]}]}]})");
    Json inter = interactions;
    inter["interactions"].push_back(Json::parse(
        R"({"id": "eat", "category": "household", "pre": ["has:meal>=1"], "eff": ["has:meal-1"], "duration": 1})"));
    return Content::fromJson(items, objects, needs, Json::object(), goals, htn, inter, Json::object(), config,
                             dialogue);
}

} // namespace

// ---- the formatter -------------------------------------------------------

TEST(lr_sim_loc_format) {
    const Json noun = {{"nom", "очаг"}, {"gen", "очага"}, {"dat", "очагу"}, {"g", "m"}};
    const Json her = {{"nom", "Ирма"}, {"dat", "Ирме"}, {"g", "f"}};
    CHECK_EQ(formatLoc("у {o:gen}, к {o:dat}, это {o}", {{"o", noun}}, "ru"), std::string("у очага, к очагу, это очаг"));
    // A missing case form falls back to nom.
    CHECK_EQ(formatLoc("{o:ins}", {{"o", noun}}, "ru"), std::string("очаг"));
    CHECK_EQ(formatLoc("{s} рад{s:g||а}", {{"s", her}}, "ru"), std::string("Ирма рада"));
    CHECK_EQ(formatLoc("рад{s:g||а}", {{"s", noun}}, "ru"), std::string("рад"));
    CHECK_EQ(formatLoc("скажи {s:dat}", {{"s", her}}, "ru"), std::string("скажи Ирме"));
    // ru plural: one / few / many (+ fractions take the few form).
    const std::string p = "{n} {n:plural|ягода|ягоды|ягод}";
    CHECK_EQ(formatLoc(p, {{"n", 1}}, "ru"), std::string("1 ягода"));
    CHECK_EQ(formatLoc(p, {{"n", 3}}, "ru"), std::string("3 ягоды"));
    CHECK_EQ(formatLoc(p, {{"n", 5}}, "ru"), std::string("5 ягод"));
    CHECK_EQ(formatLoc(p, {{"n", 11}}, "ru"), std::string("11 ягод"));
    CHECK_EQ(formatLoc(p, {{"n", 21}}, "ru"), std::string("21 ягода"));
    CHECK_EQ(formatLoc(p, {{"n", 24}}, "ru"), std::string("24 ягоды"));
    CHECK_EQ(formatLoc(p, {{"n", 112}}, "ru"), std::string("112 ягод"));
    CHECK_EQ(formatLoc(p, {{"n", 1.5}}, "ru"), std::string("1,5 ягоды"));
    // en: two forms.
    const std::string e = "{n} {n:plural|berry|berries}";
    CHECK_EQ(formatLoc(e, {{"n", 1}}, "en"), std::string("1 berry"));
    CHECK_EQ(formatLoc(e, {{"n", 0}}, "en"), std::string("0 berries"));
    CHECK_EQ(formatLoc(e, {{"n", 2.5}}, "en"), std::string("2.5 berries"));
    // Escapes, arrays, missing arguments.
    CHECK_EQ(formatLoc("{{x}} {a}", {{"a", Json::array({"a", "b"})}}, "ru"), std::string("{x} a, b"));
    std::vector<std::string> errs;
    CHECK_EQ(formatLoc("[{nope}]", Json::object(), "ru", &errs), std::string("[{nope}]"));
    CHECK(mentions(errs, "missing argument 'nope'"));
    // Syntax checks.
    std::string why;
    CHECK(!checkLocPattern("{n:plural|a|b}", "ru", why)); // ru needs 3 forms
    CHECK(checkLocPattern("{n:plural|a|b}", "en", why));
    CHECK(!checkLocPattern("{x:abl}", "ru", why));
    CHECK(!checkLocPattern("{x:g|one}", "ru", why));
    CHECK(!checkLocPattern("broken {x", "ru", why));
    CHECK(!checkLocPattern("broken }", "ru", why));
}

TEST(lr_sim_loc_fallback) {
    const Json ru = Json::parse(R"({"a": {"b": "А {x}"}, "only_ru": "только", "noun": {"nom": "кот", "g": "m"}})");
    const Json en = Json::parse(R"({"a": {"b": "A {x}"}})");
    const Loc l = Loc::fromJson("ru", "ru", {{"ru", {ru}}, {"en", {en}}});
    CHECK_EQ(l.text("en", "a.b", {{"x", 1}}), std::string("A 1"));
    CHECK_EQ(l.text("en", "only_ru"), std::string("только")); // missing translation -> fallback
    CHECK_EQ(l.text("de", "a.b", {{"x", 2}}), std::string("А 2")); // unknown language -> fallback
    CHECK_EQ(l.text("en", "nowhere"), std::string("[nowhere]"));
    CHECK_EQ(l.text("ru", "noun"), std::string("кот"));
    CHECK_EQ(l.noun("en", "noun", "?")["g"].get<std::string>(), std::string("m"));
    CHECK_EQ(l.noun("en", "nowhere", "raw")["nom"].get<std::string>(), std::string("raw"));
}

// ---- the validator -----------------------------------------------------------

TEST(lr_sim_loc_validates) {
    const ValidationReport r = validateLoc(content(), loc());
    for (const auto& e : r.errors) std::cerr << "    " << e << "\n";
    for (const auto& w : r.warnings) std::cerr << "    warning: " << w << "\n";
    CHECK(r.ok());
    CHECK(r.warnings.empty()); // en is complete
    // Data carries keys only: every content id has its name in the base language.
    CHECK(loc().has("ru", "interaction.gather_berries.name"));
    CHECK(loc().has("ru", "dialogue.greet.warm"));
    CHECK(loc().has("en", "trace.goal.done"));
}

TEST(lr_sim_loc_validator_catches_problems) {
    // Base lacks a required key; en lacks a translation and has an orphan, a
    // bad pattern, a wrong plural form count and a placeholder the base lacks.
    Json ru = Json::object();
    Json en = Json::object();
    for (const auto& k : requiredLocKeys(content())) {
        ru[k] = "x {a}";
        en[k] = "x {a}";
    }
    ru.erase("need.hunger.name");
    en.erase("goal.eat.name");
    en["orphan.key"] = "?";
    en["trace.goal.done"] = "{a";
    en["trace.goal.failed"] = "{n:plural|one|few|many}";
    en["trace.goal.dropped"] = "{a} {extra}";
    const Loc l = Loc::fromJson("ru", "ru", {{"ru", {ru}}, {"en", {en}}});
    const ValidationReport r = validateLoc(content(), l);
    CHECK(mentions(r.errors, "loc[ru]: missing key 'need.hunger.name'"));
    CHECK(mentions(r.warnings, "loc[en]: missing translation 'goal.eat.name'"));
    CHECK(mentions(r.warnings, "orphan.key: not in the base language"));
    CHECK(mentions(r.errors, "trace.goal.done: unclosed"));
    CHECK(mentions(r.errors, "trace.goal.failed: {n:plural|...} needs 2 forms for en"));
    CHECK(mentions(r.warnings, "placeholder {extra} is not in the base text"));
    // Noun entries: forms and gender are checked.
    const Json bad = Json::parse(R"({"item": {"meal": {"name": {"nom": "x", "abl": "y", "g": "q"}}}})");
    Json ru2 = ru;
    ru2.erase("item.meal.name");
    const ValidationReport r2 = validateLoc(content(), Loc::fromJson("ru", "ru", {{"ru", {ru2, bad}}}));
    CHECK(mentions(r2.errors, "unknown noun form 'abl'"));
    CHECK(mentions(r2.errors, "bad gender 'q'"));
}

// Every trace code and reason the code emits has a loc key: run a world with
// switching and orders, collect what came out.
TEST(lr_sim_loc_covers_emitted_codes) {
    World w = aiWorld(3);
    w.run(kTicksPerDay);
    auto cmd = [](const std::string& type, Json args) {
        Command c;
        c.source = "player:1";
        c.type = type;
        c.args = std::move(args);
        c.args["player"] = 1;
        return c;
    };
    std::vector<EntityId> adults;
    for (const auto& [id, c] : w.characters())
        if (c.body.stage() == "adult") adults.push_back(id);
    if (adults.size() < 2) {
        CHECK(adults.size() >= 2);
        return;
    }
    const EntityId a = adults[0], b = adults[1];
    w.applyNow(cmd("take_control", {{"id", a}}));
    w.applyNow(cmd("hint", {{"id", a}, {"goal", "sleep"}, {"delta", 0.2}}));
    w.applyNow(cmd("set_goal", {{"id", a}, {"goal", "eat"}}));
    w.applyNow(cmd("queue_task", {{"id", a}, {"task", "stock_food"}}));
    w.run(30);
    w.applyNow(cmd("take_control", {{"id", b}}));
    w.run(30);
    w.applyNow(cmd("release_control", Json::object()));
    w.run(kTicksPerDay);
    const std::set<std::string> codes(traceCodes().begin(), traceCodes().end());
    const std::set<std::string> reasons(reasonCodes().begin(), reasonCodes().end());
    int lines = 0;
    for (const auto& [id, c] : w.characters()) {
        for (const auto& e : c.ai.trace) {
            ++lines;
            if (!codes.count(e.code)) std::cerr << "    unlisted trace code " << e.code << "\n";
            CHECK(codes.count(e.code) > 0);
            for (const char* lang : {"ru", "en"}) {
                const std::string t = traceText(loc(), lang, w, c, e);
                if (t.find('[') != std::string::npos || t.find('{') != std::string::npos)
                    std::cerr << "    unresolved: " << e.code << " -> " << t << "\n";
                CHECK(t.find("[trace.") == std::string::npos);
                CHECK(t.find('{') == std::string::npos); // every placeholder had its argument
            }
        }
        for (const auto& [r, n] : c.ai.stats.failReasons) {
            if (!reasons.count(head(r))) std::cerr << "    unlisted reason " << r << "\n";
            CHECK(reasons.count(head(r)) > 0);
        }
    }
    CHECK(lines > 50);
    // Report text.
    Json rep = aiReport(w);
    localizeReport(rep, loc(), "ru", w);
    for (const auto& e : rep["summary"]["why_idle"]) CHECK(e["text"].get<std::string>().find('[') == std::string::npos);
}

TEST(lr_sim_loc_chronicle_and_reasons) {
    World w = aiWorld();
    Character& c = w.characters().begin()->second;
    c.body.sex = 'f';
    c.needs["hunger"] = 0.0;
    c.body.health = 1e-6;
    w.run(2);
    CHECK(!c.body.alive);
    const ChronicleEntry& d = w.chronicleLog().back();
    CHECK_EQ(d.code, std::string("chronicle.death"));
    const std::string ru = loc().text("ru", d.code, locArgs(loc(), "ru", w, d.params));
    CHECK_EQ(ru, c.name + " умерла от голода");
    CHECK_EQ(loc().text("en", d.code, locArgs(loc(), "en", w, d.params)), c.name + " died of starvation");
    CHECK_EQ(reasonText(loc(), "ru", "goap.no_target:hearth"), std::string("нет доступного объекта: очаг"));
    CHECK_EQ(reasonText(loc(), "ru", "fsm.lost:pre:has:meal>=1"),
             std::string("действие сорвалось: не выполнено условие has:meal>=1"));
    CHECK_EQ(reasonText(loc(), "en", "no_such_code:x"), std::string("no_such_code:x"));
}

// ---- mini-language extensions -------------------------------------------------

TEST(lr_sim_schema_parses_extensions) {
    const Condition t = parseCondition("!t.age:child");
    CHECK(t.onTarget && t.negate);
    CHECK_EQ(t.subject, std::string("age"));
    const Condition p = parseCondition("has:$item>=2");
    CHECK(p.isParam());
    CHECK_EQ(p.param(), std::string("item"));
    const Condition b = bindCondition(p, {{"item", "meal"}});
    CHECK_EQ(b.key, std::string("meal"));
    CHECK_EQ(b.text, std::string("has:meal>=2"));
    const Effect e = parseEffect("t.skill:$skill+0.05");
    CHECK(e.onTarget && e.isParam());
    CHECK_EQ(bindEffect(e, {{"skill", "cooking"}}).key, std::string("cooking"));
    CHECK(parseEffect("status_pair:married=0").op == '=');
    CHECK(parseCondition("rel:enmity>=0.5").key == "enmity");
    bool threw = false;
    try {
        parseCondition("t.:x");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        parseCondition("has:$>=1");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        bindCondition(p, {});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // The shipped content uses every extension and validates (content_validates
    // covers errors); its dialogue file is loaded.
    CHECK(content().dialogue.count("propose_marriage") > 0);
    CHECK(content().interaction("brawl")->combat.set);
    CHECK(content().interaction("lend_item")->terms.set);
    CHECK(content().interaction("teach_skill")->chance.set);
}

TEST(lr_sim_schema_validator_extensions) {
    const Json inter = Json::parse(R"({"interactions": [
      {"id": "p_undeclared", "category": "economy", "target": "character", "pre": ["has:$thing>=1"], "duration": 1},
      {"id": "p_type", "category": "economy", "target": "character", "params": {"n": "need"},
       "pre": ["has:$n>=1"], "duration": 1},
      {"id": "p_domain", "category": "economy", "target": "character", "params": {"item": {"type": "item", "of": ["gold"]}},
       "pre": ["has:$item>=1", "has:$other>=1"], "duration": 1},
      {"id": "t_on_self", "category": "social", "target": "self", "eff": ["t.need:hunger+0.1", "rel:trust+0.1"], "duration": 1},
      {"id": "bad_axes", "category": "social", "target": "character",
       "pre": ["rel:love>0.1", "reputation:fame>0", "t.knows_all:x"], "eff": ["t.spawn:x"], "duration": 1},
      {"id": "bad_social", "category": "social", "target": "character",
       "eff": ["status_pair:cousins", "household_join:king", "commitment:oath", "death:boredom", "status_pair:married+1"],
       "duration": 1},
      {"id": "bad_chance", "category": "work", "target": "self", "continuous": true, "until": "need:hunger>=1",
       "chance": {"base": 1.5}, "risk": [{"p": 2, "eff": []}], "duration": 2},
      {"id": "bad_combat", "category": "violence", "target": "self",
       "combat": {"damage": [0.5, 0.2], "outcomes": {"tie": []}}, "duration": 1},
      {"id": "bad_terms", "category": "economy", "target": "character",
       "terms": {"give": {"gold": 1}, "take": {"meal": 0}, "due": 10}, "duration": 1},
      {"id": "bad_speech", "category": "social", "target": "character", "speech": ["sing"], "duration": 1}
    ]})");
    const Json dialogue = Json::parse(R"({"speech_acts": ["greet"], "templates": [
      {"id": "hello", "speech": "greet", "pre": ["t.need:hunger<0.5"], "variants": ["plain"], "replies": ["nobody"]},
      {"id": "song", "speech": "sing", "variants": []}
    ]})");
    const Json config = Json::parse(R"({"status_pairs": ["married"], "commitments": {"debt": {"due": 0}}})");
    const Content c = miniContent(inter, dialogue, config);
    const ValidationReport r = validate(c);
    auto has = [&](const std::string& s) {
        const bool m = mentions(r.errors, s);
        if (!m) std::cerr << "    expected error: " << s << "\n";
        return m;
    };
    CHECK(has("p_undeclared: parameter '$thing' outside a parametric interaction"));
    CHECK(has("p_type: parameter '$n' is a need, 'has:' wants a item"));
    CHECK(has("p_domain: parameter 'item' allows unknown item 'gold'"));
    CHECK(has("p_domain: undeclared parameter '$other'"));
    CHECK(has("t_on_self: 't.need:hunger+0.1' needs a character target"));
    CHECK(has("t_on_self: 'rel:trust+0.1' needs a character target"));
    CHECK(has("bad_axes: unknown rel axis 'love'"));
    CHECK(has("bad_axes: unknown reputation axis 'fame'"));
    CHECK(has("bad_axes: t.knows_all is not a target condition"));
    CHECK(has("bad_axes: t.spawn is not a target effect"));
    CHECK(has("bad_social: unknown status pair 'cousins'"));
    CHECK(has("bad_social: unknown household role 'king'"));
    CHECK(has("bad_social: unknown commitment 'oath'"));
    CHECK(has("bad_social: unknown death cause 'boredom'"));
    CHECK(has("bad_social: effect 'status_pair:married+1' has the wrong form"));
    CHECK(has("bad_chance: chance on a continuous interaction"));
    CHECK(has("bad_chance: chance base out of [0, 1]"));
    CHECK(has("bad_chance: risk without id"));
    CHECK(has("bad_combat: combat needs target: character"));
    CHECK(has("bad_combat: combat damage must be"));
    CHECK(has("bad_combat: unknown combat outcome 'tie'"));
    CHECK(has("bad_terms: terms.give unknown item 'gold'"));
    CHECK(has("bad_terms: terms.take count for 'meal' must be > 0"));
    CHECK(has("bad_terms: deferred terms (due > 0) need a commitment"));
    CHECK(has("bad_speech: unknown speech act 'sing'"));
    CHECK(has("dialogue song: unknown speech act 'sing'"));
    CHECK(has("dialogue song: no variants"));
    CHECK(has("dialogue hello: reply names unknown template 'nobody'"));
    CHECK(has("commitment debt: due must be > 0"));
    CHECK(!mentions(r.errors, "dialogue hello: 't.need")); // dialogue always has a listener
    // Loader-level syntax: a t. prefix on a dotted subject is a syntax error.
    bool threw = false;
    try {
        miniContent(Json::parse(R"({"interactions": [{"id": "x", "category": "social", "target": "character",
            "pre": ["t.a.b:c"], "duration": 1}]})"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// Interpreter stage 1: t.* conditions and effects, rel/rel_back (new axes),
// reputation, moodlet conditions, chance/on_fail/risk, death, bound params.
TEST(lr_sim_schema_interpreter_stage1) {
    World w = aiWorld();
    auto it = w.characters().begin();
    Character& a = it->second;
    Character& b = (++it)->second;
    const Content& ct = w.content();
    // chat: rel + rel_back + t.moodlet.
    applyEffects(w, a, *ct.interaction("chat"), nullptr, &b);
    CHECK(a.relations[b.id].familiarity > 0.0);
    CHECK(b.relations[a.id].familiarity > 0.0);
    CHECK(b.hasMoodlet("chatted", w.now()));
    const EvalCtx x{w, a, nullptr, &b};
    CHECK(evalCondition(x, parseCondition("t.moodlet:chatted")));
    CHECK(!evalCondition(x, parseCondition("t.moodlet:wounded")));
    // give_item bound to berries: t.has.
    a.inventory["berries"] = 3;
    const int before = b.has("berries");
    const InteractionDef give = bindParams(*ct.interaction("give_item"), {{"item", "berries"}});
    std::string why;
    CHECK(canPerform(w, a, give, nullptr, &why));
    applyEffects(w, a, give, nullptr, &b);
    CHECK_EQ(a.has("berries"), 2);
    CHECK_EQ(b.has("berries"), before + 1);
    CHECK(b.relations[a.id].affinity > 0.0);
    bool threw = false;
    try {
        bindParams(*ct.interaction("give_item"), {{"item", "gold"}});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // New rel axes and reputation through the generic effect list.
    applyEffectList(w, a, {parseEffect("rel:enmity+0.7"), parseEffect("rel:respect-0.2"),
                           parseEffect("rel:attraction=0.4"), parseEffect("reputation:valor+0.3"),
                           parseEffect("t.reputation:honesty-2")},
                    nullptr, &b);
    CHECK(evalCondition(x, parseCondition("rel:enmity>=0.7")));
    CHECK(evalCondition(x, parseCondition("rel:respect<0")));
    CHECK(evalCondition(x, parseCondition("rel:attraction==0.4")));
    CHECK(evalCondition(x, parseCondition("reputation:valor>0.29")));
    CHECK(evalCondition(x, parseCondition("t.reputation:honesty==-1"))); // clamped
    // teach_skill: chance with skill; the roll is deterministic per tick.
    a.skills["cooking"] = 0.9;
    b.skills["cooking"] = 0.1;
    const InteractionDef teach = bindParams(*ct.interaction("teach_skill"), {{"skill", "cooking"}});
    CHECK_EQ(teach.chance.skill, std::string("cooking"));
    CHECK(canPerform(w, a, teach, nullptr, &why));
    World w2 = w; // same state, same ticks: the same rolls
    auto lessons = [&](World& ww) {
        Character& t = *ww.character(a.id);
        Character& s = *ww.character(b.id);
        int learned = 0;
        for (int i = 0; i < 40; ++i) {
            const double s0 = s.skills["cooking"];
            applyEffects(ww, t, teach, nullptr, &s);
            if (s.skills["cooking"] > s0) ++learned;
            s.skills["cooking"] = 0.1;
            ww.run(1);
        }
        return learned;
    };
    const int learned = lessons(w);
    CHECK(learned > 20); // p = 0.4 + 0.5 * 0.9 = 0.85
    CHECK(learned < 40);
    CHECK_EQ(lessons(w2), learned);
    // risk: p = 1 always fires; death through t.death records the cause.
    InteractionDef risky;
    risky.id = "risky";
    risky.risk.push_back(RiskDef{"r", 1.0, {parseEffect("body:fatigue+0.3")}});
    risky.eff.push_back(parseEffect("t.death:combat"));
    const double f0 = a.body.fatigue;
    applyEffects(w, a, risky, nullptr, &b);
    CHECK(a.body.fatigue > f0);
    CHECK(!b.body.alive);
    CHECK_EQ(b.body.causeOfDeath, std::string("combat"));
    CHECK_EQ(w.chronicleLog().back().params["cause"].get<std::string>(), std::string("combat"));
    // Without a target character t.* effects are skipped, not misapplied.
    InteractionDef selfOnly;
    selfOnly.eff.push_back(parseEffect("t.death:combat"));
    applyEffects(w, a, selfOnly, nullptr, nullptr);
    CHECK(a.body.alive);
}
