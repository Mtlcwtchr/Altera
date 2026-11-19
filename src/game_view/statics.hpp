#pragma once
// The village's buildings and works as static meshes: one bake
// (tools/game_assets/bake_buildings.py, ASTM v1) of pieces assembled from the
// kit, drawn instanced per piece with assets/shaders/altera_game/statics.hlsl.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/render/device.hpp"
#include "terrain_core/lighting.hpp"

namespace altera::game {

struct Placed {
    int piece = 0;
    double rel[3] = {0, 0, 0};   // eye-relative origin (footprint centre on the ground)
    float yaw = 0;
    float scale = 1;
    float highlight = 0;
    float tint[3] = {1, 1, 1};
};

class StaticSet {
public:
    bool load(engine::Device& device, const std::filesystem::path& file, std::string* why);
    bool loaded() const { return pipeline_ ? true : false; }
    int piece(const std::string& name) const;
    // Footprint half extents of a piece (metres, before scale).
    void halfSize(int piece, float& hx, float& hy) const;

    void prepare(SDL_GPUCommandBuffer* commands, const std::vector<Placed>& placed);
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const float viewProjection[16],
              const terrain_core::LightUniforms& light);

    static constexpr std::size_t kMaxPlaced = 2048;

    StaticSet() = default;
    StaticSet(const StaticSet&) = delete;
    StaticSet& operator=(const StaticSet&) = delete;
    ~StaticSet();

private:
    struct Piece {
        std::string name;
        std::uint32_t firstIndex = 0, indexCount = 0;
        float min[3] = {}, max[3] = {};
    };
    std::vector<Piece> pieces_;
    engine::Device* device_ = nullptr;
    engine::Buffer vertices_, indices_, instances_;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    engine::Texture layers_;
    engine::Sampler sampler_;
    engine::GraphicsPipeline pipeline_;
    struct Batch {
        int piece;
        std::uint32_t first, count;
    };
    std::vector<Batch> batches_;
};

// RGBA8 layers (each width x height, rows top-down) as a sampled 2D array texture.
engine::Texture uploadRgbaArray(engine::Device& device, const std::uint8_t* rgba, std::uint32_t width,
                                std::uint32_t height, std::uint32_t layers);

} // namespace altera::game
