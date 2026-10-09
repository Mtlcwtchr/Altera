#pragma once
// The terrain core on the GPU: the macro map, the clipmap it generates from it,
// and the one instanced draw of the selected patches.
//
// Generation runs in the frame's own command buffer before the draw, so a
// texel the draw reads is never stale: whatever came into view this frame is
// generated this frame. What that costs is exactly what the benchmark is for.

#include "engine/render/device.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/macro_map.hpp"
#include "terrain_core/params.hpp"

#include <cstdint>
#include <vector>

namespace terrain_core {

struct ViewUniforms {
    float viewProjection[16];   // row-major, eye-relative
    float eyeZ;
    float spacing;
    std::uint32_t clipSize;
    std::uint32_t grid;
    std::uint32_t levels;
    float rangeFactor;
    float morphStart;
    float pad0;
};

struct LookUniforms {
    float sunDirection[3];
    float hazeDistance;
    float skyColour[3];
    std::uint32_t debugLevels;
    std::uint32_t clipSize;
    std::uint32_t levels;
    std::uint32_t flags;      // kLookPlainGround: colour by height and slope, no materials
    float pad;
};

constexpr std::uint32_t kLookPlainGround = 1;
constexpr std::uint32_t kLookNoSurfaceMaps = 2;   // materials without their normal and AO maps

class Renderer {
public:
    bool open(engine::Device& device, const Params& params, const MacroMap& macro, const BiomeTable& table,
              const BiomeMap& biomes);

    // Records the generation of `rects` into `commands` (compute passes, so
    // outside any render pass). Returns the texels generated.
    std::uint64_t generate(SDL_GPUCommandBuffer* commands, const std::vector<Rect>& rects);
    // Copies the patch list into `commands` (a copy pass, so before the
    // render pass). In the frame's own command buffer: a separate submit per
    // frame for a few kilobytes was the frame's only multi-millisecond spike.
    bool upload(SDL_GPUCommandBuffer* commands, const Selection& selection);
    ~Renderer();
    // Inside a render pass.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const ViewUniforms& view,
              const LookUniforms& look, const LightUniforms& light, SDL_GPUTexture* shadowAtlas,
              SDL_GPUSampler* shadowSampler);
    // The patches the sun's shadows are drawn from: chosen without the eye's
    // frustum (a mountain behind the eye still shades what it sees).
    bool uploadShadow(SDL_GPUCommandBuffer* commands, const Selection& selection);
    // Inside the shadow atlas pass, one cascade: `light` carries its matrix.
    void drawShadow(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const ViewUniforms& light);
    // The view's patches with whatever pipeline is bound: the water draws its
    // surface over the very same grid.
    void drawPatches(SDL_GPURenderPass* pass) { drawSet(pass, view_); }

    std::uint32_t trianglesDrawn() const { return triangles_; }
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    SDL_GPUTexture* clipmap() const { return clipmap_.get(); }
    SDL_GPUTexture* biomeMap() const { return biomeMap_.get(); }
    SDL_GPUTexture* materials() const { return materials_.get(); }
    SDL_GPUTexture* materialNormals() const { return materialNormals_.get(); }
    SDL_GPUTexture* materialProperties() const { return materialProperties_.get(); }
    std::uint32_t biomeSize() const { return ground_.biomeSize; }
    const GroundUniforms& ground() const { return ground_; }

private:
    engine::Device* device_ = nullptr;
    Params params_;
    std::uint32_t macroSize_ = 0;
    std::uint32_t macroMips_ = 1;
    engine::Texture macro_, clipmap_;
    engine::Sampler nearest_, repeat_;
    engine::Texture biomeMap_, materials_, materialNormals_, materialProperties_;
    GroundUniforms ground_{};
    engine::ComputePipeline generate_;
    engine::GraphicsPipeline terrain_;
    engine::Buffer indices_;
    // One selection's patches on the card: for the view and for the shadows.
    struct PatchSet {
        engine::Buffer buffer;
        SDL_GPUTransferBuffer* transfer = nullptr;
        std::uint32_t capacity = 0;
        std::uint32_t counts[4] = {}, firsts[4] = {};
        std::vector<Patch> staging;
    };
    bool uploadSet(SDL_GPUCommandBuffer* commands, const Selection& selection, PatchSet& set);
    void drawSet(SDL_GPURenderPass* pass, const PatchSet& set);
    PatchSet view_, shadow_;
    engine::GraphicsPipeline shadowPipeline_;
    std::uint32_t quarterIndices_ = 0;
    std::uint32_t triangles_ = 0;
    std::uint64_t gpuBytes_ = 0;
};

} // namespace terrain_core
