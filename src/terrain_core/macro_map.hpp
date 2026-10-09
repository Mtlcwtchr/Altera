#pragma once
// The macro height map: one 16-bit picture stretched over the whole world.
//
// Either imported (any 16-bit greyscale PNG) or drawn by the generator, which
// writes the same kind of PNG - so everything after this point has one path.
// On the GPU it is the coarse shape the procedural detail rides on; on the CPU
// it answers "how high can the ground be in this square", which is all culling
// needs, through a min/max pyramid.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace terrain_core {

class MacroMap {
public:
    bool load(const std::filesystem::path& png, std::string* why = nullptr);
    // A continent-and-ranges map from fBm, `size` texels square. Deterministic
    // in `seed`. Written as PNG16 by save().
    void generate(std::uint32_t size, std::uint32_t seed);
    bool save(const std::filesystem::path& png, std::string* why = nullptr) const;

    std::uint32_t size() const { return size_; }
    const std::vector<std::uint16_t>& texels() const { return levels_.empty() ? empty_ : levels_[0].lo; }

    // Bilinear macro value (0..1) at map coordinates (0..1). The CPU's idea of
    // the ground before the GPU has said anything: a camera placed on it.
    float sample(double u, double v) const;
    // Lowest and highest macro value (0..1) over the square [u0,u1]x[v0,v1] of
    // the map, in 0..1 map coordinates. Conservative, never tight.
    std::pair<float, float> range(double u0, double v0, double u1, double v1) const;

private:
    void buildPyramid(std::vector<std::uint16_t> base);
    struct Level { std::uint32_t size; std::vector<std::uint16_t> lo, hi; };
    std::vector<Level> levels_;
    std::vector<std::uint16_t> empty_;
    std::uint32_t size_ = 0;
};

} // namespace terrain_core
