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

    // Barks (sim chooseBark, AL-53 part 2): what the people within `radius`
    // metres of (vx, vy) mutter to themselves - presentation only, the world is
    // not touched. Paced by the viewer's clock `nowSec` (real seconds), not by
    // ticks, so a bubble can be read at any speed: one new bark per `gapSec`
    // at most, each person again after 20..45 s, never one who spoke a line
    // (poll) in the last 10 s. `skip` (the player's person) never barks.
    std::vector<SpeechEvent> barks(const World& w, const Loc& loc, const std::string& lang, double vx, double vy,
                                   double radius, double nowSec, EntityId skip = kNone, double gapSec = 4.0);

    void reset() {
        cur_.clear();
        nextBark_.clear();
        primed_ = false;
        lastBark_ = -1e9;
        clock_ = 0;
    }

private:
    struct Cursor {
        Tick tick = 0;   // the last tick read
        int atTick = 0;  // how many entries of that tick were read
    };
    std::map<EntityId, Cursor> cur_;
    std::map<EntityId, double> nextBark_; // viewer seconds: not before
    double lastBark_ = -1e9, clock_ = 0;  // the last bark; the latest nowSec seen
    bool primed_ = false;
};

} // namespace altera::sim
