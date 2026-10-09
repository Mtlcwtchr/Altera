#include "terrain_core/renderer.hpp"

#include <algorithm>
#include <cstring>

namespace terrain_core {
namespace {

// Must match cbuffer Generate in assets/shaders/terrain_core/height.hlsli.
struct GenerateUniforms {
    std::int32_t rectOrigin[2];
    std::uint32_t rectSize[2];
    std::uint32_t level, clipSize, seed, macroSize;
    float spacing, worldSize, macroHeightRange, macroSeaLevel;
    float detailAmplitude;
    std::uint32_t octaveTop;
    std::uint32_t macroMips;
    float pad;
};
static_assert(sizeof(GenerateUniforms) == 64);

} // namespace

bool Renderer::open(engine::Device& device, const Params& params, const MacroMap& macro, const BiomeTable& table,
                    const BiomeMap& biomes) {
    device_ = &device;
    params_ = params;
    macroSize_ = macro.size();
    gpuBytes_ = 0;

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = macroSize_;
    info.layer_count_or_depth = 1;
    // A full mip chain, box-filtered here: the far clipmap levels read the
    // map at their own scale (see terrainHeight).
    std::vector<std::vector<std::uint16_t>> mips{macro.texels()};
    for (std::uint32_t size = macroSize_; size > 1;) {
        const std::uint32_t next = std::max(1u, size / 2);
        const auto& fine = mips.back();
        std::vector<std::uint16_t> coarse(std::size_t(next) * next);
        for (std::uint32_t y = 0; y < next; ++y)
            for (std::uint32_t x = 0; x < next; ++x) {
                // Half the mean, half the highest: a plain mean sinks every
                // crest a little more each level, and a far range came out
                // hundreds of metres lower than it stands.
                std::uint32_t sum = 0, top = 0;
                for (std::uint32_t k = 0; k < 4; ++k) {
                    const std::uint32_t fx = std::min(size - 1, x * 2 + (k & 1)), fy = std::min(size - 1, y * 2 + (k >> 1));
                    const std::uint32_t v = fine[std::size_t(fy) * size + fx];
                    sum += v;
                    top = std::max(top, v);
                }
                coarse[std::size_t(y) * next + x] = std::uint16_t((sum + 4 * top + 4) / 8);
            }
        mips.push_back(std::move(coarse));
        size = next;
    }
    macroMips_ = std::uint32_t(mips.size());
    info.num_levels = macroMips_;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    macro_ = device.makeTexture(info);
    if (!macro_) return false;
    {
        // In strips of at most 32 MB: a 16k map (512 MB) in one staging
        // buffer is more host-visible memory than a driver always gives.
        for (std::uint32_t m = 0; m < macroMips_; ++m) {
            const std::uint32_t size = std::max(1u, macroSize_ >> m);
            const std::uint32_t rows = std::max(1u, std::min(size, (32u << 20) / (size * 2)));
            for (std::uint32_t y = 0; y < size; y += rows) {
                engine::Device::Uploader upload(device);
                const std::uint32_t h = std::min(rows, size - y);
                if (!upload.refillRegion(macro_.get(), mips[m].data() + std::size_t(y) * size, 0, y, size, h, 2, 0, m) ||
                    !upload.finish())
                    return false;
            }
            gpuBytes_ += std::uint64_t(size) * size * 2;
        }
    }
    info.num_levels = 1;

    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    info.width = info.height = params.clipSize;
    info.layer_count_or_depth = params.levels;
    clipmap_ = device.makeTexture(info);
    if (!clipmap_) return false;
    gpuBytes_ += params.clipmapBytes();

    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampling.max_lod = 1000.0f;   // the default of 0 would pin every read to mip 0
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_ = device.makeSampler(sampling);
    // The ground pictures: repeating, trilinear, anisotropic - the ground is
    // seen at a grazing angle almost everywhere.
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampling.enable_anisotropy = true;
    sampling.max_anisotropy = 8.0f;
    repeat_ = device.makeSampler(sampling);

    // Biome ids, one byte a texel; read through the nearest sampler as UNORM
    // and turned back into an id in the shader.
    {
        SDL_GPUTextureCreateInfo ids{};
        ids.type = SDL_GPU_TEXTURETYPE_2D;
        ids.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        ids.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ids.width = ids.height = biomes.size();
        ids.layer_count_or_depth = 1;
        ids.num_levels = 1;
        biomeMap_ = device.makeTexture(ids);
        if (!biomeMap_) return false;
        engine::Device::Uploader upload(device);
        if (!upload.refillRegion(biomeMap_.get(), biomes.ids().data(), 0, 0, biomes.size(), biomes.size(), 1) ||
            !upload.finish())
            return false;
        gpuBytes_ += std::uint64_t(biomes.size()) * biomes.size();
    }
    std::vector<std::vector<std::filesystem::path>> layers;
    for (const auto& file : table.materialFiles()) layers.push_back({file});
    materials_ = device.loadArrayMipped(layers, true);
    if (!materials_) return false;
    gpuBytes_ += std::uint64_t(layers.size()) * 128 * 128 * 4 * 4 / 3;
    ground_ = table.uniforms();
    ground_.biomeSize = biomes.size();

    engine::ComputeWanted compute;
    compute.shaderFile = "terrain_core/generate.hlsl";
    compute.entry = "generateCs";
    generate_ = device.makeCompute(compute);

    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_core/terrain.hlsl";
    wanted.vertexEntry = "terrainVs";
    wanted.fragmentEntry = "terrainPs";
    wanted.depthClip = true;
    wanted.cull = SDL_GPU_CULLMODE_NONE;
    wanted.buffers = {{0, sizeof(Patch), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 0},
                         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16}};
    terrain_ = device.makePipeline(wanted);
    if (!nearest_ || !generate_ || !terrain_) return false;

    // The grid's index buffer, quarter by quarter, so that each quarter is one
    // contiguous range and a half-drawn node is a draw of two or three of them.
    const std::uint32_t g = params.grid, half = g / 2, side = g + 1;
    std::vector<std::uint32_t> indices;
    indices.reserve(std::size_t(g) * g * 6);
    for (std::uint32_t q = 0; q < 4; ++q) {
        const std::uint32_t qx = (q & 1) * half, qy = (q >> 1) * half;
        for (std::uint32_t y = qy; y < qy + half; ++y)
            for (std::uint32_t x = qx; x < qx + half; ++x) {
                const std::uint32_t a = y * side + x, b = a + 1, c = a + side, d = c + 1;
                indices.insert(indices.end(), {a, b, d, a, d, c});
            }
    }
    quarterIndices_ = half * half * 6;
    indices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), indices.size() * 4);
    gpuBytes_ += indices.size() * 4;
    return bool(indices_);
}

std::uint64_t Renderer::generate(SDL_GPUCommandBuffer* commands, const std::vector<Rect>& rects) {
    std::uint64_t texels = 0;
    for (std::uint32_t level = 0; level < params_.levels; ++level) {
        bool any = false;
        for (const Rect& r : rects) any = any || r.level == level;
        if (!any) continue;
        SDL_GPUStorageTextureReadWriteBinding target{};
        target.texture = clipmap_.get();
        target.layer = level;
        target.mip_level = 0;
        auto* pass = SDL_BeginGPUComputePass(commands, &target, 1, nullptr, 0);
        if (!pass) continue;
        SDL_BindGPUComputePipeline(pass, generate_.get());
        const SDL_GPUTextureSamplerBinding macro{macro_.get(), nearest_.get()};
        SDL_BindGPUComputeSamplers(pass, 0, &macro, 1);
        for (const Rect& r : rects) {
            if (r.level != level) continue;
            GenerateUniforms u{};
            u.rectOrigin[0] = r.x;
            u.rectOrigin[1] = r.y;
            u.rectSize[0] = r.w;
            u.rectSize[1] = r.h;
            u.level = level;
            u.clipSize = params_.clipSize;
            u.seed = params_.seed;
            u.macroSize = macroSize_;
            u.spacing = float(params_.spacing);
            u.worldSize = float(params_.worldSize);
            u.macroHeightRange = float(params_.macroHeightRange);
            u.macroSeaLevel = float(params_.macroSeaLevel);
            u.detailAmplitude = float(params_.detailAmplitude);
            u.octaveTop = params_.octaveTop();
            u.macroMips = macroMips_;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_DispatchGPUCompute(pass, (r.w + 7) / 8, (r.h + 7) / 8, 1);
            texels += std::uint64_t(r.w) * r.h;
        }
        SDL_EndGPUComputePass(pass);
    }
    return texels;
}

Renderer::~Renderer() {
    if (device_ && patchStaging_) SDL_ReleaseGPUTransferBuffer(device_->handle(), patchStaging_);
}

bool Renderer::upload(SDL_GPUCommandBuffer* commands, const Selection& selection) {
    staging_.clear();
    for (std::uint32_t q = 0; q < 4; ++q) {
        firsts_[q] = std::uint32_t(staging_.size());
        counts_[q] = std::uint32_t(selection.byQuarter[q].size());
        staging_.insert(staging_.end(), selection.byQuarter[q].begin(), selection.byQuarter[q].end());
    }
    triangles_ = std::uint32_t(staging_.size()) * quarterIndices_ / 3;
    if (staging_.empty()) return true;
    if (staging_.size() > patchCapacity_) {
        patchCapacity_ = std::max<std::uint32_t>(1024, std::uint32_t(staging_.size()) * 2);
        const auto bytes = std::uint32_t(std::size_t(patchCapacity_) * sizeof(Patch));
        patches_ = device_->makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, bytes);
        if (patchStaging_) SDL_ReleaseGPUTransferBuffer(device_->handle(), patchStaging_);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = bytes;
        patchStaging_ = SDL_CreateGPUTransferBuffer(device_->handle(), &info);
        if (!patches_ || !patchStaging_) return false;
    }
    const auto bytes = std::uint32_t(staging_.size() * sizeof(Patch));
    // Cycling: while the card still reads last frame's copy SDL hands out
    // another, so this never waits and never allocates in steady state.
    void* mapped = SDL_MapGPUTransferBuffer(device_->handle(), patchStaging_, true);
    if (!mapped) return false;
    std::memcpy(mapped, staging_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_->handle(), patchStaging_);
    auto* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation from{patchStaging_, 0};
    const SDL_GPUBufferRegion to{patches_.get(), 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

void Renderer::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const ViewUniforms& view,
                    const LookUniforms& look) {
    if (staging_.empty()) return;
    SDL_BindGPUGraphicsPipeline(pass, terrain_.get());
    SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
    SDL_PushGPUFragmentUniformData(commands, 0, &look, sizeof(look));
    SDL_PushGPUFragmentUniformData(commands, 1, &ground_, sizeof(ground_));
    const SDL_GPUTextureSamplerBinding clip{clipmap_.get(), nearest_.get()};
    SDL_BindGPUVertexSamplers(pass, 0, &clip, 1);
    const SDL_GPUTextureSamplerBinding fragment[3] = {
            clip, {biomeMap_.get(), nearest_.get()}, {materials_.get(), repeat_.get()}};
    SDL_BindGPUFragmentSamplers(pass, 0, fragment, 3);
    const SDL_GPUBufferBinding index{indices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (std::uint32_t q = 0; q < 4; ++q) {
        if (!counts_[q]) continue;
        const SDL_GPUBufferBinding instances{patches_.get(), firsts_[q] * std::uint32_t(sizeof(Patch))};
        SDL_BindGPUVertexBuffers(pass, 0, &instances, 1);
        SDL_DrawGPUIndexedPrimitives(pass, quarterIndices_, counts_[q], q * quarterIndices_, 0, 0);
    }
}

} // namespace terrain_core
