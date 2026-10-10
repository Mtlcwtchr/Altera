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
#include <future>
#include <map>
#include <string>
#include <vector>

namespace terrain_core {

struct PropsFrame {
    Vec3d eye;
    Frustum frustum;           // eye-relative
    float pixelScale = 1;      // pixels per metre of size at one metre
    float minPixels = 2.0f;    // smaller than this on screen: not drawn
    double shadowRange = 0;    // casters within this of the eye go into the shadow atlas
    double detailRange = 1400; // cliff tiles nearer than this get their refined surface
    int detailBuilds = 1;      // refined surfaces made per frame at most
};

// Must match cbuffer Rock in props.hlsl.
struct PropsLook {
    float tileMetres = 4;      // the ground's material tile
    float textureScale = 1;    // as GroundUniforms::textureScale
    float mossLayer = 0;       // the material laid as moss (props.json "moss_material")
    float pad = 0;
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
    float mossLayer() const { return mossLayer_; }
    std::uint32_t modelCount() const { return std::uint32_t(models_.size()); }
    std::uint32_t drawn() const { return drawn_; }
    std::uint64_t trianglesDrawn() const { return triangles_; }
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    double cullMs() const { return cullMs_; }
    // Cliff tiles drawn refined this frame, kept refined, and what the last
    // refinement cost on the CPU.
    std::uint32_t detailDrawn() const { return detailDrawn_; }
    std::size_t detailKept() const { return detail_.size(); }
    double detailMs() const { return detailMs_; }

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
        float moss = 0;               // 0..1: how much moss its upward faces carry
        double cx = 0, cy = 0, cz = 0;   // bounding sphere centre
        float radius = 1;
        bool detail = false;          // a cliff tile: refined near the eye
    };
    // A cliff tile's surface split twice and roughened, near the eye: made on
    // the CPU when the eye comes close, dropped when it is long gone.
    struct DetailMesh {
        engine::Buffer vertices, indices;
        SDL_GPUTransferBuffer* transfer = nullptr;   // its upload
        std::uint32_t vertexBytes = 0, indexBytes = 0;   // what the buffers hold at most
        std::uint32_t indexCount = 0;
        std::uint64_t lastUsed = 0;
    };
    struct CpuMesh { std::vector<float> vertices; std::vector<std::uint32_t> indices; };
    // One draw: a run of the instance buffer.
    struct Batch { std::uint32_t model, level, first, count; const DetailMesh* detail = nullptr; };
    struct Gpu { float posYaw[4], scale[4], anchor[4]; };   // 48 bytes, the vertex shader's instance input

    void fill(const PropsFrame& frame, bool shadow, std::vector<Batch>& batches);
    Gpu gpuOf(std::uint32_t instance, const PropsFrame& frame) const;
    void drawBatches(SDL_GPURenderPass* pass, const std::vector<Batch>& batches);
    // A refined surface, made on a worker thread: vertices (8 floats each), indices.
    using Surface = std::pair<std::vector<float>, std::vector<std::uint32_t>>;
    static Surface refineSurface(const CpuMesh* base, double x, double y, double z);
    // Copied in the frame's own command buffer: no wait on the card.
    bool upload(SDL_GPUCommandBuffer* commands, std::uint32_t instance, const Surface& surface);
    // Takes in the surfaces the workers have finished (all of them, waiting, with `wait`).
    void collect(SDL_GPUCommandBuffer* commands, bool wait);
    void forget(std::map<std::uint32_t, DetailMesh>::iterator it);

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
    std::map<std::uint32_t, CpuMesh> cpu_;            // by model: level 0 kept for refining
    std::map<std::uint32_t, DetailMesh> detail_;      // by instance
    std::map<std::uint32_t, std::future<Surface>> pending_;   // by instance, being refined
    std::vector<DetailMesh> spare_;                   // buffers of forgotten surfaces, for the next
    std::uint64_t frame_ = 0;
    std::uint32_t detailDrawn_ = 0;
    double detailMs_ = 0;
    float mossLayer_ = 0;
    std::uint32_t drawn_ = 0;
    std::uint64_t triangles_ = 0, gpuBytes_ = 0;
    double cullMs_ = 0;
};

} // namespace terrain_core
