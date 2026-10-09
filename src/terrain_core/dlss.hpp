#pragma once
#include "engine/render/device.hpp"
#include <array>
#include <memory>

namespace terrain_core {
// DLSS Super Resolution, with an explicit native-resolution fallback. GPU
// resources and NGX are destroyed before the Device owning them.
class Dlss {
public:
    Dlss();
    ~Dlss();
    Dlss(const Dlss&) = delete;
    Dlss& operator=(const Dlss&) = delete;
    bool open(engine::Device& device);
    bool prepare(bool enabled, unsigned width, unsigned height, int quality);
    // Eye-relative, row-major camera matrices, as used by Terrain Lab.
    void jitter(float matrix[16]);
    SDL_GPUTexture* evaluate(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* colour,
                             SDL_GPUTexture* depth, const float unjittered[16],
                             const double eye[3], float milliseconds);
    void resetHistory();
    unsigned renderWidth() const;
    unsigned renderHeight() const;
    std::array<float, 2> jitterOffset() const;
    bool active() const;
    const std::string& status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace terrain_core
