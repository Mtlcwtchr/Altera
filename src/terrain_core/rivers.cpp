#include "terrain_core/rivers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace terrain_core {

bool Rivers::open(engine::Device& device, const std::vector<RiverSegment>& segments, double worldSize, std::string* why) {
    // Tiles of about half a kilometre on a 500 km world, two on a 2000 km one.
    tilesPerSide_ = segments.empty() ? 0 : 1024;
    const double tile = worldSize / std::max(1u, tilesPerSide_);
    std::vector<std::uint32_t> counts(std::size_t(tilesPerSide_) * tilesPerSide_, 0);
    auto cover = [&](const RiverSegment& s, auto&& visit) {
        // Everything a point could need from this segment: the water, the
        // banks, and a little over.
        const double reach = std::max(s.width0, s.width1) * 0.5 + s.bank + 2.0;
        const int n = int(tilesPerSide_);
        const int x0 = std::clamp(int(std::floor((std::min(s.x0, s.x1) - reach) / tile)), 0, n - 1);
        const int x1 = std::clamp(int(std::floor((std::max(s.x0, s.x1) + reach) / tile)), 0, n - 1);
        const int y0 = std::clamp(int(std::floor((std::min(s.y0, s.y1) - reach) / tile)), 0, n - 1);
        const int y1 = std::clamp(int(std::floor((std::max(s.y0, s.y1) + reach) / tile)), 0, n - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) visit(std::size_t(y) * n + x);
    };
    for (const RiverSegment& s : segments) cover(s, [&](std::size_t t) { ++counts[t]; });
    std::vector<std::uint32_t> tiles(2 + counts.size() * 2, 0);
    const float tileF = float(tile);
    std::memcpy(&tiles[0], &tileF, 4);
    tiles[1] = tilesPerSide_;
    std::uint32_t offset = 0;
    for (std::size_t t = 0; t < counts.size(); ++t) {
        tiles[2 + t * 2] = offset;
        offset += counts[t];
        longestList_ = std::max(longestList_, counts[t]);
        counts[t] = 0;
    }
    std::vector<std::uint32_t> list(std::max<std::uint32_t>(offset, 1), 0);
    for (std::uint32_t i = 0; i < segments.size(); ++i)
        cover(segments[i], [&](std::size_t t) { list[tiles[2 + t * 2] + counts[t]++] = i; });
    for (std::size_t t = 0; t < counts.size(); ++t) tiles[3 + t * 2] = counts[t];

    const auto usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    const RiverSegment none{};
    segments_ = device.uploadBuffer(usage, segments.empty() ? &none : segments.data(),
                                    std::max<std::size_t>(segments.size(), 1) * sizeof(RiverSegment));
    tiles_ = device.uploadBuffer(usage, tiles.data(), tiles.size() * 4);
    list_ = device.uploadBuffer(usage, list.data(), list.size() * 4);
    if (!segments_ || !tiles_ || !list_) {
        if (why) *why = "river buffers: " + device.error();
        return false;
    }
    gpuBytes_ = std::max<std::size_t>(segments.size(), 1) * sizeof(RiverSegment) + tiles.size() * 4 + list.size() * 4;
    return true;
}

} // namespace terrain_core
