#include "terrain_core/outline.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>

namespace terrain_core {

bool loadOutlineSettings(const std::filesystem::path& file, OutlineSettings& out, std::string* why) {
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
    out.width = std::clamp(config.value("width", out.width), 0.5f, 8.0f);
    out.depthEdge = std::max(1e-4f, config.value("depth_edge", out.depthEdge));
    out.creaseEdge = std::max(1e-4f, config.value("crease_edge", out.creaseEdge));
    out.creaseStrength = std::clamp(config.value("crease_strength", out.creaseStrength), 0.0f, 1.0f);
    out.opacity = std::clamp(config.value("opacity", out.opacity), 0.0f, 1.0f);
    out.fadeStart = std::max(0.0f, config.value("fade_start", out.fadeStart));
    out.fadeEnd = std::max(out.fadeStart + 1.0f, config.value("fade_end", out.fadeEnd));
    out.farStrength = std::clamp(config.value("far_strength", out.farStrength), 0.0f, 1.0f);
    out.roughness = std::clamp(config.value("roughness", out.roughness), 0.0f, 1.0f);
    out.inkShowThrough = std::clamp(config.value("ink_show_through", out.inkShowThrough), 0.0f, 1.0f);
    if (config.contains("ink") && config["ink"].is_array() && config["ink"].size() == 3)
        for (int i = 0; i < 3; ++i) out.ink[i] = std::clamp(config["ink"][i].get<float>(), 0.0f, 1.0f);
    return true;
}

bool Outline::open(engine::Device& device) {
    device_ = &device;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/outline.hlsl";
    wanted.vertexEntry = "outlineVs";
    wanted.fragmentEntry = "outlinePs";
    wanted.depthTest = wanted.depthWrite = false;
    wanted.colourFormats = {engine::Device::kColourFormat};
    pipeline_ = device.makePipeline(wanted);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampling);
    return pipeline_ && sampler_;
}

void Outline::apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, SDL_GPUTexture* depth, float nearPlane,
                    const OutlineSettings& settings) {
    if (!settings.enabled || !pipeline_ || !depth || !targets.width() || !targets.height()) return;
    const std::uint32_t w = targets.width(), h = targets.height();
    if (!copy_ || width_ != w || height_ != h) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = engine::Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        copy_ = device_->makeTexture(info);
        width_ = w;
        height_ = h;
        if (!copy_) return;
    }
    // The picture as it is, to read while the frame is drawn over.
    SDL_GPUBlitInfo blit{};
    blit.source.texture = targets.resolved();
    blit.source.w = w;
    blit.source.h = h;
    blit.destination.texture = copy_.get();
    blit.destination.w = w;
    blit.destination.h = h;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);

    SDL_GPUColorTargetInfo colour{};
    colour.texture = targets.resolved();
    colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    const SDL_GPUTextureSamplerBinding sources[2] = {{copy_.get(), sampler_.get()}, {depth, sampler_.get()}};
    SDL_BindGPUFragmentSamplers(pass, 0, sources, 2);
    const struct {
        float source[2];
        float nearPlane, width;
        float depthEdge, creaseEdge, creaseStrength, opacity;
        float fadeStart, fadeEnd, farStrength, roughness;
        float ink[4];
    } u{{float(w), float(h)},
        nearPlane,
        settings.width,
        settings.depthEdge,
        settings.creaseEdge,
        settings.creaseStrength,
        settings.opacity,
        settings.fadeStart,
        settings.fadeEnd,
        settings.farStrength,
        settings.roughness,
        {settings.ink[0], settings.ink[1], settings.ink[2], settings.inkShowThrough}};
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace terrain_core
