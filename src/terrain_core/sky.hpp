#pragma once
// The sky (a full-screen pass drawn first) and the volumetric clouds (a
// compute march at half resolution the final pass lays over the frame).
// Both read lighting.hlsli's uniforms; both are modules that can be off.

#include "engine/render/device.hpp"
#include "engine/render/targets.hpp"
#include "terrain_core/lighting.hpp"

namespace terrain_core {

// The camera as the sky and the clouds need it: rays from the eye.
struct RayCamera {
    float forward[4];
    float right[4];   // scaled by tan(fov/2) * aspect
    float up[4];      // scaled by tan(fov/2)
};
RayCamera rayCamera(const double forward[3], double fovY, double aspect);

class Sky {
public:
    bool open(engine::Device& device);
    // Inside the frame's render pass, before anything else.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const LightUniforms& light,
              const RayCamera& camera);

private:
    engine::GraphicsPipeline pipeline_;
    engine::Sampler sampler_;
    engine::Texture dummy_;
};

class Clouds {
public:
    bool open(engine::Device& device);
    // After the frame's render pass (it reads the depth): marches the layer
    // into texture(). False if there is nothing to draw.
    bool march(SDL_GPUCommandBuffer* commands, engine::Targets& targets, const LightUniforms& light,
               const RayCamera& camera, float nearPlane, std::uint32_t frame);
    SDL_GPUTexture* texture() const { return texture_.get(); }

private:
    engine::Device* device_ = nullptr;
    engine::ComputePipeline pipeline_;
    engine::Texture texture_;
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace terrain_core
