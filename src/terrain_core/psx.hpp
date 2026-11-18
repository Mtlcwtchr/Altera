#pragma once
// The PSX look as the very last pass over the frame (assets/shaders/terrain_core/psx.hlsl):
// the picture drawn at a fraction of the resolution, light ordered dither,
// colour cut to a few bits a channel, a little grain - the same steps, in the
// same units, as the asset previews' filter (assets_catalog/tools/psx_filter.py),
// so a preview and the game look alike. A vibe, not an emulation: no vertex
// jitter, no affine textures, no palette limits. Settings in
// content/config/terrain_core/psx.json; off, nothing is drawn.

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace terrain_core {

struct PsxSettings {
    bool enabled = false;
    std::uint32_t pixelScale = 3;   // screen pixels per picture pixel (1: full resolution)
    std::uint32_t colourBits = 5;   // bits a channel (8: no cut)
    float dither = 8.0f;            // ordered dither's amplitude, in 0..255 steps
    float grain = 3.0f;             // grain's deviation, in 0..255 steps
    bool grainAnimated = true;      // new grain every frame (false: fixed, as the previews)
};

// False with `why` if the file is missing or bad; `out` then keeps its defaults.
bool loadPsxSettings(const std::filesystem::path& file, PsxSettings& out, std::string* why);

class Psx {
public:
    bool open(engine::Device& device);
    // After everything else (styles included): reads targets.resolved() and
    // writes the filtered picture back into it, so a screenshot sees it too.
    void apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, const PsxSettings& settings, float time);

private:
    engine::Device* device_ = nullptr;
    engine::GraphicsPipeline pipeline_;
    engine::Sampler sampler_;
    engine::Texture small_;   // the picture at the low resolution
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace terrain_core
