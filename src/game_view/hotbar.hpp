#pragma once
// AL-80: the quick bars of the HUD (gdd/ui/hud_wow_skyrim/HUD_WOW_SKYRIM.md):
// two rows of eight slots, keys 1-8 and Shift+1-8, three pages each and a
// page of the fight the bar turns to by itself while the person fights. A
// slot holds an act of the wheel, a goal (a "recipe": the person finds the
// place themselves) or a thing of the bag. Every person has their own layout,
// kept in the save; a new one gets the starting one.
//
// Data only (std and nlohmann/json): the sim-only tests check it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace lr::game {

struct BarRef {
    std::string kind;   // "" empty | act (an interaction) | goal | item
    std::string id;
    int aim = 0;        // RMB: 0 as the act wants, 1 on oneself, 2 on the selected one, 3 ask (the aim cursor)
    bool empty() const { return kind.empty() || id.empty(); }
    bool operator==(const BarRef& o) const { return kind == o.kind && id == o.id && aim == o.aim; }
};

struct Hotbars {
    static constexpr int kBars = 2, kPages = 3, kSlots = 8;
    static constexpr int kFightPage = kPages;   // the page of the fight (shown, not paged to by hand)

    struct Layout {
        BarRef slot[kBars][kPages + 1][kSlots];
        int page[kBars] = {0, 0};
    };

    std::map<std::uint64_t, Layout> people;
    bool locked = false;   // the lock: dragging only with Shift

    // The layout a new person starts with: talk, eat, drink, rest; the fight
    // page: attack, defend, flee, give up (the things of the bag go after).
    static Layout starting() {
        Layout l;
        l.slot[0][0][0] = {"act", "chat", 0};
        l.slot[0][0][1] = {"goal", "eat", 0};
        l.slot[0][0][2] = {"goal", "drink", 0};
        l.slot[0][0][3] = {"goal", "rest", 0};
        l.slot[0][kFightPage][0] = {"act", "attack", 0};
        l.slot[0][kFightPage][1] = {"act", "defend_other", 0};
        l.slot[0][kFightPage][2] = {"act", "flee", 0};
        l.slot[0][kFightPage][3] = {"act", "surrender", 0};
        return l;
    }
    Layout& of(std::uint64_t who) {
        auto it = people.find(who);
        if (it == people.end()) it = people.emplace(who, starting()).first;
        return it->second;
    }
    static bool valid(int bar, int page, int slot) {
        return bar >= 0 && bar < kBars && page >= 0 && page <= kFightPage && slot >= 0 && slot < kSlots;
    }
    BarRef get(std::uint64_t who, int bar, int page, int slot) {
        return valid(bar, page, slot) ? of(who).slot[bar][page][slot] : BarRef{};
    }
    void set(std::uint64_t who, int bar, int page, int slot, const BarRef& r) {
        if (valid(bar, page, slot)) of(who).slot[bar][page][slot] = r;
    }
    // A slot dragged onto another: they change places (within a bar or between the two).
    void swap(std::uint64_t who, int bar, int page, int slot, int bar2, int page2, int slot2) {
        if (!valid(bar, page, slot) || !valid(bar2, page2, slot2)) return;
        Layout& l = of(who);
        std::swap(l.slot[bar][page][slot], l.slot[bar2][page2][slot2]);
    }
    void turnPage(std::uint64_t who, int bar, int step) {
        if (bar < 0 || bar >= kBars) return;
        int& p = of(who).page[bar];
        p = ((p + step) % kPages + kPages) % kPages;
    }

    nlohmann::json toJson() const {
        nlohmann::json j{{"locked", locked}, {"people", nlohmann::json::object()}};
        for (const auto& [who, l] : people) {
            nlohmann::json bars = nlohmann::json::array();
            for (int b = 0; b < kBars; ++b) {
                nlohmann::json pages = nlohmann::json::array();
                for (int p = 0; p <= kFightPage; ++p) {
                    nlohmann::json slots = nlohmann::json::array();
                    for (int k = 0; k < kSlots; ++k) {
                        const BarRef& r = l.slot[b][p][k];
                        if (r.empty()) slots.push_back(nullptr);
                        else {
                            nlohmann::json s{{"kind", r.kind}, {"id", r.id}};
                            if (r.aim) s["aim"] = r.aim;
                            slots.push_back(std::move(s));
                        }
                    }
                    pages.push_back(std::move(slots));
                }
                bars.push_back({{"page", l.page[b]}, {"pages", std::move(pages)}});
            }
            j["people"][std::to_string(who)] = std::move(bars);
        }
        return j;
    }
    // Reads what toJson wrote; anything odd in it is skipped, never thrown on.
    void fromJson(const nlohmann::json& j) {
        people.clear();
        if (!j.is_object()) return;
        locked = j.value("locked", false);
        const nlohmann::json ps = j.value("people", nlohmann::json::object());
        if (!ps.is_object()) return;
        for (const auto& [key, bars] : ps.items()) {
            std::uint64_t who = 0;
            try {
                who = std::stoull(key);
            } catch (...) {
                continue;
            }
            if (!bars.is_array()) continue;
            Layout l;   // a saved layout is the person's own: empty slots stay empty
            for (int b = 0; b < kBars && b < int(bars.size()); ++b) {
                const nlohmann::json& bar = bars[std::size_t(b)];
                if (!bar.is_object()) continue;
                l.page[b] = std::clamp(bar.value("page", 0), 0, kPages - 1);
                const nlohmann::json pages = bar.value("pages", nlohmann::json::array());
                for (int p = 0; p <= kFightPage && p < int(pages.size()); ++p) {
                    const nlohmann::json& slots = pages[std::size_t(p)];
                    if (!slots.is_array()) continue;
                    for (int k = 0; k < kSlots && k < int(slots.size()); ++k) {
                        const nlohmann::json& s = slots[std::size_t(k)];
                        if (!s.is_object()) continue;
                        BarRef r{s.value("kind", std::string()), s.value("id", std::string()), std::clamp(s.value("aim", 0), 0, 3)};
                        if (r.kind == "act" || r.kind == "goal" || r.kind == "item") l.slot[b][p][k] = r;
                    }
                }
            }
            people[who] = l;
        }
    }
};

// What pressing a slot comes to: the command for the sim, or the aim cursor
// first (an act on a person with nobody chosen), or nothing (an empty slot).
struct BarUse {
    enum Kind { None, Command, Aim } kind = None;
    std::string type;          // perform | set_goal
    nlohmann::json args;       // without "id" (the game adds the person)
};

// `actTarget`: what the act is done to ("character", "self", "object:...",
// "creature:...", "" unknown); `itemAct`: the act that uses up the thing (for
// an item slot, "" if none); `chosen`: the person picked (0 none).
inline BarUse barUse(const BarRef& r, const std::string& actTarget, const std::string& itemAct, std::uint64_t chosen) {
    BarUse u;
    if (r.empty()) return u;
    if (r.kind == "goal") {
        u.kind = BarUse::Command;
        u.type = "set_goal";
        u.args = {{"goal", r.id}};
        return u;
    }
    const std::string act = r.kind == "item" ? itemAct : r.id;
    if (act.empty()) return u;
    u.type = "perform";
    u.args = {{"interaction", act}};
    const bool onPerson = r.kind == "act" && actTarget == "character";
    if (r.aim == 1 || !onPerson) {   // on oneself, or the sim finds the place (a tree, the well, the store)
        u.kind = BarUse::Command;
        return u;
    }
    if (r.aim == 3 || chosen == 0) {
        u.kind = BarUse::Aim;
        return u;
    }
    u.kind = BarUse::Command;
    u.args["target"] = chosen;
    return u;
}

} // namespace lr::game
