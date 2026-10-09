#pragma once
// The grass cover: solid grass wherever the ground is grass, in the ground's
// own colour, near and far (assets/shaders/terrain_core/grass_spawn.hlsl).
//
// Rings round the eye, each a fixed grid of the world twice as coarse as the
// one inside it with tufts twice as wide; past the last ring the ground is
// already painted the colour the tufts were. Remade every frame on the GPU,
// one indirect draw. A module: off, it costs nothing.

#include "engine/render/device.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/params.hpp"

#include <filesystem>
#include <string>

namespace terrain_core {

struct GrassSettings {
    double firstSpacing = 0.16;   // metres between tufts in the first ring
    double firstRadius = 8.0;     // outer radius of the first ring; each next doubles
    std::uint32_t rings = 5;      // 8, 16, 32, 64, 128 m
    float tuftHeight = 0.45f;     // metres
    float tuftWidth = 2.4f;       // in the ring's cells: overlapping, so the cover is solid
    float flowerChance = 0.04f;
    std::uint32_t capacity = 400000;
};

struct GrassFrame {
    Vec3d eye;
    Frustum frustum;
    float pixelScale = 1;
    float sunDirection[3];
    float skyColour[3];
};

class Grass {
public:
    ~Grass();
    bool open(engine::Device& device, const Params& params, const GrassSettings& settings,
              const std::filesystem::path& spriteDir, std::string* why = nullptr);
    // Compute: reset, spawn every ring, arguments. Reads the clipmap, the biome
    // map and the material array the ground draws with, and its uniforms.
    void spawn(SDL_GPUCommandBuffer* commands, const GrassFrame& frame, SDL_GPUTexture* clipmap,
               SDL_GPUTexture* biomeMap, SDL_GPUTexture* materials, SDL_GPUTexture* normals,
               SDL_GPUTexture* properties, const GroundUniforms& ground);
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const float viewProjection[16],
              const void* look, std::size_t lookBytes);

    std::uint32_t tufts() const { return lastCount_; }   // two frames late
    std::uint64_t candidates() const { return candidates_; }
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    GrassSettings& settings() { return settings_; }

private:
    engine::Device* device_ = nullptr;
    Params params_;
    GrassSettings settings_;
    engine::Buffer tufts_, count_, args_;
    engine::Texture sprites_;
    engine::Sampler nearest_, linear_, repeat_;
    engine::ComputePipeline spawn_, reset_, argsPass_;
    engine::GraphicsPipeline draw_;
    SDL_GPUTransferBuffer* readback_[3] = {};
    std::uint64_t frame_ = 0, candidates_ = 0, gpuBytes_ = 0;
    std::uint32_t lastCount_ = 0;
};

} // namespace terrain_core
