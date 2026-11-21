#include "sim/speech_feed.hpp"

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
            if (!ev.line.empty()) out.push_back(std::move(ev));
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
    primed_ = true;
    return out;
}

} // namespace altera::sim
