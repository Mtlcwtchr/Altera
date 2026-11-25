#pragma once
// AL-110: the sky over the 2D map, hour by hour. The sim keeps the day's mean
// temperature and rain of a settlement (sim/weather.cpp); this spreads them over
// the hours as the window draws them: the sun (altitude, where it stands, the
// shadows it casts), the light colour of the hour (dawn, day, dusk, the moon),
// clouds, rain / sleet / snow and thunder, fog, wind, the snow lying and how wet
// the ground is. A pure function of the world and the tick (the config is
// content/sprites_2d/sky2d.json): nothing to save, a replay sees the same sky.
// Only the client reads it; the sim's own needs (cold, wet) stay with the sim.

#include "sim/world.hpp"

namespace altera::client2d {

struct Sky2d {
    double hour = 12;              // 0..24
    double sunAlt = 45;            // degrees; below 0 the sun is down
    double sunAz = 1.57;           // radians: 0 east, pi/2 south (map +y), pi west
    double shadowX = 0, shadowY = 0;   // a shadow's length per metre of height, along the map
    double sunShadow = 0;          // 0..1: how dark the sun's shadows are (clouds soften them)
    double r = 1, g = 1, b = 1;    // the light colour of the hour, 0..1
    double lum = 1;                // its brightness
    double lights = 0;             // 0..1: how much fires and windows show (0 at bright day)
    double cloud = 0;              // 0..1
    std::string precip;            // "" | rain | sleet | snow
    double precipMmH = 0;          // mm an hour
    double intensity = 0;          // 0..1, what the window draws
    double thunder = 0;            // 0..1
    double fog = 0;                // 0..1
    double windX = 1, windY = 0;   // where the wind blows to (unit)
    double wind = 0;               // m/s
    double snowCover = 0;          // 0..1 of the ground white
    double wet = 0;                // 0..1 how wet the ground looks
    double tempC = 10;             // the hour's temperature
    double moon = 0.5;             // 0 new .. 1 full
};

// The sky at `tick` (any tick: the window asks the next one too, to blend).
// `cfg` is sky2d.json (missing keys take the defaults written there).
Sky2d skyAt(const sim::World& w, const sim::Json& cfg, sim::Tick tick);
sim::Json skyJson(const Sky2d& s);
// The light colour, brightness and the sun's shadow strength from the sun, the
// moon and the weather fields (skyAt ends with it).
void lightHour(Sky2d& s, const sim::Json& cfg);
// For frames and trying the look: the weather of `s` replaced by one of clear |
// cloudy | rain | storm | snow | sleet | fog | snowy (snow lying, none falling);
// anything else leaves it as it is. (altera_game_2d: ALTERA_2D_WEATHER.)
Sky2d withWeather(Sky2d s, const sim::Json& cfg, const std::string& kind);

} // namespace altera::client2d
