#pragma once
// Ink outline over the finished frame (assets/shaders/terrain_core/outline.hlsl):
// dark, uneven lines on silhouettes and creases, found in the scene's depth.
// After the picture style, before the PSX look, so the PSX cut runs over the
// lines as over everything else. A style-profile option: settings in
// content/config/terrain_core/outline.json; off, nothing is drawn.

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace terrain_core {

struct OutlineSettings {
    bool enabled = false;
    float width = 1.5f;           // line half-width, display pixels
    float depthEdge = 0.06f;      // relative distance step that makes a silhouette
    float creaseEdge = 0.02f;     // relative second difference that makes a crease
    float creaseStrength = 0.6f;
    float opacity = 0.85f;
    float fadeStart = 40.0f;      // metres
    float fadeEnd = 900.0f;
    float farStrength = 0.45f;
    float roughness = 0.6f;       // 0 clean, 1 very broken
    float ink[3] = {0.07f, 0.045f, 0.035f};
    float inkShowThrough = 0.25f; // how much of the picture's own colour stays under the line
};

// False with `why` if the file is missing or bad; `out` then keeps its defaults.
bool loadOutlineSettings(const std::filesystem::path& file, OutlineSettings& out, std::string* why);

class Outline {
public:
    bool open(engine::Device& device);
    // Reads targets.resolved() and `depth` (the scene's sampled depth, of any
    // size: it is read by uv) and writes the inked picture back into the frame.
    void apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, SDL_GPUTexture* depth, float nearPlane,
               const OutlineSettings& settings);

private:
    engine::Device* device_ = nullptr;
    engine::GraphicsPipeline pipeline_;
    engine::Sampler sampler_;
    engine::Texture copy_;   // the frame before the lines
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace terrain_core
