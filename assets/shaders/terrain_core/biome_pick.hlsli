// Which biome is where: the biome map at the four texels round a point, the
// lookup pushed about by a noise of a few hundred metres so a border is a
// ragged band and not the map's grid. Shared by the ground's colour and by
// the foliage, so a forest ends where its ground does.
#ifndef TERRAIN_CORE_BIOME_PICK
#define TERRAIN_CORE_BIOME_PICK

#include "ground_noise.hlsli"

struct BiomeMapInfo {
    float size;        // texels a side
    float worldSize;   // metres
    float spacing;     // metres per level-0 texel
    int tileLog2;      // the ground tile, log2 of level-0 texels: scales the border noise
};

// The (up to four) biomes round p and their weights: the same biome at
// several corners merged, each weight jittered by its own noise. Unused
// entries are -1. Returns the largest weight.
float biomeCorners(Texture2D<float> map, SamplerState at, BiomeMapInfo info, GroundPoint p,
                   out uint ids[4], out float weights[4]) {
    const float texelMetres = info.worldSize / info.size;
    const float2 metres = (float2(p.base) + p.sub) * info.spacing;
    const int warpCell = int(round(log2(max(texelMetres * 0.35 / info.spacing, 1.0))));
    const float2 warp = float2(cellNoise(p, warpCell, 31u), cellNoise(p, warpCell, 37u)) - 0.5;
    const float2 where = metres / texelMetres - 0.5 + warp * 0.9;
    const float2 base = floor(where);
    const float2 f = where - base;
    weights[0] = (1 - f.x) * (1 - f.y);
    weights[1] = f.x * (1 - f.y);
    weights[2] = (1 - f.x) * f.y;
    weights[3] = f.x * f.y;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 t = clamp(base + float2(k & 1, k >> 1), 0, info.size - 1);
        ids[k] = uint(round(map.SampleLevel(at, (t + 0.5) / info.size, 0) * 255.0));
    }
    [unroll] for (int a = 0; a < 4; ++a) {
        [unroll] for (int b = 0; b < a; ++b)
            if (ids[b] == ids[a] && weights[b] >= 0) { weights[b] += weights[a]; weights[a] = -1; }
    }
    float best = -1;
    [unroll] for (int c = 0; c < 4; ++c) {
        if (weights[c] < 0) continue;
        weights[c] += (cellNoise(p, info.tileLog2 + 4, 61u + ids[c]) - 0.5) * 0.35;
        best = max(best, weights[c]);
    }
    return best;
}

uint dominantBiome(Texture2D<float> map, SamplerState at, BiomeMapInfo info, GroundPoint p) {
    uint ids[4];
    float weights[4];
    const float best = biomeCorners(map, at, info, p, ids, weights);
    uint id = ids[0];
    [unroll] for (int k = 0; k < 4; ++k)
        if (weights[k] == best) id = ids[k];
    return id;
}

#endif
