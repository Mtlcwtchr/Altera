// What the client shows of the talk (AL-57): every new "say.line" once, from
// the speaker's side, as a bubble text and a journal line; nothing old after a
// load; nothing twice; lines said within one tick all come through.

#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/speech_feed.hpp"
#include "sim/worldgen.hpp"

using namespace altera::sim;

namespace {

const Content& feedContent() {
    static const Content c = Content::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ai");
    return c;
}

const Loc& feedLoc() {
    static const Loc l = Loc::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/loc");
    return l;
}

bool cleanText(const std::string& s) {
    return !s.empty() && s.find('[') == std::string::npos && s.find('{') == std::string::npos;
}

} // namespace

TEST(altera_speech_feed_new_lines_once) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(feedContent(), cfg, 11);
    installAi(w);
    std::vector<Character*> people;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") people.push_back(&c);
    CHECK(people.size() >= 2);
    if (people.size() < 2) return;
    Character& a = *people[0];
    Character& b = *people[1];

    // talk before the first look stays unshown (a loaded save)
    CHECK(sayRefusal(w, b, a).ok());
    SpeechFeed feed;
    CHECK(feed.poll(w, feedLoc(), "ru").empty());

    // a new line: once, from the speaker's side, in words
    const SaidLine said = sayRefusal(w, b, a);
    CHECK(said.ok());
    auto got = feed.poll(w, feedLoc(), "ru");
    CHECK_EQ(got.size(), std::size_t(1));
    if (got.size() == 1) {
        CHECK_EQ(got[0].speaker, b.id);
        CHECK_EQ(got[0].listener, a.id);
        CHECK_EQ(got[0].key, said.key);
        CHECK(cleanText(got[0].line));
        CHECK(cleanText(got[0].full));
        CHECK(got[0].full.find(got[0].line) != std::string::npos);
        // the speaker named first (in ru the name is in its Russian form, not b.name)
        CHECK(got[0].full.find(": ") != std::string::npos && got[0].full.find(": ") > 0);
    }
    CHECK(feed.poll(w, feedLoc(), "ru").empty());

    // two lines in the same tick, by both sides: both come, each once
    CHECK(sayRefusal(w, a, b).ok());
    CHECK(sayRefusal(w, b, a).ok());
    got = feed.poll(w, feedLoc(), "en");
    CHECK_EQ(got.size(), std::size_t(2));
    if (got.size() == 2) {
        CHECK_EQ(got[0].speaker == a.id || got[1].speaker == a.id, true);
        CHECK_EQ(got[0].speaker == b.id || got[1].speaker == b.id, true);
        CHECK(cleanText(got[0].line) && cleanText(got[1].line));
    }
    CHECK(feed.poll(w, feedLoc(), "en").empty());

    // a later tick still reads on
    w.step();
    CHECK(sayRefusal(w, b, a).ok());
    got = feed.poll(w, feedLoc(), "ru");
    CHECK(!got.empty());
    for (const auto& e : got) CHECK(cleanText(e.line));
}

TEST(altera_speech_feed_barks_paced) {
    static const Json cfg = loadJsonFile(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/sim/test_world.json");
    World w = makeTestWorld(feedContent(), cfg, 5);
    installAi(w);
    w.run(kTicksPerDay / 2); // midday: people awake and about
    std::vector<Character*> people;
    for (auto& [id, c] : w.characters())
        if (!c.isAnimal() && c.body.alive && c.body.stage() == "adult") people.push_back(&c);
    CHECK(people.size() >= 2);
    if (people.size() < 2) return;
    const Vec2 at = people[0]->pos;
    const EntityId skip = people[1]->id;
    SpeechFeed feed;
    feed.poll(w, feedLoc(), "ru");
    const std::size_t traceBefore = people[0]->ai.trace.size();
    int n = 0;
    double last = -1e9;
    for (int sec = 0; sec < 300; ++sec)
        for (const auto& e : feed.barks(w, feedLoc(), "ru", at.x, at.y, 1e6, double(sec), skip)) {
            ++n;
            CHECK(double(sec) - last >= 4.0);
            last = double(sec);
            CHECK(e.speaker != skip);
            CHECK_EQ(e.act, std::string("bark"));
            CHECK(cleanText(e.line));
        }
    CHECK(n > 0);
    CHECK(n <= 300 / 4 + 1);
    CHECK_EQ(people[0]->ai.trace.size(), traceBefore); // presentation only

    // one who just said a line does not mutter over it
    SpeechFeed f2;
    f2.poll(w, feedLoc(), "ru");
    Character& b = *people[0];
    CHECK(sayRefusal(w, b, *people[1]).ok());
    f2.barks(w, feedLoc(), "ru", 0, 0, 0.0, 100.0); // the viewer's clock reaches 100
    CHECK(!f2.poll(w, feedLoc(), "ru").empty());
    for (int sec = 100; sec < 109; ++sec)
        for (const auto& e : f2.barks(w, feedLoc(), "ru", b.pos.x, b.pos.y, 0.5, double(sec))) CHECK(e.speaker != b.id);
}
