#include "terrain_core/macro_map.hpp"

#include "engine/world_source/png_io.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace terrain_core {
namespace {

std::uint32_t hash3(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = std::uint32_t(x) * 0x8da6b343u ^ std::uint32_t(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float lattice(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    return float(hash3(x, y, seed) & 0xffffffu) / float(0xffffff) * 2.0f - 1.0f;
}

// Value noise, quintic fade, period-free. Good enough for a continent outline;
// the fine detail is the GPU's.
float valueNoise(float x, float y, std::uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const auto ix = std::int32_t(fx), iy = std::int32_t(fy);
    const float tx = x - fx, ty = y - fy;
    const float ux = tx * tx * tx * (tx * (tx * 6 - 15) + 10);
    const float uy = ty * ty * ty * (ty * (ty * 6 - 15) + 10);
    const float a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const float c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

float fbm(float x, float y, int octaves, std::uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x, y, seed + std::uint32_t(o) * 101u);
        norm += amp;
        x = x * 2.03f + 17.1f; y = y * 2.03f - 9.7f; amp *= 0.5f;
    }
    return sum / norm;
}

float ridged(float x, float y, int octaves, std::uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0, weight = 1;
    for (int o = 0; o < octaves; ++o) {
        float n = 1.0f - std::fabs(valueNoise(x, y, seed + 7777u + std::uint32_t(o) * 131u));
        n *= n * weight;
        weight = std::clamp(n * 1.5f, 0.0f, 1.0f);
        sum += amp * n; norm += amp;
        x = x * 2.1f + 3.3f; y = y * 2.1f + 1.7f; amp *= 0.5f;
    }
    return sum / norm;
}

} // namespace

bool MacroMap::load(const std::filesystem::path& png, std::string* why) {
    if (png.extension() == ".r16") {
        std::ifstream in(png, std::ios::binary | std::ios::ate);
        if (!in) { if (why) *why = "cannot read " + png.string(); return false; }
        const auto bytes = std::uint64_t(in.tellg());
        const auto side = std::uint32_t(std::llround(std::sqrt(double(bytes / 2))));
        if (std::uint64_t(side) * side * 2 != bytes) { if (why) *why = "r16 map is not square"; return false; }
        std::vector<std::uint16_t> base(std::size_t(side) * side);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(base.data()), std::streamsize(bytes));
        size_ = side;
        buildPyramid(std::move(base));
        return bool(in);
    }
    auto image = engine::world_source::readPng(png, why);
    if (!image) return false;
    if (image->width != image->height || image->width == 0) {
        if (why) *why = "macro map must be square";
        return false;
    }
    std::vector<std::uint16_t> base(std::size_t(image->width) * image->height);
    const std::uint32_t scale = image->bits == 16 ? 1u : 257u;
    for (std::uint32_t y = 0; y < image->height; ++y)
        for (std::uint32_t x = 0; x < image->width; ++x)
            base[std::size_t(y) * image->width + x] = std::uint16_t(image->at(x, y, 0) * scale);
    size_ = image->width;
    buildPyramid(std::move(base));
    return true;
}

void MacroMap::generate(std::uint32_t size, std::uint32_t seed) {
    // What was here drew its mountains as the crests of a ridged noise at
    // nine cycles a world, squared: lines one or two texels wide - two
    // kilometres on a 2000 km world - which the eye reads as a scratch, not a
    // range. Mountains are belts tens of kilometres across with peaks inside
    // them, so they are built that way: a few long belts where a low-frequency
    // noise crosses zero, widened, and a ridged massif of peaks only inside
    // them. The map is normalised at the end so the highest peaks reach the
    // top of the range whatever the noise happened to do.
    std::vector<float> field(std::size_t(size) * size);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (float(x) + 0.5f) / float(size), v = (float(y) + 0.5f) / float(size);
            // Warped continents, pulled down towards the edge of the world so
            // it ends in sea rather than in a cut.
            const float wx = fbm(u * 3.0f, v * 3.0f, 4, seed + 1) * 0.35f;
            const float wy = fbm(u * 3.0f + 5.2f, v * 3.0f + 1.3f, 4, seed + 2) * 0.35f;
            const float pu = u + wx, pv = v + wy;
            float land = fbm(pu * 2.5f, pv * 2.5f, 7, seed + 3);
            const float dx = u - 0.5f, dy = v - 0.5f;
            land += 0.25f - 1.2f * (dx * dx + dy * dy);
            const float coast = std::clamp((land + 0.02f) * 6.0f, 0.0f, 1.0f);
            const float inland = std::clamp((land - 0.04f) * 4.0f, 0.0f, 1.0f);

            // Belts: where a two-cycle noise crosses zero, ~5% of the world wide.
            const float beltNoise = fbm(pu * 2.2f + 11.0f, pv * 2.2f - 4.0f, 4, seed + 5);
            const float belt = std::clamp(1.0f - std::fabs(beltNoise) / 0.25f, 0.0f, 1.0f);
            const float beltShape = belt * belt * (3.0f - 2.0f * belt);
            // Peaks inside the belts: a ridged massif, its crests the summits.
            const float massif = ridged(pu * 9.0f, pv * 9.0f, 6, seed + 4);
            // Rolling ground everywhere else, and a high plateau now and then.
            const float hills = fbm(pu * 18.0f, pv * 18.0f, 5, seed + 6) * 0.5f + 0.5f;
            const float plateau = std::clamp((fbm(pu * 4.0f - 2.0f, pv * 4.0f + 9.0f, 3, seed + 7) - 0.15f) * 4.0f, 0.0f, 1.0f);

            float h = 0.13f * coast + inland * (0.05f + 0.06f * hills + 0.10f * plateau +
                                                beltShape * (0.22f + 0.6f * massif * std::sqrt(massif)));
            if (land < -0.02f) h = std::max(0.0f, 0.12f + land);   // sea floor below the coast
            field[std::size_t(y) * size + x] = h;
        }
    }
    // Scale the land (not the sea floor) so the 99.95th percentile of it
    // stands at 0.97 of the range.
    std::vector<float> high;
    high.reserve(field.size() / 8);
    for (std::size_t i = 0; i < field.size(); i += 7) if (field[i] > 0.13f) high.push_back(field[i]);
    float top = 1.0f;
    if (!high.empty()) {
        const std::size_t k = std::min(high.size() - 1, std::size_t(double(high.size()) * 0.9995));
        std::nth_element(high.begin(), high.begin() + std::ptrdiff_t(k), high.end());
        top = high[k];
    }
    const float stretch = top > 0.13f ? (0.97f - 0.13f) / (top - 0.13f) : 1.0f;
    std::vector<std::uint16_t> base(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        const float h = field[i] > 0.13f ? 0.13f + (field[i] - 0.13f) * stretch : field[i];
        base[i] = std::uint16_t(std::clamp(h, 0.0f, 1.0f) * 65535.0f);
    }
    size_ = size;
    buildPyramid(std::move(base));
}

void MacroMap::assign(std::uint32_t size, std::vector<std::uint16_t> texels) {
    size_ = size;
    buildPyramid(std::move(texels));
}

bool MacroMap::save(const std::filesystem::path& png, std::string* why) const {
    if (png.extension() == ".r16") {
        std::error_code ignored;
        std::filesystem::create_directories(png.parent_path(), ignored);
        std::ofstream out(png, std::ios::binary);
        out.write(reinterpret_cast<const char*>(texels().data()), std::streamsize(texels().size() * 2));
        if (!out && why) *why = "cannot write " + png.string();
        return bool(out);
    }
    engine::world_source::Image image;
    image.width = image.height = size_;
    image.channels = 1;
    image.bits = 16;
    image.words = texels();
    std::error_code ignored;
    std::filesystem::create_directories(png.parent_path(), ignored);
    return engine::world_source::writePng(png, image, why);
}

void MacroMap::buildPyramid(std::vector<std::uint16_t> base) {
    levels_.clear();
    Level level{size_, std::move(base), {}};
    levels_.push_back(std::move(level));
    while (levels_.back().size > 1) {
        const Level& fine = levels_.back();
        Level coarse;
        coarse.size = (fine.size + 1) / 2;
        coarse.lo.assign(std::size_t(coarse.size) * coarse.size, 65535);
        coarse.hi.assign(std::size_t(coarse.size) * coarse.size, 0);
        const auto& fineHi = fine.hi.empty() ? fine.lo : fine.hi;
        for (std::uint32_t y = 0; y < fine.size; ++y)
            for (std::uint32_t x = 0; x < fine.size; ++x) {
                const std::size_t f = std::size_t(y) * fine.size + x;
                const std::size_t c = std::size_t(y / 2) * coarse.size + x / 2;
                coarse.lo[c] = std::min(coarse.lo[c], fine.lo[f]);
                coarse.hi[c] = std::max(coarse.hi[c], fineHi[f]);
            }
        levels_.push_back(std::move(coarse));
    }
}

float MacroMap::sample(double u, double v) const {
    if (levels_.empty()) return 0.0f;
    const auto& t = levels_[0].lo;
    const double x = std::clamp(u * size_ - 0.5, 0.0, double(size_ - 1));
    const double y = std::clamp(v * size_ - 0.5, 0.0, double(size_ - 1));
    const auto x0 = std::uint32_t(x), y0 = std::uint32_t(y);
    const std::uint32_t x1 = std::min(x0 + 1, size_ - 1), y1 = std::min(y0 + 1, size_ - 1);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](std::uint32_t a, std::uint32_t b) { return double(t[std::size_t(b) * size_ + a]) / 65535.0; };
    return float((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy);
}

std::pair<float, float> MacroMap::range(double u0, double v0, double u1, double v1) const {
    if (levels_.empty()) return {0.0f, 0.0f};
    // The level at which the square is a few texels across; one texel of
    // margin round it for the bicubic's reach.
    const double span = std::max(u1 - u0, v1 - v0) * size_;
    std::uint32_t k = 0;
    while (k + 1 < levels_.size() && double(1u << (k + 1)) < span * 0.5) ++k;
    const Level& level = levels_[k];
    const double texel = double(1u << k) / double(size_);   // one level-k texel, in map units
    auto clampi = [&](double t) {
        return std::uint32_t(std::clamp(t, 0.0, double(level.size - 1)));
    };
    const std::uint32_t x0 = clampi(std::floor(u0 / texel) - 1), x1 = clampi(std::floor(u1 / texel) + 1);
    const std::uint32_t y0 = clampi(std::floor(v0 / texel) - 1), y1 = clampi(std::floor(v1 / texel) + 1);
    std::uint16_t lo = 65535, hi = 0;
    for (std::uint32_t y = y0; y <= y1; ++y)
        for (std::uint32_t x = x0; x <= x1; ++x) {
            lo = std::min(lo, level.lo[std::size_t(y) * level.size + x]);
            hi = std::max(hi, (level.hi.empty() ? level.lo : level.hi)[std::size_t(y) * level.size + x]);
        }
    // Catmull-Rom overshoots its samples by up to an eighth of a step.
    const float spread = float(hi - lo) / 65535.0f * 0.125f;
    return {float(lo) / 65535.0f - spread, float(hi) / 65535.0f + spread};
}

} // namespace terrain_core
