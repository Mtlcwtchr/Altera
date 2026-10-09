#pragma once
// Picture styles over the finished frame: dark fantasy, Frazetta, Vallejo
// (assets/shaders/terrain_core/post.hlsl). A module like the others: off (0),
// it is not drawn at all.

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"

#include <cstdint>

namespace terrain_core {

class Post {
public:
    static constexpr std::uint32_t kStyles = 4;   // 0 off, 1 dark fantasy, 2 Frazetta, 3 Vallejo
    static const char* name(std::uint32_t style);

    bool open(engine::Device& device);
    // After the scene's render pass: reads targets.colour(), writes the styled
    // picture back into it (through a scratch target), so whatever comes after
    // - the window, a screenshot - sees the styled frame.
    void apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, std::uint32_t style, float time);

private:
    engine::Device* device_ = nullptr;
    engine::GraphicsPipeline pipeline_;
    engine::Sampler sampler_;
    engine::Texture scratch_;
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace terrain_core
