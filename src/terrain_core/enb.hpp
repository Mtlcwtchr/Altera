#pragma once
// The ENB-like frame chain (AL-77): the scene in half floats, then light,
// atmosphere, lens and grade before the picture styles
// (assets/shaders/terrain_core/enb*.hlsl):
//   ao         ambient occlusion + contact shadows from the depth (half res)
//   composite  clouds, AO, more height fog, the sun's in-scatter, aerial
//              perspective, a sun halo; into the working space (linear light)
//   bloom      a threshold and a six-level down/up chain (also meters the frame)
//   rays       light shafts from the bright sky round the sun (quarter res)
//   adapt      the eye's exposure, eased over time (one texel)
//   final      exposure, ACES / AgX tone curve, lift-gamma-gain, contrast,
//              saturation, split toning by the time of day, vignette, grain
// Every number comes from content/config/terrain_core/enb.json, by preset:
// "enb" (the default) and "classic" (all of it neutral: the old picture).

#include "engine/render/device.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/sky.hpp"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace terrain_core {

struct EnbTints {
    float shadows[3] = {1, 1, 1};
    float highlights[3] = {1, 1, 1};
};

// Neutral by default: a preset that says nothing is the old picture.
struct EnbSettings {
    std::string preset = "classic";
    bool hdr = true;              // false: no half-float scene, no chain at all (the pre-AL-77 path)
    // ambient occlusion
    float aoStrength = 0, aoRadius = 0.9f, aoDistance = 150, aoPower = 1.3f;
    // contact shadows
    float contactStrength = 0, contactLength = 0.7f, contactDistance = 45, contactDarkness = 0.55f;
    // atmosphere
    float fogExtra = 0;           // more of the scene's own height fog (0: as the scene has it)
    float scatter = 0, scatterG = 0.76f;
    float aerial = 0, aerialDistance = 9000;
    float aerialTint[3] = {0.72f, 0.80f, 0.95f};
    float sunHalo = 0;
    // bloom
    float bloom = 0, bloomThreshold = 1.0f, bloomKnee = 0.5f, bloomRadius = 0.8f;
    // light shafts
    float rays = 0, raysDecay = 0.97f, raysDensity = 0.85f;
    // exposure
    float exposure = 1.0f;
    float adaptation = 0;         // 0: fixed exposure; 1: the frame's mean brought fully to the key
    float adaptSpeed = 1.5f;      // per second
    float exposureMin = 0.5f, exposureMax = 2.0f;
    float key = 0.18f;
    // tone and grade
    int tonemap = 0;              // 0 none (clamp), 1 ACES, 2 AgX
    bool linearize = false;       // work in linear light (value^2.2), write the display code back
    float contrast = 1, pivot = 0.45f, saturation = 1, vibrance = 0;
    float lift[3] = {0, 0, 0}, gamma[3] = {1, 1, 1}, gain[3] = {1, 1, 1};
    float splitStrength = 0, splitBalance = 0;
    EnbTints day, golden, night;  // split-tone tints by the sun's height
    // lens
    float vignette = 0, vignetteReach = 1.15f, vignetteUmber = 0;
    float grain = 0, grainSize = 1.5f;
    // warm point lights (EnbFrame::lights): their strength, and how much of it stays by day
    float pointLights = 0, pointLightsByDay = 0.15f, pointGlow = 0.5f;
};

// A warm light the chain adds to the frame (a fire, a hearth): no shadow of
// its own, lit by the depth's normals, with a glow round it in the air.
struct EnbLight {
    float position[3] = {0, 0, 0};   // eye-relative metres
    float radius = 10;               // metres it reaches
    float colour[3] = {1.0f, 0.55f, 0.22f};
    float flicker = 0;               // a phase, so two fires do not flicker as one
};

// The file's presets (names, in file order) and the one it starts with.
struct EnbPresets {
    std::vector<std::string> names;
    std::string start;
};

// Reads `preset` from the file (empty: the file's "preset"). False with `why`
// if the file or the preset is missing or bad; `out` then keeps its defaults.
bool loadEnbSettings(const std::filesystem::path& file, const std::string& preset, EnbSettings& out,
                     std::string* why, EnbPresets* presets = nullptr);

// Must match cbuffer Enb in assets/shaders/terrain_core/enb.hlsli.
struct EnbUniforms {
    float camF[4], camR[4], camU[4];
    float res[4], src[4];
    float sun[4], sunColour[4], zenith[4], horizon[4], sunUv[4];
    float ao[4], contact[4], atmos[4], aerialTint[4];
    float bloom[4], rays[4], expo[4], expo2[4], tone[4];
    float lift[4], gamma[4], gain[4], shadowTint[4], highTint[4];
    float lens[4], flags[4];
    float lightPos[8][4];   // xyz eye-relative, w radius
    float lightCol[8][4];   // rgb (times strength), w flicker phase
};
static_assert(sizeof(EnbUniforms) == 42 * 16);

// The frame's uniforms (pure: tested). `seconds` the clock, `dt` the frame's
// time; `width`, `height` the scene's.
EnbUniforms enbUniforms(const EnbSettings& s, const LightUniforms& light, const RayCamera& camera, float nearPlane,
                        float eyeZ, std::uint32_t width, std::uint32_t height, float seconds, float dt);
// The split-tone tints for a sun this high (its direction's z): day, golden
// hour and night blended.
EnbTints enbTintsFor(const EnbSettings& s, float sunZ);
// Puts the nearest of `lights` (at most Enb::kMaxLights) into `u`, their
// colour times the preset's strength for this sun; flags[3] their count.
void enbPutLights(EnbUniforms& u, const EnbSettings& s, float sunAbove, const std::vector<EnbLight>& lights);

struct EnbFrame {
    SDL_GPUTexture* scene = nullptr;    // half float, single sample, sampled
    SDL_GPUTexture* depth = nullptr;    // the scene's, sampled
    SDL_GPUTexture* clouds = nullptr;   // the cloud layer to lay over the frame, or null
    std::uint32_t width = 0, height = 0;
    SDL_GPUTexture* out = nullptr;      // kColourFormat, same size, a colour target
    RayCamera camera{};
    float nearPlane = 0.1f;
    float eyeZ = 0;
    const LightUniforms* light = nullptr;
    float seconds = 0, dt = 0;
    bool grain = true;                  // false before an upscaler (grain at its own resolution would smear)
    const std::vector<EnbLight>* lights = nullptr;   // the nearest kMaxLights are used
};

class Enb {
public:
    static constexpr SDL_GPUTextureFormat kSceneFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    static constexpr std::uint32_t kBloomLevels = 6;
    static constexpr std::uint32_t kMaxLights = 8;

    bool open(engine::Device& device);
    void apply(SDL_GPUCommandBuffer* commands, const EnbFrame& frame, const EnbSettings& settings);
    // An 8-bit target of the scene's size for `out` when the frame goes on to
    // an upscaler; null if it cannot be made.
    SDL_GPUTexture* scratchOut(std::uint32_t width, std::uint32_t height);
    // Forget the eye's history (a cut: a new preset, a jump of the camera).
    void resetAdaptation() { reset_ = true; }

private:
    bool size(std::uint32_t width, std::uint32_t height);
    void pass(SDL_GPUCommandBuffer* commands, const engine::GraphicsPipeline& pipeline, SDL_GPUTexture* target,
              std::uint32_t w, std::uint32_t h, std::initializer_list<SDL_GPUTextureSamplerBinding> sources,
              EnbUniforms u, float mode = 0);

    engine::Device* device_ = nullptr;
    engine::GraphicsPipeline ao_, composite_, down_, up_, rays_, adapt_, final_;
    engine::Sampler linear_, point_;
    engine::Texture aoTex_, lit_, raysTex_, out_;
    engine::Texture downTex_[kBloomLevels], upTex_[kBloomLevels];
    engine::Texture adapted_[2];
    std::uint32_t width_ = 0, height_ = 0, outWidth_ = 0, outHeight_ = 0;
    std::uint32_t levelW_[kBloomLevels] = {}, levelH_[kBloomLevels] = {};
    std::uint32_t flip_ = 0;
    bool reset_ = true;
};

} // namespace terrain_core
