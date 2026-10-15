// Fine relief (src/terrain_core/relief.hpp): windows of the ground at a metre
// or two a texel laid over what the macro map and its noise make - a marsh
// hill's low terraces. Within a patch the ground is the patch's height,
// eased into the land round it by the patch's weight.
//
// Define RELIEF_PATCHES and RELIEF_TEXELS (registers) before including.

struct ReliefPatch {
    int2 origin;    // south-west corner, in the finest spacing's texels
    uint2 size;     // texels
    float texel;    // metres
    float base;     // metres: the heights are over this
    uint offset;    // dense: first texel in reliefTexels; sparse: its table there
    uint tile;      // 0: dense; else texels a side of a sparse patch's tiles
    uint4 pad;
};

StructuredBuffer<ReliefPatch> reliefPatches : RELIEF_PATCHES;
StructuredBuffer<uint> reliefTexels : RELIEF_TEXELS;

// (height over base, weight, present) at texel `c` of patch `p`, clamped to
// its edge; the height comes times `present`. A sparse patch's tile it does
// not store is not present (all 0): the filters below sum all three and
// divide the height by the presence (reliefAt), so the ground at a window's
// edge is never pulled down to a base. A dense patch's texels all are.
float3 reliefTexel(ReliefPatch p, int2 c) {
    c = clamp(c, int2(0, 0), int2(p.size) - 1);
    if (p.tile == 0) {
        const uint v = reliefTexels[p.offset + uint(c.y) * p.size.x + uint(c.x)];
        return float3(f16tof32(v & 0xffffu), f16tof32(v >> 16), 1.0);
    }
    const uint2 t = uint2(c) / p.tile, l = uint2(c) % p.tile;
    const uint entry = p.offset + 2u * (t.y * ((p.size.x + p.tile - 1u) / p.tile) + t.x);
    const uint first = reliefTexels[entry];
    if (first == 0xffffffffu) return float3(0, 0, 0);
    const uint v = reliefTexels[first + l.y * p.tile + l.x];
    return float3(asfloat(reliefTexels[entry + 1u]) + f16tof32(v & 0xffffu), f16tof32(v >> 16), 1.0);
}

// Bilinear at `q` (patch texels, texel centres at +0.5).
float3 reliefBilinear(ReliefPatch p, float2 q) {
    const float2 s = q - 0.5;
    const int2 c = int2(floor(s));
    const float2 f = s - float2(c);
    const float3 a = reliefTexel(p, c), b = reliefTexel(p, c + int2(1, 0));
    const float3 d = reliefTexel(p, c + int2(0, 1)), e = reliefTexel(p, c + int2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

// The patch at `q` as a level `footprint` patch texels apart reads it: a
// level coarser than the patch takes a tent over its own texel (3 x 3 taps),
// not a point - sampled sparsely a step a metre wide aliases into a row of
// ripples. (height over base, weight)
float2 reliefAt(ReliefPatch p, float2 q, float footprint) {
    float3 sum;
    if (footprint < 1.0) {
        sum = reliefBilinear(p, q);
    } else {
        const float o = footprint * 0.5;
        sum = 0;
        [unroll] for (int j = -1; j <= 1; ++j)
            [unroll] for (int i = -1; i <= 1; ++i)
                sum += reliefBilinear(p, q + float2(i, j) * o) * ((2 - abs(i)) * (2 - abs(j)));
        sum /= 16.0;
    }
    return float2(sum.z > 1e-4 ? sum.x / sum.z : 0.0, sum.y);
}

// `h` (height, dh/dx, dh/dy) at `u` (position in finest-spacing texels) with
// every patch over it laid on.
float3 applyRelief(float3 h, int2 u, float spacing, float levelSpacing) {
    uint count, stride;
    reliefPatches.GetDimensions(count, stride);
    for (uint k = 0; k < count; ++k) {
        const ReliefPatch p = reliefPatches[k];
        const float2 q = float2(u - p.origin) * (spacing / p.texel);
        if (any(q < 0.0) || any(q >= float2(p.size))) continue;
        const float footprint = levelSpacing / p.texel;
        const float2 v = reliefAt(p, q, footprint);
        if (v.y <= 0.0) continue;
        // The slope from either side, a texel (or this level's) apart.
        const float d = max(1.0, footprint);
        const float gx = (reliefAt(p, q + float2(d, 0), footprint).x - reliefAt(p, q - float2(d, 0), footprint).x) / (2.0 * d * p.texel);
        const float gy = (reliefAt(p, q + float2(0, d), footprint).x - reliefAt(p, q - float2(0, d), footprint).x) / (2.0 * d * p.texel);
        h = lerp(h, float3(p.base + v.x, gx, gy), saturate(v.y));
    }
    return h;
}

// For the ground's shading, where the mesh is too coarse to hold the steps:
// the fine relief's (height, dh/dx, dh/dy) and its weight at a point -
// `base` + `sub` in the finest spacing's texels - filtered over the pixel's
// `footprint` (metres). `texel`: the patch's (metres).
float3 reliefShade(int2 base, float2 sub, float spacing, float footprint, out float weight, out float texel) {
    uint count, stride;
    reliefPatches.GetDimensions(count, stride);
    float3 h = 0;
    weight = 0;
    texel = 1.0;
    for (uint k = 0; k < count; ++k) {
        const ReliefPatch p = reliefPatches[k];
        const float2 q = (float2(base - p.origin) + sub) * (spacing / p.texel);
        if (any(q < 0.0) || any(q >= float2(p.size))) continue;
        const float f = footprint / p.texel;
        const float2 v = reliefAt(p, q, f);
        if (v.y <= 0.0) continue;
        const float d = max(0.75, f);
        const float gx = (reliefAt(p, q + float2(d, 0), f).x - reliefAt(p, q - float2(d, 0), f).x) / (2.0 * d * p.texel);
        const float gy = (reliefAt(p, q + float2(0, d), f).x - reliefAt(p, q - float2(0, d), f).x) / (2.0 * d * p.texel);
        h = lerp(h, float3(p.base + v.x, gx, gy), saturate(v.y));
        weight = max(weight, saturate(v.y));
        texel = p.texel;
        if (weight >= 1.0) break;
    }
    return h;
}
