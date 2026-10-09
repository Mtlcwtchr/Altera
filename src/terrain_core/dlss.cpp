#include "terrain_core/dlss.hpp"
#include "terrain_core/dlss_math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if ASR_HAS_DLSS
#define NOMINMAX
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
extern "C" {
void* Campfire_D3D12Device(SDL_GPUDevice*);
void* Campfire_D3D12Commands(SDL_GPUCommandBuffer*);
void* Campfire_D3D12Texture(SDL_GPUCommandBuffer*, SDL_GPUTexture*, bool, bool);
void Campfire_D3D12Restore(SDL_GPUCommandBuffer*);
}
#endif

namespace terrain_core {
struct Dlss::Impl {
    engine::Device* device = nullptr;
    bool supported = false, enabled = false, reset = true;
    unsigned width = 0, height = 0, renderWidth = 0, renderHeight = 0, frame = 0;
    int quality = -1;
    float jitter[2]{}, jittered[16]{}, previous[16]{};
    double previousEye[3]{};
    std::string message = "DLSS unavailable: build without NVIDIA SDK";
    engine::ComputePipeline motionPipeline;
    engine::Sampler depthSampler;
    engine::Texture motion, output;
#if ASR_HAS_DLSS
    ID3D12Device* native = nullptr;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool initialized = false;
    ID3D12InfoQueue1* debugQueue = nullptr;
    DWORD debugCookie = 0;
    void failure(const char* step, NVSDK_NGX_Result result) {
        char text[128];
        std::snprintf(text, sizeof text, "DLSS unavailable: %s (NGX 0x%08x)", step, unsigned(result));
        message = text;
        supported = enabled = false;
        SDL_Log("%s", text);
    }
#endif
    void release() {
        if (device && (motion || output)) SDL_WaitForGPUIdle(device->handle());
#if ASR_HAS_DLSS
        if (feature) { NVSDK_NGX_D3D12_ReleaseFeature(feature); feature = nullptr; }
#endif
        motion.reset(); output.reset(); enabled = false; reset = true; frame = 0;
    }
    ~Impl() {
        release();
#if ASR_HAS_DLSS
        if (parameters) NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if (initialized) NVSDK_NGX_D3D12_Shutdown1(native);
        if (debugQueue) { debugQueue->UnregisterMessageCallback(debugCookie); debugQueue->Release(); }
#endif
    }
};

Dlss::Dlss() : impl_(std::make_unique<Impl>()) {}
Dlss::~Dlss() = default;
bool Dlss::open(engine::Device& device) {
    auto& s = *impl_;
    s.device = &device;
#if ASR_HAS_DLSS
    if (std::strcmp(SDL_GetGPUDeviceDriver(device.handle()), "direct3d12") != 0) {
        s.message = "DLSS unavailable: requires Direct3D 12"; return false;
    }
    s.native = static_cast<ID3D12Device*>(Campfire_D3D12Device(device.handle()));
    if (SDL_getenv("ASR_GPU_DEBUG") && SUCCEEDED(s.native->QueryInterface(IID_PPV_ARGS(&s.debugQueue)))) {
        s.debugQueue->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
            D3D12_MESSAGE_ID, LPCSTR message, void*) {
            if (severity <= D3D12_MESSAGE_SEVERITY_WARNING) SDL_Log("D3D12: %s", message);
        }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &s.debugCookie);
    }
    // Writable per-user NGX logs, never the installation directory.
    char* pref = SDL_GetPrefPath("Campfire", "TerrainLab");
    if (!pref) { s.message = "DLSS unavailable: cannot create NGX log directory"; return false; }
    const auto logs = std::filesystem::path(pref).wstring();
    SDL_free(pref);
    auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("b5fcd06a-b2aa-477d-a181-c2336c3749e6",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", logs.c_str(), s.native);
    if (NVSDK_NGX_FAILED(result)) { s.failure("initialization / GPU or driver", result); return false; }
    s.initialized = true;
    result = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
    if (NVSDK_NGX_FAILED(result)) { s.failure("capability query", result); return false; }
    int available = 0, needsDriver = 0;
    s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!available || needsDriver) {
        s.message = needsDriver ? "DLSS unavailable: update NVIDIA driver" : "DLSS unavailable: unsupported GPU";
        return false;
    }
    s.motionPipeline = device.makeCompute({"terrain_core/dlss_motion.hlsl", "motionCs"});
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    s.depthSampler = device.makeSampler(sampler);
    if (!s.motionPipeline || !s.depthSampler) { s.message = "DLSS unavailable: motion shader failed"; return false; }
    s.supported = true;
    s.message = "DLSS off (F12)";
    return true;
#else
    return false;
#endif
}

bool Dlss::prepare(bool requested, unsigned width, unsigned height, int quality) {
    auto& s = *impl_;
    if (!requested) {
        if (s.enabled) s.release();
        if (s.supported) s.message = "DLSS off (F12)";
        return false;
    }
    if (!s.supported || !width || !height) return false;
#if ASR_HAS_DLSS
    quality = std::clamp(quality, 0, 2);
    if (s.enabled && width == s.width && height == s.height && quality == s.quality) return true;
    s.release();
    static constexpr NVSDK_NGX_PerfQuality_Value modes[] = {
        NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf};
    // One model for every mode: left to NGX, Performance gets preset M,
    // which on this generation of card costs more than Quality's K at a
    // lower input resolution - the slowest mode was the "fast" one. K is the
    // SDK's recommended transformer preset; pinned, a driver update does not
    // change what the modes mean.
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance})
        NVSDK_NGX_Parameter_SetUI(s.parameters, hint, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    unsigned maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0;
    auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(s.parameters, width, height, modes[quality],
        &s.renderWidth, &s.renderHeight, &maxW, &maxH, &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(result) || !s.renderWidth || !s.renderHeight) {
        s.failure("optimal resolution", result); return false;
    }
    s.width = width; s.height = height; s.quality = quality;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    info.width = s.renderWidth; info.height = s.renderHeight;
    info.layer_count_or_depth = 1; info.num_levels = 1; info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    s.motion = s.device->makeTexture(info);
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width; info.height = height;
    s.output = s.device->makeTexture(info);
    if (!s.motion || !s.output) {
        s.message = "DLSS unavailable: cannot allocate textures"; s.supported = false; s.release(); return false;
    }
    auto* commands = SDL_AcquireGPUCommandBuffer(s.device->handle());
    if (!commands) { s.message = "DLSS unavailable: cannot acquire commands"; s.supported = false; s.release(); return false; }
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = s.renderWidth; create.Feature.InHeight = s.renderHeight;
    create.Feature.InTargetWidth = width; create.Feature.InTargetHeight = height;
    create.Feature.InPerfQualityValue = modes[quality];
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    result = NGX_D3D12_CREATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(Campfire_D3D12Commands(commands)),
                                     1, 1, &s.feature, s.parameters, &create);
    Campfire_D3D12Restore(commands);
    // Submit even if NGX fails: it may have recorded initialization work.
    const bool submitted = s.device->submitFrame(commands);
    SDL_WaitForGPUIdle(s.device->handle());
    if (NVSDK_NGX_FAILED(result) || !submitted) { s.failure("feature creation", result); s.release(); return false; }
    s.enabled = true;
    static constexpr const char* names[] = {"Quality", "Balanced", "Performance"};
    char text[144];
    std::snprintf(text, sizeof text, "DLSS %s %ux%u -> %ux%u (F12)", names[quality], s.renderWidth, s.renderHeight, width, height);
    s.message = text;
    SDL_Log("%s", text);
    return true;
#else
    return false;
#endif
}

void Dlss::jitter(float matrix[16]) {
    auto& s = *impl_;
    if (!s.enabled) return;
    const unsigned phases = std::max(8u, unsigned(std::ceil(8.0 * s.width * s.width / (double(s.renderWidth) * s.renderWidth))));
    const unsigned sample = s.frame % phases + 1;
    s.jitter[0] = dlss_math::halton(sample, 2) - 0.5f;
    s.jitter[1] = dlss_math::halton(sample, 3) - 0.5f;
    for (int c = 0; c < 4; ++c) {
        matrix[c] -= 2 * s.jitter[0] / float(s.renderWidth) * matrix[12 + c];
        matrix[4 + c] += 2 * s.jitter[1] / float(s.renderHeight) * matrix[12 + c];
    }
    std::memcpy(s.jittered, matrix, sizeof(s.jittered));
}

SDL_GPUTexture* Dlss::evaluate(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* colour, SDL_GPUTexture* depth,
                             const float unjittered[16], const double eye[3], float milliseconds) {
    auto& s = *impl_;
    if (!s.enabled) return nullptr;
#if ASR_HAS_DLSS
    double displacement[3]{};
    double distance2 = 0;
    for (int i = 0; i < 3; ++i) { displacement[i] = eye[i] - s.previousEye[i]; distance2 += displacement[i] * displacement[i]; }
    float orientation = 0;
    for (int i = 12; i < 15; ++i) orientation += unjittered[i] * s.previous[i];
    if (distance2 > 10000 || orientation < 0.5f) s.reset = true;
    if (s.reset) {
        std::memcpy(s.previous, unjittered, sizeof(s.previous));
        std::fill_n(displacement, 3, 0.0);
    }
    struct MotionUniforms { float matrix[16]; float resolution[2]; float jitter[2]; } u{};
    if (!dlss_math::reprojection(s.jittered, s.previous, displacement, u.matrix)) {
        s.message = "DLSS unavailable: invalid camera matrix"; s.enabled = s.supported = false; return nullptr;
    }
    u.resolution[0] = float(s.renderWidth); u.resolution[1] = float(s.renderHeight);
    std::copy_n(s.jitter, 2, u.jitter);
    SDL_GPUStorageTextureReadWriteBinding write{s.motion.get(), 0, 0, false};
    auto* pass = SDL_BeginGPUComputePass(commands, &write, 1, nullptr, 0);
    SDL_BindGPUComputePipeline(pass, s.motionPipeline.get());
    const SDL_GPUTextureSamplerBinding depthBinding{depth, s.depthSampler.get()};
    SDL_BindGPUComputeSamplers(pass, 0, &depthBinding, 1);
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    SDL_DispatchGPUCompute(pass, (s.renderWidth + 7) / 8, (s.renderHeight + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);

    NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
    const auto resource = [&](SDL_GPUTexture* texture, bool output, bool restore = false) {
        return static_cast<ID3D12Resource*>(Campfire_D3D12Texture(commands, texture, output, restore));
    };
    eval.Feature.pInColor = resource(colour, false);
    eval.Feature.pInOutput = resource(s.output.get(), true);
    eval.pInDepth = resource(depth, false);
    eval.pInMotionVectors = resource(s.motion.get(), false);
    eval.InJitterOffsetX = s.jitter[0]; eval.InJitterOffsetY = s.jitter[1];
    eval.InRenderSubrectDimensions = {s.renderWidth, s.renderHeight};
    eval.InMVScaleX = eval.InMVScaleY = 1;
    eval.InPreExposure = eval.InExposureScale = 1;
    eval.InReset = s.reset ? 1 : 0;
    eval.InFrameTimeDeltaInMsec = milliseconds;
    const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(
        static_cast<ID3D12GraphicsCommandList*>(Campfire_D3D12Commands(commands)), s.feature, s.parameters, &eval);
    resource(colour, false, true); resource(s.output.get(), true, true);
    resource(depth, false, true); resource(s.motion.get(), false, true);
    Campfire_D3D12Restore(commands);
    if (NVSDK_NGX_FAILED(result)) { s.failure("evaluation", result); return nullptr; }
    std::memcpy(s.previous, unjittered, sizeof(s.previous));
    std::copy_n(eye, 3, s.previousEye);
    s.reset = false; ++s.frame;
    return s.output.get();
#else
    return nullptr;
#endif
}
void Dlss::resetHistory() { impl_->reset = true; impl_->frame = 0; }
unsigned Dlss::renderWidth() const { return impl_->renderWidth; }
unsigned Dlss::renderHeight() const { return impl_->renderHeight; }
std::array<float, 2> Dlss::jitterOffset() const { return {impl_->jitter[0], impl_->jitter[1]}; }
bool Dlss::active() const { return impl_->enabled; }
const std::string& Dlss::status() const { return impl_->message; }
} // namespace terrain_core
