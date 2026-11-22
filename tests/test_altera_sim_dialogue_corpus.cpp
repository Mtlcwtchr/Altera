// The corpus of lines and its quality checks (AL-54, T7): the linter catches
// broken lines, the frame table catches agreement errors, and the corpus of the
// real content is deterministic, has no broken keys / placeholders, agrees in
// gender and case, does not repeat itself to one listener and reaches the rules.

#include <algorithm>

#include "framework.hpp"
#include "sim/dialogue_corpus.hpp"
#include "sim/loc.hpp"

using namespace altera::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

bool hasIssue(const std::vector<std::string>& v, const std::string& s) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& x) { return x.find(s) != std::string::npos; });
}

bool hasIssue(const std::vector<CorpusIssue>& v, const std::string& s) {
    return std::any_of(v.begin(), v.end(), [&](const CorpusIssue& x) { return x.message.find(s) != std::string::npos; });
}

// The first item of the content with its noun, for a fixture loc.
std::string anItem() { return realContent().items.begin()->first; }

Loc fixtureLoc(const std::string& clause, const std::string& objectGov = "acc") {
    Json t = Json::parse(R"({
        "lex": {"verb": {"steal": {"inf": "украсть", "pfv": true,
            "past": {"m": "украл", "f": "украла", "n": "украло", "pl": "украли"},
            "fut": {"1s": "украду", "2s": "украдёшь", "3s": "украдёт", "1p": "украдём", "2p": "украдёте", "3p": "украдут"}}}},
        "frame": {"theft": {"verb": "steal", "roles": {"doer": "nom", "victim": "у+gen"},
                             "nominal": "кража {object:gen?}", "about": "о краже {object:gen?}"}}
    })");
    t["frame"]["theft"]["clause"] = clause;
    t["frame"]["theft"]["roles"]["object"] = objectGov;
    t["item"][anItem()]["name"] = Json{{"nom", "мука"}, {"gen", "муки"}, {"dat", "муке"}, {"acc", "муку"},
                                       {"ins", "мукой"}, {"pre", "муке"}, {"g", "f"}};
    return Loc::fromJson("ru", "ru", {{"ru", {t}}});
}

} // namespace

TEST(altera_dialogue_corpus_lint_catches_broken_lines) {
    CHECK(lintDialogueLine("Сама видела: Олег утащил муку.", "ru").empty());
    CHECK(lintDialogueLine("Ик… слышь, ну и дела!", "ru").empty());
    CHECK(hasIssue(lintDialogueLine("Говорят, [say.inform.x].", "ru"), "missing key"));
    CHECK(hasIssue(lintDialogueLine("Говорят, {prop}.", "ru"), "placeholder"));
    CHECK(hasIssue(lintDialogueLine("Он взял|взяла муку.", "ru"), "stray"));
    CHECK(hasIssue(lintDialogueLine("Олег #12 украл.", "ru"), "id in the text"));
    CHECK(hasIssue(lintDialogueLine("", "ru"), "empty"));
    CHECK(hasIssue(lintDialogueLine("Олег украл украл муку.", "ru"), "doubled word"));
    CHECK(hasIssue(lintDialogueLine("Олег украл муку,.", "ru"), "punctuation"));
    CHECK(hasIssue(lintDialogueLine("Олег украл муку", "ru"), "final punctuation"));
    CHECK(hasIssue(lintDialogueLine("Олег stole муку.", "ru"), "Latin"));
    CHECK(hasIssue(lintDialogueLine("Oleg украл the flour.", "en"), "Cyrillic"));
    CHECK(!hasIssue(lintDialogueLine("Погоди... о чём бишь я.", "ru"), "'..'"));
}

TEST(altera_dialogue_corpus_frame_table_catches_agreement) {
    // a right frame: no issues on the table of names and items
    {
        const Loc l = fixtureLoc("{doer} {verb:agree=doer} {object?} {victim?}");
        int n = 0;
        const auto issues = checkFrameAgreement(realContent(), l, "ru", &n);
        for (const auto& i : issues) ::testing::fail(__FILE__, __LINE__, i.message.c_str());
        CHECK(n > 20);
    }
    // a verb written in the pattern does not agree with a woman
    {
        const Loc l = fixtureLoc("{doer} украл {object?} {victim?}");
        const auto issues = checkFrameAgreement(realContent(), l, "ru");
        CHECK(hasIssue(issues, "verb of the other gender украл (m)"));
        CHECK(hasIssue(issues, "Brana"));
    }
    // the object in the wrong case
    {
        const Loc l = fixtureLoc("{doer} {verb:agree=doer} {object:dat?} {victim?}");
        CHECK(hasIssue(checkFrameAgreement(realContent(), l, "ru"), "role object not in acc"));
    }
    // a preposition dropped
    {
        const Loc l = fixtureLoc("{doer} {verb:agree=doer} {object?} {victim:gen?}");
        CHECK(hasIssue(checkFrameAgreement(realContent(), l, "ru"), "role victim not in у+gen"));
    }
}

TEST(altera_dialogue_corpus_is_deterministic) {
    CorpusOptions o;
    o.count = 120;
    o.seed = 5;
    const auto a = generateDialogueCorpus(realContent(), realLoc(), o);
    const auto b = generateDialogueCorpus(realContent(), realLoc(), o);
    CHECK_EQ(a.size(), size_t{120});
    CHECK_EQ(corpusText(a), corpusText(b));
    o.seed = 6;
    CHECK(corpusText(generateDialogueCorpus(realContent(), realLoc(), o)) != corpusText(a));
}

TEST(altera_dialogue_corpus_real_content_quality) {
    for (const char* lang : {"ru", "en"}) {
        CorpusOptions o;
        o.count = 600;
        o.seed = 1;
        o.lang = lang;
        const auto lines = generateDialogueCorpus(realContent(), realLoc(), o);
        const CorpusReport r = checkDialogueCorpus(lines, realContent(), realLoc(), lang, o.run);
        int shown = 0;
        for (const auto& i : r.issues)
            if (i.severity == "error" && shown++ < 25)
                ::testing::fail(__FILE__, __LINE__, (std::string(lang) + " #" + std::to_string(i.line) + " " + i.message).c_str());
        CHECK_EQ(r.errors(), 0);
        // every speech act says something
        for (const auto& act : realContent().speechActs) CHECK(r.perAct.count(act) > 0);
        // not the same line over and over
        CHECK(r.dupShare < 0.5);
        CHECK(r.distinct2 > 0.15);
        int repeats = 0;
        for (const auto& i : r.issues) repeats += i.check == "repeat";
        CHECK(repeats <= r.lines / 50);
        // the agreement check reached the lines with a doer
        CHECK(r.agreementChecked > 0);
        // most rules are reachable from the corpus moments
        CHECK(r.rulesFired * 2 >= r.rulesTotal);
    }
}

TEST(altera_dialogue_corpus_check_flags_bad_lines) {
    // hand-made lines: a missing key, a verb against the doer, a repeat to the same listener
    std::vector<CorpusLine> lines(4);
    for (int i = 0; i < 4; ++i) {
        lines[i].index = i;
        lines[i].u.act = "inform";
        lines[i].u.speaker = 1;
        lines[i].u.listener = 2;
        lines[i].rule = "inform.seen";
        lines[i].say = "say.inform.seen";
    }
    lines[0].text = "Говорят, [say.nothing].";
    Prop p;
    p.pred = "theft";
    p.roles["doer"] = Ref{"char", 3, "", 0.0, {}};
    lines[1].u.content = p;
    lines[1].doerAgree = "f";
    lines[1].text = "Сама видела: Брана украл муку.";
    lines[2].text = "Ну и дела.";
    lines[3].text = "Ну и дела.";
    const CorpusReport r = checkDialogueCorpus(lines, realContent(), realLoc(), "ru");
    auto has = [&](const char* check, int line) {
        return std::any_of(r.issues.begin(), r.issues.end(),
                           [&](const CorpusIssue& i) { return i.check == check && i.line == line; });
    };
    CHECK(has("broken", 0));
    CHECK(has("agreement", 1));
    CHECK(has("repeat", 3));
    CHECK(!has("broken", 2));
    CHECK_EQ(r.lines, 4);
    CHECK_EQ(r.distinct, 3);
}
