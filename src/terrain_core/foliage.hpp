#pragma once
// Foliage on the GPU (stage 3 of doc/rework/TERRAIN_REWORK_PLAN.md).
//
// Layers (grass, shrubs, conifers ...) of models, each scattered over a grid
// of candidate cells round the eye and remade every frame by a compute pass:
// nothing is streamed, nothing is kept, so nothing lags and nothing pops in
// late. Each candidate is tested - range, frustum, slope, sea, biome rules,
// size on screen - and what survives is drawn with one indirect draw per
// model. Every layer can be switched off on its own, the whole module too.
//
// Config: content/config/terrain_core/foliage.json. Models: built by
// tools/terrain_lab/build_foliage.py into assets/terrain_core/foliage.

#include "engine/render/device.hpp"
#include "terrain_core/biomes.hpp"
#include "terrain_core/clearings.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/params.hpp"
#include "terrain_core/rivers.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace terrain_core {

struct FoliageLayer {
    std::string name;
    double spacing = 1, range = 50;
    std::uint32_t firstModel = 0, modelCount = 0, maxCount = 0;
    float scaleMin = 1, scaleMax = 1, minNormalZ = 0.7f;
    std::array<std::array<float, 4>, kMaxBiomes> rules{};   // density, log2 cluster cell, cover, -
    bool enabled = true;
    bool held = false;   // AL-78: trees the game may hold (forestHold): taken over inside its rect
    // Last frame read back (two frames late): instances drawn, candidates tried.
    std::uint32_t drawn = 0, candidates = 0;
};

struct FoliageFrame {
    Vec3d eye;
    Frustum frustum;
    float viewProjection[16];
    float pixelScale = 1;      // pixels per metre of size at one metre
    float minPixels = 1.5f;
};

class Foliage {
public:
    bool open(engine::Device& device, const Params& params, const std::filesystem::path& config,
              const GroundUniforms& ground, std::string* why = nullptr);

    // Compute passes: reset, spawn every enabled layer, arguments. Outside
    // any render pass. Needs the clipmap and biome map the terrain draws with.
    void spawn(SDL_GPUCommandBuffer* commands, const FoliageFrame& frame, SDL_GPUTexture* clipmap,
               SDL_GPUTexture* biomeMap, std::uint32_t biomeSize, SDL_GPUTexture* water, std::uint32_t waterSize,
               const Rivers& rivers);
    // Inside the render pass, after the terrain. `look` is the terrain's.
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view,
              const LightUniforms& light, SDL_GPUTexture* shadowAtlas, SDL_GPUSampler* shadowSampler);
    // Into the shadow atlas, one cascade (`view` carries its matrix).
    void drawShadow(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view);

    std::vector<FoliageLayer>& layers() { return layers_; }
    std::uint32_t modelCount() const { return std::uint32_t(models_.size()); }
    std::uint64_t drawnInstances() const;
    std::uint64_t drawnTriangles() const { return triangles_; }
    std::uint64_t candidates() const;
    std::uint64_t gpuBytes() const { return gpuBytes_; }

    // AL-78: the trees the GPU would scatter in `rect` (world metres x0, y0,
    // x1, y1) for the held layers - the same cells, hashes, biome rules,
    // patches, clearings - on the CPU, with the game's ground: `height` (m)
    // and `keep` (false: water, a river, no place for a tree).
    std::vector<HeldTree> heldTrees(const double rect[4], const BiomeMap& biomes,
                                           const std::function<float(double, double)>& height,
                                           const std::function<bool(double, double)>& keep) const;
    // the model's crown radius and height at scale 1 (m)
    float modelRadius(std::uint32_t m) const { return m < models_.size() ? models_[m].radius : 0.0f; }
    float modelHeight(std::uint32_t m) const { return m < models_.size() ? models_[m].height : 0.0f; }

private:
    struct Model { std::string name; std::uint32_t firstIndex, indexCount, offset, capacity; float radius, height; };
    engine::Device* device_ = nullptr;
    Params params_;
    float tileLog2_ = 0;
    std::vector<FoliageLayer> layers_;
    std::vector<Model> models_;
    engine::Buffer vertices_, indices_, instances_, counters_, args_, modelInfo_, modelSlots_;
    engine::Texture textures_;
    engine::Sampler nearest_, linear_;
    engine::ComputePipeline spawn_, reset_, argsPass_, hold_;
    engine::Buffer held_;                 // AL-78: forestHold().trees, two float4 a tree
    std::uint32_t heldCount_ = 0;
    std::uint64_t heldVersion_ = ~0ull;
    engine::GraphicsPipeline draw_, shadow_;
    SDL_GPUTransferBuffer* readback_[3] = {};
    std::uint64_t frame_ = 0;
    std::vector<std::uint32_t> lastCounts_;
    std::uint64_t triangles_ = 0, gpuBytes_ = 0;
public:
    ~Foliage();
};

} // namespace terrain_core
