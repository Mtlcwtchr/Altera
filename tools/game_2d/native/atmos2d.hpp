#pragma once
// AL-110: light, shadows and weather of the 2D window, in 2D with the SDL
// renderer's blend modes (no shaders of our own):
//  - the sky of the hour comes from the sim side (src/client2d/sky2d, /api/state
//    `sky` and `sky_next`), blended between the two ticks and eased;
//  - shadows: sprite silhouettes sheared along the sun (or away from a fire at
//    night) and the swept footprints of roofs, drawn opaque into one buffer and
//    laid over the ground at the sun's strength, so overlaps don't darken twice;
//  - light: a half-size light map cleared to the hour's light, fires, windows and
//    torches added as soft discs (ADD), multiplied over the map (MOD);
//  - weather: wet and snowy ground, fog banks drifting with the
//    wind, smoke bent downwind, rain / sleet / snow particles anchored to the
//    world, splashes, lightning flashes.
// The window (window2d.cpp) says what casts and what shines; this draws it.
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

namespace game2d {

struct SkyNow {
    double sunAlt = 45, shadowX = 0, shadowY = 0, sunShadow = 0;
    double r = 1, g = 1, b = 1, lum = 1, lights = 0;
    double cloud = 0, intensity = 0, thunder = 0, fog = 0;
    double windX = 1, windY = 0, wind = 0;
    double snow = 0, wet = 0;
    std::string precip;   // "" | rain | sleet | snow
};

class Atmos {
public:
    using Json = nlohmann::json;

    void attach(SDL_Renderer* r);
    void release();                      // the textures (a device reset or the end)
    void configure(const Json& sky2d);   // content/sprites_2d/sky2d.json

    // The light a thing type gives (null: none) and who smokes; shadow heights (m).
    const Json* lightOf(const std::string& type) const;
    bool smokes(const std::string& type) const;
    double heightOf(const std::string& kind, double fallback) const;

    // Once a frame: the two skies of the state, how far between them, the real time.
    void update(const Json& sky, const Json& next, double frac, double dtSec, double nowSec);
    const SkyNow& sky() const { return now_; }
    bool enabled = true;                 // off in the symbol mode

    // Once a frame before drawing: the map area and the world -> screen mapping.
    void frame(float mapW, float mapH, double camX, double camY, double tilePx, float px);

    // --- shadows -------------------------------------------------------------------
    void beginShadows();
    // A sprite's silhouette sheared from its foot line (footY, screen): its top moves
    // by (ox, oy) screen px. `direct` with alpha draws straight to the screen (fires).
    void shadowSprite(SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst, float footY, float ox, float oy);
    // The footprint of a box (world rect x0,y0 - x1,y1) of `heightM` swept along the sun.
    void shadowBox(double x0, double y0, double x1, double y1, double heightM);
    void endShadows();                   // the buffer onto the ground
    void fireShadow(SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst, float footY, float ox, float oy, float alpha);

    // --- ground and air (lit by the light map after) ------------------------------
    void drawGround();                   // wet darkening, snow lying
    void snowOn(float x, float y, float w, float h);   // snow on a roof (screen rect)
    // kind: 0 smoke, 1 steam (white, quick), 2 a fire's thick smoke
    void addSmoke(double wx, double wy, std::uint64_t key, double strength, int kind = 0);
    void drawAir();                      // smoke, fog

    // --- light ---------------------------------------------------------------------
    void addLight(double wx, double wy, double radiusM, const double rgb[3], double intensity, double flicker, std::uint64_t key);
    void drawLight();                    // the light map over the map
    void drawPrecip();                   // rain, sleet, snow, splashes, lightning (after the light)
    // A small flame that shines by itself (after the light): a torch in hand, a lit window.
    void glow(float x, float y, float w, float h, double r, double g, double b, double a);
    double flick(std::uint64_t key, double amount) const;   // 1 +- amount, the flame's flutter
    double timeSec() const { return t_; }

private:
    struct Light {
        float x, y, r;   // screen
        float cr, cg, cb, a;
    };
    struct Smoke {
        double x, y, s;
        std::uint64_t key;
        int kind;
    };
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* lightMap_ = nullptr;
    SDL_Texture* shadowBuf_ = nullptr;
    SDL_Texture* disc_ = nullptr;
    SDL_Texture* noise_ = nullptr;
    SDL_Texture* puff_ = nullptr;
    int lmW_ = 0, lmH_ = 0, sbW_ = 0, sbH_ = 0;
    Json cfg_ = Json::object();
    SkyNow now_, target_;
    bool haveSky_ = false;
    double t_ = 0, flashUntil_ = 0, nextFlash_ = 0;
    float mapW_ = 0, mapH_ = 0, px_ = 1;
    double camX_ = 0, camY_ = 0, tile_ = 16;
    bool shadowsOpen_ = false;
    std::vector<Light> lights_;
    std::vector<Smoke> smoke_;
    SDL_Texture* prevTarget_ = nullptr;

    void toScreen(double wx, double wy, float& sx, float& sy) const {
        sx = float((wx - camX_) * tile_ + mapW_ / 2);
        sy = float((wy - camY_) * tile_ + mapH_ / 2);
    }
    void ensure();
    void tiled(SDL_Texture* tex, double worldSize, double offX, double offY, float r, float g, float b, float a);
};

} // namespace game2d
