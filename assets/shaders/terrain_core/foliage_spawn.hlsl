// Foliage, entirely on the GPU and remade every frame: no streaming, no state.
//
// spawnCs   one thread per candidate cell of one layer round the eye. The
//           candidate's place in its cell is a hash of the cell's integer
//           index, so the same tree stands in the same place whatever the eye
//           does. It is dropped, cheapest test first, if it is: out of range;
//           out of the frustum (a vertical segment through all the heights
//           there could be, before anything is read); on ground too steep, in
//           the sea, or not wanted by its biome (biome_pick.hlsli - the same
//           choice the ground's colour makes); outside the frustum as a
//           sphere; under a pixel and a half on screen. What survives is
//           appended to its model's slice of the instance buffer.
// foliage_args.hlsl zeroes the counters and turns them into draw arguments;
// foliage_draw.hlsl draws the instances.

#include "biome_pick.hlsli"

struct Instance {
    float4 positionYaw;   // eye-relative position (m), yaw
    float4 extra;         // scale, texture layer, fade, height (m)
};

// ---- spawning -------------------------------------------------------------

Texture2DArray<float4> clipmap : register(t0, space0);
Texture2D<float> biomeMap : register(t1, space0);
Texture2D<float4> waterMap : register(t2, space0);
StructuredBuffer<float4> modelInfo : register(t3, space0);   // radius, height, texture layer, -
StructuredBuffer<uint4> modelSlots : register(t4, space0);   // instance offset, capacity, first index, index count
SamplerState clipSampler : register(s0, space0);
SamplerState biomeSampler : register(s1, space0);
SamplerState waterSampler : register(s2, space0);
#include "water_map.hlsli"
#define RIVER_SEGMENTS register(t5, space0)
#define RIVER_TILES register(t6, space0)
#define RIVER_LIST register(t7, space0)
#include "rivers.hlsli"
#include "clearings.hlsli"
RWStructuredBuffer<Instance> instances : register(u0, space1);
RWStructuredBuffer<uint> counters : register(u1, space1);

cbuffer Spawn : register(b0, space2) {
    float4 planes[6];          // eye-relative frustum, inside >= 0
    int2 camCell;              // the eye, in this layer's cells
    float2 camCellFrac;
    int2 camTexel;             // the eye, in level-0 texels
    float2 camTexelFrac;
    float eyeZ;
    float spacing0;            // metres per level-0 texel
    float cellSize;            // this layer's spacing, metres
    float range;               // metres
    uint gridSide;             // cells a side of the candidate square
    uint clipSize;
    uint levels;
    uint seed;
    uint firstModel;
    uint modelCount;
    float scaleMin;
    float scaleMax;
    float minNormalZ;
    float pixelScale;          // pixels per unit of size at one metre
    float minPixels;
    float shoreHeight;
    float biomeSize;
    float worldSize;
    int tileLog2;
    float heightLow;           // lowest and highest ground there can be, eye-relative
    float heightHigh;
    float waterMapSize;
    float spawnHeld;           // AL-78: 1 = inside holdRect the game holds this layer's trees
    float spawnPad;
    float4 holdRect;           // world metres x0, y0, x1, y1
    float4 biomeRules[32];     // density, log2 cluster cell (level-0 texels, 0 = even), cover, -
};

uint hashCell(int2 c, uint s) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float unit(uint h) { return float(h & 0xffffu) / 65536.0; }

int2 wrapSlot(int2 t) { const int n = int(clipSize); return ((t % n) + n) % n; }

float4 texel(int2 slot, uint level) {
    return clipmap.SampleLevel(clipSampler, float3((float2(wrapSlot(slot)) + 0.5) / float(clipSize), float(level)), 0);
}

// Height and gradient at a level-0 texel position (integer + fraction), from
// the finest clipmap level whose window holds it.
float4 groundAt(int2 base, float2 sub, float distance) {
    const float reach = (float(clipSize) * 0.5 - 3.0) * spacing0;
    const uint level = uint(clamp(ceil(log2(max(distance / reach, 1e-6))), 0.0, float(levels - 1)));
    const int mask = (1 << level) - 1;
    const float2 at = (float2(base & mask) + sub) / float(1 << level);
    const float2 whole = floor(at);
    const int2 t = (base >> level) + int2(whole);
    const float2 f = at - whole;
    const float4 a = texel(t, level), b = texel(t + int2(1, 0), level);
    const float4 c = texel(t + int2(0, 1), level), d = texel(t + int2(1, 1), level);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

bool boxOutside(float3 lo, float3 hi) {
    [unroll] for (int k = 0; k < 6; ++k) {
        const float4 p = planes[k];
        const float3 far = float3(p.x >= 0 ? hi.x : lo.x, p.y >= 0 ? hi.y : lo.y, p.z >= 0 ? hi.z : lo.z);
        if (dot(p.xyz, far) + p.w < 0) return true;
    }
    return false;
}

bool sphereOutside(float3 centre, float radius) {
    [unroll] for (int k = 0; k < 6; ++k)
        if (dot(planes[k].xyz, centre) + planes[k].w < -radius) return true;
    return false;
}

[numthreads(8, 8, 1)]
void spawnCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gridSide || id.y >= gridSide) return;
    const int2 cell = camCell - int(gridSide / 2) + int2(id.xy);
    const uint h = hashCell(cell, seed);
    const float2 jitter = float2(h & 0xffffu, h >> 16) / 65536.0;
    const float2 rel = (float2(cell - camCell) + jitter - camCellFrac) * cellSize;
    const float distance = length(rel);
    if (distance > range) return;
    // Before reading anything: could anything here be seen at all?
    const float biggest = 20.0;
    if (boxOutside(float3(rel - biggest, heightLow), float3(rel + biggest, heightHigh + biggest))) return;

    const float2 t0 = camTexelFrac + rel / spacing0;
    GroundPoint p;
    p.base = camTexel + int2(floor(t0));
    p.sub = t0 - floor(t0);
    p.footprint = 0;
    const float4 ground = groundAt(p.base, p.sub, distance);
    if (ground.x < shoreHeight) return;
    const float normalZ = rsqrt(1.0 + dot(ground.yz, ground.yz));
    if (normalZ < minNormalZ) return;
    // Nothing grows under water: a lake, a river, its wet bank.
    float wet;
    const float2 metres = (float2(p.base) + p.sub) * spacing0;
    // AL-78: there the trees are the game's own (foliage_hold.hlsl draws them)
    if (spawnHeld > 0.5 && all(metres >= holdRect.xy) && all(metres < holdRect.zw)) return;
    if (waterAt(waterMap, waterSampler, metres, worldSize, waterMapSize, wet).x > ground.x - 0.3) return;
    const RiverHit river = nearestRiver(metres);
    if (river.found && river.distance < river.halfWidth + 1.5) return;

    BiomeMapInfo info;
    info.size = biomeSize;
    info.worldSize = worldSize;
    info.spacing = spacing0;
    info.tileLog2 = tileLog2;
    const float4 rule = biomeRules[min(dominantBiome(biomeMap, biomeSampler, info, p), 31u)];
    if (rule.x <= 0) return;
    // Thinner towards the end of the range, so the edge is not a line.
    const float fade = 1.0 - smoothstep(0.7 * range, range, distance);
    float chance = rule.x * fade;
    if (rule.y > 0) {
        const float n = cellNoise(p, int(rule.y), seed + 7u) * 0.7 + cellNoise(p, int(rule.y) - 2, seed + 11u) * 0.3;
        chance *= smoothstep(1.0 - rule.z - 0.08, 1.0 - rule.z + 0.08, n);
    }
    chance *= clearingKeep(metres);   // the village's yards and works
    const uint h2 = hashCell(cell, seed + 101u);
    if (unit(h2) >= chance) return;

    const uint h3 = hashCell(cell, seed + 202u);
    const uint model = firstModel + (h3 % modelCount);
    const float scale = lerp(scaleMin, scaleMax, unit(h3 >> 8));
    const float4 info4 = modelInfo[model];
    const float radius = info4.x * scale, height = info4.y * scale;
    const float3 base3 = float3(rel, ground.x - eyeZ);
    if (sphereOutside(base3 + float3(0, 0, height * 0.5), radius)) return;
    // Visibility by size: under `minPixels` across it is noise, not a plant.
    if (radius * pixelScale / max(distance, 1.0) < minPixels) return;

    uint slot;
    InterlockedAdd(counters[model], 1u, slot);
    const uint4 slots = modelSlots[model];
    if (slot >= slots.y) return;
    Instance o;
    o.positionYaw = float4(base3, unit(h2 >> 16) * 6.2831853);
    o.extra = float4(scale, info4.z, fade, height);   // height: the draw lights the crown by it
    instances[slots.x + slot] = o;
}
