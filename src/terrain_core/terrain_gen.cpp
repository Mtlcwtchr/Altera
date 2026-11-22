// Ported from Campfire (CampfireLite core/src/landscape/terrain_gen.cpp): the
// same steps and constants, on terrain_core's own grid; see terrain_gen.hpp.

#include "terrain_core/terrain_gen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>

namespace terrain_core::gen {
namespace {

constexpr int Dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int Dz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr float Dist[8] = {1, 1, 1, 1, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};

std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// Each coordinate goes through the mix on its own: a hash that only XORs them
// draws lines along the axes and the diagonals into the noise.
std::uint32_t hash3(int x, int z, std::uint32_t seed) {
    return mix32(mix32(mix32(seed ^ 0x9e3779b9u) ^ std::uint32_t(x)) ^ std::uint32_t(z) * 0x85ebca6bu);
}

// Deterministic random numbers (splitmix64), as Campfire's.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
};

struct Graph {
    std::vector<int> receiver;
    std::vector<float> dist;
    std::vector<int> order;   // ascending height: every cell after its receiver
};

// D8 steepest descent over a depression-free surface; border cells and cells
// at or below the base drain nowhere.
Graph drainage(const Grid& g, const std::vector<float>& filled, float base, std::uint32_t wander = 0) {
    Graph gr;
    const int n = g.w * g.h;
    gr.receiver.resize(std::size_t(n));
    gr.dist.assign(std::size_t(n), 1.f);
    for (int j = 0; j < g.h; ++j)
        for (int i = 0; i < g.w; ++i) {
            const int k = int(g.index(i, j));
            gr.receiver[std::size_t(k)] = k;
            if (i == 0 || j == 0 || i == g.w - 1 || j == g.h - 1 || filled[std::size_t(k)] <= base) continue;
            float best = 0;
            for (int d = 0; d < 8; ++d) {
                const int nk = int(g.index(i + Dx[d], j + Dz[d]));
                float slope = (filled[std::size_t(k)] - filled[std::size_t(nk)]) / Dist[d];
                // wander: not always the steepest - the water's way varies
                // from step to step, so the channels do not set into straight
                // lines along the grid's eight directions
                if (wander && slope > 0) slope *= 0.55f + 0.9f * float(hash3(k, d, wander) & 0xFFFF) / 65535.f;
                if (slope > best) {
                    best = slope;
                    gr.receiver[std::size_t(k)] = nk;
                    gr.dist[std::size_t(k)] = Dist[d] * g.spacing;
                }
            }
        }
    gr.order.resize(std::size_t(n));
    std::iota(gr.order.begin(), gr.order.end(), 0);
    std::stable_sort(gr.order.begin(), gr.order.end(),
                     [&](int a, int b) { return filled[std::size_t(a)] < filled[std::size_t(b)]; });
    return gr;
}

std::vector<float> accumulate(const Graph& gr, float cellArea) {
    std::vector<float> area(gr.order.size(), cellArea);
    for (auto it = gr.order.rbegin(); it != gr.order.rend(); ++it) {
        const int r = gr.receiver[std::size_t(*it)];
        if (r != *it) area[std::size_t(r)] += area[std::size_t(*it)];
    }
    return area;
}

} // namespace

Grid Grid::make(int w, int h, float spacing, float height) {
    Grid g;
    g.w = w;
    g.h = h;
    g.spacing = spacing;
    g.heights.assign(std::size_t(w) * std::size_t(h), height);
    return g;
}

float Grid::sample(double i, double j) const {
    // Catmull-Rom: a coarse grid blown up bilinearly keeps a crease along
    // every one of its cell edges, and water finds them.
    i = std::clamp(i, 0.0, double(w - 1));
    j = std::clamp(j, 0.0, double(h - 1));
    const int i0 = int(i), j0 = int(j);
    const float u = float(i - i0), v = float(j - j0);
    auto cr = [](float p0, float p1, float p2, float p3, float t) {
        return p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    };
    float row[4];
    for (int b = -1; b <= 2; ++b) {
        const int jj = std::clamp(j0 + b, 0, h - 1);
        float p[4];
        for (int a = -1; a <= 2; ++a) p[a + 1] = at(std::clamp(i0 + a, 0, w - 1), jj);
        row[b + 1] = cr(p[0], p[1], p[2], p[3], u);
    }
    return cr(row[0], row[1], row[2], row[3], v);
}

Grid Grid::resampled(int w2, int h2) const {
    Grid out = make(w2, h2, spacing * float(w - 1) / float(std::max(1, w2 - 1)));
    for (int j = 0; j < h2; ++j)
        for (int i = 0; i < w2; ++i)
            out.at(i, j) = sample(double(i) * (w - 1) / std::max(1, w2 - 1), double(j) * (h - 1) / std::max(1, h2 - 1));
    return out;
}

float valueNoise(double x, double z, std::uint32_t seed) {
    const double fx = std::floor(x), fz = std::floor(z);
    const int ix = int(std::int64_t(fx)), iz = int(std::int64_t(fz));
    float tx = float(x - fx), tz = float(z - fz);
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
    auto h = [seed](int a, int b) { return float(hash3(a, b, seed) & 0xFFFFFF) / 16777215.f; };
    const float a = h(ix, iz), b = h(ix + 1, iz), c = h(ix, iz + 1), d = h(ix + 1, iz + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

float fbm(double x, double z, int octaves, bool ridged, std::uint32_t seed) {
    float sum = 0, amp = 1, norm = 0;
    double f = 1;
    for (int o = 0; o < std::clamp(octaves, 1, 12); ++o) {
        const float n = valueNoise(x * f + o * 17.31, z * f - o * 9.73, seed + std::uint32_t(o) * 1013u);
        if (ridged) {
            const float r = 1 - std::abs(n * 2 - 1);
            sum += r * r * amp;
        } else
            sum += (n * 2 - 1) * amp;
        norm += amp;
        amp *= 0.5f;
        f *= 2.0;
    }
    return sum / norm;
}

void fillDepressions(std::vector<float>& h, int w, int ht, float eps) {
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    std::vector<std::uint8_t> closed(h.size(), 0);
    for (int j = 0; j < ht; ++j)
        for (int i = 0; i < w; ++i)
            if (i == 0 || j == 0 || i == w - 1 || j == ht - 1) {
                const int k = j * w + i;
                closed[std::size_t(k)] = 1;
                open.emplace(h[std::size_t(k)], k);
            }
    while (!open.empty()) {
        const auto [hc, k] = open.top();
        open.pop();
        const int i = k % w, j = k / w;
        for (int d = 0; d < 8; ++d) {
            const int ni = i + Dx[d], nj = j + Dz[d];
            if (ni < 0 || nj < 0 || ni >= w || nj >= ht) continue;
            const int nk = nj * w + ni;
            if (closed[std::size_t(nk)]) continue;
            closed[std::size_t(nk)] = 1;
            float& v = h[std::size_t(nk)];
            v = std::max(v, hc + eps * Dist[d]);
            open.emplace(v, nk);
        }
    }
}

std::vector<float> catchment(const Grid& g, float seaLevel) {
    std::vector<float> filled = g.heights;
    fillDepressions(filled, g.w, g.h, 1e-3f);
    const Graph gr = drainage(g, filled, seaLevel);
    return accumulate(gr, 1.0f);
}

void geology(Grid& g, const std::vector<float>& uplift, const GeologySettings& s, float seaLevel, const Progress& p) {
    // a faint jitter so flat or evenly tilted ground does not drain in parallel grid lines
    for (int j = 0; j < g.h; ++j)
        for (int i = 0; i < g.w; ++i)
            g.at(i, j) += (float(hash3(i, j, 977) & 0xFFFF) / 65535.f - 0.5f) * g.spacing * 0.05f;
    const std::size_t n = g.heights.size();
    const std::vector<float> u = uplift.size() == n ? uplift : std::vector<float>(n, 1.f);
    const float m = std::clamp(s.areaExponent, 0.1f, 1.f);
    const float dt = std::max(1.f, s.timeStep), k = std::max(0.f, s.erodibility), rate = std::max(0.f, s.upliftRate);
    const int iterations = std::clamp(s.iterations, 1, 500);
    for (int it = 0; it < iterations; ++it) {
        if (p) p(float(it) / float(iterations));
        fillDepressions(g.heights, g.w, g.h, 1e-3f);
        const Graph gr = drainage(g, g.heights, seaLevel, std::uint32_t(it) * 7919u + 1u);
        const std::vector<float> area = accumulate(gr, g.spacing * g.spacing);
        for (const int c : gr.order) {
            const auto ci = std::size_t(c);
            const int r = gr.receiver[ci];
            if (r == c) continue;   // base level: border or sea
            const float f = k * dt * std::pow(area[ci], m) / gr.dist[ci];
            g.heights[ci] = (g.heights[ci] + dt * rate * u[ci] + f * g.heights[std::size_t(r)]) / (1 + f);
        }
        // hillslopes creep (linear diffusion): the walls between the channels
        // round off instead of standing as one-cell trenches and ribs
        if (s.diffusion > 0) {
            std::vector<float> next = g.heights;
            for (int j = 1; j < g.h - 1; ++j)
                for (int i = 1; i < g.w - 1; ++i) {
                    const std::size_t k = g.index(i, j);
                    if (g.heights[k] <= seaLevel) continue;
                    const float lap = g.heights[k - 1] + g.heights[k + 1] + g.heights[k - std::size_t(g.w)] +
                                      g.heights[k + std::size_t(g.w)] - 4 * g.heights[k];
                    next[k] += s.diffusion * lap;
                }
            g.heights.swap(next);
        }
    }
    if (s.maxHeight > 0) {
        float top = seaLevel;
        for (const float h : g.heights) top = std::max(top, h);
        if (top > seaLevel + 1e-3f) {
            const float scale = (s.maxHeight - seaLevel) / (top - seaLevel);
            for (float& h : g.heights)
                if (h > seaLevel) h = seaLevel + (h - seaLevel) * scale;
        }
    }
    if (p) p(1);
}

void water(Grid& g, const WaterSettings& s, std::uint32_t seed, const std::vector<float>& mask, const Progress& p) {
    const int w = g.w, ht = g.h;
    if (w < 4 || ht < 4) return;
    // Heights in units of a 32-cell rise: the droplet constants (Beyer 2015)
    // were made for maps of 0..1 over a few hundred cells, so slopes keep that
    // meaning whatever the cell size.
    const float unit = g.spacing * 32.f;
    std::vector<float> hn(g.heights.size());
    for (std::size_t k = 0; k < hn.size(); ++k) hn[k] = g.heights[k] / unit;
    const bool masked = mask.size() == hn.size();
    auto msk = [&](int i, int j) { return masked ? mask[std::size_t(j) * std::size_t(w) + std::size_t(i)] : 1.f; };
    // Height and gradient at a point. The gradient is the bilinear blend of
    // the nodes' central differences, not the cell's own edges: on the edges'
    // gradient droplets lock onto the grid's lines and cut it into long
    // straight furrows along the axes.
    std::vector<float> gxs(hn.size(), 0.f), gzs(hn.size(), 0.f);
    auto refresh = [&] {   // every few thousand droplets: they follow the ground as it is cut, near enough
        for (int j = 0; j < ht; ++j)
            for (int i = 0; i < w; ++i) {
                const std::size_t k = std::size_t(j) * std::size_t(w) + std::size_t(i);
                const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, w - 1), j0 = std::max(j - 1, 0), j1 = std::min(j + 1, ht - 1);
                gxs[k] = (hn[std::size_t(j) * std::size_t(w) + std::size_t(i1)] - hn[std::size_t(j) * std::size_t(w) + std::size_t(i0)]) / float(i1 - i0);
                gzs[k] = (hn[std::size_t(j1) * std::size_t(w) + std::size_t(i)] - hn[std::size_t(j0) * std::size_t(w) + std::size_t(i)]) / float(j1 - j0);
            }
    };
    refresh();
    auto heightGrad = [&](double x, double z, float& gx, float& gz) {
        const int i = int(x), j = int(z);
        const float u = float(x - i), v = float(z - j);
        const std::size_t k = std::size_t(j) * std::size_t(w) + std::size_t(i), kw = k + std::size_t(w);
        const float wa = (1 - u) * (1 - v), wb = u * (1 - v), wc = (1 - u) * v, wd = u * v;
        gx = gxs[k] * wa + gxs[k + 1] * wb + gxs[kw] * wc + gxs[kw + 1] * wd;
        gz = gzs[k] * wa + gzs[k + 1] * wb + gzs[kw] * wc + gzs[kw + 1] * wd;
        return hn[k] * wa + hn[k + 1] * wb + hn[kw] * wc + hn[kw + 1] * wd;
    };
    const int rad = std::clamp(s.radius, 1, 8);
    // erosion brush: cells within the radius, weights summing to 1
    std::vector<std::array<int, 2>> offs;
    std::vector<float> wts;
    float wsum = 0;
    for (int dz = -rad; dz <= rad; ++dz)
        for (int dx = -rad; dx <= rad; ++dx) {
            const float d = std::sqrt(float(dx * dx + dz * dz));
            if (d > float(rad)) continue;
            offs.push_back({dx, dz});
            wts.push_back(float(rad) - d + 1e-3f);
            wsum += float(rad) - d + 1e-3f;
        }
    for (float& x : wts) x /= wsum;

    Rng rng(std::uint64_t(seed) * 7919u + 17u);
    const std::int64_t drops = std::max<std::int64_t>(1, std::llround(double(s.drops) * (w - 1) * (ht - 1)));
    const float inertia = std::clamp(s.inertia, 0.f, 0.99f), evap = std::clamp(s.evaporation, 0.f, 1.f);
    const int maxSteps = std::clamp(s.maxSteps, 1, 1000);
    for (std::int64_t drop = 0; drop < drops; ++drop) {
        if ((drop & 4095) == 4095) refresh();
        if ((drop & 65535) == 0 && p) p(float(drop) / float(drops));
        double x = rng.uniform() * (w - 1.001), z = rng.uniform() * (ht - 1.001);
        float dx = 0, dz = 0, speed = 1, wat = 1, sediment = 0;
        for (int step = 0; step < maxSteps; ++step) {
            const int i = int(x), j = int(z);
            const float u = float(x - i), v = float(z - j);
            float gx, gz;
            const float h = heightGrad(x, z, gx, gz);
            dx = dx * inertia - gx * (1 - inertia);
            dz = dz * inertia - gz * (1 - inertia);
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-9f) break;
            dx /= len;
            dz /= len;
            const double nx = x + dx, nz = z + dz;
            if (nx < 0 || nz < 0 || nx >= w - 1 || nz >= ht - 1) {
                sediment = 0;   // carried out of the area
                break;
            }
            float ngx, ngz;
            const float dh = heightGrad(nx, nz, ngx, ngz) - h;
            const float cap = std::max(-dh, s.minSlope) * speed * wat * s.capacity;
            const float m = msk(i, j);
            if (sediment > cap || dh > 0) {
                float amount = dh > 0 ? std::min(dh, sediment) : (sediment - cap) * s.deposition;
                sediment -= amount;
                amount *= m;
                const std::size_t k0 = std::size_t(j) * std::size_t(w) + std::size_t(i);
                hn[k0] += amount * (1 - u) * (1 - v);
                hn[k0 + 1] += amount * u * (1 - v);
                hn[k0 + std::size_t(w)] += amount * (1 - u) * v;
                hn[k0 + std::size_t(w) + 1] += amount * u * v;
            } else {
                const float amount = std::min((cap - sediment) * s.erosion, -dh) * m;
                for (std::size_t b = 0; b < offs.size(); ++b) {
                    const int ci = i + offs[b][0], cj = j + offs[b][1];
                    if (ci < 0 || cj < 0 || ci >= w || cj >= ht) continue;
                    float& c = hn[std::size_t(cj) * std::size_t(w) + std::size_t(ci)];
                    const float take = std::min(amount * wts[b], std::max(0.f, c - (h + dh)));   // no pit below where it flows next
                    c -= take;
                    sediment += take;
                }
            }
            speed = std::sqrt(std::max(0.f, speed * speed - dh * s.gravity));
            wat *= 1 - evap;
            x = nx;
            z = nz;
            if (wat < 1e-3f) break;
        }
        if (sediment > 0) {   // the droplet dried up or stopped: its load settles where it is
            const int i = std::clamp(int(x), 0, w - 1), j = std::clamp(int(z), 0, ht - 1);
            for (std::size_t b = 0; b < offs.size(); ++b) {
                const int ci = i + offs[b][0], cj = j + offs[b][1];
                if (ci >= 0 && cj >= 0 && ci < w && cj < ht)
                    hn[std::size_t(cj) * std::size_t(w) + std::size_t(ci)] += sediment * wts[b] * msk(ci, cj);
            }
        }
    }
    for (std::size_t k = 0; k < hn.size(); ++k) g.heights[k] = hn[k] * unit;
    if (p) p(1);
}

void thermal(Grid& g, float talusDegrees, int iterations) {
    const float talus = std::tan(std::clamp(talusDegrees, 1.f, 89.f) * 3.14159265f / 180.f) * g.spacing;
    std::vector<float> delta(g.heights.size());
    for (int it = 0; it < std::clamp(iterations, 1, 500); ++it) {
        std::fill(delta.begin(), delta.end(), 0.f);
        for (int j = 1; j < g.h - 1; ++j)
            for (int i = 1; i < g.w - 1; ++i) {
                const float h = g.at(i, j);
                for (int d = 0; d < 8; ++d) {
                    const float diff = h - g.at(i + Dx[d], j + Dz[d]);
                    const float limit = talus * Dist[d];
                    if (diff <= limit) continue;
                    const float move = (diff - limit) * 0.5f / 8.f;
                    delta[g.index(i, j)] -= move;
                    delta[g.index(i + Dx[d], j + Dz[d])] += move;
                }
            }
        for (std::size_t k = 0; k < delta.size(); ++k) g.heights[k] += delta[k];
    }
}

void noise(Grid& g, float amplitude, float scale, int octaves, bool ridged, float warp, std::uint32_t seed) {
    const double sc = std::max(1e-6f, scale);
    // no features finer than four cells (they would alias into speckles)
    octaves = std::clamp(octaves, 1, 12);
    const double nyquist = 0.25 / std::max(1e-3f, g.spacing);
    while (octaves > 0 && sc * std::pow(2.0, octaves - 1) > nyquist) --octaves;
    if (octaves == 0) return;
    for (int j = 0; j < g.h; ++j)
        for (int i = 0; i < g.w; ++i) {
            double x = double(i) * g.spacing, z = double(j) * g.spacing;
            if (warp > 0) {
                x += (valueNoise(x * sc * 0.5, z * sc * 0.5, seed + 101) * 2 - 1) * warp;
                z += (valueNoise(x * sc * 0.5 + 5.2, z * sc * 0.5 + 1.3, seed + 202) * 2 - 1) * warp;
            }
            g.at(i, j) += fbm(x * sc, z * sc, octaves, ridged, seed) * amplitude;
        }
}

void smooth(Grid& g, int passes) {
    std::vector<float> next = g.heights;
    for (int pass = 0; pass < std::clamp(passes, 1, 64); ++pass) {
        for (int j = 1; j < g.h - 1; ++j)
            for (int i = 1; i < g.w - 1; ++i) {
                float sum = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) sum += g.at(i + dx, j + dz);
                next[g.index(i, j)] = sum / 9.f;
            }
        g.heights = next;
    }
}

} // namespace terrain_core::gen
