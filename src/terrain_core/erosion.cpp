#include "terrain_core/erosion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace terrain_core {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Must match cbuffer Erode in assets/shaders/terrain_core/erosion.hlsl.
struct ErodeUniforms {
    std::uint32_t size, sourceSize, scale, seed;
    float unitsPerTexelHeight, seaUnits, detailTexels, talus;
    float rate, pad[3];
};
static_assert(sizeof(ErodeUniforms) == 48);

constexpr double kUnits = 16777216.0;   // 2^24 height units = the source's whole range

bool submitAndWait(engine::Device& device, SDL_GPUCommandBuffer* commands) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) return false;
    SDL_WaitForGPUFences(device.handle(), true, &fence, 1);
    SDL_ReleaseGPUFence(device.handle(), fence);
    return true;
}

} // namespace

bool erode(engine::Device& device, const MacroMap& source, const Params& params, const ErosionSettings& settings,
           MacroMap& out, ErosionTimings& timings, std::string* why) {
    auto fail = [&](const std::string& what) {
        if (why) *why = what + (device.error().empty() ? "" : ": " + device.error());
        return false;
    };
    const std::uint32_t sourceSize = source.size();
    const std::uint32_t size = sourceSize * settings.scale;
    const std::uint64_t texels = std::uint64_t(size) * size;

    auto start = Clock::now();
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = sourceSize;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    engine::Texture sourceTexture = device.makeTexture(info);
    if (!sourceTexture) return fail("source texture");
    {
        engine::Device::Uploader upload(device);
        if (!upload.refillRegion(sourceTexture.get(), source.texels().data(), 0, 0, sourceSize, sourceSize, 2) ||
            !upload.finish())
            return fail("source upload");
    }
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    engine::Sampler nearest = device.makeSampler(sampling);
    engine::Buffer heights = device.makeBuffer(
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, texels * 4);
    if (!heights) return fail("height buffer of " + std::to_string(texels * 4 >> 20) + " MB");
    engine::ComputePipeline upscale = device.makeCompute({"terrain_core/erosion.hlsl", "upscaleCs"});
    engine::ComputePipeline thermal = device.makeCompute({"terrain_core/erosion.hlsl", "thermalCs"});
    if (!upscale || !thermal || !nearest) return fail("erosion shaders");
    timings.uploadMs = msSince(start);

    ErodeUniforms u{};
    u.size = size;
    u.sourceSize = sourceSize;
    u.scale = settings.scale;
    u.seed = params.seed;
    const double texelMetres = params.worldSize / double(size);
    u.unitsPerTexelHeight = float(texelMetres / (params.macroHeightRange / kUnits));
    u.seaUnits = float(params.macroSeaLevel * kUnits);
    u.detailTexels = float(settings.detail);
    u.talus = float(std::tan(settings.talusDegrees * 3.14159265358979 / 180.0));
    u.rate = float(settings.rate);

    SDL_GPUStorageBufferReadWriteBinding target{};
    target.buffer = heights.get();
    auto dispatch = [&](SDL_GPUCommandBuffer* commands, SDL_GPUComputePipeline* pipeline, bool sample) {
        auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &target, 1);
        SDL_BindGPUComputePipeline(pass, pipeline);
        if (sample) {
            const SDL_GPUTextureSamplerBinding binding{sourceTexture.get(), nearest.get()};
            SDL_BindGPUComputeSamplers(pass, 0, &binding, 1);
        }
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        SDL_DispatchGPUCompute(pass, (size + 7) / 8, (size + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
    };

    start = Clock::now();
    {
        auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
        dispatch(commands, upscale.get(), true);
        if (!submitAndWait(device, commands)) return fail("upscale");
    }
    timings.upscaleMs = msSince(start);

    // Thermal passes, a few to a submission: one long submission would trip
    // Windows' two-second GPU watchdog on a big map.
    start = Clock::now();
    std::uint32_t perSubmit = 1;
    for (std::uint32_t done = 0; done < settings.iterations;) {
        const auto batchStart = Clock::now();
        const std::uint32_t n = std::min(perSubmit, settings.iterations - done);
        auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
        for (std::uint32_t i = 0; i < n; ++i) dispatch(commands, thermal.get(), false);
        if (!submitAndWait(device, commands)) return fail("thermal");
        done += n;
        const double ms = msSince(batchStart);
        perSubmit = std::clamp<std::uint32_t>(std::uint32_t(300.0 / std::max(ms / n, 0.1)), 1, 64);
    }
    timings.thermalMs = msSince(start);

    // Back in chunks: one 1 GiB download buffer is more host-visible memory
    // than a driver will always give.
    start = Clock::now();
    std::vector<std::int32_t> units(texels);
    {
        const std::uint32_t chunk = 64u << 20;
        SDL_GPUTransferBufferCreateInfo create{};
        create.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        create.size = chunk;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device.handle(), &create);
        if (!transfer) return fail("download buffer");
        bool ok = true;
        for (std::uint64_t offset = 0; ok && offset < texels * 4; offset += chunk) {
            const auto bytes = std::uint32_t(std::min<std::uint64_t>(chunk, texels * 4 - offset));
            auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
            auto* copy = SDL_BeginGPUCopyPass(commands);
            const SDL_GPUBufferRegion region{heights.get(), std::uint32_t(offset), bytes};
            const SDL_GPUTransferBufferLocation location{transfer, 0};
            SDL_DownloadFromGPUBuffer(copy, &region, &location);
            SDL_EndGPUCopyPass(copy);
            ok = submitAndWait(device, commands);
            if (void* mapped = ok ? SDL_MapGPUTransferBuffer(device.handle(), transfer, false) : nullptr) {
                std::memcpy(reinterpret_cast<std::uint8_t*>(units.data()) + offset, mapped, bytes);
                SDL_UnmapGPUTransferBuffer(device.handle(), transfer);
            } else {
                ok = false;
            }
        }
        SDL_ReleaseGPUTransferBuffer(device.handle(), transfer);
        if (!ok) return fail("readback");
    }
    timings.readbackMs = msSince(start);

    start = Clock::now();
    std::vector<std::uint16_t> words(texels);
    for (std::uint64_t i = 0; i < texels; ++i)
        words[i] = std::uint16_t(std::clamp(double(units[i]) / kUnits, 0.0, 1.0) * 65535.0 + 0.5);
    units.clear();
    units.shrink_to_fit();
    out.assign(size, std::move(words));
    timings.convertMs = msSince(start);
    return true;
}

} // namespace terrain_core
