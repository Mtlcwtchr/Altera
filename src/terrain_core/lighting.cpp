#include "terrain_core/lighting.hpp"

#include <algorithm>
#include <cmath>

namespace terrain_core {
namespace {

constexpr double kPi = 3.14159265358979;

void set3(float out[3], double r, double g, double b) {
    out[0] = float(r);
    out[1] = float(g);
    out[2] = float(b);
}

double mix(double a, double b, double t) { return a + (b - a) * t; }

void mix3(float out[3], const double a[3], const double b[3], double t) {
    for (int i = 0; i < 3; ++i) out[i] = float(mix(a[i], b[i], t));
}

} // namespace

void Lighting::update(const LightSettings& s, const Vec3d& eye, const double forward[3], double fovY, double aspect,
                      double seconds) {
    // The sun: rises in the east at six, highest at one, sets in the west at
    // twenty; a tilt so it never stands overhead.
    const double day = (s.timeOfDay - 6.5) / 13.5;               // 0 sunrise .. 1 sunset
    const double elevation = std::sin(std::clamp(day, -0.15, 1.15) * kPi) * 0.95;   // sine of it
    const double azimuth = kPi * (0.15 + day * 0.7);              // east -> south -> west
    const double horizontal = std::sqrt(std::max(0.0, 1.0 - elevation * elevation));
    const double sun[3] = {std::cos(azimuth) * horizontal * -1.0, -std::sin(azimuth) * horizontal, elevation};
    set3(u_.sun, sun[0], sun[1], sun[2]);
    u_.sunAbove = float(std::clamp(elevation * 8.0 + 0.3, 0.0, 1.0));

    // Colours: noon, golden hour and dusk, by how high the sun stands.
    const double high = std::clamp(elevation / 0.6, 0.0, 1.0);    // 1 by mid-morning
    const double low = 1.0 - high;
    const double noonSun[3] = {1.10, 1.05, 0.94}, lowSun[3] = {1.25, 0.72, 0.38};
    const double noonZenith[3] = {0.36, 0.52, 0.78}, lowZenith[3] = {0.24, 0.30, 0.50};
    const double noonHorizon[3] = {0.66, 0.74, 0.84}, lowHorizon[3] = {0.95, 0.62, 0.42};
    double sunColour[3];
    for (int i = 0; i < 3; ++i) sunColour[i] = mix(noonSun[i], lowSun[i], low * low) * std::clamp(elevation * 6.0, 0.0, 1.0);
    set3(u_.sunColour, sunColour[0], sunColour[1], sunColour[2]);
    mix3(u_.zenith, noonZenith, lowZenith, low);
    mix3(u_.horizon, noonHorizon, lowHorizon, low * low);
    // Night: the sky dims, but never to black (a lab, not a horror game).
    const double night = std::clamp(-elevation * 6.0, 0.0, 1.0);
    for (int i = 0; i < 3; ++i) {
        u_.zenith[i] = float(mix(u_.zenith[i], 0.05, night));
        u_.horizon[i] = float(mix(u_.horizon[i], 0.08, night));
    }
    set3(u_.ground, 0.30 * (1 - night * 0.8), 0.27 * (1 - night * 0.8), 0.22 * (1 - night * 0.8));
    // Overcast: less sun, a greyer sky.
    const double overcast = std::clamp((s.cloudCover - 0.5) * 2.0, 0.0, 1.0) * (s.clouds ? 1.0 : 0.0);
    for (int i = 0; i < 3; ++i) {
        u_.sunColour[i] *= float(1.0 - overcast * 0.6);
        u_.zenith[i] = float(mix(u_.zenith[i], 0.55, overcast * 0.6));
        u_.horizon[i] = float(mix(u_.horizon[i], 0.62, overcast * 0.5));
    }

    u_.time = float(std::fmod(seconds, 100000.0));
    u_.fogDensity = float(0.000045 * s.fog * (1.0 + overcast));
    u_.fogFalloff = 1.0f / 1600.0f;
    u_.eyeZ = float(eye.z);
    u_.eyeXY[0] = float(eye.x);
    u_.eyeXY[1] = float(eye.y);
    u_.cloudCover = s.clouds ? float(s.cloudCover) : 0.0f;
    u_.cloudBase = float(s.cloudBase);
    u_.cloudTop = float(s.cloudTop);
    u_.shadows = s.shadows ? 1.0f : 0.0f;
    u_.cloudShadows = s.clouds && s.cloudShadows ? 1.0f : 0.0f;
    u_.wind = s.windOn ? float(s.wind) : 0.0f;
    u_.windDirection[0] = float(std::cos(s.windAngle));
    u_.windDirection[1] = float(std::sin(s.windAngle));
    // Clouds drift with the wind, a few times faster than it blows at the ground.
    u_.cloudDrift[0] = float(std::fmod(std::cos(s.windAngle) * seconds * (6.0 + 14.0 * s.wind), 90000.0));
    u_.cloudDrift[1] = float(std::fmod(std::sin(s.windAngle) * seconds * (6.0 + 14.0 * s.wind), 90000.0));

    // Cascades: spheres round slices of the view, each a square ortho tile
    // looking along the sun. The tile's centre is snapped to its own texels in
    // light space, from the eye's absolute position, so the shadows do not
    // crawl as the eye moves.
    const double splits[kCascades + 1] = {0.0, 45.0, 260.0, 1600.0};
    double toLight[3] = {sun[0], sun[1], sun[2]};
    if (toLight[2] < 0.12) {   // a sun on the horizon makes endless shadows; hold it up a little
        toLight[2] = 0.12;
        const double n = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
        for (double& v : toLight) v /= n;
    }
    const double down[3] = {-toLight[0], -toLight[1], -toLight[2]};   // light travels this way
    double right[3] = {down[1], -down[0], 0.0};
    double rl = std::hypot(right[0], right[1]);
    if (rl < 1e-6) { right[0] = 1; right[1] = 0; rl = 1; }
    right[0] /= rl;
    right[1] /= rl;
    const double up[3] = {right[1] * down[2] - right[2] * down[1], right[2] * down[0] - right[0] * down[2],
                          right[0] * down[1] - right[1] * down[0]};
    const double tanY = std::tan(fovY * 0.5), tanX = tanY * aspect;
    const double diagonal = std::sqrt(1.0 + tanX * tanX + tanY * tanY);
    for (std::uint32_t c = 0; c < kCascades; ++c) {
        const double n = splits[c], f = splits[c + 1];
        const double mid = (n + f) * 0.5;
        const double radius = std::max(f * diagonal - mid, (f - n) * 0.5) * 1.05 + 2.0;
        double centre[3] = {forward[0] * mid, forward[1] * mid, forward[2] * mid};
        // Snap in light space, from absolute coordinates.
        const double texel = 2.0 * radius / kCascadeSize;
        const double abs[3] = {eye.x + centre[0], eye.y + centre[1], eye.z + centre[2]};
        const double ar = abs[0] * right[0] + abs[1] * right[1] + abs[2] * right[2];
        const double au = abs[0] * up[0] + abs[1] * up[1] + abs[2] * up[2];
        const double sr = std::round(ar / texel) * texel - ar, su = std::round(au / texel) * texel - au;
        for (int i = 0; i < 3; ++i) centre[i] += right[i] * sr + up[i] * su;
        const double depth = radius + 6000.0;   // mountains well beyond the slice still cast into it
        float* m = u_.shadowMatrix[c];
        for (int i = 0; i < 3; ++i) {
            m[0 + i] = float(right[i] / radius);
            m[4 + i] = float(up[i] / radius);
            m[8 + i] = float(-down[i] / (2.0 * depth));
            m[12 + i] = 0.0f;
        }
        m[3] = float(-(centre[0] * right[0] + centre[1] * right[1] + centre[2] * right[2]) / radius);
        m[7] = float(-(centre[0] * up[0] + centre[1] * up[1] + centre[2] * up[2]) / radius);
        m[11] = float(0.5 + (centre[0] * down[0] + centre[1] * down[1] + centre[2] * down[2]) / (2.0 * depth));
        m[15] = 1.0f;
        u_.cascadeFar[c] = float(f);
    }
    u_.cascadeFar[3] = 1.0f / float(kCascadeSize * kCascades);
}

bool ShadowAtlas::open(engine::Device& device) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = engine::Device::kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = Lighting::kCascadeSize * Lighting::kCascades;
    info.height = Lighting::kCascadeSize;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    atlas_ = device.makeTexture(info);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampling);
    return atlas_ && sampler_;
}

SDL_GPURenderPass* ShadowAtlas::begin(SDL_GPUCommandBuffer* commands) {
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = atlas_.get();
    depth.clear_depth = 0.0f;   // reversed: nothing is nearer the sun than the far end
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    return SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
}

void ShadowAtlas::viewport(SDL_GPURenderPass* pass, std::uint32_t cascade) {
    const SDL_GPUViewport v{float(cascade * Lighting::kCascadeSize), 0.0f, float(Lighting::kCascadeSize),
                            float(Lighting::kCascadeSize), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &v);
    const SDL_Rect scissor{int(cascade * Lighting::kCascadeSize), 0, int(Lighting::kCascadeSize),
                           int(Lighting::kCascadeSize)};
    SDL_SetGPUScissor(pass, &scissor);
}

} // namespace terrain_core
