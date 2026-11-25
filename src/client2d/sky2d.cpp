// AL-110: the sky over the 2D map (see sky2d.hpp).
#include "client2d/sky2d.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "sim/ai.hpp"

namespace altera::client2d {

using sim::Json;

namespace {

constexpr double kPi = 3.14159265358979323846;

double hashU(std::uint64_t seed, std::int64_t i) {
    sim::Rng r(sim::hashCombine(seed, static_cast<std::uint64_t>(i)));
    return r.uniform();
}
// smooth value noise in [-1, 1], one node per unit
double vnoise(std::uint64_t seed, double x) {
    const double fl = std::floor(x);
    const auto i = static_cast<std::int64_t>(fl);
    const double f = x - fl, t = f * f * (3.0 - 2.0 * f);
    const double a = hashU(seed, i) * 2.0 - 1.0, b = hashU(seed, i + 1) * 2.0 - 1.0;
    return a + (b - a) * t;
}
double val(const Json& j, const char* k, double d) {
    if (!j.is_object()) return d;
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}
const Json& sub(const Json& j, const char* k) {
    static const Json none = Json::object();
    if (!j.is_object()) return none;
    const auto it = j.find(k);
    return it != j.end() ? *it : none;
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double smooth(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

struct Ctx {
    const sim::World& w;
    const Json& cfg;
    sim::EntityId st = sim::kNone;
    std::uint64_t key = 0;
    std::uint64_t salt(const char* s) const { return sim::hashCombine(key, sim::hashStr(s)); }
    sim::DayWeather day(int epochDay) const {
        if (st == sim::kNone) return {};
        return sim::weatherAt(w, st, epochDay);
    }
};

// When a wet day rains: a window of hours around a start hour, a bell of mm/h.
struct RainWindow {
    double start = 0, hours = 0, peak = 0;
};
RainWindow rainWindow(const Ctx& c, int epochDay, const sim::DayWeather& dw) {
    RainWindow r;
    if (!dw.wet || dw.rainMm <= 0) return r;
    const Json& p = sub(c.cfg, "precip");
    r.hours = std::clamp(val(p, "hours_base", 2) + dw.rainMm * val(p, "hours_per_mm", 0.8), 1.0, val(p, "hours_max", 18));
    r.start = hashU(c.salt("rain_start"), epochDay) * 24.0;
    r.peak = dw.rainMm / (r.hours * 2.0 / kPi);   // the bell's integral is hours*2/pi
    return r;
}
// mm/h at hour h of epochDay (a window may run on from the day before)
double rainAt(const Ctx& c, int epochDay, double h) {
    double mm = 0;
    for (int back = 0; back <= 1; ++back) {
        const sim::DayWeather dw = c.day(epochDay - back);
        const RainWindow r = rainWindow(c, epochDay - back, dw);
        if (r.hours <= 0) continue;
        const double t = h + 24.0 * back - r.start;
        if (t >= 0 && t <= r.hours) mm += r.peak * std::sin(kPi * t / r.hours);
    }
    return mm;
}
double hourTemp(const Ctx& c, const sim::DayWeather& dw, double h) {
    return dw.tempC + val(sub(c.cfg, "precip"), "temp_swing", 4.0) * std::sin(2 * kPi * (h - 9.0) / 24.0);
}

// Snow lying at the start of epochDay (cm), from the days before; cached per world/day.
double snowCm(const Ctx& c, int epochDay) {
    static std::unordered_map<std::uint64_t, double> cache;
    const std::uint64_t k = sim::hashCombine(c.key, static_cast<std::uint64_t>(epochDay));
    if (const auto it = cache.find(k); it != cache.end()) return it->second;
    const Json& s = sub(c.cfg, "snow");
    const Json& p = sub(c.cfg, "precip");
    const int back = int(val(s, "days_back", 30));
    double cm = 0;
    for (int d = epochDay - back; d < epochDay; ++d) {
        const sim::DayWeather dw = c.day(d);
        if (dw.wet && dw.tempC <= val(p, "snow_c", 0.5) + 1.0) cm += dw.rainMm * val(s, "cm_per_mm", 1.0);
        else if (dw.wet) cm -= dw.rainMm * 0.5;          // rain on snow
        cm -= std::max(0.0, dw.tempC - 1.0) * val(s, "melt", 0.6);
        cm = std::max(0.0, cm);
    }
    if (cache.size() > 4096) cache.clear();
    cache[k] = cm;
    return cm;
}

} // namespace

Sky2d skyAt(const sim::World& w, const Json& cfg, sim::Tick tick) {
    Sky2d s;
    Ctx c{w, cfg};
    if (!w.settlements().empty()) c.st = w.settlements().begin()->first;
    c.key = sim::hashCombine(w.seed(), sim::hashCombine(c.st, sim::hashStr("sky2d")));
    const int dayN = int(tick / sim::kTicksPerDay);
    const int epochDay = w.startDay() + dayN;
    const int doy = ((epochDay % sim::kDaysPerYear) + sim::kDaysPerYear) % sim::kDaysPerYear;
    const double h = double(tick % sim::kTicksPerDay) * 24.0 / double(sim::kTicksPerDay);
    s.hour = h;
    static const char* kSeasons[4] = {"spring", "summer", "autumn", "winter"};
    const std::string season = kSeasons[std::min(3, doy / (sim::kDaysPerYear / 4))];

    // --- the sun -----------------------------------------------------------------
    const Json& sun = sub(cfg, "sun");
    const double yearPhase = std::sin(2 * kPi * (doy - 45.0) / sim::kDaysPerYear);   // 1 at midsummer
    const double dayLen = val(sun, "day_len", 12) + val(sun, "day_len_swing", 4.5) * yearPhase;
    const double noon = val(sun, "noon", 12.5);
    const double rise = noon - dayLen / 2, set = noon + dayLen / 2;
    const double altNoon = val(sun, "alt_noon", 40) + val(sun, "alt_swing", 22) * yearPhase;
    if (h >= rise && h <= set) s.sunAlt = altNoon * std::sin(kPi * (h - rise) / dayLen);
    else {
        const double off = h < rise ? std::min(rise - h, h + 24 - set) : std::min(h - set, rise + 24 - h);
        s.sunAlt = std::max(-40.0, -off * 12.0);
    }
    s.sunAz = kPi * std::clamp((h - rise) / dayLen, 0.0, 1.0);
    if (s.sunAlt > 0) {
        const double len = std::clamp(0.6 / std::tan(std::max(4.0, s.sunAlt) * kPi / 180.0), 0.15, 1.1);
        s.shadowX = -std::cos(s.sunAz) * len;
        s.shadowY = -std::sin(s.sunAz) * len;
    }
    // the moon: a 29.5 day round
    s.moon = 0.5 - 0.5 * std::cos(2 * kPi * std::fmod(epochDay / 29.53 + hashU(c.salt("moon"), 0), 1.0));

    // --- the day's weather and its hours ------------------------------------------
    const sim::DayWeather dw = c.day(epochDay);
    const sim::DayWeather yesterday = c.day(epochDay - 1);
    s.tempC = hourTemp(c, dw, h);
    const Json& pc = sub(cfg, "precip");
    s.precipMmH = rainAt(c, epochDay, h);
    if (s.precipMmH > 0.02) {
        s.precip = s.tempC <= val(pc, "snow_c", 0.5) ? "snow" : s.tempC <= val(pc, "sleet_c", 2.5) ? "sleet" : "rain";
        s.intensity = std::clamp(0.18 + s.precipMmH / 4.0, 0.0, 1.0);
        if (s.precip == "rain" && s.precipMmH >= val(pc, "thunder_mmh", 2.6) && dw.tempC >= val(pc, "thunder_c", 13))
            s.thunder = std::clamp((s.precipMmH - val(pc, "thunder_mmh", 2.6)) / 3.0 + 0.3, 0.0, 1.0);
    } else s.precipMmH = 0;

    // clouds: the day's base, a slow drift, thick while it rains and an hour around it
    double cloudDay = dw.wet ? 0.55 + 0.35 * std::min(1.0, dw.rainMm / 8.0) : 0.55 * std::pow(hashU(c.salt("cloud"), epochDay), 1.6);
    s.cloud = std::clamp(cloudDay + 0.22 * vnoise(c.salt("cloud_h"), (dayN * 24.0 + h) / 5.0), 0.0, 1.0);
    const double near = std::max({rainAt(c, epochDay, std::max(0.0, h - 1.0)), rainAt(c, epochDay, std::min(23.99, h + 1.0)), s.precipMmH});
    if (near > 0.02) s.cloud = std::max(s.cloud, 0.7 + 0.3 * std::min(1.0, near / 3.0));

    // wind: the sim's day wind (AL-115: DayWeather.windFromDeg, clockwise from north = -y; fire
    // spreads by it), turned a little and gusting by the hour, stronger in rain and storms
    const Json& wd = sub(cfg, "wind");
    const double from = (dw.windFromDeg + 20.0 * vnoise(c.salt("wind_dir_h"), (dayN * 24.0 + h) / 3.0)) * kPi / 180.0;
    s.windX = -std::sin(from), s.windY = std::cos(from);   // where it blows to
    s.wind = dw.windMs + val(wd, "rain_boost", 3.0) * s.intensity + val(wd, "storm_boost", 6.0) * s.thunder;
    s.wind = std::max(0.0, s.wind * (1.0 + 0.3 * vnoise(c.salt("gust"), (dayN * 24.0 + h) * 1.5)));

    // fog: some mornings, more after a wet day, blown off by the wind; a haze in rain and snow
    const Json& fg = sub(cfg, "fog");
    double chance = val(sub(fg, "chance"), season.c_str(), 0.2) + (yesterday.wet ? val(fg, "after_wet", 0.25) : 0.0);
    if (hashU(c.salt("fog"), epochDay) < chance) {
        const double f0 = rise - val(fg, "fog_from", 3.0), f1 = rise + val(fg, "fog_to", 3.5);
        const double t = h;
        if (t >= f0 && t <= f1) {
            const double peak = rise + 0.5;
            const double shape = t <= peak ? smooth(f0, peak, t) : 1.0 - smooth(peak, f1, t);
            const double depth = 0.55 + 0.45 * hashU(c.salt("fog_depth"), epochDay);
            s.fog = depth * shape * std::clamp(1.0 - s.wind / val(fg, "wind_clear", 5.0), 0.0, 1.0);
        }
    }
    if (s.precip == "rain" || s.precip == "sleet") s.fog = std::max(s.fog, val(fg, "rain_haze", 0.25) * s.intensity);
    if (s.precip == "snow") s.fog = std::max(s.fog, val(fg, "snow_haze", 0.35) * s.intensity);

    // snow lying: the days before, plus today's snow so far, less today's melt so far
    double cm = snowCm(c, epochDay);
    const Json& sn = sub(cfg, "snow");
    for (double hh = 0.5; hh < h; hh += 1.0) {
        const double mm = rainAt(c, epochDay, hh), t = hourTemp(c, dw, hh);
        if (t <= val(pc, "snow_c", 0.5) + 0.5) cm += mm * val(sn, "cm_per_mm", 1.0);
        else cm -= mm * 0.5;
        cm -= std::max(0.0, t - 1.0) * val(sn, "melt", 0.6) / 24.0;
        cm = std::max(0.0, cm);
    }
    s.snowCover = std::clamp(cm / val(sn, "full_cm", 4.0), 0.0, 1.0);

    // wet ground: rain of the last hours, drying off over ~5 hours (slower without sun)
    double wet = 0;
    for (int back = 0; back <= 12; ++back) {
        double hh = h - back;
        int d = epochDay;
        if (hh < 0) hh += 24, --d;
        const double mm = rainAt(c, d, hh);
        if (mm > 0.02) wet = std::max(wet, std::min(1.0, 0.35 + mm / 2.0) * std::exp(-back / (s.sunAlt > 10 ? 3.5 : 6.0)));
    }
    s.wet = s.snowCover > 0.6 ? 0.0 : wet;

    lightHour(s, cfg);
    return s;
}

void lightHour(Sky2d& s, const Json& cfg) {
    const Json& amb = sub(cfg, "ambient");
    double rgb[3] = {1, 1, 1};
    if (amb.is_array() && !amb.empty()) {
        auto at = [&](std::size_t i, int k) { return amb[i]["rgb"][k].get<double>(); };
        std::size_t i = 0;
        while (i + 1 < amb.size() && s.sunAlt > amb[i + 1].value("alt", 0.0)) ++i;
        if (s.sunAlt <= amb[0].value("alt", 0.0) || i + 1 >= amb.size())
            for (int k = 0; k < 3; ++k) rgb[k] = at(i, k);
        else {
            const double a0 = amb[i].value("alt", 0.0), a1 = amb[i + 1].value("alt", 0.0);
            const double t = smooth(a0, a1, s.sunAlt);
            for (int k = 0; k < 3; ++k) rgb[k] = lerp(at(i, k), at(i + 1, k), t);
        }
    }
    // the moon lifts the night
    const double night = 1.0 - smooth(-14.0, -4.0, s.sunAlt);
    const double moonLift = 1.0 + night * val(cfg, "moon", 0.45) * (s.moon - 0.4) * (1.0 - 0.8 * s.cloud);
    for (double& v : rgb) v *= moonLift;
    // clouds grey the light and dim it; rain and storms more
    const double lumA = 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2];
    const double grey = 0.65 * s.cloud;
    const double dim = 1.0 - val(cfg, "cloud_dim", 0.28) * s.cloud - val(cfg, "rain_dim", 0.14) * s.intensity - val(cfg, "storm_dim", 0.25) * s.thunder;
    for (double& v : rgb) v = std::clamp(lerp(v, lumA * 1.02, grey) * dim, 0.0, 1.0);
    // a cold blue in snow, a pale lift in fog
    if (s.precip == "snow" || s.snowCover > 0.3) rgb[2] = std::min(1.0, rgb[2] * 1.06);
    for (double& v : rgb) v = lerp(v, std::min(1.0, lumA * 1.1), 0.25 * s.fog);
    s.r = rgb[0], s.g = rgb[1], s.b = rgb[2];
    s.lum = 0.3 * s.r + 0.59 * s.g + 0.11 * s.b;
    s.lights = std::clamp((0.82 - s.lum) / 0.5, 0.0, 1.0);
    s.sunShadow = smooth(0.0, 8.0, s.sunAlt) * (1.0 - 0.85 * s.cloud) * (1.0 - 0.6 * s.fog);
}

Sky2d withWeather(Sky2d s, const Json& cfg, const std::string& kind) {
    auto wet = [&](const char* p, double mmh, double cloud) {
        s.precip = p, s.precipMmH = mmh, s.intensity = std::clamp(0.18 + mmh / 4.0, 0.0, 1.0), s.cloud = cloud;
    };
    if (kind == "clear") s.precip.clear(), s.precipMmH = s.intensity = s.thunder = s.fog = 0, s.cloud = 0.05, s.wet = 0;
    else if (kind == "cloudy") s.precip.clear(), s.precipMmH = s.intensity = s.thunder = 0, s.cloud = 0.5;
    else if (kind == "rain") wet("rain", 1.6, 0.9), s.wet = 0.9, s.wind = std::max(s.wind, 4.0), s.thunder = 0;
    else if (kind == "storm") wet("rain", 5.0, 1.0), s.wet = 1.0, s.thunder = 0.9, s.wind = 12.0;
    else if (kind == "snow") wet("snow", 1.4, 0.9), s.snowCover = 0.8, s.wet = 0, s.tempC = -3, s.thunder = 0;
    else if (kind == "sleet") wet("sleet", 1.4, 0.9), s.snowCover = 0.2, s.wet = 0.6, s.tempC = 1.5, s.thunder = 0;
    else if (kind == "fog") s.precip.clear(), s.precipMmH = s.intensity = s.thunder = 0, s.fog = 0.85, s.wind = 1.0, s.cloud = 0.6;
    else if (kind == "snowy") s.precip.clear(), s.precipMmH = s.intensity = 0, s.snowCover = 0.9, s.cloud = 0.2, s.tempC = -5;
    else return s;
    lightHour(s, cfg);
    return s;
}

Json skyJson(const Sky2d& s) {
    auto r3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    Json j;
    j["hour"] = r3(s.hour);
    j["sun"] = {{"alt", r3(s.sunAlt)}, {"az", r3(s.sunAz)}, {"sx", r3(s.shadowX)}, {"sy", r3(s.shadowY)}, {"shadow", r3(s.sunShadow)}};
    j["light"] = {r3(s.r), r3(s.g), r3(s.b)};
    j["lum"] = r3(s.lum);
    j["lights"] = r3(s.lights);
    j["cloud"] = r3(s.cloud);
    j["precip"] = s.precip;
    j["mmh"] = r3(s.precipMmH);
    j["intensity"] = r3(s.intensity);
    j["thunder"] = r3(s.thunder);
    j["fog"] = r3(s.fog);
    j["wind"] = {{"x", r3(s.windX)}, {"y", r3(s.windY)}, {"ms", r3(s.wind)}};
    j["snow"] = r3(s.snowCover);
    j["wet"] = r3(s.wet);
    j["t"] = std::round(s.tempC * 10) / 10;
    j["moon"] = r3(s.moon);
    return j;
}

} // namespace altera::client2d
