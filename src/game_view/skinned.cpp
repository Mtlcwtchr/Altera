#include "game_view/skinned.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "sim/core.hpp"
#include "sim/worldgen.hpp"

namespace altera::game {
namespace {

namespace fs = std::filesystem;

bool readFile(const fs::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(std::size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

struct Reader {
    const std::vector<std::uint8_t>& b;
    std::size_t at = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (at + sizeof(T) > b.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, b.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str(std::size_t n) {
        if (at + n > b.size()) {
            ok = false;
            return {};
        }
        const char* s = reinterpret_cast<const char*>(b.data() + at);
        at += n;
        return std::string(s, strnlen(s, n));
    }
    const std::uint8_t* take(std::size_t n) {
        if (at + n > b.size()) {
            ok = false;
            return nullptr;
        }
        const std::uint8_t* p = b.data() + at;
        at += n;
        return p;
    }
};

// Row-major 4x4 helpers (the bake writes Blender's matrices row by row).
void mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    std::memcpy(out, r, sizeof r);
}

void basis(const float* q, const float* t, float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w), m[3] = t[0];
    m[4] = 2 * (x * y + z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z - x * w), m[7] = t[1];
    m[8] = 2 * (x * z - y * w), m[9] = 2 * (y * z + x * w), m[10] = 1 - 2 * (x * x + y * y), m[11] = t[2];
    m[12] = m[13] = m[14] = 0, m[15] = 1;
}

// Normalised blend of two quaternions, the shorter way round.
void nlerp(const float* a, const float* b, float k, float* out) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float s = d < 0 ? -1.0f : 1.0f;
    float l = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = a[i] * (1 - k) + b[i] * s * k;
        l += out[i] * out[i];
    }
    l = l > 0 ? 1.0f / std::sqrt(l) : 1.0f;
    for (int i = 0; i < 4; ++i) out[i] *= l;
}

struct InstanceRow {
    float placeYaw[4], scale[4], skin[4], hair[4], cloth[4];
};
static_assert(sizeof(InstanceRow) == 80);

} // namespace

engine::Texture uploadRgba(engine::Device& device, const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    engine::Texture tex = device.makeTexture(info);
    if (!tex) return tex;
    SDL_GPUTransferBufferCreateInfo t{};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = width * height * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device.handle(), &t);
    if (!tb) return {};
    if (void* map = SDL_MapGPUTransferBuffer(device.handle(), tb, false)) {
        std::memcpy(map, rgba, t.size);
        SDL_UnmapGPUTransferBuffer(device.handle(), tb);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device.handle());
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb;
        src.pixels_per_row = width;
        src.rows_per_layer = height;
        SDL_GPUTextureRegion dst{};
        dst.texture = tex.get();
        dst.w = width;
        dst.h = height;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cb);
    }
    SDL_ReleaseGPUTransferBuffer(device.handle(), tb);
    return tex;
}

SkinnedSet::~SkinnedSet() {
    if (device_) {
        if (paletteTransfer_) SDL_ReleaseGPUTransferBuffer(device_->handle(), paletteTransfer_);
        if (instanceTransfer_) SDL_ReleaseGPUTransferBuffer(device_->handle(), instanceTransfer_);
    }
}

bool SkinnedSet::load(engine::Device& device, const fs::path& mesh, const fs::path& anim, const fs::path& animJson,
                      std::string* why) {
    device_ = &device;
    auto fail = [&](const std::string& s) {
        if (why) *why = s;
        return false;
    };
    std::vector<std::uint8_t> bytes;
    if (!readFile(mesh, bytes)) return fail("cannot read " + mesh.string());
    Reader r{bytes};
    if (r.str(4) != "ASKN" || r.get<std::uint32_t>() != 1) return fail("not an ASKN v1 file: " + mesh.string());
    const auto nj = r.get<std::uint32_t>(), ns = r.get<std::uint32_t>(), nl = r.get<std::uint32_t>();
    const auto nv = r.get<std::uint32_t>(), ni = r.get<std::uint32_t>(), aw = r.get<std::uint32_t>(),
               ah = r.get<std::uint32_t>();
    joints_.resize(nj);
    for (auto& j : joints_) {
        j.parent = r.get<std::int32_t>();
        j.name = r.str(32);
    }
    skeletons_.assign(ns, std::vector<Bone>(nj));
    for (auto& s : skeletons_)
        for (auto& b : s) {
            const std::uint8_t* p = r.take(sizeof(Bone));
            if (p) std::memcpy(&b, p, sizeof(Bone));
        }
    looks_.resize(nl);
    for (auto& l : looks_) {
        l.name = r.str(32);
        l.skeleton = r.get<std::uint32_t>();
        l.firstIndex = r.get<std::uint32_t>();
        l.indexCount = r.get<std::uint32_t>();
        l.height = r.get<float>();
    }
    constexpr std::size_t kStride = 44;
    const std::uint8_t* verts = r.take(std::size_t(nv) * kStride);
    const std::uint8_t* idx = r.take(std::size_t(ni) * 4);
    const std::uint8_t* pixels = r.take(std::size_t(aw) * ah * 4);
    if (!r.ok) return fail("truncated " + mesh.string());

    if (!anim.empty() && fs::exists(anim)) {
        std::vector<std::uint8_t> ab;
        if (!readFile(anim, ab)) return fail("cannot read " + anim.string());
        Reader a{ab};
        if (a.str(4) != "AANM" || a.get<std::uint32_t>() != 1) return fail("not an AANM v1 file: " + anim.string());
        const auto anj = a.get<std::uint32_t>(), nc = a.get<std::uint32_t>();
        fps_ = a.get<float>();
        if (anj != nj) return fail("clips and meshes have different joints: " + anim.string());
        clips_.resize(nc);
        std::size_t total = 0;
        for (std::size_t i = 0; i < nc; ++i) {
            Clip& c = clips_[i];
            c.name = a.str(64);
            c.frames = a.get<std::uint32_t>();
            c.loop = a.get<std::uint32_t>();
            c.first = a.get<std::uint32_t>();
            clipIndex_[c.name] = int(i);
            total = std::max<std::size_t>(total, std::size_t(c.first) + c.frames);
        }
        const std::size_t floats = total * (std::size_t(nj) * 7 + 2);
        const std::uint8_t* f = a.take(floats * 4);
        if (!a.ok) return fail("truncated " + anim.string());
        frames_.resize(floats);
        std::memcpy(frames_.data(), f, floats * 4);
        if (!animJson.empty() && fs::exists(animJson)) {
            try {
                const sim::Json j = sim::loadJsonFile(animJson.string());
                const sim::Json states = j.value("state", sim::Json::object());
                const sim::Json inters = j.value("interaction", sim::Json::object());
                for (const auto& [k, v] : states.items())
                    if (int c = clip(v.get<std::string>()); c >= 0) stateClips_[k] = c;
                for (const auto& [k, v] : inters.items())
                    if (int c = clip(v.get<std::string>()); c >= 0) interactionClips_[k] = c;
            } catch (const std::exception& e) {
                return fail(animJson.string() + ": " + e.what());
            }
        }
        // AL-70: a clip may have a female take ("<clip>@f", Skyrim's female/ folder);
        // looks on the second skeleton play it in place of the male one.
        femaleTake_.assign(clips_.size(), -1);
        for (std::size_t i = 0; i < clips_.size(); ++i)
            if (const int f = clip(clips_[i].name + "@f"); f >= 0) femaleTake_[i] = f;
    }

    vertices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, verts, std::size_t(nv) * kStride);
    indices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, idx, std::size_t(ni) * 4);
    atlas_ = uploadRgba(device, pixels, aw, ah);
    const std::size_t paletteBytes = kMaxFigures * nj * 48;
    palette_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, paletteBytes);
    instances_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, kMaxFigures * sizeof(InstanceRow));
    SDL_GPUTransferBufferCreateInfo t{};
    t.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    t.size = std::uint32_t(paletteBytes);
    paletteTransfer_ = SDL_CreateGPUTransferBuffer(device.handle(), &t);
    t.size = std::uint32_t(kMaxFigures * sizeof(InstanceRow));
    instanceTransfer_ = SDL_CreateGPUTransferBuffer(device.handle(), &t);
    SDL_GPUSamplerCreateInfo s{};
    s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
    s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(s);

    engine::PipelineWanted w;
    w.shaderFile = "altera_game/skinned.hlsl";
    w.vertexEntry = "skinnedVs";
    w.fragmentEntry = "skinnedPs";
    w.depthClip = true;
    w.cull = SDL_GPU_CULLMODE_NONE;
    w.buffers = {{0, std::uint32_t(kStride), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                 {1, sizeof(InstanceRow), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    w.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 12},
                    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
                    {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, 32},
                    {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 36},
                    {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 40},
                    {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                    {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                    {8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 32},
                    {9, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 48},
                    {10, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 64}};
    pipeline_ = device.makePipeline(w);
    if (!vertices_ || !indices_ || !atlas_ || !palette_ || !instances_ || !paletteTransfer_ || !instanceTransfer_ ||
        !sampler_ || !pipeline_)
        return fail("skinned GPU objects (" + mesh.filename().string() + "): " + device.error());
    return true;
}

int SkinnedSet::look(const std::string& name) const {
    for (std::size_t i = 0; i < looks_.size(); ++i)
        if (looks_[i].name == name) return int(i);
    return -1;
}
int SkinnedSet::clip(const std::string& name) const {
    const auto it = clipIndex_.find(name);
    return it == clipIndex_.end() ? -1 : it->second;
}
int SkinnedSet::stateClip(const std::string& s) const {
    const auto it = stateClips_.find(s);
    return it == stateClips_.end() ? -1 : it->second;
}
int SkinnedSet::interactionClip(const std::string& s) const {
    const auto it = interactionClips_.find(s);
    return it == interactionClips_.end() ? -1 : it->second;
}
double SkinnedSet::clipSeconds(int c) const {
    if (c < 0 || c >= int(clips_.size())) return 1.0;
    return double(clips_[std::size_t(c)].frames) / fps_;
}
bool SkinnedSet::clipHolds(int c) const { return c >= 0 && c < int(clips_.size()) && clips_[std::size_t(c)].loop == 2; }

void SkinnedSet::sample(int c, double time, Pose& out) const {
    const std::size_t nj = joints_.size();
    out.q.assign(nj * 4, 0.0f);
    out.t.assign(nj * 3, 0.0f);
    for (std::size_t j = 0; j < nj; ++j) out.q[j * 4 + 3] = 1;
    if (c < 0 || c >= int(clips_.size())) return;
    const Clip& cl = clips_[std::size_t(c)];
    if (!cl.frames) return;
    const double f = std::max(0.0, time) * fps_;
    std::uint32_t a, b;
    float k;
    if (cl.loop == 2) {   // once, then hold
        const double ff = std::min(f, double(cl.frames - 1));
        a = std::uint32_t(ff);
        b = std::min(a + 1, cl.frames - 1);
        k = float(ff - a);
    } else {   // looped (a window of a longer take wraps too)
        const double ff = std::fmod(f, double(cl.frames));
        a = std::uint32_t(ff) % cl.frames;
        b = (a + 1) % cl.frames;
        k = float(ff - std::floor(ff));
    }
    const std::size_t row = nj * 7 + 2;
    const float* A = frames_.data() + (std::size_t(cl.first) + a) * row;
    const float* B = frames_.data() + (std::size_t(cl.first) + b) * row;
    for (std::size_t j = 0; j < nj; ++j) {
        nlerp(A + j * 7, B + j * 7, k, &out.q[j * 4]);
        for (int i = 0; i < 3; ++i) out.t[j * 3 + i] = A[j * 7 + 4 + i] * (1 - k) + B[j * 7 + 4 + i] * k;
    }
    out.anchor[0] = A[nj * 7] * (1 - k) + B[nj * 7] * k;
    out.anchor[1] = A[nj * 7 + 1] * (1 - k) + B[nj * 7 + 1] * k;
}

const std::string& SkinnedSet::clipName(int c) const {
    static const std::string none;
    return c >= 0 && c < int(clips_.size()) ? clips_[std::size_t(c)].name : none;
}

void SkinnedSet::forward(const Pose& p, const std::vector<Bone>& bones, float* world) const {
    const std::size_t nj = joints_.size();
    for (std::size_t j = 0; j < nj; ++j) {
        float b[16], lb[16];
        basis(&p.q[j * 4], &p.t[j * 3], b);
        mul(bones[j].local, b, lb);
        if (joints_[j].parent >= 0) mul(&world[std::size_t(joints_[j].parent) * 16], lb, &world[j * 16]);
        else std::memcpy(&world[j * 16], lb, sizeof lb);
    }
}

void SkinnedSet::setLoco(const loco::Config& c) {
    loco_ = c;
    parents_.clear();
    for (const auto& j : joints_) parents_.push_back(j.parent);
    rig_ = loco::resolveRig(c, [&](const std::string& n) {
        for (std::size_t i = 0; i < joints_.size(); ++i)
            if (joints_[i].name == n) return int(i);
        return -1;
    });
    gaits_.assign(clips_.size(), loco::Gait{});
    gaitDone_.assign(clips_.size(), 0);
    if (!rig_.ok || skeletons_.empty()) return;
    // the ankles' height in the rest pose: "planted"
    Pose rest;
    sample(-1, 0, rest);
    std::vector<float> w(joints_.size() * 16);
    forward(rest, skeletons_[0], w.data());
    for (int i = 0; i < 2; ++i) rig_.footRestZ[i] = loco::jointPos(w.data(), rig_.foot[i]).z;
}

loco::Gait SkinnedSet::gait(int c) const {
    if (c < 0 || c >= int(clips_.size()) || skeletons_.empty()) return {};
    if (gaitDone_.size() != clips_.size()) gaitDone_.assign(clips_.size(), 0), gaits_.assign(clips_.size(), {});
    if (gaitDone_[std::size_t(c)]) return gaits_[std::size_t(c)];
    gaitDone_[std::size_t(c)] = 1;
    loco::Gait& g = gaits_[std::size_t(c)];
    const Clip& cl = clips_[std::size_t(c)];
    g.cycle = float(cl.frames) / fps_;
    if (!rig_.ok || cl.frames < 4) return g;
    std::vector<loco::V3> l, r;
    std::vector<float> w(joints_.size() * 16);
    Pose p;
    for (std::uint32_t f = 0; f < cl.frames; ++f) {
        sample(c, double(f) / fps_, p);
        forward(p, skeletons_[0], w.data());
        loco::V3 a = loco::jointPos(w.data(), rig_.foot[0]), b = loco::jointPos(w.data(), rig_.foot[1]);
        a.x -= p.anchor[0], a.y -= p.anchor[1], b.x -= p.anchor[0], b.y -= p.anchor[1];
        l.push_back(a), r.push_back(b);
    }
    g = loco::analyseGait(l, r, fps_, rig_.forward);
    if (!g.ok) g.cycle = float(cl.frames) / fps_;
    return g;
}

void SkinnedSet::palette(const Figure& fig, float* rows) const {
    const std::size_t nj = joints_.size();
    const bool female = looks_[std::size_t(fig.look)].skeleton == 1;
    auto take = [&](int c) {
        return female && c >= 0 && c < int(femaleTake_.size()) && femaleTake_[std::size_t(c)] >= 0
                   ? femaleTake_[std::size_t(c)]
                   : c;
    };
    Pose p, from;
    if (fig.layerCount > 0) {
        // AL-79: the animator's layers, blended in turn by their share of the weight so far
        float sum = 0;
        bool first = true;
        for (int i = 0; i < fig.layerCount && i < loco::kMaxLayers; ++i) {
            const loco::Layer& L = fig.layers[std::size_t(i)];
            if (L.weight <= 0) continue;
            const int c = take(L.clip);
            double t = L.time;
            if (L.phase >= 0) t = loco::clipTime(gait(c), L.phase);   // the take's own beat
            Pose& dst = first ? p : from;
            sample(c, t, dst);
            sum += L.weight;
            if (first) {
                first = false;
                continue;
            }
            const float k = L.weight / sum;
            for (std::size_t j = 0; j < nj; ++j) {
                nlerp(&p.q[j * 4], &from.q[j * 4], k, &p.q[j * 4]);
                for (int a = 0; a < 3; ++a) p.t[j * 3 + a] += (from.t[j * 3 + a] - p.t[j * 3 + a]) * k;
            }
            for (int a = 0; a < 2; ++a) p.anchor[a] += (from.anchor[a] - p.anchor[a]) * k;
        }
        if (first) sample(-1, 0, p);
    } else {
        sample(take(fig.clip), fig.time, p);
        if (fig.fromClip >= 0 && fig.fade > 0) {
            sample(take(fig.fromClip), fig.fromTime, from);
            const float k = std::clamp(fig.fade, 0.0f, 1.0f);
            for (std::size_t j = 0; j < nj; ++j) {
                nlerp(&p.q[j * 4], &from.q[j * 4], k, &p.q[j * 4]);
                for (int i = 0; i < 3; ++i) p.t[j * 3 + i] += (from.t[j * 3 + i] - p.t[j * 3 + i]) * k;
            }
            for (int i = 0; i < 2; ++i) p.anchor[i] += (from.anchor[i] - p.anchor[i]) * k;
        }
    }
    const auto& bones = skeletons_[std::min<std::size_t>(looks_[std::size_t(fig.look)].skeleton, skeletons_.size() - 1)];
    world_.resize(nj * 16);
    float* world = world_.data();
    forward(p, bones, world);
    // AL-79: look, lean, breathing and the feet on the ground, on the clip's pose
    if (fig.proc.on && rig_.ok) loco::applyProcedural(loco_, rig_, parents_, world, fig.proc, p.anchor[0], p.anchor[1], scratch_);
    for (std::size_t j = 0; j < nj; ++j) {
        float s[16];
        mul(&world[j * 16], bones[j].invRest, s);
        // The clip's travel stays with the sim: take the anchor's drift off.
        s[3] -= p.anchor[0];
        s[7] -= p.anchor[1];
        std::memcpy(rows + j * 12, s, 12 * sizeof(float));
    }
}

void SkinnedSet::prepare(SDL_GPUCommandBuffer* commands, const std::vector<Figure>& figures) {
    drawLooks_.clear();
    if (!pipeline_ || figures.empty()) return;
    const std::size_t nj = joints_.size();
    const std::size_t n = std::min(figures.size(), kMaxFigures);
    auto* pal = static_cast<float*>(SDL_MapGPUTransferBuffer(device_->handle(), paletteTransfer_, true));
    auto* ins = static_cast<InstanceRow*>(SDL_MapGPUTransferBuffer(device_->handle(), instanceTransfer_, true));
    if (!pal || !ins) {
        if (pal) SDL_UnmapGPUTransferBuffer(device_->handle(), paletteTransfer_);
        if (ins) SDL_UnmapGPUTransferBuffer(device_->handle(), instanceTransfer_);
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Figure& f = figures[i];
        palette(f, pal + i * nj * 12);
        InstanceRow& r = ins[i];
        r.placeYaw[0] = float(f.rel[0]), r.placeYaw[1] = float(f.rel[1]), r.placeYaw[2] = float(f.rel[2]);
        r.placeYaw[3] = f.yaw;
        r.scale[0] = r.scale[1] = r.scale[2] = f.scale;
        r.scale[3] = f.highlight;
        for (int k = 0; k < 3; ++k) r.skin[k] = f.skin[k], r.hair[k] = f.hair[k], r.cloth[k] = f.cloth[k];
        r.skin[3] = float(i * nj * 3);
        r.hair[3] = 1;
        r.cloth[3] = 0;
        drawLooks_.push_back(std::clamp(f.look, 0, int(looks_.size()) - 1));
    }
    SDL_UnmapGPUTransferBuffer(device_->handle(), paletteTransfer_);
    SDL_UnmapGPUTransferBuffer(device_->handle(), instanceTransfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation fromP{paletteTransfer_, 0}, fromI{instanceTransfer_, 0};
    const SDL_GPUBufferRegion toP{palette_.get(), 0, std::uint32_t(n * nj * 48)};
    const SDL_GPUBufferRegion toI{instances_.get(), 0, std::uint32_t(n * sizeof(InstanceRow))};
    SDL_UploadToGPUBuffer(copy, &fromP, &toP, true);
    SDL_UploadToGPUBuffer(copy, &fromI, &toI, true);
    SDL_EndGPUCopyPass(copy);
}

void SkinnedSet::draw(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const float vp[16],
                      const terrain_core::LightUniforms& light) {
    if (!pipeline_ || drawLooks_.empty()) return;
    SDL_BindGPUGraphicsPipeline(pass, pipeline_.get());
    const SDL_GPUBufferBinding bufs[2] = {{vertices_.get(), 0}, {instances_.get(), 0}};
    SDL_BindGPUVertexBuffers(pass, 0, bufs, 2);
    const SDL_GPUBufferBinding ib{indices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_GPUBuffer* storage = palette_.get();
    SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
    const SDL_GPUTextureSamplerBinding tex{atlas_.get(), sampler_.get()};
    SDL_BindGPUFragmentSamplers(pass, 0, &tex, 1);
    SDL_PushGPUVertexUniformData(commands, 0, vp, 64);
    SDL_PushGPUFragmentUniformData(commands, 0, &light, sizeof light);
    for (std::size_t i = 0; i < drawLooks_.size(); ++i) {
        const Look& l = looks_[std::size_t(drawLooks_[i])];
        SDL_DrawGPUIndexedPrimitives(pass, l.indexCount, 1, l.firstIndex, 0, std::uint32_t(i));
    }
}

} // namespace altera::game
