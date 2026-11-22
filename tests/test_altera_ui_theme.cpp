// The interface theme as data (src/game_view/ui_theme.hpp, AL-62): the Edge
// theme file loads, a key it leaves out or gets wrong keeps the default, the
// scale stays in its range.

#include <cmath>

#include "framework.hpp"
#include "game_view/ui_theme.hpp"

using altera::game::ThemeColour;
using altera::game::UiTheme;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 1.5f / 255.f; }
} // namespace

TEST(altera_ui_theme_edge_file_loads) {
    std::string why;
    std::vector<std::string> skipped;
    const UiTheme t = UiTheme::load(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ui/themes/edge.json", &why, &skipped);
    CHECK(why.empty());
    CHECK(skipped.empty());
    CHECK_EQ(t.id, std::string("edge"));
    const ThemeColour acc = t.colour("acc");
    CHECK(near(acc.r, 0xC9 / 255.f) && near(acc.g, 0xB0 / 255.f) && near(acc.b, 0x7A / 255.f) && acc.a == 1.f);
    CHECK(near(t.colour("frost").a, 0xC2 / 255.f));
    CHECK_EQ(t.font("text"), std::string("Jost-Regular.ttf"));
    CHECK(!t.flag("blur"));
    CHECK(std::fabs(t.size("title_spacing") - 0.14f) < 1e-6f);
    // every token the code defaults has a value in the file too
    const UiTheme d;
    for (const auto& [k, v] : d.colours) CHECK(t.colours.count(k) == 1);
    const auto ids = UiTheme::list(std::string(ALTERA_SIM_SOURCE_DIR) + "/content/ui/themes");
    CHECK(std::find(ids.begin(), ids.end(), "edge") != ids.end());
}

TEST(altera_ui_theme_missing_key_keeps_the_default) {
    UiTheme t;
    std::vector<std::string> skipped;
    t.apply(nlohmann::json::parse(R"({"id": "test", "colours": {"acc": [255, 0, 0], "ink": "nonsense", "new": "#00FF0080"},
                                      "sizes": {"line": "wide", "bar": 8}, "flags": {"blur": true, "caps": 1}})"),
            &skipped);
    CHECK_EQ(t.id, std::string("test"));
    CHECK(t.colour("acc") == (ThemeColour{1, 0, 0, 1}));
    CHECK(t.colour("ink") == UiTheme().colour("ink"));      // broken value: the default stays
    CHECK(t.colour("mute") == UiTheme().colour("mute"));    // left out: the default
    CHECK(near(t.colour("new").a, 0.5f));
    CHECK(t.size("line") == 1.0f && t.size("bar") == 8.0f);
    CHECK(t.size("nothing", 3.0f) == 3.0f);
    CHECK(t.flag("blur") && t.flag("caps"));                // caps: 1 is not a bool, the default (on) stays
    CHECK_EQ(skipped.size(), std::size_t(3));
    // a missing file: the defaults and a reason
    std::string why;
    const UiTheme m = UiTheme::load("no/such/theme.json", &why);
    CHECK(!why.empty());
    CHECK(m.colour("acc") == UiTheme().colour("acc"));
}

TEST(altera_ui_theme_scale_stays_in_range) {
    CHECK(std::fabs(altera::game::uiScaleFor(100, 1080) - 1.0f) < 1e-6f);
    CHECK(std::fabs(altera::game::uiScaleFor(50, 1080) - 0.8f) < 1e-6f);    // 80 % at least
    CHECK(std::fabs(altera::game::uiScaleFor(200, 1080) - 1.3f) < 1e-6f);   // 130 % at most
    CHECK(std::fabs(altera::game::uiScaleFor(100, 720) - 0.8f) < 1e-6f);    // a small screen shrinks it a little
    CHECK(std::fabs(altera::game::uiScaleFor(100, 2160) - 2.0f) < 1e-6f);
}
