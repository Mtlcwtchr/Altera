#include "terrain_core/relief.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace terrain_core {

bool Relief::load(const std::filesystem::path& file, double spacing, std::string* why) {
    std::ifstream in(file, std::ios::binary);
    auto fail = [&](const char* what) {
        if (why) *why = file.string() + ": " + what;
        patches_.clear();
        texels_.clear();
        return false;
    };
    if (!in) return fail("cannot open");
    char magic[4];
    std::uint32_t version = 0, count = 0;
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&count), 4);
    if (!in || std::memcmp(magic, "RELF", 4) != 0 || version != 1) return fail("not a relief file (RELF 1)");
    patches_.clear();
    texels_.clear();
    for (std::uint32_t k = 0; k < count; ++k) {
        double x0 = 0, y0 = 0;
        float texel = 0, base = 0;
        std::uint32_t w = 0, h = 0;
        in.read(reinterpret_cast<char*>(&x0), 8);
        in.read(reinterpret_cast<char*>(&y0), 8);
        in.read(reinterpret_cast<char*>(&texel), 4);
        in.read(reinterpret_cast<char*>(&base), 4);
        in.read(reinterpret_cast<char*>(&w), 4);
        in.read(reinterpret_cast<char*>(&h), 4);
        if (!in || w == 0 || h == 0 || w > 8192 || h > 8192 || !(texel > 0)) return fail("bad patch header");
        Patch p{};
        p.originX = std::int32_t(std::lround(x0 / spacing));
        p.originY = std::int32_t(std::lround(y0 / spacing));
        p.width = w;
        p.height = h;
        p.texel = texel;
        p.base = base;
        p.offset = std::uint32_t(texels_.size());
        const std::size_t n = std::size_t(w) * h;
        texels_.resize(texels_.size() + n);
        in.read(reinterpret_cast<char*>(texels_.data() + p.offset), std::streamsize(n * 4));
        if (!in) return fail("truncated");
        patches_.push_back(p);
    }
    return true;
}

bool Relief::open(engine::Device& device, std::string* why) {
    const auto usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    const Patch none{};
    const std::uint32_t zero = 0;
    patchBuffer_ = device.uploadBuffer(usage, patches_.empty() ? &none : patches_.data(),
                                       std::max<std::size_t>(patches_.size(), 1) * sizeof(Patch));
    texelBuffer_ = device.uploadBuffer(usage, texels_.empty() ? &zero : texels_.data(),
                                       std::max<std::size_t>(texels_.size(), 1) * 4);
    if (!patchBuffer_ || !texelBuffer_) {
        if (why) *why = "relief buffers: " + device.error();
        return false;
    }
    gpuBytes_ = std::max<std::size_t>(patches_.size(), 1) * sizeof(Patch) + std::max<std::size_t>(texels_.size(), 1) * 4;
    texels_.clear();
    texels_.shrink_to_fit();
    return true;
}

} // namespace terrain_core
