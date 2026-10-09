// The ground material cache's bake (game/render/terrain_material_cache.cpp):
// terrainMaterial - which ground, which variant, the blend, moss, stones,
// decals - drawn once for each texel of a clipmap level instead of once for
// every pixel of every frame. A texel is a regular grid point of the world, so
// the screen derivatives the material filters by are the texel's own size:
// the same code, at the cache's resolution.
//
// Samplers as the terrain's (t0..t11, t13, t14) with the climate planes as one
// array in the shadow's slot: the bake casts no shadow, and SDL allows sixteen.
#define SHADOW_TEXTURE_SLOT t15
#define SHADOW_SAMPLER_SLOT s15
#include "terrain_pages.hlsl"

Texture2DArray climateArray : register(t12, space2);
SamplerState climateArraySampler : register(s12, space2);

cbuffer MaterialBake : register(b2, space3)
{
    float4 bakeAnchor;   // anchor xy (a multiple of 64 m), the window's start in metres from it
    float4 bakeTexel;    // texel metres, texels a side, the window's start modulo the side
    float4 bakeLevel;    // clipmap level, coarsest page dataset worth reading, lite (gMaterialLite), -
};

struct BakeOut {
    float4 albedo : SV_Target0;   // colour (sRGB target), concavity
    float4 normal : SV_Target1;   // octahedral normal, roughness, occlusion
    float2 detail : SV_Target2;   // the near detail's layer and class, over 255
};

float4 MaterialBakeVS(uint id : SV_VertexID) : SV_Position
{
    const float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * 2.0 - 1.0, 0.0, 1.0);
}

// climateAt, read from the array in the pixel stage.
SurfaceClimate bakeClimateAt(float2 worldXY)
{
    const float2 uv = worldXY * parametersPS[20].xy + parametersPS[20].zw;
    const float4 a = climateArray.SampleLevel(climateArraySampler, float3(uv, 0), 0);
    const float4 b = climateArray.SampleLevel(climateArraySampler, float3(uv, 1), 0);
    const float4 c = climateArray.SampleLevel(climateArraySampler, float3(uv, 2), 0);
    SurfaceClimate result;
    result.foliage = a;
    result.desert = b.x;
    result.environment = float4(b.y, b.z, b.w, 1.0);
    result.geography = float4((c.x - 0.5) * kClimateSignedSpan, (c.y - 0.5) * kClimateSignedSpan, 0.0, c.z);
    result.forest = c.w;
    return result;
}

float2 octEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

BakeOut MaterialBakePS(float4 position : SV_Position)
{
    const float texel = bakeTexel.x, side = bakeTexel.y;
    // Which texel of the window: the target is addressed modulo the side.
    const float2 k = fmod(floor(position.xy) - bakeTexel.zw + side, side);
    const float2 local = bakeAnchor.zw + (k + 0.5) * texel;
    const float2 p = bakeAnchor.xy + local;
    // The finest page data there is; no finer than the texel needs.
    const int level = max(finestResidentLevel(p), (int)bakeLevel.y);
    const PageDetail detail = pageDetailAt(p, level);
    gEnvironment = pageEnvironmentAt(p, level);
    const float3 shape = pageShapeAt(p, level);

    TerrainOut input = (TerrainOut)0;
    input.position = position;
    input.worldXY = p;
    input.frameXY = local;
    input.worldHeight = detail.bed;
    input.normal = normalize(float3(-shape.xy, 1.0));
    input.weights0 = detail.weights0;
    input.weights1 = detail.weights1;
    pageBlurredWeights(p, detail, input.weights0, input.weights1);
    input.waterDepth = shoreDepthAt(p, detail.head - detail.bed);
    const SurfaceClimate place = bakeClimateAt(p);
    input.foliage = place.foliage;
    input.desertCover = place.desert;
    input.environment = place.environment;
    input.forest = place.forest;
    input.geography = float4(place.geography.x, place.geography.y, shape.z, place.geography.w);
    input.windExposure = 0.5;
    input.travelCost = 1.0;
    input.weather = 0.0;

    gMaterialLite = bakeLevel.z;
    gDetailLayer = 0;
    gDetailClass = 0;
    const SurfaceMaterial m = terrainMaterial(input);
    // Roughness as terrainShade works it out, less the weather's share.
    const float roughNoise = filteredMaterialNoise(p / 0.7, texel / 0.7);
    float rough = clamp(m.properties.g + (roughNoise - 0.5) * 0.06, 0.38, 0.98);
    if (m.decalRough >= 0.0) rough = lerp(rough, m.decalRough, m.decalCoverage);
    rough = lerp(rough, m.sandRoughness, m.sandCover);

    BakeOut o;
    o.albedo = float4(saturate(m.colour), saturate(m.hollow));
    o.normal = float4(octEncode(normalize(m.normal)), rough, saturate(m.properties.r));
    o.detail = float2(gDetailLayer, gDetailClass) / 255.0;
    return o;
}
