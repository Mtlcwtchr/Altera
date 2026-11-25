// AL-77: the ENB-like frame chain's settings and per-frame uniforms (the GPU
// passes themselves are checked by the shader compile and in the game).
#include "framework.hpp"

#include "terrain_core/enb.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace terrain_core;

namespace {

std::filesystem::path enbFile() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "content/config/terrain_core/enb.json";
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

LightUniforms noonLight() {
    LightUniforms l{};
    l.sun[0] = 0.0f;
    l.sun[1] = 0.6f;
    l.sun[2] = 0.8f;
    l.sunAbove = 1.0f;
    for (int i = 0; i < 3; ++i) l.sunColour[i] = 1.0f;
    l.fogDensity = 4.5e-5f;
    l.fogFalloff = 1.0f / 1600.0f;
    return l;
}

RayCamera lookingAlong(float x, float y, float z) {
    const double n = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
    const double f[3] = {x / n, y / n, z / n};
    return rayCamera(f, 0.9, 16.0 / 9.0);
}

} // namespace

// The shipped file starts on "enb" and offers "classic"; "enb" is the full
// chain (half floats, ACES in linear light, AO, bloom), "classic" all neutral.
TEST(terrain_core_enb_presets_from_the_shipped_file) {
    EnbSettings s;
    EnbPresets presets;
    std::string why;
    CHECK(loadEnbSettings(enbFile(), "", s, &why, &presets));
    CHECK(presets.start == "enb");
    CHECK(std::find(presets.names.begin(), presets.names.end(), "classic") != presets.names.end());
    CHECK(s.preset == "enb");
    CHECK(s.hdr);
    CHECK(s.tonemap == 1);
    CHECK(s.linearize);
    CHECK(s.aoStrength > 0);
    CHECK(s.bloom > 0);
    CHECK(s.exposureMax >= s.exposureMin);

    EnbSettings c;
    CHECK(loadEnbSettings(enbFile(), "classic", c, &why));
    const EnbSettings neutral;
    CHECK(c.preset == "classic");
    CHECK(c.tonemap == 0 && !c.linearize);
    CHECK(c.aoStrength == 0 && c.contactStrength == 0 && c.bloom == 0 && c.rays == 0);
    CHECK(c.scatter == 0 && c.aerial == 0 && c.fogExtra == 0 && c.sunHalo == 0);
    CHECK(c.adaptation == 0 && c.exposure == 1);
    CHECK(c.contrast == 1 && c.saturation == 1 && c.vibrance == 0 && c.splitStrength == 0);
    CHECK(c.vignette == 0 && c.grain == 0);
    CHECK(c.gain[0] == neutral.gain[0] && c.lift[2] == neutral.lift[2]);
}

// A preset with a "base" starts from it and changes only what it says.
TEST(terrain_core_enb_preset_base_and_bad_names) {
    EnbSettings base, soft;
    std::string why;
    CHECK(loadEnbSettings(enbFile(), "enb", base, &why));
    CHECK(loadEnbSettings(enbFile(), "enb_soft", soft, &why));
    CHECK(soft.preset == "enb_soft");
    CHECK(soft.tonemap == 2);
    CHECK(soft.aoStrength == base.aoStrength);
    CHECK(soft.grain == 0);

    EnbSettings kept;
    kept.exposure = 3.0f;
    CHECK(!loadEnbSettings(enbFile(), "no_such_preset", kept, &why));
    CHECK(!why.empty());
    CHECK(kept.exposure == 3.0f);
    CHECK(!loadEnbSettings("no/such/file.json", "", kept, &why));
}

// Values out of range are held to their limits.
TEST(terrain_core_enb_values_are_clamped) {
    const auto file = std::filesystem::temp_directory_path() / "lr_enb_clamp_test.json";
    {
        std::ofstream out(file);
        out << R"({"preset": "x", "presets": {"x": {"ao": {"strength": 99, "radius": -1},
              "exposure": {"min": 2.0, "max": 1.0}, "tone": {"curve": "agx"}, "lens": {"grain": 7}}}})";
    }
    EnbSettings s;
    std::string why;
    CHECK(loadEnbSettings(file, "", s, &why));
    CHECK(s.aoStrength == 4.0f);
    CHECK(s.aoRadius == 0.05f);
    CHECK(s.exposureMax >= s.exposureMin);
    CHECK(s.tonemap == 2);
    CHECK(s.grain == 0.5f);
    std::filesystem::remove(file);
}

// Split toning follows the sun: day tints high, golden ones low, night ones under the horizon.
TEST(terrain_core_enb_split_tone_by_sun_height) {
    EnbSettings s;
    s.day.highlights[0] = 1.1f;
    s.golden.highlights[0] = 1.3f;
    s.night.highlights[0] = 0.7f;
    CHECK(near(enbTintsFor(s, 0.8f).highlights[0], 1.1f));
    CHECK(near(enbTintsFor(s, 0.02f).highlights[0], 1.3f));
    CHECK(near(enbTintsFor(s, -0.3f).highlights[0], 0.7f));
    const float between = enbTintsFor(s, 0.27f).highlights[0];
    CHECK(between > 1.1f && between < 1.3f);
}

// The sun lands in the middle of the screen when the eye looks at it, and is
// not used at all behind the eye.
TEST(terrain_core_enb_sun_on_screen) {
    const EnbSettings s;
    const LightUniforms light = noonLight();
    const EnbUniforms at = enbUniforms(s, light, lookingAlong(0.0f, 0.6f, 0.8f), 0.1f, 300.0f, 1600, 900, 5.0f, 0.016f);
    CHECK(near(at.sunUv[0], 0.5f));
    CHECK(near(at.sunUv[1], 0.5f));
    CHECK(at.sunUv[2] > 0.9f);
    CHECK(near(at.sunUv[3], 300.0f));
    CHECK(near(at.camF[3], 0.1f));
    CHECK(near(at.res[0], 1600.0f) && near(at.res[3], 1.0f / 900.0f, 1e-7f));

    const EnbUniforms behind = enbUniforms(s, light, lookingAlong(0.0f, -0.6f, -0.2f), 0.1f, 300.0f, 1600, 900, 5.0f, 0.016f);
    CHECK(behind.sunUv[2] == 0.0f);

    // Looking level and to the side of it: up and right of the centre, fading.
    const EnbUniforms side = enbUniforms(s, light, lookingAlong(0.3f, 1.0f, 0.0f), 0.1f, 300.0f, 1600, 900, 5.0f, 0.016f);
    CHECK(side.sunUv[1] < 0.5f);
}
