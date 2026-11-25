// AL-110: light, shadows and weather of the 2D window (see atmos2d.hpp).
#include "native/atmos2d.hpp"

#include <algorithm>
#include <cmath>

namespace game2d {

namespace {


std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
double h01(std::uint64_t a, std::uint64_t b = 0) { return double(mix(a * 0x9e3779b97f4a7c15ULL + b) >> 11) * (1.0 / 9007199254740992.0); }
double num(const nlohmann::json& j, const char* k, double d) {
    if (!j.is_object()) return d;
    const auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : d;
}
double fracPos(double x) { return x - std::floor(x); }
double wrap(double v, double lo, double span) { return lo + (v - lo) - std::floor((v - lo) / span) * span; }

SDL_Texture* makeTexture(SDL_Renderer* r, int n, const std::vector<std::uint32_t>& px, SDL_BlendMode mode) {
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, n, n);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, px.data(), n * 4);
    SDL_SetTextureBlendMode(t, mode);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

} // namespace

void Atmos::attach(SDL_Renderer* r) {
    release();
    ren_ = r;
}

void Atmos::release() {
    for (SDL_Texture** t : {&lightMap_, &shadowBuf_, &disc_, &noise_, &puff_})
        if (*t) SDL_DestroyTexture(*t), *t = nullptr;
    lmW_ = lmH_ = sbW_ = sbH_ = 0;
}

void Atmos::configure(const Json& sky2d) { cfg_ = sky2d.is_object() ? sky2d : Json::object(); }

const nlohmann::json* Atmos::lightOf(const std::string& type) const {
    const auto l = cfg_.find("lights");
    if (l == cfg_.end() || !l->is_object()) return nullptr;
    const auto it = l->find(type);
    return it == l->end() ? nullptr : &*it;
}

bool Atmos::smokes(const std::string& type) const {
    const auto s = cfg_.find("smoke");
    if (s == cfg_.end() || !s->is_array()) return false;
    for (const auto& v : *s)
        if (v.is_string() && v.get<std::string>() == type) return true;
    return false;
}

double Atmos::heightOf(const std::string& kind, double fallback) const {
    const auto s = cfg_.find("shadow");
    return s == cfg_.end() ? fallback : num(*s, kind.c_str(), fallback);
}

// --- the sky of the hour ---------------------------------------------------------------
void Atmos::update(const Json& sky, const Json& next, double frac, double dtSec, double nowSec) {
    t_ = nowSec;
    if (!sky.is_object()) return;
    auto read = [](const Json& j, SkyNow& s) {
        const Json sun = j.value("sun", Json::object());
        s.sunAlt = num(sun, "alt", 45), s.shadowX = num(sun, "sx", 0), s.shadowY = num(sun, "sy", 0), s.sunShadow = num(sun, "shadow", 0);
        const Json l = j.value("light", Json::array());
        if (l.is_array() && l.size() >= 3) s.r = l[0].get<double>(), s.g = l[1].get<double>(), s.b = l[2].get<double>();
        s.lum = num(j, "lum", 1), s.lights = num(j, "lights", 0), s.cloud = num(j, "cloud", 0), s.intensity = num(j, "intensity", 0);
        s.thunder = num(j, "thunder", 0), s.fog = num(j, "fog", 0), s.snow = num(j, "snow", 0), s.wet = num(j, "wet", 0);
        const Json w = j.value("wind", Json::object());
        s.windX = num(w, "x", 1), s.windY = num(w, "y", 0), s.wind = num(w, "ms", 0);
        s.precip = j.value("precip", std::string());
    };
    SkyNow a, b;
    read(sky, a);
    read(next.is_object() ? next : sky, b);
    const double f = std::clamp(frac, 0.0, 1.0);
    auto L = [f](double x, double y) { return x + (y - x) * f; };
    SkyNow t = a;
    t.sunAlt = L(a.sunAlt, b.sunAlt), t.shadowX = L(a.shadowX, b.shadowX), t.shadowY = L(a.shadowY, b.shadowY);
    t.sunShadow = L(a.sunShadow, b.sunShadow), t.r = L(a.r, b.r), t.g = L(a.g, b.g), t.b = L(a.b, b.b), t.lum = L(a.lum, b.lum);
    t.lights = L(a.lights, b.lights), t.cloud = L(a.cloud, b.cloud), t.intensity = L(a.intensity, b.intensity);
    t.thunder = L(a.thunder, b.thunder), t.fog = L(a.fog, b.fog), t.snow = L(a.snow, b.snow), t.wet = L(a.wet, b.wet);
    t.windX = L(a.windX, b.windX), t.windY = L(a.windY, b.windY), t.wind = L(a.wind, b.wind);
    target_ = t;
    if (!haveSky_) {
        now_ = target_;
        haveSky_ = true;
    }
    // eased: a jump of the clock (a load, a fast-forward) fades over about a second
    const double k = std::clamp(dtSec / 0.6, 0.0, 1.0);
    auto E = [k](double& v, double to) { v += (to - v) * k; };
    E(now_.sunAlt, t.sunAlt), E(now_.shadowX, t.shadowX), E(now_.shadowY, t.shadowY), E(now_.sunShadow, t.sunShadow);
    E(now_.r, t.r), E(now_.g, t.g), E(now_.b, t.b), E(now_.lum, t.lum), E(now_.lights, t.lights), E(now_.cloud, t.cloud);
    E(now_.thunder, t.thunder), E(now_.fog, t.fog), E(now_.snow, t.snow), E(now_.wet, t.wet);
    E(now_.windX, t.windX), E(now_.windY, t.windY), E(now_.wind, t.wind);
    // the rain starts and stops by its strength, not by a switch
    if (t.precip.empty()) {
        E(now_.intensity, 0.0);
        if (now_.intensity < 0.02) now_.precip.clear();
    } else {
        if (now_.precip != t.precip && now_.intensity > 0.1) E(now_.intensity, 0.0);
        else now_.precip = t.precip, E(now_.intensity, t.intensity);
    }
    const double wl = std::hypot(now_.windX, now_.windY);
    if (wl > 1e-6) now_.windX /= wl, now_.windY /= wl;
    // lightning: a flash every few seconds in a storm
    if (now_.thunder > 0.05 && now_.precip == "rain") {
        if (nextFlash_ <= 0) nextFlash_ = t_ + 2 + 6 * h01(std::uint64_t(t_ * 10));
        if (t_ >= nextFlash_) {
            flashUntil_ = t_ + 0.09 + 0.12 * h01(std::uint64_t(t_ * 1000), 3);
            nextFlash_ = t_ + (3 + 14 * h01(std::uint64_t(t_ * 1000), 7)) / std::max(0.2, now_.thunder);
        }
    } else nextFlash_ = 0;
}

void Atmos::frame(float mapW, float mapH, double camX, double camY, double tilePx, float px) {
    mapW_ = mapW, mapH_ = mapH, camX_ = camX, camY_ = camY, tile_ = tilePx, px_ = px;
    lights_.clear();
    smoke_.clear();
    ensure();
}

void Atmos::ensure() {
    if (!ren_) return;
    if (!disc_) {   // a soft disc: light and smoke
        const int n = 64;
        std::vector<std::uint32_t> px(std::size_t(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dx = (x + 0.5) / n * 2 - 1, dy = (y + 0.5) / n * 2 - 1, d2 = dx * dx + dy * dy;
                const double a = d2 >= 1 ? 0 : std::pow(1 - d2, 2.2);
                px[std::size_t(y) * n + x] = std::uint32_t(std::lround(a * 255)) << 24 | 0xffffffu;
            }
        disc_ = makeTexture(ren_, n, px, SDL_BLENDMODE_ADD);
        puff_ = makeTexture(ren_, n, px, SDL_BLENDMODE_BLEND);
    }
    if (!noise_) {   // tileable value noise, three octaves: fog banks, cloud shadows, snow patches
        const int n = 128;
        std::vector<double> v(std::size_t(n) * n, 0);
        double norm = 0;
        for (int o = 0; o < 3; ++o) {
            const int g = 4 << o;   // grid cells over the tile
            const double amp = 1.0 / (1 << o);
            norm += amp;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const double fx = double(x) * g / n, fy = double(y) * g / n;
                    const int ix = int(fx), iy = int(fy);
                    double tx = fx - ix, ty = fy - iy;
                    tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
                    auto at = [&](int a, int b) { return h01(std::uint64_t((a % g) + g * (b % g)), 1000 + o); };
                    const double a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
                    v[std::size_t(y) * n + x] += amp * (a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty);
                }
        }
        std::vector<std::uint32_t> px(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double s = std::clamp((v[i] / norm - 0.35) / 0.4, 0.0, 1.0);   // banks with gaps between
            px[i] = std::uint32_t(std::lround(s * s * (3 - 2 * s) * 255)) << 24 | 0xffffffu;
        }
        noise_ = makeTexture(ren_, n, px, SDL_BLENDMODE_BLEND);
    }
    const int lw = std::max(1, int(mapW_ / 2)), lh = std::max(1, int(mapH_ / 2));
    if (!lightMap_ || lw != lmW_ || lh != lmH_) {
        if (lightMap_) SDL_DestroyTexture(lightMap_);
        lightMap_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, lw, lh);
        if (lightMap_) {
            SDL_SetTextureBlendMode(lightMap_, SDL_BLENDMODE_MOD);
            SDL_SetTextureScaleMode(lightMap_, SDL_SCALEMODE_LINEAR);
        }
        lmW_ = lw, lmH_ = lh;
    }
    const int sw = std::max(1, int(mapW_)), sh = std::max(1, int(mapH_));
    if (!shadowBuf_ || sw != sbW_ || sh != sbH_) {
        if (shadowBuf_) SDL_DestroyTexture(shadowBuf_);
        shadowBuf_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, sw, sh);
        if (shadowBuf_) {
            SDL_SetTextureBlendMode(shadowBuf_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(shadowBuf_, SDL_SCALEMODE_NEAREST);
        }
        sbW_ = sw, sbH_ = sh;
    }
}

double Atmos::flick(std::uint64_t key, double amount) const {
    const double k = double(key % 997);
    return 1.0 + amount * 0.5 * (0.6 * std::sin(t_ * 9.1 + k) + 0.4 * std::sin(t_ * 23.7 + k * 1.37));
}

// --- shadows ---------------------------------------------------------------------------
void Atmos::beginShadows() {
    shadowsOpen_ = false;
    if (!enabled || !shadowBuf_ || now_.sunShadow < 0.02) return;
    prevTarget_ = SDL_GetRenderTarget(ren_);
    if (!SDL_SetRenderTarget(ren_, shadowBuf_)) return;
    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
    SDL_RenderClear(ren_);
    shadowsOpen_ = true;
}

namespace {
void shearQuad(SDL_Renderer* ren, SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst, float footY, float ox, float oy, float alpha) {
    float tw = 1, th = 1;
    SDL_GetTextureSize(tex, &tw, &th);
    const float vFoot = std::clamp((footY - dst.y) / dst.h, 0.0f, 1.0f);
    const float u0 = src.x / tw, u1 = (src.x + src.w) / tw, v0 = src.y / th, v1 = (src.y + src.h * vFoot) / th;
    const SDL_FColor c{0, 0, 0, alpha};
    SDL_Vertex v[4];
    v[0] = {{dst.x + ox, footY + oy}, c, {u0, v0}};
    v[1] = {{dst.x + dst.w + ox, footY + oy}, c, {u1, v0}};
    v[2] = {{dst.x + dst.w, footY}, c, {u1, v1}};
    v[3] = {{dst.x, footY}, c, {u0, v1}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(ren, tex, v, 4, idx, 6);
}
} // namespace

void Atmos::shadowSprite(SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst, float footY, float ox, float oy) {
    if (!shadowsOpen_ || !tex) return;
    shearQuad(ren_, tex, src, dst, footY, ox, oy, 1.0f);
}

void Atmos::fireShadow(SDL_Texture* tex, const SDL_FRect& src, const SDL_FRect& dst, float footY, float ox, float oy, float alpha) {
    if (!enabled || !tex || alpha <= 0.01f) return;
    shearQuad(ren_, tex, src, dst, footY, ox, oy, alpha);
}

void Atmos::shadowBox(double x0, double y0, double x1, double y1, double heightM) {
    if (!shadowsOpen_) return;
    float ax, ay, bx, by;
    toScreen(x0, y0, ax, ay);
    toScreen(x1, y1, bx, by);
    const float ox = float(now_.shadowX * heightM * tile_), oy = float(now_.shadowY * heightM * tile_);
    const SDL_FColor c{0, 0, 0, 1};
    // the box's footprint and its copy at the shadow's end, joined by its four edges swept along
    const SDL_FPoint p[4] = {{ax, ay}, {bx, ay}, {bx, by}, {ax, by}};
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    auto quad = [&](SDL_FPoint a, SDL_FPoint b, SDL_FPoint cc, SDL_FPoint d) {
        const int base = int(v.size());
        for (const SDL_FPoint& q : {a, b, cc, d}) v.push_back({q, c, {0, 0}});
        for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(base + k);
    };
    auto sh = [&](SDL_FPoint q) { return SDL_FPoint{q.x + ox, q.y + oy}; };
    quad(sh(p[0]), sh(p[1]), sh(p[2]), sh(p[3]));
    for (int e = 0; e < 4; ++e) quad(p[e], p[(e + 1) % 4], sh(p[(e + 1) % 4]), sh(p[e]));
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren_, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void Atmos::endShadows() {
    if (!shadowsOpen_) return;
    shadowsOpen_ = false;
    SDL_SetRenderTarget(ren_, prevTarget_);
    SDL_SetTextureAlphaModFloat(shadowBuf_, float(std::clamp(now_.sunShadow * 0.42, 0.0, 0.6)));
    const SDL_FRect d{0, 0, float(sbW_), float(sbH_)};
    SDL_RenderTexture(ren_, shadowBuf_, nullptr, &d);
}

// --- ground and air ------------------------------------------------------------------
void Atmos::tiled(SDL_Texture* tex, double worldSize, double offX, double offY, float r, float g, float b, float a) {
    if (!tex || a <= 0.003f) return;
    const double S = worldSize * tile_;
    if (S < 4) return;
    double wx0 = camX_ - mapW_ / 2 / tile_, wy0 = camY_ - mapH_ / 2 / tile_;
    const double gx = std::floor((wx0 + offX) / worldSize) * worldSize - offX, gy = std::floor((wy0 + offY) / worldSize) * worldSize - offY;
    float sx0, sy0;
    toScreen(gx, gy, sx0, sy0);
    SDL_SetTextureColorModFloat(tex, r, g, b);
    SDL_SetTextureAlphaModFloat(tex, a);
    // one call, the texture repeated (wrapped sampling: no seams between the tiles)
    float tw = 1, th = 1;
    SDL_GetTextureSize(tex, &tw, &th);
    const SDL_FRect d{sx0, sy0, mapW_ - sx0 + float(S), mapH_ - sy0 + float(S)};
    SDL_RenderTextureTiled(ren_, tex, nullptr, float(S) / tw, &d);
    SDL_SetTextureColorModFloat(tex, 1, 1, 1);
    SDL_SetTextureAlphaModFloat(tex, 1);
}

void Atmos::drawGround() {
    if (!enabled) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    const SDL_FRect all{0, 0, mapW_, mapH_};
    if (now_.wet > 0.02) {   // wet earth goes dark and cold
        SDL_SetRenderDrawColorFloat(ren_, 0.04f, 0.07f, 0.13f, float(now_.wet * 0.2));
        SDL_RenderFillRect(ren_, &all);
    }
    if (now_.snow > 0.02) {   // snow: a white veil, patches of it where it lies thin
        SDL_SetRenderDrawColorFloat(ren_, 0.93f, 0.95f, 1.0f, float(std::clamp(now_.snow * 1.1 - 0.15, 0.0, 0.72)));
        SDL_RenderFillRect(ren_, &all);
        tiled(noise_, 6.0, 3.0, 7.0, 0.96f, 0.97f, 1.0f, float(std::min(1.0, now_.snow * 1.6) * 0.55));
    }
}

void Atmos::snowOn(float x, float y, float w, float h) {
    if (!enabled || now_.snow < 0.05) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(ren_, 0.94f, 0.96f, 1.0f, float(std::min(0.8, now_.snow)));
    const SDL_FRect r{x, y, w, h};
    SDL_RenderFillRect(ren_, &r);
}

void Atmos::addSmoke(double wx, double wy, std::uint64_t key, double strength, int kind) {
    if (enabled && strength > 0.01) smoke_.push_back({wx, wy, strength, key, kind});
}

void Atmos::drawAir() {
    if (!enabled) return;
    // smoke: puffs rising and leaning downwind, more lean in more wind
    if (puff_ && !smoke_.empty()) {
        const double lean = 0.6 + now_.wind * 0.35;
        for (const Smoke& s : smoke_) {
            const bool steam = s.kind == 1, thick = s.kind == 2;
            const int puffs = thick ? 12 : 7;
            for (int k = 0; k < puffs; ++k) {
                const double p = fracPos(t_ / (steam ? 3.5 : 7.0) + h01(s.key, k));
                const double wx = s.x + now_.windX * p * lean * 4.0 + std::sin(t_ * 0.7 + k) * 0.25 * p;
                const double wy = s.y + now_.windY * p * lean * 4.0 - p * 2.2;
                float sx, sy;
                toScreen(wx, wy, sx, sy);
                const float r = float((0.35 + p * (thick ? 2.6 : 1.5)) * tile_);
                const float a = float(std::min(1.0, s.s) * (thick ? 0.55 : 0.42) * (1 - p) * std::min(1.0, p * 6.0) * (1.0 - 0.6 * now_.intensity));
                const float g = steam ? 0.92f : thick ? float(0.28 + 0.3 * p) : float(0.55 + 0.25 * p);
                SDL_SetTextureColorModFloat(puff_, g, g, g * 1.02f);
                SDL_SetTextureAlphaModFloat(puff_, a);
                const SDL_FRect d{sx - r, sy - r, 2 * r, 2 * r};
                SDL_RenderTexture(ren_, puff_, nullptr, &d);
            }
        }
        SDL_SetTextureColorModFloat(puff_, 1, 1, 1);
        SDL_SetTextureAlphaModFloat(puff_, 1);
    }
    // cloud shadows on a broken-cloud day, drifting with the wind
    const double broken = 4.0 * now_.cloud * (1.0 - now_.cloud);
    const double drift = t_ * (1.0 + now_.wind * 0.6);
    if (now_.sunAlt > 0) {
        const float a = float(std::clamp(broken * 0.34 * std::min(1.0, now_.sunAlt / 10.0) * (1 - now_.fog), 0.0, 0.32));
        tiled(noise_, 90.0, -now_.windX * drift, -now_.windY * drift, 0.05f, 0.06f, 0.1f, a);
    }
    // fog: a veil and two banks moving at different speeds
    if (now_.fog > 0.02) {
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColorFloat(ren_, 0.82f, 0.85f, 0.88f, float(now_.fog * 0.32));
        const SDL_FRect all{0, 0, mapW_, mapH_};
        SDL_RenderFillRect(ren_, &all);
        const double f = 0.25 + now_.wind * 0.15;
        tiled(noise_, 38.0, -now_.windX * t_ * f, -now_.windY * t_ * f, 0.86f, 0.88f, 0.9f, float(now_.fog * 0.75));
        tiled(noise_, 15.0, 17.0 - now_.windX * t_ * f * 1.7, 5.0 - now_.windY * t_ * f * 1.7, 0.9f, 0.92f, 0.94f, float(now_.fog * 0.45));
    }
}

// --- light ---------------------------------------------------------------------------
void Atmos::addLight(double wx, double wy, double radiusM, const double rgb[3], double intensity, double flicker, std::uint64_t key) {
    if (!enabled) return;
    float sx, sy;
    toScreen(wx, wy, sx, sy);
    const double fl = flick(key, flicker);
    const float r = float(radiusM * tile_ * (0.94 + 0.06 * fl));
    if (sx + r < 0 || sy + r < 0 || sx - r > mapW_ || sy - r > mapH_) return;
    const double a = std::clamp(intensity * fl * (0.15 + 0.85 * now_.lights), 0.0, 1.0);
    if (a < 0.01) return;
    lights_.push_back({sx, sy, r, float(rgb[0]), float(rgb[1]), float(rgb[2]), float(a)});
}

void Atmos::drawLight() {
    if (!enabled || !lightMap_ || !disc_) return;
    const bool flash = t_ < flashUntil_;
    if (now_.lum > 0.985 && lights_.empty() && !flash) return;   // plain day: nothing to multiply
    SDL_Texture* was = SDL_GetRenderTarget(ren_);
    if (!SDL_SetRenderTarget(ren_, lightMap_)) return;
    double r = now_.r, g = now_.g, b = now_.b;
    if (flash) r = g = b = 1.0;
    SDL_SetRenderDrawColorFloat(ren_, float(r), float(g), float(b), 1);
    SDL_RenderClear(ren_);
    const float k = float(lmW_) / std::max(1.0f, mapW_);
    for (const Light& l : lights_) {
        // a hot core and a wide warm halo
        SDL_SetTextureColorModFloat(disc_, l.cr, l.cg, l.cb);
        SDL_SetTextureAlphaModFloat(disc_, l.a);
        const SDL_FRect d{(l.x - l.r) * k, (l.y - l.r) * k, 2 * l.r * k, 2 * l.r * k};
        SDL_RenderTexture(ren_, disc_, nullptr, &d);
        SDL_SetTextureAlphaModFloat(disc_, l.a * 0.6f);
        const float c = l.r * 0.35f;
        const SDL_FRect d2{(l.x - c) * k, (l.y - c) * k, 2 * c * k, 2 * c * k};
        SDL_RenderTexture(ren_, disc_, nullptr, &d2);
    }
    SDL_SetTextureColorModFloat(disc_, 1, 1, 1);
    SDL_SetTextureAlphaModFloat(disc_, 1);
    SDL_SetRenderTarget(ren_, was);
    const SDL_FRect all{0, 0, mapW_, mapH_};
    SDL_RenderTexture(ren_, lightMap_, nullptr, &all);
}

void Atmos::glow(float x, float y, float w, float h, double r, double g, double b, double a) {
    if (!enabled || a < 0.02) return;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(ren_, float(r), float(g), float(b), float(std::min(1.0, a)));
    const SDL_FRect d{x, y, w, h};
    SDL_RenderFillRect(ren_, &d);
}

void Atmos::drawPrecip() {
    if (!enabled) return;
    const double I = now_.intensity;
    const double wxm = mapW_ / tile_ + 4, wym = mapH_ / tile_ + 4;   // the view in metres, a margin round it
    const double left = camX_ - wxm / 2, top = camY_ - wym / 2;
    const double area = double(mapW_) * mapH_ / (1600.0 * 900.0);
    const float dim = float(0.45 + 0.55 * std::clamp(now_.lum * 1.2, 0.0, 1.0));   // rain at night is grey, not white
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    const bool rain = now_.precip == "rain" || now_.precip == "sleet", snow = now_.precip == "snow" || now_.precip == "sleet";
    if (I > 0.01 && rain) {
        const int n = int(std::min(2200.0, I * 900 * area * (now_.precip == "sleet" ? 0.5 : 1.0)));
        const double period = 0.55;
        // the streak: down the screen, leaning with the wind
        double fx = now_.windX * std::min(1.2, now_.wind * 0.09), fy = 1.0;
        const double fl = std::hypot(fx, fy);
        fx /= fl, fy /= fl;
        const float len = float(px_ * (6 + 8 * I));
        for (int i = 0; i < n; ++i) {
            const double ph = t_ / period + h01(i, 1);
            const auto cyc = std::uint64_t(std::floor(ph));
            const double p = ph - std::floor(ph);
            const double wx = wrap(left + h01(i * 7919 + cyc, 2) * wxm, left, wxm);
            const double wy = wrap(top + h01(i * 7919 + cyc, 3) * wym, top, wym);
            float sx, sy;
            toScreen(wx, wy, sx, sy);
            if (p < 0.55) {
                const float q = float(p / 0.55);
                const float ex = sx - float(fx) * len * (1 - q) * 2.2f, ey = sy - float(fy) * len * (1 - q) * 2.2f;
                SDL_SetRenderDrawColorFloat(ren_, 0.72f * dim, 0.78f * dim, 0.9f * dim, 0.5f);
                SDL_RenderLine(ren_, ex - float(fx) * len, ey - float(fy) * len, ex, ey);
            } else if (p < 0.85) {   // the splash
                const float q = float((p - 0.55) / 0.3), r = px_ * (0.8f + 2.2f * q);
                SDL_SetRenderDrawColorFloat(ren_, 0.78f * dim, 0.84f * dim, 0.95f * dim, 0.45f * (1 - q));
                const SDL_FRect d{sx - r, sy - r * 0.5f, 2 * r, r};
                SDL_RenderRect(ren_, &d);
            }
        }
    }
    if (I > 0.01 && snow) {
        const int n = int(std::min(1800.0, I * 650 * area * (now_.precip == "sleet" ? 0.5 : 1.0)));
        const double drift = 0.3 + now_.wind * 0.35;
        for (int i = 0; i < n; ++i) {
            const double sp = 0.7 + 0.6 * h01(i, 4);
            const double wx = wrap(left + h01(i, 5) * wxm + now_.windX * drift * t_ * sp + std::sin(t_ * 1.3 * sp + i) * 0.4, left, wxm);
            const double wy = wrap(top + h01(i, 6) * wym + now_.windY * drift * t_ * sp + t_ * 0.9 * sp, top, wym);
            float sx, sy;
            toScreen(wx, wy, sx, sy);
            const float s = px_ * float(h01(i, 8) < 0.3 ? 1.5 : 1.0);
            SDL_SetRenderDrawColorFloat(ren_, 0.95f * dim, 0.96f * dim, 1.0f * dim, 0.85f);
            const SDL_FRect d{sx, sy, s, s};
            SDL_RenderFillRect(ren_, &d);
        }
    }
    if (t_ < flashUntil_) {   // lightning
        SDL_SetRenderDrawColorFloat(ren_, 0.92f, 0.94f, 1.0f, 0.22f);
        const SDL_FRect all{0, 0, mapW_, mapH_};
        SDL_RenderFillRect(ren_, &all);
    }
}

} // namespace game2d
