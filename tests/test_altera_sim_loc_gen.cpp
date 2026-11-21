// LOC v0.2 morphology (AL-47): sub-rules, frames, agreement, pronouns,
// prepositions, register, counts, the validator. ru + en.

#include <set>

#include "framework.hpp"
#include "sim/dialogue_gen.hpp"
#include "sim/loc.hpp"
#include "sim/loc_gen.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

bool mentions(const std::vector<std::string>& list, const std::string& s) {
    for (const auto& e : list)
        if (e.find(s) != std::string::npos) return true;
    return false;
}

Json grammarOf(const std::string& lang) {
    return loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc/" + lang + "/_grammar.json");
}

const char* kRu = R"({
 "lex": {
  "verb": {
   "steal": {"inf": "украсть", "pfv": true,
     "past": {"m": "украл", "f": "украла", "n": "украло", "pl": "украли"},
     "fut": {"1s": "украду", "2s": "украдёшь", "3s": "украдёт", "1p": "украдём", "2p": "украдёте", "3p": "украдут"}},
   "go_missing": {"inf": "пропасть", "pfv": true,
     "past": {"m": "пропал", "f": "пропала", "n": "пропало", "pl": "пропали"},
     "fut": {"1s": "пропаду", "2s": "пропадёшь", "3s": "пропадёт", "1p": "пропадём", "2p": "пропадёте", "3p": "пропадут"}},
   "know": {"inf": "знать", "pfv": false,
     "past": {"m": "знал", "f": "знала", "n": "знало", "pl": "знали"},
     "pres": {"1s": "знаю", "2s": "знаешь", "3s": "знает", "1p": "знаем", "2p": "знаете", "3p": "знают"}},
   "say": {"inf": "сказать", "pfv": true,
     "past": {"m": "сказал", "f": "сказала", "n": "сказало", "pl": "сказали"}}
  },
  "adj": {
   "whole": {"m": {"nom": "весь", "gen": "всего", "dat": "всему", "acc": "весь", "acc_an": "всего", "ins": "всем", "pre": "всём"},
             "f": {"nom": "вся", "gen": "всей", "dat": "всей", "acc": "всю", "ins": "всей", "pre": "всей"},
             "n": {"nom": "всё", "gen": "всего", "dat": "всему", "acc": "всё", "ins": "всем", "pre": "всём"},
             "pl": {"nom": "все", "gen": "всех", "dat": "всем", "acc": "все", "acc_an": "всех", "ins": "всеми", "pre": "всех"}}
  },
  "noun": {
   "theft": {"nom": "кража", "gen": "кражи", "dat": "краже", "acc": "кражу", "ins": "кражей", "pre": "краже", "g": "f"}
  }
 },
 "frame": {
  "theft": {"verb": "steal", "noun": "lex.noun.theft",
    "roles": {"doer": "nom", "object": "acc", "victim": "у+gen", "victim_family": "у+gen"},
    "clause": "{doer} {time?} {verb:agree=doer} {victim?} {object?}",
    "nominal": "{noun} {object:gen?} {victim?}"},
  "lose": {"verb": "go_missing", "roles": {"object": "nom", "victim": "у+gen"},
    "clause": "{victim?} {verb:agree=object} {object}"},
  "say": {"verb": "say", "roles": {"speaker": "nom", "listener": "dat", "content": "acc"},
    "clause": "{speaker} {verb:agree=speaker}, что {content}"}
 },
 "item": {"flour": {"nom": "мука", "gen": "муки", "dat": "муке", "acc": "муку", "ins": "мукой", "pre": "муке", "g": "f"}},
 "unit": {"sack": {"nom": "мешок", "gen": "мешка", "dat": "мешку", "acc": "мешок", "ins": "мешком", "pre": "мешке", "g": "m",
                   "pl": {"nom": "мешки", "gen": "мешков", "dat": "мешкам", "acc": "мешки", "ins": "мешками", "pre": "мешках"}}},
 "role": {"miller": {"nom": "мельник", "gen": "мельника", "dat": "мельнику", "acc": "мельника", "ins": "мельником", "pre": "мельнике", "g": "m", "anim": true}},
 "wrap": {
  "seen": ["Сама видела: {>body}.", "Своими глазами видела, как {>body}."],
  "told": "{src} говорит, {>body}.",
  "time": "{when?} {>body}",
  "loop_a": "{>wrap.loop_b}",
  "loop_b": "{>wrap.loop_a}"
 },
 "line": {
  "accuse": "Это {>p}!",
  "sorry": "Прости за {>p:nominal:acc}.",
  "ask": "Что {listener:ty|знаешь|знаете} {>p:about}?",
  "ask_verb": "Что {verb.know:present:agree=listener} {>p:about}?",
  "greet": "{listener:addr?}, здравствуй{listener:ty||те}!",
  "opt": "{who} {>wrap.time?} пришёл."
 }
})";

const char* kEn = R"({
 "lex": {"verb": {
   "steal": {"base": "steal", "past": "stole", "3s": "steals", "ing": "stealing", "pp": "stolen"},
   "go_missing": {"base": "go missing", "past": "went missing", "3s": "goes missing"}}},
 "frame": {
  "theft": {"verb": "steal", "roles": {"doer": "nom", "object": "acc", "victim": "from+acc"},
    "clause": "{doer} {verb:agree=doer} {object?} {victim?}", "nominal": "the theft of {object?}"}
 },
 "item": {"flour": "flour", "apple": "apple"},
 "line": {"accuse": "It was {>p}!", "an": "{x:prep|a}"}
})";

Json person(const Loc& l, const std::string& lang, const std::string& name, char sex, int id) {
    Json n = l.personNoun(lang, name, sex);
    n["id"] = id;
    return n;
}

const Loc& L() {
    static const Loc l = [] {
        Json ru = Json::parse(kRu), en = Json::parse(kEn);
        ru["_grammar"] = grammarOf("ru");
        en["_grammar"] = grammarOf("en");
        return Loc::fromJson("ru", "ru", {{"ru", {ru}}, {"en", {en}}});
    }();
    return l;
}

Json noun(const std::string& lang, const std::string& key) { return L().noun(lang, key, key); }

Json theft(const Json& doer, const Json& victim, const Json& object = nullptr) {
    Json p = {{"pred", "theft"}, {"tense", "past"}, {"neg", false}, {"roles", {{"doer", doer}, {"victim", victim}}}};
    p["roles"]["object"] = object.is_null() ? noun("ru", "item.flour") : object;
    return p;
}

} // namespace

TEST(altera_sim_loc_gen_frames) {
    const Json oleg = person(L(), "ru", "Олег", 'm', 1), brana = person(L(), "ru", "Брана", 'f', 2);
    const Json miller = noun("ru", "role.miller");
    std::vector<std::string> errs;
    CHECK_EQ(realizeProp(L(), "ru", theft(oleg, miller), "clause", "", Json::object(), 0, &errs),
             std::string("Олег украл у мельника муку"));
    CHECK(errs.empty());
    CHECK_EQ(realizeProp(L(), "ru", theft(brana, miller)), std::string("Брана украла у мельника муку"));
    // optional role and time: absent -> no gap; present -> in place
    Json p = theft(oleg, miller);
    p["roles"].erase("victim");
    CHECK_EQ(realizeProp(L(), "ru", p), std::string("Олег украл муку"));
    p["time"] = "ночью";
    CHECK_EQ(realizeProp(L(), "ru", p), std::string("Олег ночью украл муку"));
    // negation and tense
    Json n = theft(oleg, miller);
    n["neg"] = true;
    CHECK_EQ(realizeProp(L(), "ru", n), std::string("Олег не украл у мельника муку"));
    n["neg"] = false;
    n["tense"] = "future";
    CHECK_EQ(realizeProp(L(), "ru", n), std::string("Олег украдёт у мельника муку"));
    // a plural subject agrees
    Json lose = {{"pred", "lose"}, {"roles", {{"object", noun("ru", "unit.sack")}, {"victim", miller}}}};
    CHECK_EQ(realizeProp(L(), "ru", lose), std::string("у мельника пропал мешок"));
    lose["roles"]["object"]["num"] = "pl";
    CHECK_EQ(realizeProp(L(), "ru", lose), std::string("у мельника пропали мешки"));
    lose["roles"]["object"] = noun("ru", "item.flour");
    CHECK_EQ(realizeProp(L(), "ru", lose), std::string("у мельника пропала мука"));
}

TEST(altera_sim_loc_gen_persons) {
    const Json oleg = person(L(), "ru", "Олег", 'm', 1), brana = person(L(), "ru", "Брана", 'f', 2);
    const Json miller = noun("ru", "role.miller");
    // the listener did it: ты / вы, the verb agrees
    Json args = {{"p", theft(oleg, miller)}, {"speaker", brana}, {"listener", oleg}};
    CHECK_EQ(sayLoc(L(), "ru", "line.accuse", args), std::string("Это ты украл у мельника муку!"));
    args["listener"]["reg"] = "vy";
    CHECK_EQ(sayLoc(L(), "ru", "line.accuse", args), std::string("Это вы украли у мельника муку!"));
    // the speaker is the victim: у меня; a female speaker as the doer: я украла
    Json p2 = theft(oleg, brana);
    CHECK_EQ(sayLoc(L(), "ru", "line.accuse", {{"p", p2}, {"speaker", brana}, {"listener", oleg}}),
             std::string("Это ты украл у меня муку!"));
    CHECK_EQ(realizeProp(L(), "ru", theft(brana, miller), "clause", "", {{"speaker", brana}}),
             std::string("я украла у мельника муку"));
    // nominal in a case and the default "about" with о/об
    CHECK_EQ(sayLoc(L(), "ru", "line.sorry", {{"p", theft(oleg, miller)}}),
             std::string("Прости за кражу муки у мельника."));
    CHECK_EQ(sayLoc(L(), "ru", "line.ask", {{"p", theft(oleg, miller)}, {"listener", oleg}}),
             std::string("Что знаешь о краже муки у мельника?"));
    Json vy = oleg;
    vy["reg"] = "vy";
    CHECK_EQ(sayLoc(L(), "ru", "line.ask_verb", {{"p", theft(oleg, miller)}, {"listener", vy}}),
             std::string("Что знаете о краже муки у мельника?"));
    // nested proposition (say -> content)
    Json say = {{"pred", "say"}, {"roles", {{"speaker", brana}, {"content", theft(oleg, miller)}}}};
    CHECK_EQ(realizeProp(L(), "ru", say), std::string("Брана сказала, что Олег украл у мельника муку"));
}

TEST(altera_sim_loc_gen_words) {
    const Json oleg = person(L(), "ru", "Олег", 'm', 1), irma = person(L(), "ru", "Ирма", 'f', 3);
    auto f = [](const std::string& pat, const Json& a) { return renderLocPattern(&L(), "ru", pat, a, 0); };
    // pronouns: name first, then a pronoun; н-forms after a preposition
    CHECK_EQ(f("{w:pro} пришёл, {w:pro} ушёл", {{"w", oleg}}), std::string("Олег пришёл, он ушёл"));
    CHECK_EQ(f("{w:pro}, у {w:pro:gen}, к {w:pro:dat}, {w:pro:acc}", {{"w", irma}}),
             std::string("Ирма, у неё, к ней, её"));
    CHECK_EQ(f("{w:pron:dat}", {{"w", oleg}}), std::string("ему"));
    // prepositions
    CHECK_EQ(f("{x:pre:prep|о}", {{"x", oleg}}), std::string("об Олеге"));
    CHECK_EQ(f("{x:pre:prep|о}", {{"x", irma}}), std::string("об Ирме"));
    CHECK_EQ(f("{x:prep|о:pre}", {{"x", noun("ru", "role.miller")}}), std::string("о мельнике"));
    CHECK_EQ(f("{x:prep|о}", {{"x", "мне"}}), std::string("обо мне"));
    CHECK_EQ(f("{x:prep|с}", {{"x", "мной"}}), std::string("со мной"));
    // adjectives: gender, case, animacy
    CHECK_EQ(f("{adj.whole:agree=o:acc} {o:acc}", {{"o", noun("ru", "unit.sack")}}), std::string("весь мешок"));
    CHECK_EQ(f("{adj.whole:agree=o:acc} {o:acc}", {{"o", noun("ru", "role.miller")}}), std::string("всего мельника"));
    CHECK_EQ(f("{adj.whole:agree=o:acc} {o:acc}", {{"o", noun("ru", "item.flour")}}), std::string("всю муку"));
    CHECK_EQ(f("{adj.whole:agree=o} {o:gen}", {{"o", noun("ru", "item.flour")}}), std::string("вся муки"));
    // counted nouns
    for (auto [n, want] : std::vector<std::pair<int, std::string>>{
             {1, "1 мешок"}, {2, "2 мешка"}, {5, "5 мешков"}, {21, "21 мешок"}, {12, "12 мешков"}})
        CHECK_EQ(f("{n} {u:count=n}", {{"n", n}, {"u", noun("ru", "unit.sack")}}), want);
    CHECK_EQ(f("{n} {u:count=n:ins}", {{"n", 3}, {"u", noun("ru", "unit.sack")}}), std::string("3 мешками"));
    // register
    Json vy = oleg;
    vy["reg"] = "vy";
    vy["addr"] = Json{{"nom", "господин"}};
    CHECK_EQ(f("{l:ty} | {l:ty:dat}", {{"l", oleg}}), std::string("ты | тебе"));
    CHECK_EQ(f("{l:ty} | {l:ty:dat}", {{"l", vy}}), std::string("вы | вам"));
    CHECK_EQ(sayLoc(L(), "ru", "line.greet", {{"listener", vy}}), std::string("Господин, здравствуйте!"));
    CHECK_EQ(renderLocPattern(&L(), "ru", "{l:addr?}здравствуй{l:ty||те}", {{"l", oleg}}, 0), std::string("здравствуй"));
}

TEST(altera_sim_loc_gen_rules) {
    const Json oleg = person(L(), "ru", "Олег", 'm', 1);
    // optional sub-rule: a missing argument inside empties it without errors
    std::vector<std::string> errs;
    CHECK_EQ(sayLoc(L(), "ru", "line.opt", {{"who", oleg}}, 0, &errs), std::string("Олег пришёл."));
    CHECK(errs.empty());
    // variants: deterministic by seed, both used across seeds
    const Json a = {{"body", "Олег украл муку"}};
    std::set<std::string> seen;
    for (std::uint64_t s = 0; s < 32; ++s) {
        const std::string t = sayLoc(L(), "ru", "wrap.seen", a, s);
        CHECK_EQ(t, sayLoc(L(), "ru", "wrap.seen", a, s));
        seen.insert(t);
    }
    CHECK_EQ(seen.size(), size_t(2));
    // a body can be a proposition too
    Json p = theft(oleg, noun("ru", "role.miller"));
    CHECK_EQ(sayLoc(L(), "ru", "wrap.told", {{"src", "Брана"}, {"body", p}}),
             std::string("Брана говорит, Олег украл у мельника муку."));
    // a cycle stops with an error, no hang
    errs.clear();
    sayLoc(L(), "ru", "wrap.loop_a", Json::object(), 0, &errs);
    CHECK(mentions(errs, "too deep or cyclic"));
    // missing frame / argument / rule
    errs.clear();
    CHECK_EQ(realizeProp(L(), "ru", {{"pred", "nope"}}, "clause", "", Json::object(), 0, &errs),
             std::string("[frame.nope]"));
    CHECK(mentions(errs, "no frame"));
    // text() understands v0.2 too
    CHECK_EQ(L().text("ru", "line.accuse", {{"p", p}}), std::string("Это Олег украл у мельника муку!"));
    // v0.1 syntax is unchanged
    std::string why;
    CHECK(!checkLocPattern("{x:abl}", "ru", why));
    CHECK(checkLocPattern("{>a.b?} {x:pro:dat} {v:agree=x} {y:pre:prep|о} {n:plural|a|b|c}", "ru", why));
    CHECK(!checkLocPattern("{x:ty|a}", "ru", why));
}

TEST(altera_sim_loc_gen_en) {
    const Json oleg = person(L(), "en", "Oleg", 'm', 1), brana = person(L(), "en", "Brana", 'f', 2);
    const Json miller = Json{{"nom", "the miller"}};
    Json p = {{"pred", "theft"},
              {"tense", "past"},
              {"roles", {{"doer", oleg}, {"object", noun("en", "item.flour")}, {"victim", miller}}}};
    CHECK_EQ(realizeProp(L(), "en", p), std::string("Oleg stole flour from the miller"));
    CHECK_EQ(sayLoc(L(), "en", "line.accuse", {{"p", p}, {"speaker", brana}, {"listener", oleg}}),
             std::string("It was you stole flour from the miller!"));
    p["neg"] = true;
    CHECK_EQ(realizeProp(L(), "en", p), std::string("Oleg did not steal flour from the miller"));
    p["neg"] = false;
    p["tense"] = "present";
    CHECK_EQ(realizeProp(L(), "en", p), std::string("Oleg steals flour from the miller"));
    p["roles"]["victim"] = brana;
    CHECK_EQ(realizeProp(L(), "en", p, "clause", "", {{"speaker", brana}}),
             std::string("Oleg steals flour from me"));
    CHECK_EQ(sayLoc(L(), "en", "line.an", {{"x", "apple"}}), std::string("An apple"));
    CHECK_EQ(sayLoc(L(), "en", "line.an", {{"x", "pear"}}), std::string("A pear"));
    CHECK_EQ(sayLoc(L(), "en", "line.an", {{"x", "user"}}), std::string("A user"));
}

TEST(altera_sim_loc_gen_prop_arg) {
    Prop p;
    p.pred = "theft";
    p.roles["doer"] = Ref{"char", 1, "", 0.0, {}};
    p.roles["object"] = Ref{"item", kNone, "flour", 0.0, {}};
    p.qty = 2;
    p.unit = "sack";
    const Json j = propLocArg(
        p,
        [](const Ref& r) {
            if (r.kind == "char") return person(L(), "ru", "Олег", 'm', static_cast<int>(r.id));
            return noun("ru", "item." + r.key);
        },
        [](const std::string& u) { return noun("ru", "unit." + u); });
    CHECK(isPropArg(j));
    CHECK_EQ(j["qty"].get<double>(), 2.0);
    CHECK_EQ(j["unit"]["nom"].get<std::string>(), std::string("мешок"));
    CHECK_EQ(realizeProp(L(), "ru", j), std::string("Олег украл муку"));
}

TEST(altera_sim_loc_gen_validate) {
    // the test tables are clean except the deliberate loop
    ValidationReport r = validateLocGen(L(), {"theft", "lose"}, {{"theft", {"doer", "victim", "victim_family", "object"}}});
    for (const auto& e : r.errors) CHECK(e.find("loop_") != std::string::npos);
    CHECK(mentions(r.errors, "cycle"));
    // missing frame, bad role, bad syn, broken verb, unknown sub-rule
    Json bad = Json::parse(R"({
      "lex": {"verb": {"x": {"inf": "икс", "past": {"m": "иксил"}, "syn": ["nope"]}}},
      "frame": {"theft": {"verb": "nope", "roles": {"doer": "nom", "thief": "acc", "victim": "у"}, "clause": "{>wrap.none}"}}
    })");
    bad["_grammar"] = grammarOf("ru");
    const Loc lb = Loc::fromJson("ru", "ru", {{"ru", {bad}}});
    r = validateLocGen(lb, {"theft", "lose"}, {{"theft", {"doer", "victim", "object"}}});
    CHECK(mentions(r.errors, "no frame for predicate 'lose'"));
    CHECK(mentions(r.errors, "role 'thief' is not a role"));
    CHECK(mentions(r.errors, "role 'victim' has no case"));
    CHECK(mentions(r.errors, "synonym 'nope'"));
    CHECK(mentions(r.errors, "past has no 'f'"));
    CHECK(mentions(r.errors, "verb 'nope' is not a lex.verb"));
    CHECK(mentions(r.errors, "sub-rule 'wrap.none' not found"));
}
