#pragma once
// What the people said since the last look (AL-57, the client side of AL-36 /
// AL-53): every "say.line" entry new in the speaker's own trace (say() puts the
// same line on both sides; the speaker's copy is the one), turned into text
// for a bubble over the speaker and for the journal. Pure reading: the world is
// not touched, so the game's sim thread calls it while it makes a snapshot.
//
// The first poll only marks where every trace ends, so a loaded save does not
// flood the screen with old talk.

#include <map>
#include <string>
#include <vector>

#include "sim/core.hpp"

namespace altera::sim {

class World;
class Loc;

struct SpeechEvent {
    Tick tick = 0;
    EntityId speaker = kNone, listener = kNone;
    std::string key;    // loc key of the line
    std::string act;    // the act it was said in (refuse, ignore, greet, ...)
    std::string line;   // the line alone: the bubble
    std::string full;   // "<speaker>: «line»" (trace.say.line): the journal
};

class SpeechFeed {
public:
    std::vector<SpeechEvent> poll(const World& w, const Loc& loc, const std::string& lang);
    void reset() {
        cur_.clear();
        primed_ = false;
    }

private:
    struct Cursor {
        Tick tick = 0;   // the last tick read
        int atTick = 0;  // how many entries of that tick were read
    };
    std::map<EntityId, Cursor> cur_;
    bool primed_ = false;
};

} // namespace altera::sim
