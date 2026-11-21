#include "terrain_core/relief.hpp"

#include <algorithm>
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
    filePatches_ = patches_.size();
    fileTexels_ = texels_.size();
    return true;
}

namespace {

std::uint16_t toHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int e = int((x >> 23) & 0xff) - 127 + 15;
    std::uint32_t m = x & 0x7fffffu;
    if (e <= 0) return std::uint16_t(sign);   // too small: zero
    if (e >= 31) return std::uint16_t(sign | 0x7c00u);
    m += 0x1000u;   // round to nearest
    if (m & 0x800000u) {
        m = 0;
        if (e + 1 >= 31) return std::uint16_t(sign | 0x7c00u);
        return std::uint16_t(sign | (std::uint32_t(e + 1) << 10));
    }
    return std::uint16_t(sign | (std::uint32_t(e) << 10) | (m >> 13));
}

} // namespace

bool Relief::setPads(const std::vector<Pad>& pads, double spacing) {
    if (!fileKept_) return false;
    patches_.resize(filePatches_);
    texels_.resize(fileTexels_);
    pads_.clear();
    for (const Pad& d : pads) {
        if (d.width == 0 || d.height == 0 || !(d.texel > 0)) continue;
        const std::size_t n = std::size_t(d.width) * d.height;
        if (d.rise.size() != n || d.weight.size() != n) continue;
        Patch p{};
        p.originX = std::int32_t(std::lround(d.x0 / spacing));
        p.originY = std::int32_t(std::lround(d.y0 / spacing));
        p.width = d.width;
        p.height = d.height;
        p.texel = d.texel;
        p.base = d.base;
        p.offset = std::uint32_t(texels_.size());
        p.tile = 0;
        for (std::size_t k = 0; k < n; ++k)
            texels_.push_back(std::uint32_t(toHalf(d.rise[k])) |
                              (std::uint32_t(toHalf(std::clamp(d.weight[k], 0.0f, 1.0f))) << 16));
        patches_.push_back(p);
        Pad kept = d;
        kept.x0 = double(p.originX) * spacing;   // where the GPU has it
        kept.y0 = double(p.originY) * spacing;
        pads_.push_back(std::move(kept));
    }
    ++padVersion_;
    return true;
}

float Relief::overPads(double x, double y, float ground) const {
    float h = ground;
    for (const Pad& d : pads_) {
        const double qx = (x - d.x0) / d.texel - 0.5, qy = (y - d.y0) / d.texel - 0.5;
        if (qx < -0.5 || qy < -0.5 || qx >= double(d.width) - 0.5 || qy >= double(d.height) - 0.5) continue;
        const int cx = int(std::floor(qx)), cy = int(std::floor(qy));
        const float fx = float(qx - cx), fy = float(qy - cy);
        auto at = [&](const std::vector<float>& v, int i, int j) {
            i = std::clamp(i, 0, int(d.width) - 1);
            j = std::clamp(j, 0, int(d.height) - 1);
            return v[std::size_t(j) * d.width + std::size_t(i)];
        };
        auto bil = [&](const std::vector<float>& v) {
            const float a = at(v, cx, cy), b = at(v, cx + 1, cy), c = at(v, cx, cy + 1), e = at(v, cx + 1, cy + 1);
            return (a + (b - a) * fx) + ((c + (e - c) * fx) - (a + (b - a) * fx)) * fy;
        };
        const float w = std::clamp(bil(d.weight), 0.0f, 1.0f);
        if (w <= 0) continue;
        h = h + (d.base + bil(d.rise) - h) * w;
    }
    return h;
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
    // The texels stay for setPads (the game's pads go after them), unless the
    // file's relief is big (a canyon's windows): then they go, and no pads.
    if (fileTexels_ * 4 > (std::size_t(256) << 20)) {
        texels_.clear();
        texels_.shrink_to_fit();
        fileKept_ = false;
    }
    return true;
}

} // namespace terrain_core
