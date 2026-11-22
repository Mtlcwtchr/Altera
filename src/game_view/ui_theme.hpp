#pragma once
// The interface's look as data (AL-62): content/ui/themes/<id>.json holds the
// tokens of a style - colours, fonts, sizes, line widths - and the HUD reads
// them instead of constants. The defaults below are the "Edge" style
// (gdd/ui/four_styles/STYLE_3_SKYRIM_MOD.md): a theme file only says what it
// changes, a key it leaves out (or gets wrong) keeps the default.
//
// Header only and free of the engine (the standard library and nlohmann/json),
// so the sim's headless tests can check the loader.
//
//     {"id": "edge", "name": "menu.settings.style.edge",
//      "colours": {"acc": "#C9B07A", "frost": "#16181AC2", "red": [168, 70, 58]},
//      "sizes": {"line": 1, "title_spacing": 0.14},
//      "fonts": {"text": "Jost-Regular.ttf", "title": "Jost-Light.ttf"},
//      "flags": {"blur": false}}

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace altera::game {

struct ThemeColour {
    float r = 1, g = 1, b = 1, a = 1;
    static ThemeColour hex(std::uint32_t rgb, float alpha = 1) {
        return {float((rgb >> 16) & 255) / 255.f, float((rgb >> 8) & 255) / 255.f, float(rgb & 255) / 255.f, alpha};
    }
    bool operator==(const ThemeColour& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

struct UiTheme {
    std::string id = "edge";
    std::string nameKey = "menu.settings.style.edge";   // loc key of its name in the settings
    std::map<std::string, ThemeColour> colours;
    std::map<std::string, float> sizes;
    std::map<std::string, std::string> fonts;
    std::map<std::string, bool> flags;

    UiTheme() { defaults(); }

    // The Edge style: frosted slate glass, old-gold hairlines, bone text.
    void defaults() {
        colours = {
            {"frost", ThemeColour::hex(0x16181A, 0.76f)},       // panels
            {"frost_solid", ThemeColour::hex(0x16181A, 0.94f)}, // panels where blur is off or costs too much
            {"card", ThemeColour::hex(0x16181A, 0.80f)},        // cards and windows that carry writing
            {"tip", ThemeColour::hex(0x121416, 0.90f)},         // tooltips
            {"ink", ThemeColour::hex(0xE6E1D6)},                // text
            {"mute", ThemeColour::hex(0x9C978C)},               // secondary text
            {"acc", ThemeColour::hex(0xC9B07A)},                // old gold: titles, selection, lines, ticks
            {"gold2", ThemeColour::hex(0xE3CF9A)},              // highlight, the active icon
            {"gold_dark", ThemeColour::hex(0x8C7748)},          // gold in shadow
            {"hot", ThemeColour::hex(0xF1E6C8)},                // text on a lit gold button
            {"on_gold", ThemeColour::hex(0xF1E6C8)},            // icons and writing on a chosen (gold) element
            {"rim", ThemeColour::hex(0x0E1012)},                // shadows under text and shapes
            {"red", ThemeColour::hex(0xA8463A)},                // health, the target bar
            {"grn", ThemeColour::hex(0x6F8F4E)},                // strength
            {"blu", ThemeColour::hex(0x4F6F9A)},                // spirit
            {"good", ThemeColour::hex(0x9BB27A)},
            {"warn", ThemeColour::hex(0xD9A648)},
            {"bad", ThemeColour::hex(0xC8553F)},
            {"unk", ThemeColour::hex(0x6F6C66)},                // unknown, disabled
            {"line", ThemeColour::hex(0xE6E1D6, 0.16f)},        // the empty part of a bar, rules
            {"frame", ThemeColour::hex(0xC9B07A, 0.40f)},       // the 1 px gold frame of a card
            {"key", ThemeColour::hex(0x141414, 0.75f)},         // a key cap
            {"key_edge", ThemeColour::hex(0xE6E1D6, 0.50f)},
            {"dim", ThemeColour::hex(0x0A0C0E, 0.55f)},         // the world behind a window
            {"sel", ThemeColour::hex(0xC9B07A, 0.22f)},         // the selected row's gradient, at its left
        };
        sizes = {
            {"line", 1.0f},             // hairline width
            {"frame_alpha", 0.40f},     // the gold frame of a card
            {"radius", 2.0f},           // corner of panels and cards
            {"button_radius", 2.0f},
            {"key_radius", 6.0f},
            {"diamond_min", 40.0f},     // the smallest diamond icon, px
            {"bar", 6.0f},              // a meter's height
            {"title_spacing", 0.14f},   // letter spacing of spaced caps, em
            {"button_spacing", 0.12f},
            {"text_px", 15.0f},         // a line of writing at scale 1
        };
        fonts = {
            {"text", "Jost-Regular.ttf"}, {"title", "Jost-Light.ttf"},
            {"text_fallback", "Nunito-SemiBold.ttf"}, {"title_fallback", "Kurale-Regular.ttf"},
        };
        flags = {
            {"blur", false},       // frosted glass blurs the world behind (else frost_solid)
            {"textured", false},   // the leather and paper tiles of style G under the panels
            {"caps", true},        // titles, tabs, names and buttons in spaced capitals
        };
    }

    ThemeColour colour(const std::string& k) const {
        const auto it = colours.find(k);
        return it == colours.end() ? ThemeColour{1, 0, 1, 1} : it->second;   // magenta: a token nobody defined
    }
    float size(const std::string& k, float fallback = 0) const {
        const auto it = sizes.find(k);
        return it == sizes.end() ? fallback : it->second;
    }
    bool flag(const std::string& k) const {
        const auto it = flags.find(k);
        return it != flags.end() && it->second;
    }
    std::string font(const std::string& k) const {
        const auto it = fonts.find(k);
        return it == fonts.end() ? std::string() : it->second;
    }

    // "#RRGGBB", "#RRGGBBAA" or [r, g, b] / [r, g, b, a] (0..255, alpha 0..1). False: left as it was.
    static bool parseColour(const nlohmann::json& v, ThemeColour& out) {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (!s.empty() && s[0] == '#') s.erase(0, 1);
            if (s.size() != 6 && s.size() != 8) return false;
            for (char c : s)
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            const auto byte = [&](int i) { return float(std::stoi(s.substr(std::size_t(i) * 2, 2), nullptr, 16)) / 255.f; };
            out = {byte(0), byte(1), byte(2), s.size() == 8 ? byte(3) : 1.f};
            return true;
        }
        if (v.is_array() && (v.size() == 3 || v.size() == 4)) {
            for (const auto& e : v)
                if (!e.is_number()) return false;
            out = {std::clamp(v[0].get<float>(), 0.f, 255.f) / 255.f, std::clamp(v[1].get<float>(), 0.f, 255.f) / 255.f,
                   std::clamp(v[2].get<float>(), 0.f, 255.f) / 255.f,
                   v.size() == 4 ? std::clamp(v[3].get<float>(), 0.f, 1.f) : 1.f};
            return true;
        }
        return false;
    }

    // Lays a theme file over the defaults. Unknown or broken values are skipped
    // (their names into `skipped`, if given), never thrown.
    void apply(const nlohmann::json& j, std::vector<std::string>* skipped = nullptr) {
        if (!j.is_object()) return;
        if (j.contains("id") && j["id"].is_string()) id = j["id"].get<std::string>();
        if (j.contains("name") && j["name"].is_string()) nameKey = j["name"].get<std::string>();
        const auto section = [&](const char* name) {
            const auto it = j.find(name);
            return it != j.end() && it->is_object() ? *it : nlohmann::json::object();
        };
        const nlohmann::json colours_ = section("colours");   // a local: items() of a temporary dangles
        for (const auto& [k, v] : colours_.items()) {
            ThemeColour c;
            if (parseColour(v, c)) colours[k] = c;
            else if (skipped) skipped->push_back("colours." + k);
        }
        const nlohmann::json sizes_ = section("sizes");   // a local: items() of a temporary dangles
        for (const auto& [k, v] : sizes_.items()) {
            if (v.is_number()) sizes[k] = v.get<float>();
            else if (skipped) skipped->push_back("sizes." + k);
        }
        const nlohmann::json fonts_ = section("fonts");   // a local: items() of a temporary dangles
        for (const auto& [k, v] : fonts_.items()) {
            if (v.is_string()) fonts[k] = v.get<std::string>();
            else if (skipped) skipped->push_back("fonts." + k);
        }
        const nlohmann::json flags_ = section("flags");   // a local: items() of a temporary dangles
        for (const auto& [k, v] : flags_.items()) {
            if (v.is_boolean()) flags[k] = v.get<bool>();
            else if (skipped) skipped->push_back("flags." + k);
        }
    }

    // content/ui/themes/<id>.json over the defaults; a missing or broken file
    // leaves the defaults and says why.
    static UiTheme load(const std::filesystem::path& file, std::string* why = nullptr,
                        std::vector<std::string>* skipped = nullptr) {
        UiTheme t;
        std::ifstream in(file);
        if (!in) {
            if (why) *why = "no theme file " + file.string();
            return t;
        }
        try {
            t.apply(nlohmann::json::parse(in), skipped);
        } catch (const std::exception& e) {
            if (why) *why = file.string() + ": " + e.what();
        }
        return t;
    }

    // The theme ids in a folder (content/ui/themes), sorted.
    static std::vector<std::string> list(const std::filesystem::path& dir) {
        std::vector<std::string> ids;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".json") ids.push_back(e.path().stem().string());
        std::sort(ids.begin(), ids.end());
        return ids;
    }
};

// The interface scale the player picks (80..130 %) times what the screen
// asks for: the HUD is laid out for 1080 lines, smaller screens shrink it a
// little (not below 0.8: the writing has to stay readable), larger ones grow it.
inline float uiScaleFor(int percent, float screenHeight) {
    const float user = float(std::clamp(percent, 80, 130)) / 100.f;
    const float screen = std::clamp(screenHeight / 1080.f, 0.8f, 2.0f);
    return user * screen;
}

} // namespace altera::game
