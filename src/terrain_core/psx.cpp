#include "terrain_core/psx.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace terrain_core {

bool loadPsxSettings(const std::filesystem::path& file, PsxSettings& out, std::string* why) {
    std::ifstream in(file);
    if (!in) {
        if (why) *why = "cannot open " + file.string();
        return false;
    }
    const auto config = nlohmann::json::parse(in, nullptr, false);
    if (config.is_discarded() || !config.is_object()) {
        if (why) *why = "bad json in " + file.string();
        return false;
    }
    out.enabled = config.value("enabled", out.enabled);
    out.pixelScale = std::clamp(config.value("pixel_scale", int(out.pixelScale)), 1, 16);
    out.colourBits = std::clamp(config.value("colour_bits", int(out.colourBits)), 1, 8);
    out.dither = std::max(0.0f, config.value("dither", out.dither));
    out.grain = std::max(0.0f, config.value("grain", out.grain));
    out.grainAnimated = config.value("grain_animated", out.grainAnimated);
    return true;
}

bool Psx::open(engine::Device& device) {
    device_ = &device;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/psx.hlsl";
    wanted.vertexEntry = "psxVs";
    wanted.fragmentEntry = "psxPs";
    wanted.depthTest = wanted.depthWrite = false;
    wanted.colourFormats = {engine::Device::kColourFormat};
    pipeline_ = device.makePipeline(wanted);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampling);
    return pipeline_ && sampler_;
}

void Psx::apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, const PsxSettings& settings, float time) {
    if (!settings.enabled || !pipeline_ || !targets.width() || !targets.height()) return;
    const std::uint32_t scale = std::max(1u, settings.pixelScale);
    const std::uint32_t w = std::max(1u, targets.width() / scale), h = std::max(1u, targets.height() / scale);
    if (!small_ || width_ != w || height_ != h) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = engine::Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        small_ = device_->makeTexture(info);
        width_ = w;
        height_ = h;
        if (!small_) return;
    }
    // Down: each low pixel is the mean of its block, then dither, grain, cut.
    SDL_GPUColorTargetInfo colour{};
    colour.texture = small_.get();
    colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    const SDL_GPUTextureSamplerBinding source{targets.resolved(), sampler_.get()};
    SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
    const struct {
        float source[2];
        std::uint32_t scale, bits;
        float dither, grain, time, pad;
    } u{{float(targets.width()), float(targets.height())},
        scale,
        std::clamp(settings.colourBits, 1u, 8u),
        settings.dither,
        settings.grain,
        settings.grainAnimated ? time : 0.0f,
        0.0f};
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    // Up: hard pixels, back into the frame's own target.
    SDL_GPUBlitInfo blit{};
    blit.source.texture = small_.get();
    blit.source.w = width_;
    blit.source.h = height_;
    blit.destination.texture = targets.resolved();
    blit.destination.w = width_ * scale;
    blit.destination.h = height_ * scale;
    blit.load_op = SDL_GPU_LOADOP_LOAD;   // a remainder strip at the edge keeps the frame
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);
}

} // namespace terrain_core
