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
    uint offset;    // first texel in reliefTexels
    uint pad;
};

StructuredBuffer<ReliefPatch> reliefPatches : RELIEF_PATCHES;
StructuredBuffer<uint> reliefTexels : RELIEF_TEXELS;

// (height over base, weight) at texel `c` of patch `p`, clamped to its edge.
float2 reliefTexel(ReliefPatch p, int2 c) {
    c = clamp(c, int2(0, 0), int2(p.size) - 1);
    const uint v = reliefTexels[p.offset + uint(c.y) * p.size.x + uint(c.x)];
    return float2(f16tof32(v & 0xffffu), f16tof32(v >> 16));
}

// Bilinear at `q` (patch texels, texel centres at +0.5).
float2 reliefBilinear(ReliefPatch p, float2 q) {
    const float2 s = q - 0.5;
    const int2 c = int2(floor(s));
    const float2 f = s - float2(c);
    const float2 a = reliefTexel(p, c), b = reliefTexel(p, c + int2(1, 0));
    const float2 d = reliefTexel(p, c + int2(0, 1)), e = reliefTexel(p, c + int2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}

// The patch at `q` as a level `footprint` patch texels apart reads it: a
// level coarser than the patch takes a tent over its own texel (3 x 3 taps),
// not a point - sampled sparsely a step a metre wide aliases into a row of
// ripples.
float2 reliefAt(ReliefPatch p, float2 q, float footprint) {
    if (footprint < 1.0) return reliefBilinear(p, q);
    const float o = footprint * 0.5;
    float2 sum = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            sum += reliefBilinear(p, q + float2(i, j) * o) * ((2 - abs(i)) * (2 - abs(j)));
    return sum / 16.0;
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
