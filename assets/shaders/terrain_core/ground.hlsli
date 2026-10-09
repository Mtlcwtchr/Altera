// What the ground is made of at a point: biome, then the materials the biome
// lays there. Fragment stage only (textures and uniforms in space2/space3).
//
// Positions arrive as an exact integer (the patch origin, in level-0 texels)
// plus a small float offset, never as a float world coordinate: a texture
// coordinate or a noise cell computed from 1 900 000 m in a float is a
// quarter of a metre out, which on a 4 m tile is a smear.

Texture2D<float> biomeMap : register(t1, space2);
SamplerState biomeSampler : register(s1, space2);
Texture2DArray<float4> materials : register(t2, space2);
SamplerState materialSampler : register(s2, space2);

// Must match terrain_core::GroundUniforms.
cbuffer Ground : register(b1, space3) {
    float4 biomeData[32 * 5];   // per biome: materials, patch, slope, high, tint
    float4 groundGlobals;       // shore material, shore height, sea floor material, log2(tile in level-0 texels)
    uint4 groundSizes;          // biome map size, -, -, -
    float4 groundWorld;         // world size, spacing, -, -
};

struct GroundPoint {
    int2 base;          // level-0 texel index of the patch origin
    float2 sub;         // offset from it, level-0 texels
    float footprint;    // level-0 texels one pixel covers
};

// How far a noise of cells `cell` texels wide has to be faded to its mean
// here: a pixel that covers half a cell or more can only alias it into grain.
float noiseFade(GroundPoint p, int log2Cell) {
    return saturate(p.footprint / float(1 << log2Cell) * 2.0 - 0.5);
}

uint groundHash(int2 c, uint seed) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

// Position in cells of 2^log2Cell level-0 texels: integer cell and fraction.
void cellOf(GroundPoint p, int log2Cell, out int2 cell, out float2 f) {
    const int size = 1 << log2Cell;
    const float2 inCell = (float2(p.base & (size - 1)) + p.sub) / float(size);
    const float2 whole = floor(inCell);
    cell = (p.base >> log2Cell) + int2(whole);
    f = inCell - whole;
}

// Value noise, 0..1, cells of 2^log2Cell level-0 texels.
float cellNoise(GroundPoint p, int log2Cell, uint seed) {
    int2 c;
    float2 f;
    cellOf(p, log2Cell, c, f);
    const float2 u = f * f * (3 - 2 * f);
    const float a = float(groundHash(c, seed) & 0xffffu), b = float(groundHash(c + int2(1, 0), seed) & 0xffffu);
    const float d = float(groundHash(c + int2(0, 1), seed) & 0xffffu), e = float(groundHash(c + int2(1, 1), seed) & 0xffffu);
    return lerp(lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y) / 65535.0, 0.5, noiseFade(p, log2Cell));
}

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

float3 material(float layer, GroundPoint p, float wander) {
    const int tile = int(groundGlobals.w);
    const float2 uv = tileUv(p, tile);
    const float2 dx = ddx(uv), dy = ddy(uv);
    const float k = cellNoise(p, tile + 1, 7u) * 8.0;
    const float i = floor(k), f = frac(k);
    const float3 a = materials.SampleGrad(materialSampler, float3(uv + offsetOf(i), layer), dx, dy).rgb;
    const float3 b = materials.SampleGrad(materialSampler, float3(uv + offsetOf(i + 1), layer), dx, dy).rgb;
    const float3 colour = lerp(a, b, smoothstep(0.2, 0.8, f - 0.1 * dot(a - b, 1)));
    return colour * (0.88 + 0.24 * wander);
}

float3 biomeGround(uint id, GroundPoint p, float height, float normalZ, float wander) {
    const float4 mats = biomeData[id * 5 + 0];
    const float4 patch = biomeData[id * 5 + 1];
    const float4 slope = biomeData[id * 5 + 2];
    const float4 high = biomeData[id * 5 + 3];
    const float4 tint = biomeData[id * 5 + 4];
    if (mats.x < 0) return float3(0.5, 0.5, 0.5);
    float3 colour = material(mats.x, p, wander);
    if (mats.y >= 0) {
        const float n = islands(p, int(patch.x), 101u + id);
        // Far off the islands are smaller than a pixel: what is left of them
        // is their share of the ground, not a speckle.
        const float m = lerp(smoothstep(1.0 - patch.y - 0.04, 1.0 - patch.y + 0.04, n), patch.y,
                             noiseFade(p, int(patch.x) - 1));
        if (m > 0) colour = lerp(colour, material(mats.y, p, wander), m);
    }
    // Ragged edges for the slope and height layers: the threshold wanders.
    const float jitter = cellNoise(p, int(groundGlobals.w) + 3, 211u + id) - 0.5;
    if (mats.z >= 0) {
        const float m = saturate((slope.x - normalZ + jitter * 0.06) / max(slope.x - slope.y, 1e-3));
        if (m > 0) colour = lerp(colour, material(mats.z, p, wander), m);
    }
    if (mats.w >= 0) {
        const float m = saturate((height - high.x + jitter * 150.0) / max(high.y - high.x, 1.0)) * saturate(normalZ * 1.6 - 0.4);
        if (m > 0) colour = lerp(colour, material(mats.w, p, wander), m);
    }
    return colour * tint.rgb;
}

float3 biomeDebugColour(uint id) {
    const uint h = groundHash(int2(int(id), 7), 3u);
    return float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0;
}

// The ground colour at p. `debug` returns the blended biome colours instead.
float3 groundColour(GroundPoint p, float height, float normalZ, bool debug) {
    const float wander = cellNoise(p, int(groundGlobals.w) + 6, 5u);

    // The biome map, at the four texels round the point, the lookup pushed
    // about by a noise of a few hundred metres so a border is ragged.
    const float size = float(groundSizes.x);
    const float texelMetres = groundWorld.x / size;
    const float2 metres = (float2(p.base) + p.sub) * groundWorld.y;
    const int warpCell = int(round(log2(max(texelMetres * 0.35 / groundWorld.y, 1.0))));
    const float2 warp = float2(cellNoise(p, warpCell, 31u), cellNoise(p, warpCell, 37u)) - 0.5;
    const float2 at = metres / texelMetres - 0.5 + warp * 0.9;
    const float2 base = floor(at);
    const float2 f = at - base;
    uint ids[4];
    float weights[4] = {(1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y};
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 t = clamp(base + float2(k & 1, k >> 1), 0, size - 1);
        ids[k] = uint(round(biomeMap.SampleLevel(biomeSampler, (t + 0.5) / size, 0) * 255.0));
    }
    // Merge the same biome found at several corners, then sharpen: the
    // transition is a ragged band, not a two-kilometre wash.
    float best = -1;
    [unroll] for (int a = 0; a < 4; ++a) {
        [unroll] for (int b = 0; b < a; ++b)
            if (ids[b] == ids[a] && weights[b] >= 0) { weights[b] += weights[a]; weights[a] = -1; }
    }
    [unroll] for (int c = 0; c < 4; ++c) {
        if (weights[c] < 0) continue;
        weights[c] += (cellNoise(p, int(groundGlobals.w) + 4, 61u + ids[c]) - 0.5) * 0.35;
        best = max(best, weights[c]);
    }
    float3 colour = 0;
    float total = 0;
    [unroll] for (int d = 0; d < 4; ++d) {
        if (weights[d] < 0) continue;
        const float w = exp((weights[d] - best) * 14.0);
        if (w < 0.02) continue;
        colour += w * (debug ? biomeDebugColour(ids[d]) : biomeGround(ids[d], p, height, normalZ, wander));
        total += w;
    }
    colour /= max(total, 1e-4);
    if (debug) return colour;

    // The coast and the sea floor belong to no biome.
    if (groundGlobals.x >= 0 && height < groundGlobals.y) {
        const float m = saturate((groundGlobals.y - height) / max(groundGlobals.y * 0.5, 0.5) + (wander - 0.5) * 0.6);
        colour = lerp(colour, material(groundGlobals.x, p, wander), m);
    }
    if (height < 0 && groundGlobals.z >= 0) {
        const float3 floorColour = material(groundGlobals.z, p, wander);
        colour = lerp(floorColour * float3(0.55, 0.75, 0.85), float3(0.05, 0.13, 0.24), saturate(-height / 40.0));
    }
    return colour;
}
