#include "terrain_core/grass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace terrain_core {
namespace {

// Must match cbuffer GrassSpawn in assets/shaders/terrain_core/grass_spawn.hlsl.
struct SpawnUniforms {
    float planes[6][4];
    std::int32_t camTexel[2];
    float camTexelFrac[2];
    std::int32_t camCell[2];
    float camCellFrac[2];
    float eyeZ, spacing0, cellSize, inner;
    float outer, tuftHeight, tuftWidth, pixelScale;
    std::uint32_t gridSide, clipSize, levels, capacity;
    std::uint32_t seed;
    float heightLow, heightHigh, flowerChance;
    float sunDirection[3], waterMapSize;
    float skyColour[3], worldSize;
};
static_assert(sizeof(SpawnUniforms) == 224);

} // namespace

Grass::~Grass() {
    if (device_)
        for (auto* b : readback_)
            if (b) SDL_ReleaseGPUTransferBuffer(device_->handle(), b);
}

bool Grass::open(engine::Device& device, const Params& params, const GrassSettings& settings,
                 const std::filesystem::path& spriteDir, std::string* why) {
    device_ = &device;
    params_ = params;
    settings_ = settings;
    auto fail = [&](const std::string& what) { if (why) *why = what + ": " + device.error(); return false; };

    std::vector<std::vector<std::filesystem::path>> layers;
    for (int i = 0;; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "%02d.png", i);
        if (!std::filesystem::exists(spriteDir / name)) break;
        layers.push_back({spriteDir / name});
    }
    if (layers.empty()) return fail("no grass sprites in " + spriteDir.string() + " (run tools/terrain_lab/build_grass.py)");
    sprites_ = device.loadArrayMipped(layers, true);

    tufts_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                               std::size_t(settings.capacity) * 32);
    count_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, 4);
    args_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, 16);
    gpuBytes_ = std::uint64_t(settings.capacity) * 32 + layers.size() * 256 * 256 * 4 * 4 / 3;

    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampling.max_lod = 1000.0f;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = device.makeSampler(sampling);
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    linear_ = device.makeSampler(sampling);
    sampling.address_mode_u = sampling.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    repeat_ = device.makeSampler(sampling);

    spawn_ = device.makeCompute({"terrain_core/grass_spawn.hlsl", "spawnCs"});
    reset_ = device.makeCompute({"terrain_core/grass_args.hlsl", "resetCs"});
    argsPass_ = device.makeCompute({"terrain_core/grass_args.hlsl", "argsCs"});
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/grass_draw.hlsl";
    wanted.vertexEntry = "grassVs";
    wanted.fragmentEntry = "grassPs";
    wanted.depthClip = true;
    wanted.cull = SDL_GPU_CULLMODE_NONE;
    wanted.buffers = {{0, 32, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0}, {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16}};
    draw_ = device.makePipeline(wanted);
    if (!sprites_ || !tufts_ || !count_ || !args_ || !spawn_ || !reset_ || !argsPass_ || !draw_ || !nearest_ || !linear_ ||
        !repeat_)
        return fail("grass GPU objects");
    SDL_GPUTransferBufferCreateInfo download{};
    download.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    download.size = 4;
    for (auto& b : readback_)
        if (!(b = SDL_CreateGPUTransferBuffer(device.handle(), &download))) return fail("grass readback");
    return true;
}

void Grass::spawn(SDL_GPUCommandBuffer* commands, const GrassFrame& frame, SDL_GPUTexture* clipmap,
                  SDL_GPUTexture* biomeMap, SDL_GPUTexture* materials, SDL_GPUTexture* normals,
                  SDL_GPUTexture* properties, SDL_GPUTexture* water, std::uint32_t waterSize, const Rivers& rivers,
                  const GroundUniforms& ground, const LightUniforms& light) {
    if (frame_ >= 3) {
        if (const auto* n = static_cast<const std::uint32_t*>(
                    SDL_MapGPUTransferBuffer(device_->handle(), readback_[frame_ % 3], false))) {
            lastCount_ = std::min(*n, settings_.capacity);
            SDL_UnmapGPUTransferBuffer(device_->handle(), readback_[frame_ % 3]);
        }
    }
    const std::uint32_t capacity[4] = {settings_.capacity, 0, 0, 0};
    {
        SDL_GPUStorageBufferReadWriteBinding rw{count_.get(), false, {}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, reset_.get());
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    {
        SDL_GPUStorageBufferReadWriteBinding rw[2] = {{tufts_.get(), false, {}}, {count_.get(), false, {}}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2);
        SDL_BindGPUComputePipeline(pass, spawn_.get());
        const SDL_GPUTextureSamplerBinding samplers[6] = {
                {clipmap, nearest_.get()}, {biomeMap, nearest_.get()}, {materials, repeat_.get()},
                {normals, repeat_.get()}, {properties, repeat_.get()}, {water, nearest_.get()}};
        SDL_BindGPUComputeSamplers(pass, 0, samplers, 6);
        const auto river = rivers.buffers();
        SDL_BindGPUComputeStorageBuffers(pass, 0, river.data(), 3);
        SDL_PushGPUComputeUniformData(commands, 1, &ground, sizeof(ground));
        SDL_PushGPUComputeUniformData(commands, 2, &light, sizeof(light));
        const double range = params_.macroHeightRange, sea = params_.macroSeaLevel;
        candidates_ = 0;
        double radius = settings_.firstRadius, spacing = settings_.firstSpacing, inner = 0;
        for (std::uint32_t ring = 0; ring < settings_.rings; ++ring) {
            SpawnUniforms u{};
            for (int k = 0; k < 6; ++k)
                for (int j = 0; j < 4; ++j) u.planes[k][j] = frame.frustum[k][j];
            const double tx = frame.eye.x / params_.spacing, ty = frame.eye.y / params_.spacing;
            u.camTexel[0] = std::int32_t(std::floor(tx));
            u.camTexel[1] = std::int32_t(std::floor(ty));
            u.camTexelFrac[0] = float(tx - std::floor(tx));
            u.camTexelFrac[1] = float(ty - std::floor(ty));
            const double cx = frame.eye.x / spacing, cy = frame.eye.y / spacing;
            u.camCell[0] = std::int32_t(std::floor(cx));
            u.camCell[1] = std::int32_t(std::floor(cy));
            u.camCellFrac[0] = float(cx - std::floor(cx));
            u.camCellFrac[1] = float(cy - std::floor(cy));
            u.eyeZ = float(frame.eye.z);
            u.spacing0 = float(params_.spacing);
            u.cellSize = float(spacing);
            u.inner = float(inner);
            u.outer = float(radius);
            u.tuftHeight = settings_.tuftHeight;
            u.tuftWidth = settings_.tuftWidth;
            u.pixelScale = frame.pixelScale;
            u.gridSide = std::uint32_t(std::ceil(2.0 * radius / spacing)) + 2;
            u.clipSize = params_.clipSize;
            u.levels = params_.levels;
            u.capacity = settings_.capacity;
            u.seed = params_.seed * 131u + ring * 7919u;
            u.heightLow = float(-sea * range - params_.detailBound() - frame.eye.z);
            u.heightHigh = float((1.0 - sea) * range + params_.detailBound() - frame.eye.z);
            u.flowerChance = settings_.flowerChance;
            u.waterMapSize = float(waterSize);
            u.worldSize = float(params_.worldSize);
            for (int i = 0; i < 3; ++i) {
                u.sunDirection[i] = frame.sunDirection[i];
                u.skyColour[i] = frame.skyColour[i];
            }
            candidates_ += std::uint64_t(u.gridSide) * u.gridSide;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_DispatchGPUCompute(pass, (u.gridSide + 7) / 8, (u.gridSide + 7) / 8, 1);
            inner = radius;
            radius *= 2.0;
            spacing *= 2.0;
        }
        SDL_EndGPUComputePass(pass);
    }
    {
        SDL_GPUStorageBufferReadWriteBinding rw[2] = {{count_.get(), false, {}}, {args_.get(), false, {}}};
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2);
        SDL_BindGPUComputePipeline(pass, argsPass_.get());
        SDL_PushGPUComputeUniformData(commands, 0, capacity, sizeof(capacity));
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
    }
    {
        auto* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUBufferRegion from{count_.get(), 0, 4};
        const SDL_GPUTransferBufferLocation to{readback_[frame_ % 3], 0};
        SDL_DownloadFromGPUBuffer(copy, &from, &to);
        SDL_EndGPUCopyPass(copy);
    }
    ++frame_;
}

void Grass::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const PlantView& view,
                 const LightUniforms& light, SDL_GPUTexture* shadowAtlas, SDL_GPUSampler* shadowSampler) {
    SDL_BindGPUGraphicsPipeline(pass, draw_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    const SDL_GPUBufferBinding instances{tufts_.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 0, &instances, 1);
    const SDL_GPUTextureSamplerBinding textures[2] = {{sprites_.get(), linear_.get()}, {shadowAtlas, shadowSampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
    SDL_DrawGPUPrimitivesIndirect(pass, args_.get(), 0, 1);
}

} // namespace terrain_core
