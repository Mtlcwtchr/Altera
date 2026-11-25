// AL-110: the sky of the 2D client (src/client2d/sky2d): the sun and the light
// of the hours, the day's rain spread over the hours, snow, fog, wind - a pure
// function of the world and the tick.
#include <cmath>
#include <string>

#include "client2d/sky2d.hpp"
#include "framework.hpp"
#include "sim/ai.hpp"
#include "sim/worldgen.hpp"

using namespace lr::sim;
namespace c2 = lr::client2d;

namespace {

const std::string kRoot = LR_SIM_SOURCE_DIR;

World& skyWorld() {
    static const Content content = Content::load(kRoot + "/content/ai");
    static World w = [] {
        Json cfg = loadJsonFile(kRoot + "/content/sim/test_world.json");
        return makeTestWorld(content, cfg, 5);
    }();
    return w;
}

const Json& skyCfg() {
    static const Json j = loadJsonFile(kRoot + "/content/sprites_2d/sky2d.json");
    return j;
}

Tick at(int day, double hour) { return Tick(day) * kTicksPerDay + Tick(std::lround(hour * kTicksPerDay / 24.0)); }

} // namespace

TEST(lr_client2d_sky_sun_and_light) {
    const World& w = skyWorld();
    int brighter = 0, days = 0;
    for (int d = 0; d < kDaysPerYear; d += 7) {
        ++days;
        const c2::Sky2d noon = c2::skyAt(w, skyCfg(), at(d, 12.5));
        const c2::Sky2d night = c2::skyAt(w, skyCfg(), at(d, 0.5));
        const c2::Sky2d morning = c2::skyAt(w, skyCfg(), at(d, 9.0));
        const c2::Sky2d evening = c2::skyAt(w, skyCfg(), at(d, 16.0));
        CHECK(noon.sunAlt > 10);
        CHECK(night.sunAlt < 0);
        CHECK(night.lights > 0.8);              // fires and windows show at night
        CHECK(night.sunShadow == 0.0);
        if (noon.lum > night.lum + 0.3) ++brighter;
        // the sun walks east -> south -> west: morning shadows fall west, evening ones east, noon's north
        CHECK(morning.shadowX < 0);
        CHECK(evening.shadowX > 0);
        CHECK(noon.shadowY < 0);
        for (const c2::Sky2d* s : {&noon, &night, &morning, &evening}) {
            CHECK(s->r >= 0 && s->r <= 1 && s->g >= 0 && s->g <= 1 && s->b >= 0 && s->b <= 1);
            CHECK(std::abs(std::hypot(s->windX, s->windY) - 1.0) < 1e-6);
            CHECK(s->fog >= 0 && s->fog <= 1 && s->cloud >= 0 && s->cloud <= 1);
        }
    }
    CHECK(brighter == days);   // even a stormy noon is brighter than midnight
    // midsummer days are longer than midwinter ones: the sun is up at 5:00 in summer only
    CHECK(c2::skyAt(w, skyCfg(), at(135 - w.startDay() + kDaysPerYear, 5.0)).sunAlt > 0);
    CHECK(c2::skyAt(w, skyCfg(), at(315 - w.startDay() + kDaysPerYear, 5.0)).sunAlt < 0);
    // the same tick, the same sky
    const Json a = c2::skyJson(c2::skyAt(w, skyCfg(), at(40, 7.3)));
    const Json b = c2::skyJson(c2::skyAt(w, skyCfg(), at(40, 7.3)));
    CHECK(a == b);
    CHECK(a.contains("sun") && a.contains("light") && a.contains("wind") && a.contains("precip"));
}

TEST(lr_client2d_sky_rain_follows_the_sim) {
    const World& w = skyWorld();
    const EntityId st = w.settlements().begin()->first;
    double simMm = 0, skyMm = 0;
    int wetDays = 0, rainyHoursOnWet = 0, rainOnDry = 0, foggy = 0, snowHours = 0, summerSnow = 0, snowLying = 0, thunder = 0;
    for (int d = 1; d < kDaysPerYear * 2; ++d) {
        const DayWeather dw = weatherAt(w, st, w.startDay() + d);
        simMm += dw.rainMm;
        bool any = false, fogDay = false;
        for (int t = 0; t < int(kTicksPerDay); ++t) {
            const c2::Sky2d s = c2::skyAt(w, skyCfg(), Tick(d) * kTicksPerDay + t);
            skyMm += s.precipMmH * 24.0 / double(kTicksPerDay);
            if (s.precipMmH > 0) any = true;
            if (s.fog > 0.4 && s.precip.empty()) fogDay = true;
            if (t % 6 == 0) {
                if (s.precip == "snow") {
                    ++snowHours;
                    const int doy = (w.startDay() + d) % kDaysPerYear;
                    if (doy >= 100 && doy < 170) ++summerSnow;
                }
                if (s.snowCover > 0.5) ++snowLying;
                if (s.thunder > 0) ++thunder;
            }
        }
        if (dw.wet) ++wetDays, rainyHoursOnWet += any;
        // a dry day rains only when the day before's shower runs on past midnight
        if (!dw.wet && any && !weatherAt(w, st, w.startDay() + d - 1).wet) ++rainOnDry;
        foggy += fogDay;
    }
    CHECK(wetDays > 20);
    CHECK(rainyHoursOnWet >= wetDays * 9 / 10);
    CHECK(rainOnDry == 0);
    // the hours carry the days' rain (windows running over midnight shift a little between days)
    CHECK(std::abs(skyMm - simMm) < simMm * 0.1);
    CHECK(foggy > 5);                // fog some mornings
    CHECK(summerSnow == 0);
    std::printf("  sky: %d wet days, %.0f / %.0f mm, %d foggy, %d snow h, %d lying, %d thunder h\n", wetDays, skyMm, simMm, foggy,
                snowHours, snowLying, thunder);
}
