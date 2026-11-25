// AL-111: news and things worth telling (sim/news.hpp): who knows a chronicle
// entry and how, word of mouth, the player's questions and the answers.

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/news.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;

namespace {

const Content& realContent() {
    static const Content c = Content::load(std::string(LR_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& realLoc() {
    static const Loc l = Loc::load(std::string(LR_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

// The test village without minds: time passes, nobody acts on his own.
World quietWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return makeTestWorld(realContent(), cfg, seed);
}

// Adults of different households (so nobody hears it as kin).
std::vector<Character*> strangers(World& w, size_t n) {
    std::vector<Character*> out;
    std::set<EntityId> fams;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        if (fams.count(c.status.family)) continue;
        bool kinOf = false;
        for (Character* o : out)
            kinOf = kinOf || o->status.spouse == c.id || c.status.spouse == o->id;
        if (kinOf) continue;
        fams.insert(c.status.family);
        out.push_back(&c);
        if (out.size() >= n) break;
    }
    return out;
}

std::vector<Character*> adults(World& w, size_t n) {
    std::vector<Character*> out;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult" && out.size() < n) out.push_back(&c);
    return out;
}

bool clean(const std::string& s) {
    return !s.empty() && s.find('[') == std::string::npos && s.find('{') == std::string::npos;
}

std::vector<std::string> linesOf(const World& w, const Character& c, const std::string& lang = "ru") {
    std::vector<std::string> out;
    for (const auto& e : c.ai.trace)
        if (e.code == "say.line") out.push_back(lineText(realLoc(), lang, w, e.params));
    return out;
}

size_t lastIndex(const World& w) { return w.chronicleLog().size() - 1; }

} // namespace

TEST(lr_news_village_hears_in_days) {
    World w = quietWorld();
    auto p = strangers(w, 3);
    CHECK(p.size() >= 3);
    if (p.size() < 3) return;
    Character& a = *p[0];
    Character& b = *p[1];
    Character& c = *p[2];
    w.chronicle("wedding", {a.id, b.id}, "chronicle.village.wedding", {{"a", a.id}, {"b", b.id}});
    const size_t i = lastIndex(w);
    // the couple saw it
    auto ea = newsKnown(w, a, i);
    CHECK(ea.has_value());
    if (ea) CHECK_EQ(ea->source, std::string("seen"));
    // the village within village_delay_days (2)
    w.run(kTicksPerDay * 3);
    auto ec = newsKnown(w, c, i);
    CHECK(ec.has_value());
    if (ec) CHECK_EQ(ec->source, std::string("heard"));
    // and it is news to tell in the right words
    bool found = false;
    for (const auto& it : knownNews(w, c, 20))
        if (it.index == i) {
            found = true;
            CHECK_EQ(it.kind.kind, std::string("wedding"));
        }
    CHECK(found);
    const std::string t = newsText(realLoc(), "ru", w, i);
    CHECK(clean(t));
    CHECK(t.find('#') == std::string::npos);
}

TEST(lr_news_gossip_goes_by_mouth) {
    World w = quietWorld();
    auto p = adults(w, 4); // a quarrel is the two's only, kin or not
    CHECK(p.size() >= 4);
    if (p.size() < 4) return;
    Character& a = *p[0];
    Character& b = *p[1];
    Character& c = *p[2];
    Character& d = *p[3];
    w.chronicle("quarrel", {a.id, b.id}, "chronicle.village.quarrel", {{"a", a.id}, {"b", b.id}});
    const size_t i = lastIndex(w);
    w.run(kTicksPerDay * 5);
    // a quarrel is only the two's (scope who) until someone tells it
    CHECK(!newsKnown(w, c, i).has_value());
    // nor do the two spread it themselves
    auto topics = newsToTell(w, a, c, 5);
    CHECK(std::none_of(topics.begin(), topics.end(), [&](const NewsItem& x) { return x.index == i; }));
    // a tells c: c knows it from a
    std::map<std::string, Ref> args;
    args["news"] = newsRef(i);
    const SaidLine l = say(w, a, c, "tell_news", std::nullopt, *newsKnown(w, a, i), "", "", args);
    CHECK(l.ok());
    auto ec = newsKnown(w, c, i);
    CHECK(ec.has_value());
    if (ec) {
        CHECK_EQ(ec->source, std::string("told"));
        CHECK(!ec->chain.empty() && ec->chain.front() == a.id);
    }
    // c has it to tell d now, and nobody tells a person what he knows
    auto toD = newsToTell(w, c, d, 5);
    CHECK(std::any_of(toD.begin(), toD.end(), [&](const NewsItem& x) { return x.index == i; }));
    auto again = newsToTell(w, b, c, 5);
    CHECK(std::none_of(again.begin(), again.end(), [&](const NewsItem& x) { return x.index == i; }));
    // c passes it on to d: a rumour now, less sure
    const SaidLine l2 = say(w, c, d, "tell_news", std::nullopt, *ec, "", "", args);
    CHECK(l2.ok());
    auto ed = newsKnown(w, d, i);
    CHECK(ed.has_value());
    if (ed && ec) {
        CHECK_EQ(ed->source, std::string("rumor"));
        CHECK(ed->confidence < ec->confidence);
        CHECK_EQ(ed->chain.size(), size_t{2});
    }
    // the lines say what happened, by name
    const auto said = linesOf(w, d);
    CHECK(!said.empty());
    for (const auto& s : said) CHECK(clean(s));
}

TEST(lr_news_realm_reaches_traders_first) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& trader = *p[0];
    trader.occupation = "trader";
    w.chronicle("realm", {}, "chronicle.realm.harvest_bad", {{"n", 60}});
    const size_t i = lastIndex(w);
    CHECK(!newsKnown(w, trader, i).has_value()); // not on the day itself
    w.run(kTicksPerDay * 5);
    auto e = newsKnown(w, trader, i);
    CHECK(e.has_value());
    if (e) CHECK(e->confidence < 0.9);
}

TEST(lr_news_chat_topics_and_ask_news) {
    World w = quietWorld();
    auto p = strangers(w, 3);
    CHECK(p.size() >= 3);
    if (p.size() < 3) return;
    Character& a = *p[0];
    Character& b = *p[1];
    Character& me = *p[2];
    w.chronicle("betrothal", {a.id, b.id}, "chronicle.village.betrothal", {{"a", a.id}, {"b", b.id}});
    const size_t i = lastIndex(w);
    w.run(kTicksPerDay / 4); // a knows, the village not yet
    if (newsKnown(w, me, i)) return; // heard it already on the first hours (village_delay_days from 0)
    // a's chat topics with me carry the news
    const auto topics = dialogueTopics(w, a, me);
    CHECK(std::any_of(topics.begin(), topics.end(), [](const MenuTopic& t) { return t.act == "tell_news"; }));
    for (const auto& t : topics)
        if (t.act == "tell_news") CHECK(clean(menuLabel(realLoc(), "ru", w, a, me, t)));
    // I ask a what's new: a answers with it and I know it after
    const auto asks = askTopics(w, me, a);
    CHECK(!asks.empty());
    const auto it = std::find_if(asks.begin(), asks.end(), [](const MenuTopic& t) { return t.act == "ask_news"; });
    CHECK(it != asks.end());
    if (it == asks.end()) return;
    const int n = askAndAnswer(w, me, a, *it);
    CHECK(n >= 1);
    CHECK(newsKnown(w, me, i).has_value());
    const auto said = linesOf(w, me);
    CHECK(said.size() >= 2); // my question and the answer
    for (const auto& s : said) {
        CHECK(clean(s));
        if (std::getenv("LR_DUMP_NEWS")) std::printf("  %s\n", s.c_str());
    }
    // nothing left to tell: the "no news" line
    const size_t before = linesOf(w, me).size();
    askAndAnswer(w, me, a, *it);
    CHECK(linesOf(w, me).size() >= before + 2);
}

TEST(lr_news_ask_self_person_place_render) {
    World w = quietWorld();
    auto p = strangers(w, 3);
    CHECK(p.size() >= 3);
    if (p.size() < 3) return;
    Character& me = *p[0];
    Character& a = *p[1];
    me.relations[p[2]->id].familiarity = 0.5; // somebody I know, to ask about
    a.relations[p[2]->id].affinity = 0.6;
    a.relations[p[2]->id].familiarity = 0.6;
    const auto asks = askTopics(w, me, a);
    CHECK(std::any_of(asks.begin(), asks.end(), [](const MenuTopic& t) { return t.act == "ask_self"; }));
    CHECK(std::any_of(asks.begin(), asks.end(), [](const MenuTopic& t) { return t.act == "ask_person"; }));
    for (const auto& t : asks) {
        CHECK(clean(menuLabel(realLoc(), "ru", w, me, a, t)));
        CHECK(clean(menuLabel(realLoc(), "en", w, me, a, t)));
        const size_t before = linesOf(w, me).size();
        const int n = askAndAnswer(w, me, a, t);
        CHECK(n >= 1);
        CHECK(linesOf(w, me).size() > before);
    }
    for (const std::string lang : {"ru", "en"})
        for (const auto& s : linesOf(w, me, lang)) {
            CHECK(clean(s));
            if (std::getenv("LR_DUMP_NEWS")) std::printf("  %s %s\n", lang.c_str(), s.c_str());
        }
}

TEST(lr_news_hostile_declines) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& me = *p[0];
    Character& a = *p[1];
    a.relations[me.id].affinity = -0.9;
    CHECK(declinesToAnswer(w, a, me));
    MenuTopic ask;
    ask.act = "ask_news";
    CHECK_EQ(askAndAnswer(w, me, a, ask), 1);
    const TraceEntry* last = nullptr;
    for (const auto& e : me.ai.trace)
        if (e.code == "say.line") last = &e;
    CHECK(last && last->params.value("act", std::string()) == "decline_ask");
}

TEST(lr_news_heard_memory_is_bounded) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    const size_t cap = newsRules(w.content()).maxHeard;
    for (size_t k = 0; k < cap + 10; ++k) {
        w.chronicle("quarrel", {a.id}, "chronicle.village.quarrel", {{"a", a.id}, {"b", a.id}});
        learnNews(w, b, a, lastIndex(w), Epistemic{});
    }
    size_t n = 0;
    for (const auto& m : b.memory) n += m.type == "heard_news";
    CHECK_EQ(n, cap);
}

// LR_NEWS_LIVE=1: a living village for a few days, the news its people told each other (for reading by eye).
TEST(lr_news_live_village_dump) {
    if (!std::getenv("LR_NEWS_LIVE")) return;
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 3);
    installAi(w);
    w.run(kTicksPerDay * 6);
    int told = 0, all = 0;
    for (const auto& [id, c] : w.characters())
        for (const auto& e : c.ai.trace)
            if (e.code == "say.line" && e.params.value("speaker", kNone) == id) {
                ++all;
                if (std::getenv("LR_NEWS_ALL")) std::printf("  [%s %s] %s\n", e.params.value("act", std::string()).c_str(), e.params.value("utt", Json::object()).value("intent", std::string()).c_str(), lineText(realLoc(), "ru", w, e.params).c_str());
                if (e.params.value("act", std::string()) == "tell_news") {
                    ++told;
                    std::printf("  %s\n", lineText(realLoc(), "ru", w, e.params).c_str());
                }
            }
    std::printf("  lines %d, news told %d, chronicle %zu\n", all, told, w.chronicleLog().size());
}
