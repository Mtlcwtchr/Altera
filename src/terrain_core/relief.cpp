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
    if (!in || std::memcmp(magic, "RELF", 4) != 0 || (version != 1 && version != 2))
        return fail("not a relief file (RELF 1 or 2)");
    patches_.clear();
    texels_.clear();
    for (std::uint32_t k = 0; k < count; ++k) {
        double x0 = 0, y0 = 0;
        float texel = 0, base = 0;
        std::uint32_t w = 0, h = 0, tile = 0, spare = 0;
        in.read(reinterpret_cast<char*>(&x0), 8);
        in.read(reinterpret_cast<char*>(&y0), 8);
        in.read(reinterpret_cast<char*>(&texel), 4);
        in.read(reinterpret_cast<char*>(&base), 4);
        in.read(reinterpret_cast<char*>(&w), 4);
        in.read(reinterpret_cast<char*>(&h), 4);
        if (version >= 2) {
            in.read(reinterpret_cast<char*>(&tile), 4);
            in.read(reinterpret_cast<char*>(&spare), 4);
        }
        if (!in || w == 0 || h == 0 || w > 65536 || h > 65536 || !(texel > 0) || tile > 256)
            return fail("bad patch header");
        Patch p{};
        p.originX = std::int32_t(std::lround(x0 / spacing));
        p.originY = std::int32_t(std::lround(y0 / spacing));
        p.width = w;
        p.height = h;
        p.texel = texel;
        p.base = base;
        p.offset = std::uint32_t(texels_.size());
        p.tile = tile;
        if (tile == 0) {
            const std::size_t n = std::size_t(w) * h;
            texels_.resize(texels_.size() + n);
            in.read(reinterpret_cast<char*>(texels_.data() + p.offset), std::streamsize(n * 4));
            if (!in) return fail("truncated");
        } else {
            // The table, then the stored tiles after it; the table's indices
            // become the tiles' first texels in the buffer.
            const std::size_t entries = std::size_t((w + tile - 1) / tile) * ((h + tile - 1) / tile);
            texels_.resize(texels_.size() + entries * 2);
            in.read(reinterpret_cast<char*>(texels_.data() + p.offset), std::streamsize(entries * 8));
            std::uint32_t stored = 0;
            in.read(reinterpret_cast<char*>(&stored), 4);
            if (!in) return fail("truncated");
            const std::size_t first = texels_.size(), per = std::size_t(tile) * tile;
            if (first + std::size_t(stored) * per >= 0xffffffffu) return fail("too big");
            texels_.resize(first + std::size_t(stored) * per);
            in.read(reinterpret_cast<char*>(texels_.data() + first), std::streamsize(std::size_t(stored) * per * 4));
            if (!in) return fail("truncated");
            for (std::size_t e = 0; e < entries; ++e) {
                std::uint32_t& index = texels_[p.offset + e * 2];
                if (index == 0xffffffffu) continue;
                if (index >= stored) return fail("bad tile index");
                index = std::uint32_t(first + std::size_t(index) * per);
            }
        }
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
