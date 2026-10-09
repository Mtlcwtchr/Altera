#include "game/render/terrain_material_cache.hpp"

#include "engine/render/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace game {
namespace {

constexpr std::size_t kRebuildBudget = 48 * 1024;    // texels a frame for rebuilds and refreshes
// While a whole level is being rebuilt (a new world, a teleport) the ground at
// the feet is the coarse fallback - mush - until the fine levels arrive, and at
// the steady budget that was four seconds of it. A rebuild buys them faster:
// about three milliseconds of the card a frame, for under a second.
constexpr std::size_t kRebuildBurst = 160 * 1024;
// Strips a level may need in one frame before it is cheaper to let it go and
// rebuild it within the budget (the shader reads the next level meanwhile):
// what a camera moving fast past the finest levels would otherwise cost.
constexpr std::int64_t kStripLimit = 64 * 1024;
constexpr std::int64_t kStrip = 64;                    // rows of a rebuild queued at once

std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
std::int64_t wrap(std::int64_t a) { return ((a % TerrainMaterialCache::kSize) + TerrainMaterialCache::kSize) % TerrainMaterialCache::kSize; }

} // namespace

bool TerrainMaterialCache::setup(engine::Device& device) {
    const auto target = [&](SDL_GPUTextureFormat format) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = info.height = kSize;
        info.layer_count_or_depth = kLevels;
        info.num_levels = 1;
        return device.makeTexture(info);
    };
    // Albedo in sRGB: eight bits spent where the eye looks, the dark ground.
    albedo_ = target(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
    normal_ = target(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    // Which scan the ground is made of, for the detail under the eye that no
    // texel of the cache can hold (terrain_cached.hlsl cachedDetail).
    detail_ = target(SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
    SDL_GPUTextureCreateInfo table{};
    table.type = SDL_GPU_TEXTURETYPE_2D;
    table.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    table.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    table.width = kLevels;
    table.height = 1;
    table.layer_count_or_depth = 1;
    table.num_levels = 1;
    table_ = device.makeTexture(table);
    if (!albedo_ || !normal_ || !detail_ || !table_) return false;

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;   // toroidal
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    SDL_GPUSamplerCreateInfo nearest{};
    nearest.min_filter = nearest.mag_filter = SDL_GPU_FILTER_NEAREST;
    nearest.address_mode_u = nearest.address_mode_v = nearest.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    tableSampler_ = device.makeSampler(nearest);
    if (!sampler_ || !tableSampler_) return false;

    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_material_bake.hlsl";
    wanted.vertexEntry = "MaterialBakeVS";
    wanted.fragmentEntry = "MaterialBakePS";
    wanted.colourFormats = {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                            SDL_GPU_TEXTUREFORMAT_R8G8_UNORM};
    wanted.depthTest = wanted.depthWrite = false;
    pipeline_ = device.makePipeline(wanted);
    invalidateAll();
    return bool(pipeline_);
}

std::vector<SDL_GPUTextureSamplerBinding> TerrainMaterialCache::lookupBindings() const {
    // The detail ids are indices: nearest, never blended.
    return {{albedo_.get(), sampler_.get()}, {normal_.get(), sampler_.get()}, {table_.get(), tableSampler_.get()},
            {detail_.get(), tableSampler_.get()}};
}

std::size_t TerrainMaterialCache::pendingTexels() const {
    std::size_t n = 0;
    for (const Job& job : jobs_) n += std::size_t((job.x1 - job.x0) * (job.y1 - job.y0));
    return n;
}

void TerrainMaterialCache::queueRect(int level, std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1,
                                     bool rebuild, bool budgeted) {
    if (x1 <= x0 || y1 <= y0) return;
    // Strips, so a big job can be spread over frames.
    for (std::int64_t y = y0; y < y1; y += kStrip) {
        jobs_.push_back({level, x0, y, x1, std::min(y1, y + kStrip), rebuild, budgeted});
        if (rebuild) ++rebuildsPending_[level];
    }
}

namespace {
// The coarsest page data a texel of this size needs (datasets H4, H8, H16, H64).
int datasetOf(double texel) { return texel <= 2.6 ? 0 : texel <= 5.2 ? 1 : texel <= 10.3 ? 2 : 3; }
} // namespace

void TerrainMaterialCache::queueRebuild(int level) {
    Level& l = levels_[level];
    l.complete = false;
    tableDirty_ = true;
    // What is queued for this level no longer matters: the rebuild covers it.
    for (auto it = jobs_.begin(); it != jobs_.end();) it = it->level == level ? jobs_.erase(it) : std::next(it);
    rebuildsPending_[level] = 0;
    queueRect(level, l.originX, l.originY, l.originX + kSize, l.originY + kSize, true, true);
}

void TerrainMaterialCache::invalidateAll() {
    for (Level& l : levels_) l.placed = l.complete = false;
    jobs_.clear();
    rebuildsPending_.fill(0);
    tableDirty_ = true;
}

void TerrainMaterialCache::invalidate(double minX, double minY, double maxX, double maxY, int dataset) {
    for (int level = 0; level < kLevels; ++level) {
        const Level& l = levels_[level];
        if (!l.placed) continue;
        const double t = texel(level);
        // The bake reads max(finest resident, the level's dataset): a page
        // finer than the level's dataset is never read there. Every page
        // arriving used to rebake all eleven levels under it - the ground
        // recolouring itself across the whole view while pages streamed.
        if (dataset < datasetOf(t)) continue;
        const auto x0 = std::max<std::int64_t>(l.originX, std::int64_t(std::floor(minX / t)));
        const auto y0 = std::max<std::int64_t>(l.originY, std::int64_t(std::floor(minY / t)));
        const auto x1 = std::min<std::int64_t>(l.originX + kSize, std::int64_t(std::ceil(maxX / t)));
        const auto y1 = std::min<std::int64_t>(l.originY + kSize, std::int64_t(std::ceil(maxY / t)));
        queueRect(level, x0, y0, x1, y1, false, true);
    }
}

void TerrainMaterialCache::update(const engine::Frame& frame, double cameraX, double cameraY,
                                  const std::array<float, 16>& ring) {
    bakedLastFrame_ = 0;
    if (!pipeline_ || !frame.commands || bindings_.size() != 15) return;
    // Every level to the camera. A level not complete keeps its window until
    // its rebuild is done; then it catches up with strips like any other.
    for (int level = kLevels - 1; level >= 0; --level) {
        Level& l = levels_[level];
        const double t = texel(level);
        const std::int64_t wantX = floorDiv(std::int64_t(std::floor(cameraX / t)) - kSize / 2, kSnap) * kSnap;
        const std::int64_t wantY = floorDiv(std::int64_t(std::floor(cameraY / t)) - kSize / 2, kSnap) * kSnap;
        if (!l.placed) {
            l.placed = true;
            l.originX = wantX;
            l.originY = wantY;
            queueRebuild(level);
            continue;
        }
        if (!l.complete || (wantX == l.originX && wantY == l.originY)) continue;
        const std::int64_t dx = wantX - l.originX, dy = wantY - l.originY;
        if (std::abs(dx) >= kSize || std::abs(dy) >= kSize) {   // a jump: start over here
            l.originX = wantX;
            l.originY = wantY;
            queueRebuild(level);
            continue;
        }
        // What the new window has that the old did not: a column strip and a row strip.
        const std::int64_t nx0 = wantX, ny0 = wantY, nx1 = wantX + kSize, ny1 = wantY + kSize;
        const std::int64_t stripTexels = std::abs(dx) * kSize + std::abs(dy) * kSize;
        if (stripTexels > kStripLimit) {
            l.originX = wantX;
            l.originY = wantY;
            queueRebuild(level);
            continue;
        }
        if (dx > 0) queueRect(level, l.originX + kSize, ny0, nx1, ny1, false, false);
        if (dx < 0) queueRect(level, nx0, ny0, l.originX, ny1, false, false);
        const std::int64_t keepX0 = std::max(nx0, l.originX), keepX1 = std::min(nx1, l.originX + kSize);
        if (dy > 0) queueRect(level, keepX0, l.originY + kSize, keepX1, ny1, false, false);
        if (dy < 0) queueRect(level, keepX0, ny0, keepX1, l.originY, false, false);
        // Pending work outside the new window is moot.
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->level == level && !it->rebuild) {
                it->x0 = std::max(it->x0, nx0); it->x1 = std::min(it->x1, nx1);
                it->y0 = std::max(it->y0, ny0); it->y1 = std::min(it->y1, ny1);
                if (it->x1 <= it->x0 || it->y1 <= it->y0) { it = jobs_.erase(it); continue; }
            }
            ++it;
        }
        l.originX = wantX;
        l.originY = wantY;
        tableDirty_ = true;
    }

    // Strips that keep a complete level complete go now, whatever they cost:
    // they are a few rows. Rebuilds go coarse first, within the budget.
    std::vector<Job> now;
    bool rebuilding = false;
    for (int pending : rebuildsPending_) rebuilding = rebuilding || pending > 0;
    std::size_t budget = rebuilding ? kRebuildBurst : kRebuildBudget;
    // Strips first; then rebuilds coarse first (a coarse level is what the
    // shader falls back to); then refreshes, near first.
    std::stable_sort(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) {
        const int ra = !a.budgeted ? 0 : a.rebuild ? 1 : 2, rb = !b.budgeted ? 0 : b.rebuild ? 1 : 2;
        if (ra != rb) return ra < rb;
        return ra == 1 ? a.level > b.level : a.level < b.level;
    });
    while (!jobs_.empty()) {
        const Job job = jobs_.front();
        const std::size_t texels = std::size_t((job.x1 - job.x0) * (job.y1 - job.y0));
        if (job.budgeted && texels > budget && !now.empty()) break;
        jobs_.pop_front();
        now.push_back(job);
        bakedLastFrame_ += texels;
        if (job.budgeted) budget = texels > budget ? 0 : budget - texels;
        if (job.rebuild) {
            if (--rebuildsPending_[job.level] == 0) {
                levels_[job.level].complete = true;
                tableDirty_ = true;
            }
        }
    }
    if (!now.empty()) bake(frame, ring, now);
    if (tableDirty_) publishTable(*frame.device);
}

void TerrainMaterialCache::bake(const engine::Frame& frame, const std::array<float, 16>& ring,
                                const std::vector<Job>& jobs) {
    // One render pass a level, a draw a toroidal piece of each job.
    for (int level = 0; level < kLevels; ++level) {
        bool any = false;
        for (const Job& job : jobs) any = any || job.level == level;
        if (!any) continue;
        SDL_GPUColorTargetInfo targets[3]{};
        SDL_GPUTexture* textures[3] = {albedo_.get(), normal_.get(), detail_.get()};
        for (int i = 0; i < 3; ++i) {
            targets[i].texture = textures[i];
            targets[i].layer_or_depth_plane = Uint32(level);
            targets[i].load_op = SDL_GPU_LOADOP_LOAD;
            targets[i].store_op = SDL_GPU_STOREOP_STORE;
        }
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.commands, targets, 3, nullptr);
        if (!pass) continue;
        SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
        SDL_BindGPUFragmentSamplers(pass, 0, bindings_.data(), Uint32(bindings_.size()));
        SDL_PushGPUFragmentUniformData(frame.commands, 1, ring.data(), sizeof(float) * 16);
        const Level& l = levels_[level];
        // The bake's own block (terrain_material_bake.hlsl MaterialBake). The
        // window's start goes as metres from an anchor near it, worked out in
        // double, so the shader's coordinates are small and exact.
        const double t = texel(level);
        const double anchorX = std::floor((double(l.originX) + kSize / 2) * t / 64.0) * 64.0;
        const double anchorY = std::floor((double(l.originY) + kSize / 2) * t / 64.0) * 64.0;
        // The coarsest page data the texel needs (datasets H4, H8, H16, H64...).
        const float dataset = float(datasetOf(t));
        const float block[12] = {float(anchorX), float(anchorY), float(double(l.originX) * t - anchorX),
                                 float(double(l.originY) * t - anchorY), float(t), float(kSize),
                                 float(wrap(l.originX)), float(wrap(l.originY)), float(level), dataset,
                                 level >= 3 ? 1.0f : 0.0f, 0};   // lite from 32 cm texels
        SDL_PushGPUFragmentUniformData(frame.commands, 2, block, sizeof(block));
        for (const Job& job : jobs) {
            if (job.level != level) continue;
            // A world rect is at most four rects of the torus.
            for (std::int64_t y = job.y0; y < job.y1;) {
                const std::int64_t ty = wrap(y), rows = std::min(job.y1 - y, kSize - ty);
                for (std::int64_t x = job.x0; x < job.x1;) {
                    const std::int64_t tx = wrap(x), cols = std::min(job.x1 - x, kSize - tx);
                    const SDL_GPUViewport viewport{float(tx), float(ty), float(cols), float(rows), 0, 1};
                    const SDL_Rect scissor{int(tx), int(ty), int(cols), int(rows)};
                    SDL_SetGPUViewport(pass, &viewport);
                    SDL_SetGPUScissor(pass, &scissor);
                    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
                    x += cols;
                }
                y += rows;
            }
        }
        SDL_EndGPURenderPass(pass);
    }
    // Leave b2 as the frame's passes expect it: zeros unless a draw sets it.
    const float zeros[16] = {};
    SDL_PushGPUFragmentUniformData(frame.commands, 2, zeros, sizeof(zeros));
}

void TerrainMaterialCache::publishTable(engine::Device& device) {
    tableDirty_ = false;
    std::array<float, kLevels * 4> rows{};
    for (int level = 0; level < kLevels; ++level) {
        const Level& l = levels_[level];
        rows[level * 4 + 0] = float(l.originX);
        rows[level * 4 + 1] = float(l.originY);
        rows[level * 4 + 2] = float(texel(level));
        rows[level * 4 + 3] = l.complete ? 1.0f : 0.0f;
    }
    engine::Device::Uploader upload(device);
    upload.refill(table_.get(), rows.data(), kLevels, 1, 16);
    upload.finish();
}

} // namespace game
