// LR-158: people talk over the moments they lived through (sim/memory_talk.hpp):
// their own (recall_moment), shared ones (reminisce), the other's answer, the
// player's "what do you remember best" (ask_memory).

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/memory_talk.hpp"
#include "sim/news.hpp"
#include "sim/social_chain.hpp"
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

World quietWorld(std::uint64_t seed = 7) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    return makeTestWorld(realContent(), cfg, seed);
}

// Adults of different households.
std::vector<Character*> strangers(World& w, size_t n) {
    std::vector<Character*> out;
    std::set<EntityId> fams;
    for (auto& [id, c] : w.characters()) {
        if (c.isAnimal() || !c.body.alive || c.body.stage() != "adult") continue;
        if (fams.count(c.status.family)) continue;
        bool kinOf = false;
        for (Character* o : out) kinOf = kinOf || o->status.spouse == c.id || c.status.spouse == o->id;
        if (kinOf) continue;
        fams.insert(c.status.family);
        out.push_back(&c);
        if (out.size() >= n) break;
    }
    return out;
}

bool clean(const std::string& s) {
    return !s.empty() && s.find('[') == std::string::npos && s.find('{') == std::string::npos;
}

struct Said {
    std::string act, rule, text;
    EntityId speaker = kNone;
};

std::vector<Said> linesOf(const World& w, const Character& c, const std::string& lang = "ru") {
    std::vector<Said> out;
    for (const auto& e : c.ai.trace)
        if (e.code == "say.line")
            out.push_back({e.params.value("act", std::string()), e.params.value("rule", std::string()),
                           lineText(realLoc(), lang, w, e.params), e.params.value("speaker", kNone)});
    return out;
}

void dump(const std::vector<Said>& v) {
    if (!std::getenv("LR_DUMP_MEMORY_TALK")) return;
    for (const auto& s : v) std::printf("  [%s %s] %s\n", s.act.c_str(), s.rule.c_str(), s.text.c_str());
}

// Forget what the quiet days brought (hunger and the like), keep only `types`.
void keepOnly(Character& c, std::set<std::string> types) {
    c.memory.erase(std::remove_if(c.memory.begin(), c.memory.end(), [&](const MemoryEntry& m) { return !types.count(m.type); }),
                   c.memory.end());
}

void lived(World& w, Character& c, const std::string& type, std::vector<EntityId> who = {}) {
    w.remember(c, MemoryEntry{0, type, std::move(who), kNone, -0.5, 0.0, "seen", 1.0});
}

} // namespace

TEST(lr_memory_talk_rules_load) {
    const MomentRules& R = momentRules(realContent());
    CHECK(R.of("attacked_by_beast") != nullptr);
    CHECK(R.of("wedding") != nullptr);
    CHECK(R.of("heard_news") == nullptr); // not one's own moment
    CHECK(R.talks.count("chat") == 1);
}

TEST(lr_memory_talk_recall_and_react) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    a.memory.clear();
    b.memory.clear();
    lived(w, a, "attacked_by_beast");
    w.run(kTicksPerDay * 3);
    keepOnly(a, {"attacked_by_beast"});
    keepOnly(b, {});
    const auto moments = momentsToTell(w, a, &b);
    CHECK_EQ(moments.size(), size_t(1));
    const double fam0 = b.relations[a.id].familiarity;
    CHECK(talkOverMoments(w, a, b, "chat", 0));
    const auto said = linesOf(w, b);
    dump(said);
    CHECK_EQ(said.size(), size_t(2));
    if (said.size() < 2) return;
    CHECK_EQ(said[0].act, std::string("recall_moment"));
    CHECK(said[0].rule.find("memory.recall") == 0);
    CHECK_EQ(said[1].act, std::string("react_moment"));
    for (const auto& s : said) CHECK(clean(s.text));
    for (const auto& s : linesOf(w, b, "en")) CHECK(clean(s.text));
    CHECK(b.relations[a.id].familiarity > fam0);
    // told once: not told the same listener again, but still to somebody else
    CHECK(momentsToTell(w, a, &b).empty());
    CHECK(!talkOverMoments(w, a, b, "chat", 0));
    // a talk group without moments in memory_talk.json: nothing
    CHECK(!talkOverMoments(w, b, a, "rude", 0));
}

TEST(lr_memory_talk_reminisce_shared) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    a.memory.clear();
    b.memory.clear();
    lived(w, a, "raided");
    lived(w, b, "raided"); // the same raid, both there
    w.run(kTicksPerDay * 2);
    keepOnly(a, {"raided"});
    keepOnly(b, {"raided"});
    const auto moments = momentsToTell(w, a, &b);
    CHECK(!moments.empty() && moments.front().shared);
    const double aff0 = a.relations[b.id].affinity;
    CHECK(talkOverMoments(w, a, b, "tender", 0)); // tender talk: only shared moments
    const auto said = linesOf(w, a);
    dump(said);
    CHECK(said.size() >= 2);
    if (said.size() < 2) return;
    CHECK_EQ(said[0].act, std::string("reminisce"));
    CHECK_EQ(said[1].act, std::string("react_reminisce"));
    CHECK(said[1].speaker == b.id);
    for (const auto& s : said) CHECK(clean(s.text));
    CHECK(a.relations[b.id].affinity > aff0); // remembering together brings closer
    // b will not bring the same raid back up to a either
    CHECK(momentsToTell(w, b, &a).empty());
}

TEST(lr_memory_talk_tender_needs_shared) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    a.memory.clear();
    lived(w, a, "starved");
    CHECK(!talkOverMoments(w, a, b, "tender", 0));
    CHECK(talkOverMoments(w, a, b, "story", 0));
}

TEST(lr_memory_talk_same_kind_answer) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    a.memory.clear();
    b.memory.clear();
    b.relations[a.id].affinity = 0.3;
    lived(w, b, "saw_beast"); // b met a beast of his own, long before
    w.run(kTicksPerDay * 3);
    keepOnly(a, {});
    keepOnly(b, {"saw_beast"});
    lived(w, a, "attacked_by_beast");
    CHECK(talkOverMoments(w, a, b, "chat", 0));
    const auto said = linesOf(w, b);
    dump(said);
    CHECK(said.size() >= 2);
    if (said.size() >= 2) CHECK_EQ(said[1].rule, std::string("memory.react.same"));
}

TEST(lr_memory_talk_with_person_and_kindness) {
    World w = quietWorld();
    auto p = strangers(w, 3);
    CHECK(p.size() >= 3);
    if (p.size() < 3) return;
    Character& a = *p[0];
    Character& b = *p[1];
    Character& c = *p[2];
    a.memory.clear();
    lived(w, a, "won_fight", {c.id});
    lived(w, a, "was_given_food", {b.id}); // b fed a: a thanks b for it
    const auto moments = momentsToTell(w, a, &b);
    CHECK_EQ(moments.size(), size_t(2));
    int n = 0;
    while (talkOverMoments(w, a, b, "chat", 0) && n < 5) ++n;
    CHECK_EQ(n, 2);
    const auto said = linesOf(w, b);
    dump(said);
    bool kindness = false, withFight = false;
    for (const auto& s : said) {
        CHECK(clean(s.text));
        kindness = kindness || s.rule == "memory.recall.with_us.kindness";
        withFight = withFight || s.rule == "memory.recall.with.fight";
    }
    CHECK(kindness);
    CHECK(withFight);
}

TEST(lr_memory_talk_player_asks) {
    World w = quietWorld();
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& me = *p[0];
    Character& a = *p[1];
    a.memory.clear();
    const auto asks = askTopics(w, me, a);
    const auto it = std::find_if(asks.begin(), asks.end(), [](const MenuTopic& t) { return t.act == "ask_memory"; });
    CHECK(it != asks.end());
    if (it == asks.end()) return;
    CHECK(clean(menuLabel(realLoc(), "ru", w, me, a, *it)));
    CHECK(clean(menuLabel(realLoc(), "en", w, me, a, *it)));
    // nothing to remember yet
    CHECK_EQ(askAndAnswer(w, me, a, *it), 1);
    CHECK_EQ(linesOf(w, me).back().act, std::string("no_moment"));
    // now there is
    lived(w, a, "wedding");
    CHECK(askAndAnswer(w, me, a, *it) >= 1);
    const auto said = linesOf(w, me);
    dump(said);
    CHECK(std::any_of(said.begin(), said.end(), [](const Said& s) { return s.act == "recall_moment"; }));
    for (const auto& s : said) CHECK(clean(s.text));
}

// A chat between two villagers brings the moment up by itself (social_chain talkBeat).
TEST(lr_memory_talk_chat_brings_it_up) {
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 7);
    installAi(w);
    auto p = strangers(w, 2);
    CHECK(p.size() >= 2);
    if (p.size() < 2) return;
    Character& a = *p[0];
    Character& b = *p[1];
    const EntityId A = a.id, B = b.id;
    for (Character* c : {&a, &b}) {
        c->ai.fsm = FsmState{};
        c->ai.goal = GoalInstance{};
        c->ai.plan = Plan{};
        for (auto& [k, v] : c->needs) v = 0.9;
        c->memory.clear();
    }
    a.relations[b.id].familiarity = b.relations[a.id].familiarity = 1.0;
    a.relations[b.id].affinity = b.relations[a.id].affinity = 0.8;
    lived(w, a, "attacked_by_beast");
    b.pos = {a.pos.x + 2.0, a.pos.y};
    std::string why;
    CHECK(beginSocial(w, a, b, *w.content().interaction("chat"), why));
    for (int i = 0; i < 400 && w.character(A)->ai.social.active(); ++i) w.step();
    const auto said = linesOf(w, *w.character(B));
    dump(said);
    CHECK(std::any_of(said.begin(), said.end(), [](const Said& s) { return s.act == "recall_moment"; }));
    CHECK(std::any_of(said.begin(), said.end(), [](const Said& s) { return s.act == "react_moment"; }));
}

// LR_MEMORY_TALK_LIVE=1: a living village for some days, the moments its people told (for reading by eye).
TEST(lr_memory_talk_live_village_dump) {
    if (!std::getenv("LR_MEMORY_TALK_LIVE")) return;
    static const Json cfg = loadJsonFile(std::string(LR_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(realContent(), cfg, 3);
    installAi(w);
    // LR_MEMORY_TALK_SEED=1: every adult lived something to tell (the test village is quiet)
    if (std::getenv("LR_MEMORY_TALK_SEED")) {
        const char* types[] = {"attacked_by_beast", "won_fight", "starved", "wedding", "built_home", "saw_magic"};
        int k = 0;
        for (auto& [id, c] : w.characters())
            if (!c.isAnimal() && c.body.stage() == "adult") lived(w, c, types[k++ % 6]);
    }
    w.run(kTicksPerDay * 10);
    int told = 0, all = 0;
    for (const auto& [id, c] : w.characters())
        for (const auto& e : c.ai.trace)
            if (e.code == "say.line" && e.params.value("speaker", kNone) == id) {
                ++all;
                const std::string act = e.params.value("act", std::string());
                if (std::getenv("LR_MEMORY_TALK_ALL") || act == "recall_moment" || act == "reminisce" || act == "react_moment" || act == "react_reminisce") {
                    ++told;
                    std::printf("  [%s] %s\n", act.c_str(), lineText(realLoc(), "ru", w, e.params).c_str());
                }
            }
    std::map<std::string, int> kept;
    for (const auto& [id, c] : w.characters())
        for (const auto& m : c.memory)
            ++kept[m.type + (momentRules(w.content()).of(m.type) ? "*" : "") + "/" + m.source];
    for (const auto& [t, n] : kept) std::printf("  memory %s x%d\n", t.c_str(), n);
    std::printf("  lines %d, moments %d\n", all, told);
}
