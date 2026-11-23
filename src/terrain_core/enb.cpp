#include "terrain_core/enb.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace terrain_core {
namespace {

using nlohmann::json;

const json* group(const json& preset, const char* name) {
    const auto it = preset.find(name);
    return it != preset.end() && it->is_object() ? &*it : nullptr;
}

void read(const json* g, const char* key, float& v, float lo, float hi) {
    if (!g) return;
    const auto it = g->find(key);
    if (it != g->end() && it->is_number()) v = std::clamp(it->get<float>(), lo, hi);
}

void read3(const json* g, const char* key, float (&v)[3], float lo, float hi) {
    if (!g) return;
    const auto it = g->find(key);
    if (it == g->end() || !it->is_array() || it->size() != 3) return;
    for (int i = 0; i < 3; ++i)
        if ((*it)[i].is_number()) v[i] = std::clamp((*it)[i].get<float>(), lo, hi);
}

void readTints(const json* split, const char* key, EnbTints& t) {
    if (!split) return;
    const json* g = group(*split, key);
    read3(g, "shadows", t.shadows, 0.0f, 2.0f);
    read3(g, "highlights", t.highlights, 0.0f, 2.0f);
}

void put(float (&out)[4], float a, float b, float c, float d) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// A preset, over what `out` already holds; "base" names a preset to read first.
bool applyPreset(const json& presets, const std::string& name, EnbSettings& out, int depth, std::string* why) {
    const auto it = presets.find(name);
    if (it == presets.end() || !it->is_object()) {
        if (why) *why = "no preset \"" + name + "\"";
        return false;
    }
    const json& p = *it;
    if (depth < 4 && p.contains("base") && p["base"].is_string())
        if (!applyPreset(presets, p["base"].get<std::string>(), out, depth + 1, why)) return false;
    out.preset = name;
    out.hdr = p.value("hdr", out.hdr);

    const json* ao = group(p, "ao");
    read(ao, "strength", out.aoStrength, 0, 4);
    read(ao, "radius", out.aoRadius, 0.05f, 10);
    read(ao, "distance", out.aoDistance, 1, 5000);
    read(ao, "power", out.aoPower, 0.2f, 4);
    const json* contact = group(p, "contact_shadows");
    read(contact, "strength", out.contactStrength, 0, 1);
    read(contact, "length", out.contactLength, 0.05f, 10);
    read(contact, "distance", out.contactDistance, 1, 500);
    read(contact, "darkness", out.contactDarkness, 0, 1);
    const json* atmos = group(p, "atmosphere");
    read(atmos, "fog_extra", out.fogExtra, 0, 20);
    read(atmos, "sun_scatter", out.scatter, 0, 10);
    read(atmos, "scatter_g", out.scatterG, -0.95f, 0.95f);
    read(atmos, "aerial", out.aerial, 0, 1);
    read(atmos, "aerial_distance", out.aerialDistance, 10, 200000);
    read3(atmos, "aerial_tint", out.aerialTint, 0, 2);
    read(atmos, "sun_halo", out.sunHalo, 0, 10);
    const json* bloom = group(p, "bloom");
    read(bloom, "strength", out.bloom, 0, 4);
    read(bloom, "threshold", out.bloomThreshold, 0, 50);
    read(bloom, "knee", out.bloomKnee, 0.01f, 1);
    read(bloom, "radius", out.bloomRadius, 0, 1.5f);
    const json* rays = group(p, "light_shafts");
    read(rays, "strength", out.rays, 0, 4);
    read(rays, "decay", out.raysDecay, 0.5f, 1);
    read(rays, "density", out.raysDensity, 0.05f, 1);
    const json* expo = group(p, "exposure");
    read(expo, "exposure", out.exposure, 0.05f, 16);
    read(expo, "adaptation", out.adaptation, 0, 1);
    read(expo, "speed", out.adaptSpeed, 0.01f, 50);
    read(expo, "min", out.exposureMin, 0.05f, 16);
    read(expo, "max", out.exposureMax, 0.05f, 16);
    read(expo, "key", out.key, 0.01f, 2);
    out.exposureMax = std::max(out.exposureMax, out.exposureMin);
    const json* tone = group(p, "tone");
    if (tone && tone->contains("curve") && (*tone)["curve"].is_string()) {
        const std::string c = (*tone)["curve"].get<std::string>();
        out.tonemap = c == "aces" ? 1 : c == "agx" ? 2 : 0;
    }
    if (tone && tone->contains("linear") && (*tone)["linear"].is_boolean()) out.linearize = (*tone)["linear"].get<bool>();
    const json* grade = group(p, "grade");
    read(grade, "contrast", out.contrast, 0.2f, 3);
    read(grade, "pivot", out.pivot, 0, 1);
    read(grade, "saturation", out.saturation, 0, 3);
    read(grade, "vibrance", out.vibrance, -1, 2);
    read3(grade, "lift", out.lift, -0.5f, 0.5f);
    read3(grade, "gamma", out.gamma, 0.2f, 5);
    read3(grade, "gain", out.gain, 0, 4);
    const json* split = group(p, "split_tone");
    read(split, "strength", out.splitStrength, 0, 1);
    read(split, "balance", out.splitBalance, -1, 1);
    readTints(split, "day", out.day);
    readTints(split, "golden", out.golden);
    readTints(split, "night", out.night);
    const json* lens = group(p, "lens");
    read(lens, "vignette", out.vignette, 0, 1);
    read(lens, "vignette_reach", out.vignetteReach, 0.3f, 3);
    read(lens, "vignette_umber", out.vignetteUmber, 0, 1);
    read(lens, "grain", out.grain, 0, 0.5f);
    read(lens, "grain_size", out.grainSize, 1, 8);
    const json* points = group(p, "point_lights");
    read(points, "strength", out.pointLights, 0, 20);
    read(points, "by_day", out.pointLightsByDay, 0, 1);
    read(points, "glow", out.pointGlow, 0, 10);
    return true;
}

} // namespace

bool loadEnbSettings(const std::filesystem::path& file, const std::string& preset, EnbSettings& out, std::string* why,
                     EnbPresets* presets) {
    std::ifstream in(file);
    if (!in) {
        if (why) *why = "cannot open " + file.string();
        return false;
    }
    const json config = json::parse(in, nullptr, false);
    if (config.is_discarded() || !config.is_object() || !config.contains("presets") || !config["presets"].is_object()) {
        if (why) *why = "bad json in " + file.string() + " (wants an object with \"presets\")";
        return false;
    }
    const json& all = config["presets"];
    const std::string start = config.value("preset", std::string("enb"));
    if (presets) {
        presets->names.clear();
        for (auto it = all.begin(); it != all.end(); ++it) presets->names.push_back(it.key());
        presets->start = start;
    }
    EnbSettings fresh;
    if (!applyPreset(all, preset.empty() ? start : preset, fresh, 0, why)) return false;
    out = fresh;
    return true;
}

EnbTints enbTintsFor(const EnbSettings& s, float sunZ) {
    // Golden hour: the sun under ~25 degrees; night: under the horizon.
    const float night = smooth(0.02f, -0.12f, sunZ);
    const float golden = (1.0f - smooth(0.12f, 0.42f, sunZ)) * (1.0f - night);
    const float day = std::max(0.0f, 1.0f - night - golden);
    EnbTints t;
    for (int i = 0; i < 3; ++i) {
        t.shadows[i] = s.day.shadows[i] * day + s.golden.shadows[i] * golden + s.night.shadows[i] * night;
        t.highlights[i] = s.day.highlights[i] * day + s.golden.highlights[i] * golden + s.night.highlights[i] * night;
    }
    return t;
}

void enbPutLights(EnbUniforms& u, const EnbSettings& s, float sunAbove, const std::vector<EnbLight>& lights) {
    u.flags[3] = 0;
    if (s.pointLights <= 0 || lights.empty()) return;
    std::vector<const EnbLight*> nearest;
    for (const auto& l : lights) nearest.push_back(&l);
    auto d2 = [](const EnbLight* l) {
        return l->position[0] * l->position[0] + l->position[1] * l->position[1] + l->position[2] * l->position[2];
    };
    std::sort(nearest.begin(), nearest.end(), [&](const EnbLight* a, const EnbLight* b) { return d2(a) < d2(b); });
    const float strength = s.pointLights * (1.0f + (s.pointLightsByDay - 1.0f) * std::clamp(sunAbove, 0.0f, 1.0f));
    const std::size_t n = std::min<std::size_t>(nearest.size(), Enb::kMaxLights);
    for (std::size_t i = 0; i < n; ++i) {
        const EnbLight& l = *nearest[i];
        put(u.lightPos[i], l.position[0], l.position[1], l.position[2], std::max(0.5f, l.radius));
        put(u.lightCol[i], l.colour[0] * strength, l.colour[1] * strength, l.colour[2] * strength, l.flicker);
    }
    u.flags[3] = float(n);
}

EnbUniforms enbUniforms(const EnbSettings& s, const LightUniforms& light, const RayCamera& camera, float nearPlane,
                        float eyeZ, std::uint32_t width, std::uint32_t height, float seconds, float dt) {
    EnbUniforms u{};
    const float w = float(std::max(1u, width)), h = float(std::max(1u, height));
    put(u.camF, camera.forward[0], camera.forward[1], camera.forward[2], nearPlane);
    put(u.camR, camera.right[0], camera.right[1], camera.right[2], std::fmod(seconds, 1000.0f));
    put(u.camU, camera.up[0], camera.up[1], camera.up[2], std::clamp(dt, 0.0f, 0.5f));
    put(u.res, w, h, 1 / w, 1 / h);
    put(u.src, w, h, 1 / w, 1 / h);
    put(u.sun, light.sun[0], light.sun[1], light.sun[2], light.sunAbove);
    put(u.sunColour, light.sunColour[0], light.sunColour[1], light.sunColour[2], 0);
    put(u.zenith, light.zenith[0], light.zenith[1], light.zenith[2], light.fogDensity);
    put(u.horizon, light.horizon[0], light.horizon[1], light.horizon[2], light.fogFalloff);

    // The sun on the screen, and how much of it to use: none behind the eye,
    // fading out as it leaves the frame.
    const float* f = camera.forward;
    const float* r = camera.right;
    const float* up = camera.up;
    const float z = light.sun[0] * f[0] + light.sun[1] * f[1] + light.sun[2] * f[2];
    float sx = 0.5f, sy = 0.5f, weight = 0;
    if (z > 0.02f) {
        const float rr = r[0] * r[0] + r[1] * r[1] + r[2] * r[2], uu = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
        const float x = (light.sun[0] * r[0] + light.sun[1] * r[1] + light.sun[2] * r[2]) / (rr * z);
        const float y = (light.sun[0] * up[0] + light.sun[1] * up[1] + light.sun[2] * up[2]) / (uu * z);
        sx = x * 0.5f + 0.5f;
        sy = 0.5f - y * 0.5f;
        const float outside = std::max({-sx, sx - 1.0f, -sy, sy - 1.0f, 0.0f});
        weight = (1.0f - smooth(0.0f, 0.6f, outside)) * smooth(0.02f, 0.25f, z) * light.sunAbove;
    }
    put(u.sunUv, sx, sy, weight, eyeZ);

    put(u.ao, s.aoStrength, s.aoRadius, s.aoDistance, s.aoPower);
    put(u.contact, s.contactStrength, s.contactLength, s.contactDistance, s.contactDarkness);
    put(u.atmos, s.scatter, s.scatterG, s.aerial, s.aerialDistance);
    put(u.aerialTint, s.aerialTint[0], s.aerialTint[1], s.aerialTint[2], s.sunHalo);
    put(u.bloom, s.bloom, s.bloomThreshold, s.bloomKnee, s.bloomRadius);
    put(u.rays, s.rays, s.raysDecay, s.raysDensity, 0);
    put(u.expo, s.exposure, s.adaptation, s.exposureMin, s.exposureMax);
    put(u.expo2, s.key, s.adaptSpeed, 0, s.linearize ? 1.0f : 0.0f);
    put(u.tone, float(s.tonemap), s.contrast, s.saturation, s.vibrance);
    put(u.lift, s.lift[0], s.lift[1], s.lift[2], s.pivot);
    put(u.gamma, s.gamma[0], s.gamma[1], s.gamma[2], 1);
    put(u.gain, s.gain[0], s.gain[1], s.gain[2], s.fogExtra);
    const EnbTints t = enbTintsFor(s, light.sun[2]);
    put(u.shadowTint, t.shadows[0], t.shadows[1], t.shadows[2], s.splitStrength);
    put(u.highTint, t.highlights[0], t.highlights[1], t.highlights[2], s.splitBalance);
    put(u.lens, s.vignette, s.vignetteReach, s.grain, s.grainSize);
    put(u.flags, 0, 1, s.vignetteUmber, 0);
    u.rays[3] = s.pointGlow;
    return u;
}

bool Enb::open(engine::Device& device) {
    device_ = &device;
    auto make = [&](const char* file, const char* entry, SDL_GPUTextureFormat format) {
        engine::PipelineWanted wanted;
        wanted.shaderFile = file;
        wanted.vertexEntry = "enbVs";
        wanted.fragmentEntry = entry;
        wanted.depthTest = wanted.depthWrite = false;
        wanted.colourFormats = {format};
        return device.makePipeline(wanted);
    };
    ao_ = make("terrain_core/enb_ao.hlsl", "aoPs", kSceneFormat);
    composite_ = make("terrain_core/enb_composite.hlsl", "compositePs", kSceneFormat);
    down_ = make("terrain_core/enb_down.hlsl", "downPs", kSceneFormat);
    up_ = make("terrain_core/enb_up.hlsl", "upPs", kSceneFormat);
    rays_ = make("terrain_core/enb_rays.hlsl", "raysPs", kSceneFormat);
    adapt_ = make("terrain_core/enb_adapt.hlsl", "adaptPs", kSceneFormat);
    final_ = make("terrain_core/enb_final.hlsl", "finalPs", engine::Device::kColourFormat);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_ = device.makeSampler(sampling);
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    point_ = device.makeSampler(sampling);
    return ao_ && composite_ && down_ && up_ && rays_ && adapt_ && final_ && linear_ && point_;
}

bool Enb::size(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_ && lit_) return true;
    auto make = [&](std::uint32_t w, std::uint32_t h) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kSceneFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = std::max(1u, w);
        info.height = std::max(1u, h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        return device_->makeTexture(info);
    };
    width_ = height_ = 0;
    lit_ = make(width, height);
    aoTex_ = make((width + 1) / 2, (height + 1) / 2);
    raysTex_ = make((width + 3) / 4, (height + 3) / 4);
    if (!lit_ || !aoTex_ || !raysTex_) return false;
    std::uint32_t w = width, h = height;
    for (std::uint32_t i = 0; i < kBloomLevels; ++i) {
        w = std::max(1u, (w + 1) / 2);
        h = std::max(1u, (h + 1) / 2);
        levelW_[i] = w;
        levelH_[i] = h;
        downTex_[i] = make(w, h);
        upTex_[i] = i + 1 < kBloomLevels ? make(w, h) : engine::Texture{};
        if (!downTex_[i] || (i + 1 < kBloomLevels && !upTex_[i])) return false;
    }
    if (!adapted_[0]) {
        adapted_[0] = make(1, 1);
        adapted_[1] = make(1, 1);
        if (!adapted_[0] || !adapted_[1]) return false;
        reset_ = true;
    }
    width_ = width;
    height_ = height;
    return true;
}

SDL_GPUTexture* Enb::scratchOut(std::uint32_t width, std::uint32_t height) {
    if (!out_ || outWidth_ != width || outHeight_ != height) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = engine::Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = std::max(1u, width);
        info.height = std::max(1u, height);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        out_ = device_->makeTexture(info);
        outWidth_ = out_ ? width : 0;
        outHeight_ = out_ ? height : 0;
    }
    return out_.get();
}

void Enb::pass(SDL_GPUCommandBuffer* commands, const engine::GraphicsPipeline& pipeline, SDL_GPUTexture* target,
               std::uint32_t w, std::uint32_t h, std::initializer_list<SDL_GPUTextureSamplerBinding> sources,
               EnbUniforms u, float mode) {
    SDL_GPUColorTargetInfo colour{};
    colour.texture = target;
    colour.load_op = SDL_GPU_LOADOP_DONT_CARE;
    colour.store_op = SDL_GPU_STOREOP_STORE;
    auto* p = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, pipeline.get());
    const SDL_GPUViewport viewport{0, 0, float(w), float(h), 0, 1};
    SDL_SetGPUViewport(p, &viewport);
    SDL_BindGPUFragmentSamplers(p, 0, sources.begin(), Uint32(sources.size()));
    const float fw = float(std::max(1u, w)), fh = float(std::max(1u, h));
    put(u.res, fw, fh, 1 / fw, 1 / fh);
    u.sunColour[3] = mode;
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
}

void Enb::apply(SDL_GPUCommandBuffer* commands, const EnbFrame& f, const EnbSettings& s) {
    if (!final_ || !f.scene || !f.depth || !f.out || !f.light || !size(f.width, f.height)) return;
    EnbUniforms u = enbUniforms(s, *f.light, f.camera, f.nearPlane, f.eyeZ, f.width, f.height, f.seconds, f.dt);
    u.flags[0] = f.clouds ? 1.0f : 0.0f;
    u.flags[1] = f.grain ? 1.0f : 0.0f;
    u.expo2[2] = reset_ ? 1.0f : 0.0f;
    reset_ = false;
    if (f.lights) enbPutLights(u, s, f.light->sunAbove, *f.lights);
    auto lin = [&](SDL_GPUTexture* t) { return SDL_GPUTextureSamplerBinding{t, linear_.get()}; };
    auto pt = [&](SDL_GPUTexture* t) { return SDL_GPUTextureSamplerBinding{t, point_.get()}; };

    pass(commands, ao_, aoTex_.get(), (f.width + 1) / 2, (f.height + 1) / 2, {pt(f.depth)}, u);
    pass(commands, composite_, lit_.get(), f.width, f.height,
         {lin(f.scene), pt(f.depth), lin(f.clouds ? f.clouds : f.scene), lin(aoTex_.get())}, u);
    pass(commands, down_, downTex_[0].get(), levelW_[0], levelH_[0], {lin(lit_.get())}, u, 0);
    for (std::uint32_t i = 1; i < kBloomLevels; ++i)
        pass(commands, down_, downTex_[i].get(), levelW_[i], levelH_[i], {lin(downTex_[i - 1].get())}, u, 1);
    for (int i = int(kBloomLevels) - 2; i >= 0; --i) {
        SDL_GPUTexture* coarse = i + 2 == int(kBloomLevels) ? downTex_[i + 1].get() : upTex_[i + 1].get();
        pass(commands, up_, upTex_[i].get(), levelW_[i], levelH_[i], {lin(coarse), lin(downTex_[i].get())}, u);
    }
    pass(commands, rays_, raysTex_.get(), (f.width + 3) / 4, (f.height + 3) / 4, {lin(lit_.get()), pt(f.depth)}, u);
    const std::uint32_t next = flip_ ^ 1u;
    pass(commands, adapt_, adapted_[next].get(), 1, 1,
         {lin(downTex_[kBloomLevels - 1].get()), pt(adapted_[flip_].get())}, u);
    flip_ = next;
    pass(commands, final_, f.out, f.width, f.height,
         {lin(lit_.get()), lin(upTex_[0].get()), lin(raysTex_.get()), pt(adapted_[flip_].get())}, u);
}

} // namespace terrain_core
