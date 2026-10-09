#include "terrain_core/ground_height.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace terrain_core {
namespace {

// gradientAt()'s directions: the angle is a 16-bit hash, so a table.
struct Directions {
    std::array<float, 65536> c{}, s{};
    Directions() {
        for (std::uint32_t i = 0; i < 65536; ++i) {
            const float a = float(i) * (6.28318530718f / 65536.0f);
            c[i] = std::cos(a);
            s[i] = std::sin(a);
        }
    }
};
const Directions& directions() {
    static const Directions table;
    return table;
}

std::uint32_t hashLattice(std::int32_t cx, std::int32_t cy, std::uint32_t octave, std::uint32_t seed) {
    std::uint32_t h = std::uint32_t(cx) * 0x8da6b343u ^ std::uint32_t(cy) * 0xd8163841u ^
                      (seed + octave * 0x9e3779b9u) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

struct Noise { float v, dx, dy; };

// gradientNoise(): value and derivative in cell units, quintic fade.
Noise gradientNoise(std::int32_t cx, std::int32_t cy, float fx, float fy, std::uint32_t octave, std::uint32_t seed) {
    const Directions& d = directions();
    auto grad = [&](std::int32_t x, std::int32_t y, float& gx, float& gy) {
        const std::uint32_t i = hashLattice(x, y, octave, seed) & 0xffffu;
        gx = d.c[i];
        gy = d.s[i];
    };
    float gax, gay, gbx, gby, gcx, gcy, gdx, gdy;
    grad(cx, cy, gax, gay);
    grad(cx + 1, cy, gbx, gby);
    grad(cx, cy + 1, gcx, gcy);
    grad(cx + 1, cy + 1, gdx, gdy);
    const float ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10), uy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    const float dux = 30 * fx * fx * (fx * (fx - 2) + 1), duy = 30 * fy * fy * (fy * (fy - 2) + 1);
    const float va = gax * fx + gay * fy, vb = gbx * (fx - 1) + gby * fy;
    const float vc = gcx * fx + gcy * (fy - 1), vd = gdx * (fx - 1) + gdy * (fy - 1);
    const float k = va - vb - vc + vd;
    Noise n;
    n.v = va + ux * (vb - va) + uy * (vc - va) + ux * uy * k;
    n.dx = gax + ux * (gbx - gax) + uy * (gcx - gax) + ux * uy * (gax - gbx - gcx + gdx) + dux * (uy * k + vb - va);
    n.dy = gay + ux * (gby - gay) + uy * (gcy - gay) + ux * uy * (gay - gby - gcy + gdy) + duy * (ux * k + vc - va);
    return n;
}

void catmullRom(float t, float w[4], float dw[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1;
    w[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
    dw[0] = -1.5f * t2 + 2 * t - 0.5f;
    dw[1] = 4.5f * t2 - 5 * t;
    dw[2] = -4.5f * t2 + 4 * t + 0.5f;
    dw[3] = 1.5f * t2 - t;
}

float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

GroundHeight::GroundHeight(const MacroMap& macro, const Params& params)
    : macro_(macro), params_(params) {
    macroTexel_ = params.worldSize / std::max(1u, macro.size());
    // The GPU's octave top follows the macro texel; take it from the map
    // itself, whatever the caller's Params were told.
    params_.macroTexel = macroTexel_;
    octaveTop_ = params_.octaveTop();
}

GroundHeight::Macro GroundHeight::macro(double x, double y) const {
    const int size = int(macro_.size());
    const auto& texels = macro_.texels();
    Macro m{0, 0, 0};
    if (size == 0 || texels.empty()) return m;
    const double px = x / macroTexel_ - 0.5, py = y / macroTexel_ - 0.5;
    const double bx = std::floor(px), by = std::floor(py);
    float wx[4], dwx[4], wy[4], dwy[4];
    catmullRom(float(px - bx), wx, dwx);
    catmullRom(float(py - by), wy, dwy);
    for (int j = 0; j < 4; ++j) {
        const int ty = std::clamp(int(by) + j - 1, 0, size - 1);
        float row = 0, drow = 0;
        for (int i = 0; i < 4; ++i) {
            const int tx = std::clamp(int(bx) + i - 1, 0, size - 1);
            const float v = float(texels[std::size_t(ty) * size + tx]) / 65535.0f;
            row += wx[i] * v;
            drow += dwx[i] * v;
        }
        m.value += wy[j] * row;
        m.dx += wy[j] * drow;
        m.dy += dwy[j] * row;
    }
    return m;
}

float GroundHeight::detailOn(const Macro& m, double x, double y, double minWavelength) const {
    const float range = float(params_.macroHeightRange), sea = float(params_.macroSeaLevel);
    const float spacing = float(params_.spacing);
    float slopeX = m.dx * range / float(macroTexel_), slopeY = m.dy * range / float(macroTexel_);
    const float elevation = saturate((m.value - sea) * 2.5f);
    const float steepness = saturate(std::sqrt(slopeX * slopeX + slopeY * slopeY) * 4.0f);
    const float land = saturate((m.value - sea) * 100.0f);
    const float rough = (0.25f + 1.75f * std::max(elevation * elevation, steepness)) * float(params_.detailAmplitude) * land;
    if (rough <= 0) return 0;
    const float ridged = saturate(elevation * 1.6f - 0.3f);

    const auto ux = std::int32_t(std::floor(x / params_.spacing + 0.5));
    const auto uy = std::int32_t(std::floor(y / params_.spacing + 0.5));
    std::uint32_t lowest = 1;
    if (minWavelength > params_.spacing)
        lowest = std::max(1u, std::uint32_t(std::ceil(std::log2(minWavelength / params_.spacing) - 1e-9)));
    float amplitude = 0.04f * spacing * float(1u << octaveTop_);
    float h = 0;
    for (std::uint32_t o = octaveTop_; o >= lowest && o < 32; --o) {
        const std::int32_t cx = ux >> o, cy = uy >> o;
        const float scale = float(1u << o);
        const float fx = float(ux - (cx << o)) / scale, fy = float(uy - (cy << o)) / scale;
        const Noise n = gradientNoise(cx, cy, fx, fy, o, params_.seed);
        const float wavelength = spacing * scale;
        const float value = n.v + (0.35f - std::abs(n.v) - n.v) * ridged;
        const float sign = n.v > 0 ? 1.0f : n.v < 0 ? -1.0f : 0.0f;
        const float sx = (n.dx + (-sign * n.dx - n.dx) * ridged) / wavelength;
        const float sy = (n.dy + (-sign * n.dy - n.dy) * ridged) / wavelength;
        const float damp = 1.0f / (1.0f + 1.5f * (slopeX * slopeX + slopeY * slopeY));
        const float a = amplitude * rough * damp;
        h += a * value;
        slopeX += a * sx;
        slopeY += a * sy;
        amplitude *= 0.48f;
        if (o == 0) break;
    }
    return h;
}

float GroundHeight::at(double x, double y, double minWavelength) const {
    const Macro m = macro(x, y);
    const float base = (m.value - float(params_.macroSeaLevel)) * float(params_.macroHeightRange);
    return base + detailOn(m, x, y, minWavelength);
}

float GroundHeight::detail(double x, double y, double minWavelength) const {
    return detailOn(macro(x, y), x, y, minWavelength);
}

} // namespace terrain_core

