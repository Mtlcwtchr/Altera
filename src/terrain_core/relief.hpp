#pragma once
// Fine relief: small windows of the ground at a metre or two a texel, for
// what the macro map (16 m a texel on a 131 km world) cannot hold - a marsh
// hill's low terraces, steps of half a man's height. The height generator
// (generate.hlsl, relief.hlsli) lays each over the ground it makes: within a
// patch the ground is the patch's own height, eased into the land round it by
// the patch's weight. Everything that reads the clipmap (the ground, the
// grass, the water's shore) then stands on it.
//
// File <world>_relief.bin (tools/terrain_lab/marsh_islands.py,
// canyon_relief.py): "RELF", u32 version (1 or 2), u32 patch count; per patch
// f64 x0, y0 (its south-west corner, metres), f32 texel (metres), f32 base
// (metres), u32 width, height, and in version 2 u32 tile and a u32 spare.
// A dense patch (version 1, or tile 0): width * height pairs of f16 (height
// over base, weight 0..1), row 0 south. A sparse one (tile > 0: a canyon's
// windows over a whole zone, most of it empty): per tile of tile x tile
// texels (row-major, row 0 south) a u32 index into the stored tiles
// (0xffffffff: none) and an f32 base (metres), then a u32 count of stored
// tiles and their tile * tile pairs of f16 (height over the tile's base,
// weight) each.
//
// On the GPU two read-only storage buffers (the generator and the ground's
// shading read them), bound together and in this
// order: the patches (48 bytes each; an empty set is one patch of no size)
// and their texels (u32: height half low, weight half high), a sparse
// patch's table there too (u32 pairs: the stored tile's first texel or
// 0xffffffff, the base's bits).

#include "engine/render/device.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace terrain_core {

class Relief {
public:
    // Reads `file`; `spacing`: the clipmap's finest spacing, the unit the
    // patches' corners are placed in.
    bool load(const std::filesystem::path& file, double spacing, std::string* why = nullptr);
    bool open(engine::Device& device, std::string* why = nullptr);
    std::array<SDL_GPUBuffer*, 2> buffers() const { return {patchBuffer_.get(), texelBuffer_.get()}; }
    std::size_t count() const { return patches_.size(); }
    std::uint64_t gpuBytes() const { return gpuBytes_; }

private:
    struct Patch {
        std::int32_t originX, originY;   // corner, in the finest spacing's texels
        std::uint32_t width, height;
        float texel, base;
        std::uint32_t offset;            // dense: the first texel; sparse: the table
        std::uint32_t tile;              // 0: dense
        std::uint32_t pad[4];
    };
    std::vector<Patch> patches_;
    std::vector<std::uint32_t> texels_;
    engine::Buffer patchBuffer_, texelBuffer_;
    std::uint64_t gpuBytes_ = 0;
};

} // namespace terrain_core
