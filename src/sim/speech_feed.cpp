#include "sim/speech_feed.hpp"

#include <algorithm>
#include <cstdint>

#include "sim/dialogue_say.hpp"
#include "sim/loc.hpp"
#include "sim/world.hpp"

namespace altera::sim {

std::vector<SpeechEvent> SpeechFeed::poll(const World& w, const Loc& loc, const std::string& lang) {
    std::vector<SpeechEvent> out;
    for (const auto& [id, c] : w.characters()) {
        const auto& tr = c.ai.trace;
        Cursor& cu = cur_[id]; // a trace met later (a newborn's) is read whole
        if (!primed_) {
            // the first look: from where every trace ends now
            if (!tr.empty()) {
                cu.tick = tr.back().tick;
                for (const auto& e : tr) cu.atTick += e.tick == cu.tick;
            }
            continue;
        }
        int at = 0;
        for (const auto& e : tr) {
            if (e.tick < cu.tick) continue;
            if (e.tick == cu.tick && ++at <= cu.atTick) continue;
            if (e.code != "say.line") continue;
            const EntityId speaker = e.params.value("speaker", kNone);
            if (speaker != id) continue; // the listener's copy of the same line
            SpeechEvent ev;
            ev.tick = e.tick;
            ev.speaker = speaker;
            ev.listener = e.params.value("listener", kNone);
            ev.key = e.params.value("key", std::string());
            ev.act = e.params.value("act", std::string());
            Utterance u;
            if (e.params.contains("utt") && fromJson(e.params["utt"], u)) ev.line = utteranceText(loc, lang, w, u, ev.key);
            ev.full = lineText(loc, lang, w, e.params);
            if (ev.line.empty()) ev.line = ev.full;
            if (ev.line.empty()) continue;
            nextBark_[speaker] = std::max(nextBark_[speaker], clock_ + 10.0); // a line just said: no muttering over it
            out.push_back(std::move(ev));
        }
        if (!tr.empty()) {
            cu.tick = tr.back().tick;
            cu.atTick = 0;
            for (const auto& e : tr) cu.atTick += e.tick == cu.tick;
        }
    }
    // the dead and the gone leave their cursors behind
    for (auto it = cur_.begin(); it != cur_.end();)
        it = w.character(it->first) ? std::next(it) : cur_.erase(it);
    for (auto it = nextBark_.begin(); it != nextBark_.end();)
        it = w.character(it->first) ? std::next(it) : nextBark_.erase(it);
    primed_ = true;
    return out;
}

std::vector<SpeechEvent> SpeechFeed::barks(const World& w, const Loc& loc, const std::string& lang, double vx,
                                           double vy, double radius, double nowSec, EntityId skip, double gapSec) {
    std::vector<SpeechEvent> out;
    clock_ = std::max(clock_, nowSec);
    if (nowSec - lastBark_ < gapSec) return out;
    // the one waiting longest of those in sight and free to mutter
    const Character* pick = nullptr;
    double pickAt = 0;
    for (const auto& [id, c] : w.characters()) {
        if (id == skip || c.isAnimal() || !c.body.alive) continue;
        const double dx = c.pos.x - vx, dy = c.pos.y - vy;
        if (dx * dx + dy * dy > radius * radius) continue;
        auto [it, met] = nextBark_.try_emplace(id, 0.0);
        if (met) it->second = nowSec + 3.0 + double((id * 2654435761u) % 1000) / 1000.0 * 15.0; // spread the first ones
        if (it->second > nowSec) continue;
        if (!pick || it->second < pickAt) pick = &c, pickAt = it->second;
    }
    if (!pick) return out;
    const double wait = 20.0 + double((std::uint64_t(pick->id) * 40503u + std::uint64_t(nowSec)) % 1000) / 1000.0 * 25.0;
    nextBark_[pick->id] = nowSec + wait;
    const SaidLine line = chooseBark(w, *pick);
    if (!line.ok()) return out; // nothing to say now: asked again after the wait
    SpeechEvent ev;
    ev.tick = w.now();
    ev.speaker = ev.listener = pick->id;
    ev.key = line.key;
    ev.act = "bark";
    ev.line = utteranceText(loc, lang, w, line.utterance, line.key);
    if (ev.line.empty()) return out;
    lastBark_ = nowSec;
    out.push_back(std::move(ev));
    return out;
}

} // namespace altera::sim
