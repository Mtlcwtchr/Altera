#pragma once
// The ground's material, baked: what the terrain shader works out per pixel -
// which ground, which biome variant, the blend between them, moss, stones,
// decals - is a property of the place, not of the frame, and it was being
// worked out again for every pixel of every frame (a hundred to six hundred
// texture reads each). Here it is drawn once into a clipmap round the camera
// and the frame reads it back: albedo, shading normal, roughness, occlusion.
// Weather, light, shadow and fog stay live in the terrain shader.
//
// Eleven levels, 1024 texels a side each, 4 cm a texel at the finest and
// twice as coarse at every level after (41 m to 42 km across). Each level is
// addressed toroidally - a world texel lives at its index modulo 1024 - so a
// moving camera bakes only the strip it moved into. Whole levels (a new world,
// a teleport, a changed biome table) are rebuilt coarse first within a budget
// of texels a frame, and the shader reads the finest level that is complete
// (terrain_pages.hlsl, materialCache*).
#include "engine/render/device.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace engine { struct Frame; }

namespace game {

class TerrainMaterialCache {
public:
    static constexpr int kLevels = 11;
    static constexpr int kSize = 1024;           // texels a side, every level
    static constexpr double kFinestTexel = 0.04; // metres
    static constexpr int kSnap = 8;              // a window moves in steps of this many texels

    bool setup(engine::Device& device);
    // What the bake reads (terrain_material_bake.hlsl): the terrain pass's
    // fragment samplers with the climate array in the shadow's slot. Handed
    // in every frame, as the terrain pass builds its own.
    void bindings(std::vector<SDL_GPUTextureSamplerBinding> bindings) { bindings_ = std::move(bindings); }
    bool ready() const { return bool(pipeline_); }
    // The coarsest level is complete: every pixel of the cut can read something.
    bool usable() const { return pipeline_ && levels_[kLevels - 1].complete; }

    // Brings every level to the camera and bakes what moved, what was
    // invalidated and as much of any rebuild as the budget allows. Records
    // render passes on the frame's command buffer: call while no pass is open.
    // `ring` is the 16-float RingDraw block the page lookups read (table size,
    // height range).
    void update(const engine::Frame& frame, double cameraX, double cameraY, const std::array<float, 16>& ring);
    // A place whose material changed (pages arrived, ground was dug), in metres.
    // `dataset`: the page data that changed there (0 H4 ... 3 H64, 4/5 coarser).
    // A level whose texels read nothing finer than their own dataset and the
    // page is finer than that does not change: it is left alone. Default: all.
    void invalidate(double minX, double minY, double maxX, double maxY, int dataset = 99);
    // Everything (a new world, a reloaded biome or ground table).
    void invalidateAll();

    // For the terrain shader (terrain_cached.hlsl): colour, normal, the
    // per-level window table and the detail ids, in that order.
    std::vector<SDL_GPUTextureSamplerBinding> lookupBindings() const;
    std::size_t bakedLastFrame() const { return bakedLastFrame_; }
    std::size_t pendingTexels() const;
    // Nothing left to bake: every level complete and no strip or refresh queued.
    bool settled() const { return !pipeline_ || jobs_.empty(); }

private:
    struct Level {
        bool placed = false;      // has a window at all
        bool complete = false;    // every texel of `origin`'s window baked
        std::int64_t originX = 0, originY = 0;   // the window's lowest world texel
    };
    struct Job {
        int level;
        std::int64_t x0, y0, x1, y1;   // world texels, half-open
        bool rebuild;                  // part of a whole-level rebuild: the level is unusable until done
        bool budgeted;                 // within the per-frame budget (rebuilds and refreshes), else now
    };
    double texel(int level) const { return kFinestTexel * double(1 << level); }
    void queueRect(int level, std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1, bool rebuild,
                   bool budgeted);
    void queueRebuild(int level);
    void bake(const engine::Frame& frame, const std::array<float, 16>& ring, const std::vector<Job>& jobs);
    void publishTable(engine::Device& device);

    engine::Texture albedo_, normal_, detail_, table_;
    engine::Sampler sampler_, tableSampler_;
    engine::GraphicsPipeline pipeline_;
    std::vector<SDL_GPUTextureSamplerBinding> bindings_;
    std::array<Level, kLevels> levels_{};
    std::deque<Job> jobs_;
    std::array<int, kLevels> rebuildsPending_{};
    bool tableDirty_ = true;
    std::size_t bakedLastFrame_ = 0;
};

} // namespace game
