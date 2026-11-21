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
        CHECK(got[0].full.find(b.name) != std::string::npos);
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
