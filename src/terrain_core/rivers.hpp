#pragma once
// The river graph's segments on the GPU (hydrology.hpp), with a grid of
// tiles over the world each listing the segments that reach into it -
// what assets/shaders/terrain_core/rivers.hlsli walks to find the nearest
// river to a point. Read by the height generator (the bed and banks), the
// ground (rivers too thin for the mesh, painted), the water (the surface) and
// the plants' spawns (nothing in the river).
//
// Three read-only storage buffers, always bound together and in this order:
// segments (three float4 each), tiles (uint2 offset, count; element 0 is the
// header: tile size in metres as float bits, tiles a side), and the list.

#include "engine/render/device.hpp"
#include "terrain_core/hydrology.hpp"

#include <array>
#include <string>

namespace terrain_core {

class Rivers {
public:
    bool open(engine::Device& device, const std::vector<RiverSegment>& segments, double worldSize,
              std::string* why = nullptr);
    // segments, tiles, list: for SDL_Bind*StorageBuffers.
    std::array<SDL_GPUBuffer*, 3> buffers() const { return {segments_.get(), tiles_.get(), list_.get()}; }
    std::uint64_t gpuBytes() const { return gpuBytes_; }
    std::uint32_t tilesPerSide() const { return tilesPerSide_; }
    std::uint32_t longestList() const { return longestList_; }

private:
    engine::Buffer segments_, tiles_, list_;
    std::uint64_t gpuBytes_ = 0;
    std::uint32_t tilesPerSide_ = 0, longestList_ = 0;
};

} // namespace terrain_core
