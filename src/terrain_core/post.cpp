#include "terrain_core/post.hpp"

namespace terrain_core {

const char* Post::name(std::uint32_t style) {
    switch (style) {
    case 1: return "dark fantasy";
    case 2: return "Frazetta";
    case 3: return "Vallejo";
    default: return "off";
    }
}

bool Post::open(engine::Device& device) {
    device_ = &device;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/post.hlsl";
    wanted.vertexEntry = "postVs";
    wanted.fragmentEntry = "postPs";
    wanted.depthTest = wanted.depthWrite = false;
    wanted.colourFormats = {engine::Device::kColourFormat};
    pipeline_ = device.makePipeline(wanted);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampling);
    return pipeline_ && sampler_;
}

void Post::apply(SDL_GPUCommandBuffer* commands, engine::Targets& targets, std::uint32_t style, float time,
                 SDL_GPUTexture* clouds) {
    if (style >= kStyles || !pipeline_ || (style == 0 && !clouds)) return;
    if (!scratch_ || width_ != targets.width() || height_ != targets.height()) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = engine::Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = targets.width();
        info.height = targets.height();
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        scratch_ = device_->makeTexture(info);
        width_ = targets.width();
        height_ = targets.height();
        if (!scratch_) return;
    }
    SDL_GPUColorTargetInfo colour{};
    colour.texture = scratch_.get();
    colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    // Without clouds the cloud slot still needs a texture: the frame itself, unread.
    const SDL_GPUTextureSamplerBinding sources[2] = {{targets.resolved(), sampler_.get()},
                                                     {clouds ? clouds : targets.resolved(), sampler_.get()}};
    SDL_BindGPUFragmentSamplers(pass, 0, sources, 2);
    const struct { std::uint32_t mode; float time; float resolution[2]; float cloudsOn, pad[3]; } u{
            style, time, {float(width_), float(height_)}, clouds ? 1.0f : 0.0f, {}};
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    // Back into the frame's own target, so nothing after this needs to know.
    SDL_GPUBlitInfo blit{};
    blit.source.texture = scratch_.get();
    blit.source.w = width_;
    blit.source.h = height_;
    blit.destination.texture = targets.resolved();
    blit.destination.w = width_;
    blit.destination.h = height_;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);
}

} // namespace terrain_core
