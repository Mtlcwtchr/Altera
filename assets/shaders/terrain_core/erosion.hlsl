// Offline erosion of the macro map (terrain_core::Erosion).
//
// upscaleCs: the source map, enlarged `scale` times with a bicubic, plus the
//   relief the source cannot hold (a few to a few dozen of the new texels):
//   ridged in the mountains, so there are crests to keep, plain lower down.
// thermalCs: thermal erosion - scree settling to the angle of repose.
//
// Heights are int32 fixed point, 2^24 units for the source's whole range, so
// every cell can hand height to its neighbours with atomics while all of them
// run at once: the whole map is the one buffer, 1 GiB at 16k.

Texture2D<float> sourceMap : register(t0, space0);
SamplerState sourceSampler : register(s0, space0);
RWStructuredBuffer<int> heights : register(u0, space1);

cbuffer Erode : register(b0, space2) {
    uint size;                 // texels a side of the eroded map
    uint sourceSize;
    uint scale;                // size / sourceSize
    uint seed;
    float unitsPerTexelHeight; // height units in one texel-width of height: texel metres / (range / 2^24)
    float seaUnits;            // below this, nothing moves
    float detailTexels;        // strength of the added relief (1 = ~180 m crests at 2 km on 32 m texels)
    float talus;               // angle of repose, as rise over run
    float rate;                // share of the excess moved per iteration, 0..1
    float3 pad;
};

static const float kUnits = 16777216.0;   // 2^24

uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

float2 gradientAt(int2 c, uint o) {
    const uint h = hashU(uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ (seed + o * 0x9e3779b9u));
    const float a = float(h & 0xffffu) * (6.28318530718 / 65536.0);
    return float2(cos(a), sin(a));
}

float gradientNoise(int2 cell, float2 f, uint o) {
    const float2 u = f * f * f * (f * (f * 6 - 15) + 10);
    const float va = dot(gradientAt(cell, o), f), vb = dot(gradientAt(cell + int2(1, 0), o), f - float2(1, 0));
    const float vc = dot(gradientAt(cell + int2(0, 1), o), f - float2(0, 1)), vd = dot(gradientAt(cell + int2(1, 1), o), f - float2(1, 1));
    return va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
}

float sourceAt(int2 t) {
    t = clamp(t, 0, int(sourceSize) - 1);
    return sourceMap.SampleLevel(sourceSampler, (float2(t) + 0.5) / float(sourceSize), 0);
}

float catmull(float a, float b, float c, float d, float t) {
    return b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

[numthreads(8, 8, 1)]
void upscaleCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size || id.y >= size) return;
    const float2 p = (float2(id.xy) + 0.5) / float(scale) - 0.5;
    const int2 b = int2(floor(p));
    const float2 t = p - floor(p);
    float rows[4];
    [unroll] for (int j = 0; j < 4; ++j)
        rows[j] = catmull(sourceAt(b + int2(-1, j - 1)), sourceAt(b + int2(0, j - 1)), sourceAt(b + int2(1, j - 1)),
                          sourceAt(b + int2(2, j - 1)), t.x);
    const float h = catmull(rows[0], rows[1], rows[2], rows[3], t.y);

    // Relief the source is too coarse to hold, and mountain structure it
    // never had: the source's ranges are smooth domes, so before the scree
    // can cut faces there have to be crests to cut them from. Ridged noise
    // (0.35 - |n|: a sharp crest wherever the noise crosses zero) from 2 km
    // down to 4 texels, amplitude in proportion to wavelength, in the
    // mountains; a plain, much weaker one below them.
    const float land = saturate((h * kUnits - seaUnits) / (kUnits - seaUnits));
    const float mountain = smoothstep(0.25, 0.6, land);
    float detail = 0;
    for (uint w = 64; w >= 4; w >>= 1) {
        const int2 cell = int2(id.xy) / int(w);
        const float2 f = (float2(id.xy) - float2(cell) * float(w)) / float(w);
        const float n = gradientNoise(cell, f, w);
        detail += lerp(n * 0.3, 0.35 - abs(n), mountain) * float(w) * 0.25;
    }
    const float units = h * kUnits + detail * detailTexels * unitsPerTexelHeight * max(mountain, 0.1 * saturate(land * 20.0));
    heights[id.y * size + id.x] = int(clamp(units, 0.0, kUnits * 1.5));
}

float heightAt(int2 t) {
    t = clamp(t, 0, int(size) - 1);
    return float(heights[t.y * size + t.x]);
}

// thermalCs: thermal erosion (Musgrave et al. 1989). Wherever ground is
// steeper than the angle of repose, part of the excess slides to the lower
// neighbours, in proportion to how far each one is over the angle. Repeated,
// every face too steep to stand settles to that angle: steep ground becomes
// flat-faced scree slopes, and where two such faces meet - the crests and the
// summits - the edge is left sharp. Done in place with atomics, so the whole
// map takes the one buffer; the order the cells move in is the card's, which
// over many iterations does not matter.
[numthreads(8, 8, 1)]
void thermalCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size || id.y >= size) return;
    const int2 c = int2(id.xy);
    const float h = float(heights[c.y * size + c.x]);
    if (h < seaUnits) return;
    // Per neighbour: the height difference beyond the angle of repose.
    const int2 offsets[8] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
                             int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)};
    float excess[8];
    float total = 0, largest = 0;
    [unroll] for (int k = 0; k < 8; ++k) {
        const float distance = k < 4 ? 1.0 : 1.41421356;
        const float d = h - heightAt(c + offsets[k]) - talus * distance * unitsPerTexelHeight;
        excess[k] = max(d, 0.0);
        total += excess[k];
        largest = max(largest, excess[k]);
    }
    if (total <= 0) return;
    // Move half the largest excess (so the slope settles at the angle rather
    // than overshooting it), shared out by each neighbour's own excess.
    const float moved = largest * 0.5 * rate;
    InterlockedAdd(heights[c.y * size + c.x], -int(round(moved)));
    [unroll] for (int j = 0; j < 8; ++j) {
        if (excess[j] <= 0) continue;
        const int2 t = clamp(c + offsets[j], 0, int(size) - 1);
        InterlockedAdd(heights[t.y * size + t.x], int(round(moved * excess[j] / total)));
    }
}
