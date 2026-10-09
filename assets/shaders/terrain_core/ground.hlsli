// What the ground is made of at a point: biome, then the materials the biome
// lays there - its colour, and how much of it is grass (the grass cover grows
// exactly where this says, in this colour).
//
// Used by the fragment stage (the ground) and by compute (the grass cover):
// define GROUND_IN_COMPUTE before including for the second. Compute has no
// screen derivatives, so it samples at a level of detail from the footprint
// it is given instead.
//
// Positions arrive as an exact integer (the patch origin, in level-0 texels)
// plus a small float offset, never as a float world coordinate: a texture
// coordinate or a noise cell computed from 1 900 000 m in a float is a
// quarter of a metre out, which on a 4 m tile is a smear.

#ifdef GROUND_IN_COMPUTE
Texture2D<float> biomeMap : register(t1, space0);
Texture2DArray<float4> materials : register(t2, space0);
SamplerState biomeSampler : register(s1, space0);
SamplerState materialSampler : register(s2, space0);
#define GROUND_UNIFORMS register(b1, space2)
#else
Texture2D<float> biomeMap : register(t1, space2);
SamplerState biomeSampler : register(s1, space2);
Texture2DArray<float4> materials : register(t2, space2);
SamplerState materialSampler : register(s2, space2);
#define GROUND_UNIFORMS register(b1, space3)
#endif

// Must match terrain_core::GroundUniforms.
cbuffer Ground : GROUND_UNIFORMS {
    float4 biomeData[32 * 5];   // per biome: materials, patch, slope, high, tint
    float4 groundGlobals;       // shore material, shore height, sea floor material, log2(tile in level-0 texels)
    uint4 groundSizes;          // biome map size, -, -, -
    float4 groundWorld;         // world size, spacing, -, -
    float4 materialGrass[8];    // per material layer: how much of it is grass, 0..1
};

#include "biome_pick.hlsli"

// Islands of a second material. Thresholded value noise on its own shows
// its grid - square blobs - so the lookup is first pushed about by a noise
// of half the cell: the edges come out ragged and the squares go.
float islands(GroundPoint p, int log2Cell, uint seed) {
    const float reach = float(1 << log2Cell) * 0.9;
    GroundPoint q = p;
    q.sub += (float2(cellNoise(p, log2Cell - 1, seed + 3u), cellNoise(p, log2Cell - 1, seed + 5u)) - 0.5) * reach;
    return cellNoise(q, log2Cell, seed) * 0.6 + cellNoise(q, log2Cell - 2, seed + 17u) * 0.3 +
           cellNoise(p, log2Cell - 4, seed + 29u) * 0.1;
}

// Texture coordinate repeating every 2^log2Tile level-0 texels.
float2 tileUv(GroundPoint p, int log2Tile) {
    const int size = 1 << log2Tile;
    return (float2(p.base & (size - 1)) + p.sub) / float(size);
}

// One material, without its repeat showing. A 4 m picture laid edge to edge
// shows a grid of its own light and dark halves from a hundred metres off;
// so each pixel takes it twice, at two of eight random offsets chosen by a
// slow noise, and blends them (Quilez, "texture repetition", technique 3).
// Gradients are taken from the unshifted coordinate so the offset jumps never
// become mip seams. Two samples, as the old two-scale blend was.
float2 offsetOf(float index) {
    const uint h = groundHash(int2(int(index), 0), 977u);
    return float2(h & 0xffffu, h >> 16) / 65536.0;
}

float grassOf(float layer) {
    const uint l = uint(layer);
    return materialGrass[l >> 2][l & 3];
}

// Colour, and how much of it is grass.
float4 material(float layer, GroundPoint p, float wander) {
    const int tile = int(groundGlobals.w);
    const float2 uv = tileUv(p, tile);
    const float k = cellNoise(p, tile + 1, 7u) * 8.0;
    const float i = floor(k), f = frac(k);
#ifdef GROUND_IN_COMPUTE
    // 128 texels a tile: texture texels per level-0 texel, times the footprint.
    const float lod = log2(max(p.footprint * 128.0 / float(1 << tile), 1.0));
    const float3 a = materials.SampleLevel(materialSampler, float3(uv + offsetOf(i), layer), lod).rgb;
    const float3 b = materials.SampleLevel(materialSampler, float3(uv + offsetOf(i + 1), layer), lod).rgb;
#else
    const float2 dx = ddx(uv), dy = ddy(uv);
    const float3 a = materials.SampleGrad(materialSampler, float3(uv + offsetOf(i), layer), dx, dy).rgb;
    const float3 b = materials.SampleGrad(materialSampler, float3(uv + offsetOf(i + 1), layer), dx, dy).rgb;
#endif
    const float3 colour = lerp(a, b, smoothstep(0.2, 0.8, f - 0.1 * dot(a - b, 1)));
    return float4(colour * (0.88 + 0.24 * wander), grassOf(layer));
}

float4 biomeGround(uint id, GroundPoint p, float height, float normalZ, float wander) {
    const float4 mats = biomeData[id * 5 + 0];
    const float4 patch = biomeData[id * 5 + 1];
    const float4 slope = biomeData[id * 5 + 2];
    const float4 high = biomeData[id * 5 + 3];
    const float4 tint = biomeData[id * 5 + 4];
    if (mats.x < 0) return float4(0.5, 0.5, 0.5, 0);
    float4 ground = material(mats.x, p, wander);
    if (mats.y >= 0) {
        const float n = islands(p, int(patch.x), 101u + id);
        // Far off the islands are smaller than a pixel: what is left of them
        // is their share of the ground, not a speckle.
        const float m = lerp(smoothstep(1.0 - patch.y - 0.04, 1.0 - patch.y + 0.04, n), patch.y,
                             noiseFade(p, int(patch.x) - 1));
        if (m > 0) ground = lerp(ground, material(mats.y, p, wander), m);
    }
    // Ragged edges for the slope and height layers: the threshold wanders.
    const float jitter = cellNoise(p, int(groundGlobals.w) + 3, 211u + id) - 0.5;
    if (mats.z >= 0) {
        const float m = saturate((slope.x - normalZ + jitter * 0.06) / max(slope.x - slope.y, 1e-3));
        if (m > 0) ground = lerp(ground, material(mats.z, p, wander), m);
    }
    if (mats.w >= 0) {
        const float m = saturate((height - high.x + jitter * 150.0) / max(high.y - high.x, 1.0)) * saturate(normalZ * 1.6 - 0.4);
        if (m > 0) ground = lerp(ground, material(mats.w, p, wander), m);
    }
    return float4(ground.rgb * tint.rgb, ground.a);
}

float3 biomeDebugColour(uint id) {
    const uint h = groundHash(int2(int(id), 7), 3u);
    return float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0;
}

// The ground at p: colour in rgb, how much of it is grass in a. `debug`
// returns the blended biome colours instead.
float4 groundSample(GroundPoint p, float height, float normalZ, bool debug) {
    const float wander = cellNoise(p, int(groundGlobals.w) + 6, 5u);

    // The biome map round the point (biome_pick.hlsli), blended with weights
    // sharpened so the transition is a ragged band, not a two-kilometre wash.
    BiomeMapInfo info;
    info.size = float(groundSizes.x);
    info.worldSize = groundWorld.x;
    info.spacing = groundWorld.y;
    info.tileLog2 = int(groundGlobals.w);
    uint ids[4];
    float weights[4];
    const float best = biomeCorners(biomeMap, biomeSampler, info, p, ids, weights);
    float4 ground = 0;
    float total = 0;
    [unroll] for (int d = 0; d < 4; ++d) {
        if (weights[d] < 0) continue;
        const float w = exp((weights[d] - best) * 14.0);
        if (w < 0.02) continue;
        ground += w * (debug ? float4(biomeDebugColour(ids[d]), 0) : biomeGround(ids[d], p, height, normalZ, wander));
        total += w;
    }
    ground /= max(total, 1e-4);
    if (debug) return ground;

    // The coast and the sea floor belong to no biome.
    if (groundGlobals.x >= 0 && height < groundGlobals.y) {
        const float m = saturate((groundGlobals.y - height) / max(groundGlobals.y * 0.5, 0.5) + (wander - 0.5) * 0.6);
        ground = lerp(ground, material(groundGlobals.x, p, wander), m);
    }
    if (height < 0 && groundGlobals.z >= 0) {
        const float3 floorColour = material(groundGlobals.z, p, wander).rgb;
        ground = float4(lerp(floorColour * float3(0.55, 0.75, 0.85), float3(0.05, 0.13, 0.24), saturate(-height / 40.0)), 0);
    }
    return ground;
}

float3 groundColour(GroundPoint p, float height, float normalZ, bool debug) {
    return groundSample(p, height, normalZ, debug).rgb;
}
