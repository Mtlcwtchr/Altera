#pragma once
// Mega structures: the mesh part of the hybrid terrain
// (doc/rework/MESH_TERRAIN_PLAN.md). What the heightfield cannot hold -
// kilometre spires, natural bridges, megaliths the size of hills - as
// placed models.
//
// Unlike the foliage nothing is remade per frame on the card: the world's
// instances are a fixed list (<world>_features.json, written by
// tools/terrain_lab/world_features.py), a few thousand at most, culled and
// given a level of detail on the CPU in double precision, and drawn with one
// instanced draw per model and level. The stone is the ground's own: the
// material array laid on in world space from three sides, so a spire and the
// canyon wall under it are the same rock.
//
// Models: assets/terrain_core/props/<name>_lod<0..2>.mesh, built by
// tools/blender/terrain_props.py.
#include "engine/render/device.hpp"
#include "terrain_core/lighting.hpp"
#include "terrain_core/lod_select.hpp"
#include "terrain_core/params.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace terrain_core {

struct PropsFrame {
    Vec3d eye;
    Frustum frustum;           // eye-relative
    float pixelScale = 1;      // pixels per metre of size at one metre
    float minPixels = 2.0f;    // smaller than this on screen: not drawn
    double shadowRange = 0;    // casters within this of the eye go into the shadow atlas
};

// Must match cbuffer Rock in props.hlsl.
struct PropsLook {
    float tileMetres = 4;      // the ground's material tile
    float textureScale = 1;    // as GroundUniforms::textureScale
    float pad[2] = {};
};

class Props {
public:
    // `ground(x, y)`: the height the terrain is drawn at, for standing models
    // on it. `materialsJson`: the ground's biome table, for material names.
    bool open(engine::Device& device, const std::filesystem::path& features, const std::filesystem::path& modelDir,
              const std::filesystem::path& materialsJson, const std::function<double(double, double)>& ground,
              std::string* why = nullptr);
    // Culls and uploads this frame's instances (a copy pass: outside any render pass).
    void prepare(SDL_GPUCommandBuffer* commands, const PropsFrame& frame);
    // Inside the scene pass, after the ground. `view` is the plants' (eye-relative).
    void draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view,
              const LightUniforms& light, const PropsLook& look, SDL_GPUTexture* materials,
              SDL_GPUTexture* normals, SDL_GPUTexture* properties, SDL_GPUTexture* shadowAtlas,
              SDL_GPUSampler* shadowSampler);
    // Into the shadow atlas, one cascade (`view` carries its matrix).
    void drawShadow(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view);

    std::size_t instanceCount() const { return instances_.size(); }
    std::uint32_t modelCount() const { return std::uint32_t(models_.size()); }
    std::uint32_t drawn() const { return drawn_; }
    std::uint64_t trianglesDrawn() const { return triangles_; }
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    double cullMs() const { return cullMs_; }

    ~Props();

private:
    static constexpr int kLevels = 3;
    struct Level { std::uint32_t firstIndex = 0, indexCount = 0; };
    struct Model {
        std::string name;
        std::array<Level, kLevels> levels;
        float lo[3], hi[3];
        float radius = 1;   // of the bounding sphere round the box centre, unscaled
    };
    struct Instance {
        std::uint32_t model = 0;
        double x = 0, y = 0, z = 0;   // the model's origin (ground level)
        float yaw = 0, sx = 1, sy = 1, sz = 1;
        float layer = 0;              // material array layer
        double cx = 0, cy = 0, cz = 0;   // bounding sphere centre
        float radius = 1;
    };
    // One draw: a run of the instance buffer.
    struct Batch { std::uint32_t model, level, first, count; };
    struct Gpu { float posYaw[4], scale[4], anchor[4]; };   // 48 bytes, the vertex shader's instance input

    void fill(const PropsFrame& frame, bool shadow, std::vector<Batch>& batches);
    void drawBatches(SDL_GPURenderPass* pass, const std::vector<Batch>& batches);

    engine::Device* device_ = nullptr;
    std::vector<Model> models_;
    std::vector<Instance> instances_;
    engine::Buffer vertices_, indices_, gpuInstances_;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    std::uint32_t capacity_ = 0;
    engine::Sampler linear_;
    engine::GraphicsPipeline draw_, shadow_;
    std::vector<Gpu> staging_;
    std::vector<std::vector<std::uint32_t>> buckets_;
    std::vector<Batch> viewBatches_, shadowBatches_;
    std::uint32_t drawn_ = 0;
    std::uint64_t triangles_ = 0, gpuBytes_ = 0;
    double cullMs_ = 0;
};

} // namespace terrain_core
