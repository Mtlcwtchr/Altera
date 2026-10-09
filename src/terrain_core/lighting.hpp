#pragma once
// Light for the terrain core: the sun and sky by time of day, height fog,
// cascaded sun shadows, wind and the cloud layer's parameters - everything
// assets/shaders/terrain_core/lighting.hlsli reads.

#include "engine/render/device.hpp"
#include "terrain_core/lod_select.hpp"

#include <array>
#include <cstdint>

namespace terrain_core {

// Must match cbuffer Light in assets/shaders/terrain_core/lighting.hlsli.
struct LightUniforms {
    float sun[3], sunAbove;
    float sunColour[3], time;
    float zenith[3], fogDensity;
    float horizon[3], fogFalloff;
    float ground[3], eyeZ;
    float eyeXY[2], cloudCover, cloudBase;
    float cloudTop, shadows, cloudShadows, wind;
    float windDirection[2], cloudDrift[2];
    float shadowMatrix[3][16];
    float cascadeFar[4];
};
static_assert(sizeof(LightUniforms) == 336);

struct LightSettings {
    double timeOfDay = 15.5;      // hours
    double cloudCover = 0.45;     // 0 clear .. 1 overcast
    double cloudBase = 4600, cloudTop = 7200;   // metres above sea level: over the peaks (5.2 km)
    double wind = 0.55;           // 0 calm .. 1 gale
    double windAngle = 0.7;       // radians, where the wind blows to
    bool shadows = true, clouds = true, cloudShadows = true, windOn = true;
    double fog = 1.0;             // multiplies the fog density
};

// What a plant's vertex stage reads: the matrix (the eye's, or a shadow
// cascade's) and the wind. Must match cbuffer View in foliage_draw.hlsl and
// grass_draw.hlsl.
struct PlantView {
    float viewProjection[16];
    float wind[4];          // direction x, y; strength; seconds
    float eyeWrapped[4];    // the eye's x, y modulo 4096 m
};

// The sun's direction and the sky's colours for a time of day, the eye, and
// the shadow cascades' matrices.
class Lighting {
public:
    static constexpr std::uint32_t kCascades = 3;
    static constexpr std::uint32_t kCascadeSize = 2048;   // texels a side of one cascade

    // `camera`: forward and up unit vectors, vertical field of view, aspect.
    void update(const LightSettings& settings, const Vec3d& eye, const double forward[3], double fovY,
                double aspect, double seconds);
    const LightUniforms& uniforms() const { return u_; }
    const float* cascadeMatrix(std::uint32_t c) const { return u_.shadowMatrix[c]; }

private:
    LightUniforms u_{};
};

// The shadow atlas: the cascades side by side in one depth texture.
class ShadowAtlas {
public:
    bool open(engine::Device& device);
    SDL_GPUTexture* texture() const { return atlas_.get(); }
    SDL_GPUSampler* sampler() const { return sampler_.get(); }
    // Starts the depth-only pass that renders every cascade (the caller sets
    // each cascade's viewport with `viewport` and draws).
    SDL_GPURenderPass* begin(SDL_GPUCommandBuffer* commands);
    void viewport(SDL_GPURenderPass* pass, std::uint32_t cascade);

private:
    engine::Texture atlas_;
    engine::Sampler sampler_;
};

} // namespace terrain_core
