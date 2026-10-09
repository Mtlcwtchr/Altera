#include "terrain_core/water.hpp"

#include <algorithm>
#include <cmath>

namespace terrain_core {
namespace {

constexpr std::uint32_t kOceanRings = 96, kOceanSegments = 96;   // must match oceanVs

struct WaterView { float worldSize, mapSize, eyeX, eyeY; };   // cbuffer WaterView
struct WaterLook {                                             // cbuffer WaterLook
    float worldSize, eyeX, eyeY;
    std::uint32_t clipSize;
    float eyeWrapped[2];
    float time, pad;
};

} // namespace

bool Water::open(engine::Device& device, const Params& params, const WaterMap& map, std::string* why) {
    params_ = params;
    mapSize_ = map.size;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = map.size;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    map_ = device.makeTexture(info);
    if (!map_) { if (why) *why = "water map texture: " + device.error(); return false; }
    // In strips of at most 32 MB, as the macro map goes up.
    const std::uint32_t rows = std::max(1u, (32u << 20) / (map.size * 16));
    for (std::uint32_t y = 0; y < map.size; y += rows) {
        engine::Device::Uploader upload(device);
        const std::uint32_t h = std::min(rows, map.size - y);
        if (!upload.refillRegion(map_.get(), map.texels.data() + std::size_t(y) * map.size * 4, 0, y, map.size, h, 16) ||
            !upload.finish()) {
            if (why) *why = "water map upload: " + device.error();
            return false;
        }
    }
    gpuBytes_ = std::uint64_t(map.size) * map.size * 16;
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = device.makeSampler(sampling);

    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/water.hlsl";
    wanted.vertexEntry = "waterVs";
    wanted.fragmentEntry = "waterPs";
    wanted.depthClip = true;
    wanted.blend = true;
    wanted.depthWrite = false;
    wanted.cull = SDL_GPU_CULLMODE_NONE;
    wanted.buffers = {{0, sizeof(Patch), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 0}, {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16}};
    patches_ = device.makePipeline(wanted);
    wanted.vertexEntry = "oceanVs";
    wanted.buffers.clear();
    wanted.attributes.clear();
    ocean_ = device.makePipeline(wanted);
    if (!patches_ || !ocean_ || !nearest_) { if (why) *why = "water pipelines: " + device.error(); return false; }
    return true;
}

void Water::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Renderer& terrain, const ViewUniforms& view,
                 const LightUniforms& light, SDL_GPUTexture* shadowAtlas, SDL_GPUSampler* shadowSampler,
                 const Vec3d& eye, float seconds) {
    const WaterView wv{float(params_.worldSize), float(mapSize_), float(eye.x), float(eye.y)};
    const WaterLook wl{float(params_.worldSize), float(eye.x), float(eye.y), params_.clipSize,
                       {float(std::fmod(eye.x, 4096.0)), float(std::fmod(eye.y, 4096.0))}, seconds, 0.0f};
    const SDL_GPUTextureSamplerBinding fragment[2] = {{terrain.clipmap(), nearest_.get()}, {shadowAtlas, shadowSampler}};

    // The open ocean first (it is the farthest), then the world's own water.
    SDL_BindGPUGraphicsPipeline(pass, ocean_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    SDL_PushGPUFragmentUniformData(commands, 1, &wl, sizeof(wl));
    SDL_BindGPUFragmentSamplers(pass, 0, fragment, 2);
    SDL_DrawGPUPrimitives(pass, kOceanRings * kOceanSegments * 6, 1, 0, 0);

    SDL_BindGPUGraphicsPipeline(pass, patches_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUVertexUniformData(commands, 1, &wv, sizeof(wv));
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    SDL_PushGPUFragmentUniformData(commands, 1, &wl, sizeof(wl));
    const SDL_GPUTextureSamplerBinding vertex{map_.get(), nearest_.get()};
    SDL_BindGPUVertexSamplers(pass, 0, &vertex, 1);
    SDL_BindGPUFragmentSamplers(pass, 0, fragment, 2);
    terrain.drawPatches(pass);
}

} // namespace terrain_core
