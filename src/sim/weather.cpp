// Daily weather of a settlement (climate.json), a pure function of the world
// seed, the settlement and the day: nothing to save, the same day always has
// the same weather, and a replay sees the same sky.
//
// The seasons give the mean temperature, its spread, the mean rain and the
// share of wet days; between the seasons' middles they blend smoothly. On top
// of that, smooth noise at three time scales - days, weeks, a year - shifts the
// temperature and the chance of rain. Nobody decides that a year is dry: when
// the slow terms run low for months, the spring is a drought; when the weekly
// term runs high at harvest, the sheaves sprout in the field.

#include <algorithm>
#include <cmath>

#include "sim/ai.hpp"

namespace altera::sim {

namespace {

double hashU(std::uint64_t seed, std::int64_t i) {
    Rng r(hashCombine(seed, static_cast<std::uint64_t>(i)));
    return r.uniform();
}

// Smooth value noise in [-1, 1], one node per unit of x.
double vnoise(std::uint64_t seed, double x) {
    const double fl = std::floor(x);
    const auto i = static_cast<std::int64_t>(fl);
    const double f = x - fl;
    const double a = hashU(seed, i) * 2.0 - 1.0, b = hashU(seed, i + 1) * 2.0 - 1.0;
    const double t = f * f * (3.0 - 2.0 * f);
    return a + (b - a) * t;
}

// The season's values at day of year `doy`, blended between season middles
// (45, 135, 225, 315) with a cosine.
SeasonClimate seasonAt(const ClimateDef& c, int doy) {
    const double q = kDaysPerYear / 4.0;
    double p = (doy - q / 2.0) / q;
    p = std::fmod(p + 4.0, 4.0);
    const int i = static_cast<int>(std::floor(p)) % 4;
    const int j = (i + 1) % 4;
    const double f = p - std::floor(p);
    const double t = 0.5 - 0.5 * std::cos(f * 3.14159265358979323846);
    const SeasonClimate& a = c.seasons[i];
    const SeasonClimate& b = c.seasons[j];
    SeasonClimate s;
    s.tempC = a.tempC + (b.tempC - a.tempC) * t;
    s.tempSd = a.tempSd + (b.tempSd - a.tempSd) * t;
    s.rainMm = a.rainMm + (b.rainMm - a.rainMm) * t;
    s.wetChance = a.wetChance + (b.wetChance - a.wetChance) * t;
    return s;
}

} // namespace

DayWeather weatherAt(const World& w, EntityId settlement, int epochDay) {
    const ClimateDef& cl = w.content().climate;
    DayWeather out;
    const int doy = ((epochDay % kDaysPerYear) + kDaysPerYear) % kDaysPerYear;
    const SeasonClimate s = seasonAt(cl, doy);
    std::string biome;
    if (auto it = w.settlements().find(settlement); it != w.settlements().end()) biome = w.map().biomeAt(it->second.center);
    double rainMult = 1.0, tempAdd = 0.0;
    if (auto it = cl.biomeRainMult.find(biome); it != cl.biomeRainMult.end()) rainMult = it->second;
    if (auto it = cl.biomeTempAdd.find(biome); it != cl.biomeTempAdd.end()) tempAdd = it->second;
    const std::uint64_t k = hashCombine(w.seed(), hashCombine(settlement, hashStr("weather")));
    auto key = [&](const char* salt) { return hashCombine(k, hashStr(salt)); };
    const double d = epochDay;
    // Temperature: the season, plus a few-day swing, a monthly spell, a warm or cold year.
    const double tAnom = 0.75 * vnoise(key("t_days"), d / 4.0) + 0.55 * vnoise(key("t_weeks"), d / 30.0) +
                         0.45 * vnoise(key("t_year"), d / 300.0);
    out.tempC = s.tempC + tempAdd + s.tempSd * 1.6 * tAnom;
    // Rain: the chance of a wet day rises and falls with spells and years.
    const double slow = vnoise(key("r_year"), d / 300.0);
    const double wet = 1.0 + 0.7 * vnoise(key("r_days"), d / 5.0) + 0.55 * vnoise(key("r_weeks"), d / 40.0) + 0.55 * slow;
    const double p = std::clamp(s.wetChance * wet, 0.02, 0.97);
    out.wet = hashU(key("r_wet"), epochDay) < p;
    if (out.wet) {
        const double mean = s.rainMm * rainMult / std::max(0.05, s.wetChance);
        const double u = hashU(key("r_amount"), epochDay);
        out.rainMm = mean * -std::log(std::max(1e-9, 1.0 - u)) * std::max(0.3, 1.0 + 0.3 * slow);
    }
    // AL-115: the wind swings round the prevailing one over days and gusts harder on wet days.
    out.windFromDeg = std::fmod(cl.windFromDeg + 70.0 * vnoise(key("w_dir"), d / 3.0) + 360.0, 360.0);
    out.windMs = std::max(0.0, cl.windMs + cl.windSd * (1.2 * vnoise(key("w_ms"), d / 2.0) + (out.wet ? 0.6 : 0.0)));
    return out;
}

DayWeather weatherToday(const World& w, EntityId settlement) {
    return weatherAt(w, settlement, w.startDay() + w.day());
}

} // namespace altera::sim
