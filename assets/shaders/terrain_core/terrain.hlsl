// The ground, drawn from the clipmap with no mesh: every vertex is a point of
// one shared G x G grid, placed by its patch (instance) and lifted by the
// height the generator wrote. Near the end of its level's range a vertex slides
// onto the next level's grid and takes that level's height (CDLOD morph), so a
// patch meets its coarser neighbour exactly and nothing pops.
//
// Plain lighting - a sun, a sky term, haze; the ground's colour is the
// biome's materials (ground.hlsli).

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative: no translation
    float eyeZ;
    float spacing;
    uint clipSize;
    uint grid;
    uint levels;
    float rangeFactor;
    float morphStart;
    float pad0;
};

Texture2DArray<float4> clipVs : register(t0, space0);
SamplerState clipVsSampler : register(s0, space0);

struct PatchIn {
    uint vertex : SV_VertexID;
    int4 origin : TEXCOORD0;     // originX, originY (level texels), level, -
    float4 rel : TEXCOORD1;      // origin - eye, metres
};

struct VsOut {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;               // grid position within the patch's node, in level texels
    nointerpolation int4 slots : TEXCOORD1; // origin mod N at this level (xy) and the next (zw)
    nointerpolation uint level : TEXCOORD2;
    float morph : TEXCOORD3;
    float3 view : TEXCOORD4;                // eye to point, metres
    nointerpolation int2 origin : TEXCOORD5; // patch origin, level texels (absolute)
};

int2 wrap(int2 t, uint size) { const int n = int(size); return ((t % n) + n) % n; }

// Through a nearest sampler at the texel's centre rather than Load: the same
// texel, and the sampler stays in the shader, so its binding is not optimised
// away from under the C++ side.
float4 fetch(Texture2DArray<float4> tex, SamplerState at, uint size, int2 slot, uint level) {
    return tex.SampleLevel(at, float3((float2(wrap(slot, size)) + 0.5) / float(size), float(level)), 0);
}

// Bilinear between texel centres at integer coordinates (texel i is the point
// i * spacing of the level, which is where the grid's vertices are).
float4 bilinear(Texture2DArray<float4> tex, SamplerState at, uint size, int2 slotOrigin, float2 local, uint level) {
    const float2 i = floor(local);
    const float2 f = local - i;
    const int2 s = slotOrigin + int2(i);
    const float4 a = fetch(tex, at, size, s, level), b = fetch(tex, at, size, s + int2(1, 0), level);
    const float4 c = fetch(tex, at, size, s + int2(0, 1), level), d = fetch(tex, at, size, s + int2(1, 1), level);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

VsOut terrainVs(PatchIn input) {
    const uint side = grid + 1;
    const float2 local = float2(input.vertex % side, input.vertex / side);
    const uint level = uint(input.origin.z);
    const float levelSpacing = spacing * float(1u << level);
    const int2 slot = wrap(input.origin.xy, clipSize);
    const int2 nextSlot = wrap(input.origin.xy / 2, clipSize);
    const bool top = level + 1 >= levels;

    // Morph by distance to the unmorphed vertex.
    const float h0 = fetch(clipVs, clipVsSampler, clipSize, slot + int2(local), level).x;
    const float2 flat = input.rel.xy + local * levelSpacing;
    const float distance = length(float3(flat, h0 - eyeZ));
    const float range = rangeFactor * float(grid) * levelSpacing;
    const float morph = top ? 0.0 : saturate((distance - morphStart * range) / ((1.0 - morphStart) * range));

    const float2 morphed = local - frac(local * 0.5) * 2.0 * morph;
    float height = bilinear(clipVs, clipVsSampler, clipSize, slot, morphed, level).x;
    if (!top && morph > 0) height = lerp(height, bilinear(clipVs, clipVsSampler, clipSize, nextSlot, morphed * 0.5, level + 1).x, morph);

    VsOut o;
    o.view = float3(input.rel.xy + morphed * levelSpacing, height - eyeZ);
    o.position = mul(viewProjection, float4(o.view, 1));
    o.local = morphed;
    o.slots = int4(slot, nextSlot);
    o.level = level;
    o.morph = morph;
    o.origin = input.origin.xy;
    return o;
}

Texture2DArray<float4> clipPs : register(t0, space2);
SamplerState clipPsSampler : register(s0, space2);

#include "ground.hlsli"

cbuffer Look : register(b0, space3) {
    float3 sunDirection;
    float hazeDistance;
    float3 skyColour;
    uint debugLevels;
    // The vertex stage's View is not visible here: each stage has its own uniforms.
    uint lookClipSize;
    uint lookLevels;
    float2 lookPad;
};

float3 levelTint(uint level) {
    const float3 tints[8] = {float3(1, 0.3, 0.3), float3(1, 0.7, 0.2), float3(0.9, 1, 0.3), float3(0.3, 1, 0.4),
                             float3(0.3, 0.9, 1), float3(0.3, 0.5, 1), float3(0.7, 0.3, 1), float3(1, 0.3, 0.8)};
    return tints[level % 8];
}

float4 terrainPs(VsOut input) : SV_Target0 {
    float4 s = bilinear(clipPs, clipPsSampler, lookClipSize, input.slots.xy, input.local, input.level);
    if (input.morph > 0 && input.level + 1 < lookLevels)
        s = lerp(s, bilinear(clipPs, clipPsSampler, lookClipSize, input.slots.zw, input.local * 0.5, input.level + 1), input.morph);
    const float height = s.x;
    const float3 normal = normalize(float3(-s.y, -s.z, 1));

    GroundPoint p;
    p.base = input.origin << input.level;
    p.sub = input.local * float(1u << input.level);
    p.footprint = max(length(ddx(p.sub)), length(ddy(p.sub)));
    float3 colour = groundColour(p, height, normal.z, debugLevels == 5);
    if (debugLevels == 5) return float4(colour, 1);
    if (debugLevels == 1) colour = lerp(colour, levelTint(input.level), 0.45);
    if (debugLevels == 2) return float4(normal * 0.5 + 0.5, 1);
    if (debugLevels == 3) return float4(input.morph, frac(float(input.level) * 0.37), 1.0 - input.morph, 1);
    if (debugLevels == 4) return float4(frac(height / 50.0), frac(height / 500.0), saturate(height / 3000.0), 1);

    const float sun = saturate(dot(normal, sunDirection));
    const float3 lit = colour * (sun * float3(1.0, 0.95, 0.85) * 1.1 + (0.35 + 0.15 * normal.z) * skyColour);
    const float haze = 1.0 - exp(-length(input.view) / hazeDistance);
    return float4(lerp(lit, skyColour, haze), 1);
}
