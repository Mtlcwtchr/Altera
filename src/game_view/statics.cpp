#include "game_view/statics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace altera::game {
namespace {

namespace fs = std::filesystem;

struct Row {
    float placeYaw[4], scale[4], tint[4];
};
static_assert(sizeof(Row) == 48);

} // namespace

engine::Texture uploadRgbaArray(engine::Device& device, const std::uint8_t* rgba, std::uint32_t width,
                                std::uint32_t height, std::uint32_t layers) {
    std::uint32_t levels = 1;
    while ((std::max(width, height) >> levels) > 0) ++levels;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = layers;
    info.num_levels = levels;
    engine::Texture tex = device.makeTexture(info);
    if (!tex) return tex;
    const std::uint32_t bytes = width * height * 4 * layers;
    SDL_GPUTransferBufferCreateInfo t{};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device.handle(), &t);
    if (!tb) return {};
    if (void* map = SDL_MapGPUTransferBuffer(device.handle(), tb, false)) {
        std::memcpy(map, rgba, bytes);
        SDL_UnmapGPUTransferBuffer(device.handle(), tb);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device.handle());
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cb);
        for (std::uint32_t l = 0; l < layers; ++l) {
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = tb;
            src.offset = l * width * height * 4;
            src.pixels_per_row = width;
            src.rows_per_layer = height;
            SDL_GPUTextureRegion dst{};
            dst.texture = tex.get();
            dst.layer = l;
            dst.w = width;
            dst.h = height;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cb, tex.get());
        SDL_SubmitGPUCommandBuffer(cb);
    }
    SDL_ReleaseGPUTransferBuffer(device.handle(), tb);
    return tex;
}

StaticSet::~StaticSet() {
    if (device_ && transfer_) SDL_ReleaseGPUTransferBuffer(device_->handle(), transfer_);
}

bool StaticSet::load(engine::Device& device, const fs::path& file, std::string* why) {
    device_ = &device;
    auto fail = [&](const std::string& s) {
        if (why) *why = s;
        return false;
    };
    std::ifstream f(file, std::ios::binary);
    if (!f) return fail("cannot read " + file.string());
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t at = 0;
    auto take = [&](std::size_t n) -> const std::uint8_t* {
        if (at + n > b.size()) return nullptr;
        const std::uint8_t* p = b.data() + at;
        at += n;
        return p;
    };
    auto u32 = [&]() {
        std::uint32_t v = 0;
        if (const auto* p = take(4)) std::memcpy(&v, p, 4);
        return v;
    };
    const auto* magic = take(4);
    if (!magic || std::memcmp(magic, "ASTM", 4) != 0 || u32() != 1) return fail("not an ASTM v1 file: " + file.string());
    const std::uint32_t nm = u32(), nv = u32(), ni = u32(), nl = u32(), ls = u32();
    pieces_.resize(nm);
    for (auto& p : pieces_) {
        const auto* n = take(32);
        if (!n) return fail("truncated " + file.string());
        p.name.assign(reinterpret_cast<const char*>(n), strnlen(reinterpret_cast<const char*>(n), 32));
        p.firstIndex = u32();
        p.indexCount = u32();
        const auto* bb = take(24);
        if (!bb) return fail("truncated " + file.string());
        std::memcpy(p.min, bb, 12);
        std::memcpy(p.max, bb + 12, 12);
    }
    constexpr std::size_t kStride = 36;
    const auto* verts = take(std::size_t(nv) * kStride);
    const auto* idx = take(std::size_t(ni) * 4);
    const auto* pixels = take(std::size_t(nl) * ls * ls * 4);
    if (!verts || !idx || !pixels) return fail("truncated " + file.string());

    vertices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, verts, std::size_t(nv) * kStride);
    indices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, idx, std::size_t(ni) * 4);
    layers_ = uploadRgbaArray(device, pixels, ls, ls, nl);
    instances_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, kMaxPlaced * sizeof(Row));
    SDL_GPUTransferBufferCreateInfo t{};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = std::uint32_t(kMaxPlaced * sizeof(Row));
    transfer_ = SDL_CreateGPUTransferBuffer(device.handle(), &t);
    SDL_GPUSamplerCreateInfo s{};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.max_lod = 16;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_ = device.makeSampler(s);

    engine::PipelineWanted w;
    w.shaderFile = "altera_game/statics.hlsl";
    w.vertexEntry = "staticsVs";
    w.fragmentEntry = "staticsPs";
    w.depthClip = true;
    w.cull = SDL_GPU_CULLMODE_NONE;
    w.buffers = {{0, std::uint32_t(kStride), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                 {1, sizeof(Row), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    w.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},  {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},  {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 32},
                    {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},   {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                    {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32}};
    pipeline_ = device.makePipeline(w);
    if (!vertices_ || !indices_ || !layers_ || !instances_ || !transfer_ || !sampler_ || !pipeline_)
        return fail("static GPU objects (" + file.filename().string() + "): " + device.error());
    return true;
}

int StaticSet::piece(const std::string& name) const {
    for (std::size_t i = 0; i < pieces_.size(); ++i)
        if (pieces_[i].name == name) return int(i);
    return -1;
}

void StaticSet::halfSize(int p, float& hx, float& hy) const {
    hx = hy = 1;
    if (p < 0 || p >= int(pieces_.size())) return;
    const Piece& q = pieces_[std::size_t(p)];
    hx = std::max(std::fabs(q.min[0]), std::fabs(q.max[0]));
    hy = std::max(std::fabs(q.min[1]), std::fabs(q.max[1]));
}

void StaticSet::prepare(SDL_GPUCommandBuffer* commands, const std::vector<Placed>& placed) {
    batches_.clear();
    if (!pipeline_ || placed.empty()) return;
    std::vector<std::uint32_t> order(placed.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return placed[a].piece < placed[b].piece; });
    const std::size_t n = std::min(order.size(), kMaxPlaced);
    auto* rows = static_cast<Row*>(SDL_MapGPUTransferBuffer(device_->handle(), transfer_, true));
    if (!rows) return;
    for (std::size_t i = 0; i < n; ++i) {
        const Placed& p = placed[order[i]];
        Row& r = rows[i];
        r.placeYaw[0] = float(p.rel[0]), r.placeYaw[1] = float(p.rel[1]), r.placeYaw[2] = float(p.rel[2]);
        r.placeYaw[3] = p.yaw;
        r.scale[0] = r.scale[1] = r.scale[2] = p.scale;
        r.scale[3] = p.highlight;
        r.tint[0] = p.tint[0], r.tint[1] = p.tint[1], r.tint[2] = p.tint[2], r.tint[3] = 1;
        if (batches_.empty() || batches_.back().piece != p.piece) batches_.push_back({p.piece, std::uint32_t(i), 0});
        ++batches_.back().count;
    }
    SDL_UnmapGPUTransferBuffer(device_->handle(), transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation from{transfer_, 0};
    const SDL_GPUBufferRegion to{instances_.get(), 0, std::uint32_t(n * sizeof(Row))};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
}

void StaticSet::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const float vp[16],
                     const terrain_core::LightUniforms& light) {
    if (!pipeline_ || batches_.empty()) return;
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    const SDL_GPUBufferBinding bufs[2] = {{vertices_.get(), 0}, {instances_.get(), 0}};
    SDL_BindGPUVertexBuffers(pass, 0, bufs, 2);
    const SDL_GPUBufferBinding ib{indices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const SDL_GPUTextureSamplerBinding tex{layers_.get(), sampler_.get()};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(commands, 0, vp, 64);
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof light);
    for (const Batch& b : batches_) {
        if (b.piece < 0 || b.piece >= int(pieces_.size())) continue;
        const Piece& p = pieces_[std::size_t(b.piece)];
        SDL_DrawGPUIndexedPrimitives(pass, p.indexCount, b.count, p.firstIndex, 0, b.first);
    }
}

} // namespace altera::game
