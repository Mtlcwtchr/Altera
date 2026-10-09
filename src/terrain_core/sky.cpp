#include "terrain_core/sky.hpp"

#include <cmath>

namespace terrain_core {

RayCamera rayCamera(const double f[3], double fovY, double aspect) {
    double r[3] = {f[1], -f[0], 0};
    const double rl = std::hypot(r[0], r[1]);
    r[0] /= rl;
    r[1] /= rl;
    const double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const double t = std::tan(fovY * 0.5);
    RayCamera c{};
    for (int i = 0; i < 3; ++i) {
        c.forward[i] = float(f[i]);
        c.right[i] = float(r[i] * t * aspect);
        c.up[i] = float(u[i] * t);
    }
    return c;
}

bool Sky::open(engine::Device& device) {
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/sky.hlsl";
    wanted.vertexEntry = "skyVs";
    wanted.fragmentEntry = "skyPs";
    wanted.depthTest = wanted.depthWrite = false;
    pipeline_ = device.makePipeline(wanted);
    return bool(pipeline_);
}

void Sky::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const LightUniforms& light,
               const RayCamera& camera) {
    if (!pipeline_) return;
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof(light));
    SDL_PushGPUFragmentUniformData(commands, 1, &camera, sizeof(camera));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

bool Clouds::open(engine::Device& device) {
    device_ = &device;
    pipeline_ = device.makeCompute({"terrain_core/clouds.hlsl", "cloudsCs"});
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = info.mag_filter = SDL_GPU_FILTER_NEAREST;
    info.address_mode_u = info.address_mode_v = info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(info);
    return pipeline_ && sampler_;
}

bool Clouds::march(SDL_GPUCommandBuffer* commands, engine::Targets& targets, const LightUniforms& light,
                   const RayCamera& camera, float nearPlane, std::uint32_t frame) {
    if (!pipeline_ || light.cloudCover <= 0) return false;
    const std::uint32_t w = targets.width() / 2, h = targets.height() / 2;
    if (!texture_ || w != width_ || h != height_) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        texture_ = device_->makeTexture(info);
        width_ = w;
        height_ = h;
        if (!texture_) return false;
    }
    SDL_GPUStorageTextureReadWriteBinding out{};
    out.texture = texture_.get();
    auto* pass = SDL_BeginGPUComputePass(commands, &out, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, pipeline_.get());
    const SDL_GPUTextureSamplerBinding depth{targets.depth(), sampler_.get()};
    SDL_BindGPUComputeSamplers(pass, 0, &depth, 1);
    struct { RayCamera camera; float info[4]; } u{camera, {nearPlane, float(targets.width()), float(targets.height()),
                                                           float(frame % 64u)}};
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    SDL_PushGPUComputeUniformData(commands, 1, &light, sizeof(light));
    SDL_DispatchGPUCompute(pass, (w + 7) / 8, (h + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

} // namespace terrain_core
