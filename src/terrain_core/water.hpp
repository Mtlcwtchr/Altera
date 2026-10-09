#pragma once
// Water on the GPU: the sea, lakes and rivers of the water map (hydrology.hpp)
// drawn over the terrain's own patches, and the open ocean past the world's
// edge (assets/shaders/terrain_core/water.hlsl). A module like the others.

#include "engine/render/device.hpp"
#include "terrain_core/hydrology.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/params.hpp"
#include "terrain_core/renderer.hpp"

namespace terrain_core {

class Water {
public:
    bool open(engine::Device& device, const Params& params, const WaterMap& map, std::string* why = nullptr);
    // Inside the frame's render pass, after everything opaque.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Renderer& terrain, const ViewUniforms& view,
              const LightUniforms& light, SDL_GPUTexture* shadowAtlas, SDL_GPUSampler* shadowSampler,
              const Vec3d& eye, float seconds);
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    // The water map, for the plants' spawns: nothing grows under water.
    SDL_GPUTexture* map() const { return map_.get(); }
    std::uint32_t mapSize() const { return mapSize_; }

private:
    Params params_;
    std::uint32_t mapSize_ = 0;
    engine::Texture map_;
    engine::Sampler nearest_;
    engine::GraphicsPipeline patches_, ocean_;
    std::uint64_t gpuBytes_ = 0;
};

} // namespace terrain_core
