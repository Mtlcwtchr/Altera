// The grass cover: a tuft wherever the ground is grass, in the ground's own
// colour, remade every frame.
//
// Rings round the eye, each a fixed grid of the world twice as coarse as the
// one inside it, with tufts twice as wide but no taller: the cover is solid
// at every distance for the same number of tufts per ring, and since a ring
// is a grid of the world, nothing slides as the eye moves. Neighbouring rings
// overlap and cross-fade by size. At the outer edge the tufts sink into the
// ground, which is already painted the colour they were - so there is no
// edge to see.
//
// Each tuft asks the ground (ground.hlsli, the same function that paints it)
// for its colour and how much grass there is, lights that colour exactly as
// the ground is lit, and stores it: the tuft's foot is the ground's colour.

#define GROUND_IN_COMPUTE
Texture2DArray<float4> clipmap : register(t0, space0);
SamplerState clipSampler : register(s0, space0);
#include "ground.hlsli"
// For the clouds' shadow, worked out once a tuft here rather than on every
// pixel of it: a cloud is a kilometre across, a tuft a few centimetres.
#define LIGHT_UNIFORMS register(b2, space2)
#define NO_SUN_SHADOW   // the sun's shadow is the draw's
#include "lighting.hlsli"
Texture2D<float4> waterMap : register(t5, space0);
SamplerState waterSampler : register(s5, space0);
#include "water_map.hlsli"
#define RIVER_SEGMENTS register(t6, space0)
#define RIVER_TILES register(t7, space0)
#define RIVER_LIST register(t8, space0)
#include "rivers.hlsli"

struct Tuft {
    float4 positionYaw;   // eye-relative foot (m), yaw
    float4 shape;         // width, height, sprite | normal x, y (8 bits each), colour + cloud shadow (RGBA8 as bits)
};

RWStructuredBuffer<Tuft> tufts : register(u0, space1);
RWStructuredBuffer<uint> tuftCount : register(u1, space1);

cbuffer GrassSpawn : register(b0, space2) {
    float4 planes[6];
    int2 camTexel;          // the eye, in level-0 texels
    float2 camTexelFrac;
    int2 camCell;           // the eye, in this ring's cells
    float2 camCellFrac;
    float eyeZ;
    float spacing0;
    float cellSize;         // this ring's spacing, m
    float inner;            // this ring's inner and outer radius, m (inner 0 for the first)
    float outer;
    float tuftHeight;
    float tuftWidth;        // in cells
    float pixelScale;
    uint gridSide;
    uint clipSize;
    uint levels;
    uint capacity;
    uint seed;
    float heightLow;
    float heightHigh;
    float flowerChance;
    float3 spawnUnusedSun;
    float spawnWaterMapSize;
    float3 spawnUnusedSky;
    float spawnWorldSize;
};

uint hashCell(int2 c, uint s) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float unit(uint h) { return float(h & 0xffffu) / 65536.0; }

int2 wrapSlot(int2 t) { const int n = int(clipSize); return ((t % n) + n) % n; }

float4 clipTexel(int2 slot, uint level) {
    return clipmap.SampleLevel(clipSampler, float3((float2(wrapSlot(slot)) + 0.5) / float(clipSize), float(level)), 0);
}

float4 heightAt(int2 base, float2 sub, float distance) {
    const float reach = (float(clipSize) * 0.5 - 3.0) * spacing0;
    const uint level = uint(clamp(ceil(log2(max(distance / reach, 1e-6))), 0.0, float(levels - 1)));
    const int mask = (1 << level) - 1;
    const float2 at = (float2(base & mask) + sub) / float(1 << level);
    const float2 whole = floor(at);
    const int2 t = (base >> level) + int2(whole);
    const float2 f = at - whole;
    return lerp(lerp(clipTexel(t, level), clipTexel(t + int2(1, 0), level), f.x),
                lerp(clipTexel(t + int2(0, 1), level), clipTexel(t + int2(1, 1), level), f.x), f.y);
}

bool boxOutside(float3 lo, float3 hi) {
    [unroll] for (int k = 0; k < 6; ++k) {
        const float4 p = planes[k];
        const float3 far = float3(p.x >= 0 ? hi.x : lo.x, p.y >= 0 ? hi.y : lo.y, p.z >= 0 ? hi.z : lo.z);
        if (dot(p.xyz, far) + p.w < 0) return true;
    }
    return false;
}

uint packColour(float3 c, float a) {
    const uint3 u = uint3(saturate(c) * 255.0 + 0.5);
    return u.r | (u.g << 8) | (u.b << 16) | (uint(saturate(a) * 255.0 + 0.5) << 24);
}

[numthreads(8, 8, 1)]
void spawnCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gridSide || id.y >= gridSide) return;
    const int2 cell = camCell - int(gridSide / 2) + int2(id.xy);
    const uint h = hashCell(cell, seed);
    const float2 rel = (float2(cell - camCell) + float2(h & 0xffffu, h >> 16) / 65536.0 - camCellFrac) * cellSize;
    const float distance = length(rel);
    if (distance > outer || (inner > 0 && distance < inner * 0.8)) return;
    const float reach = tuftWidth * cellSize;
    if (boxOutside(float3(rel - reach, heightLow), float3(rel + reach, heightHigh + tuftHeight * 1.5))) return;

    // Size, by where in the ring it stands: grows in over the inner overlap,
    // sinks out over the outer one.
    float size = 1.0 - smoothstep(outer * 0.8, outer, distance);
    if (inner > 0) size *= smoothstep(inner * 0.8, inner, distance);
    if (size <= 0.02) return;

    const float2 t0 = camTexelFrac + rel / spacing0;
    GroundPoint p;
    p.base = camTexel + int2(floor(t0));
    p.sub = t0 - floor(t0);
    // The ground as a pixel at this distance sees it, so the tuft is the
    // colour the ground is painted here and not a sharper one.
    p.footprint = max(distance / pixelScale, cellSize) / spacing0;
    const float4 hg = heightAt(p.base, p.sub, distance);
    float wet;
    if (waterAt(waterMap, waterSampler, (float2(p.base) + p.sub) * spacing0, spawnWorldSize, spawnWaterMapSize, wet).x >
        hg.x - 0.1)
        return;   // under water
    const RiverHit river = nearestRiver((float2(p.base) + p.sub) * spacing0);
    if (river.found && river.distance < river.halfWidth + 0.3) return;
    const float normalZ = rsqrt(1.0 + dot(hg.yz, hg.yz));
    const float4 ground = groundSample(p, hg.x, normalZ, false);
    const uint h2 = hashCell(cell, seed + 101u);
    if (unit(h2) >= ground.a * 1.15 - 0.1) return;

    // The ground's colour unlit and its normal: the draw lights both exactly
    // as the ground is lit, shadows and all.
    const float3 normal = normalize(float3(-hg.y, -hg.z, 1));
    const uint packedNormal = uint(saturate(normal.x * 0.5 + 0.5) * 255.0 + 0.5) |
                              (uint(saturate(normal.y * 0.5 + 0.5) * 255.0 + 0.5) << 8);

    const uint h3 = hashCell(cell, seed + 202u);
    const bool flower = unit(h3 >> 4) < flowerChance;
    const uint sprite = flower ? 14u + (h3 % 4u) : (h3 % 14u);
    uint slot;
    InterlockedAdd(tuftCount[0], 1u, slot);
    if (slot >= capacity) return;
    Tuft o;
    o.positionYaw = float4(rel, hg.x - eyeZ, unit(h2 >> 16) * 3.14159265);
    o.shape = float4(reach * lerp(0.8, 1.2, unit(h3 >> 12)) * size, tuftHeight * lerp(0.7, 1.3, unit(h3 >> 20)) * size,
                     asfloat(sprite | (packedNormal << 8)), asfloat(packColour(ground.rgb, cloudShadow(float3(rel, hg.x - eyeZ)))));
    tufts[slot] = o;
}
