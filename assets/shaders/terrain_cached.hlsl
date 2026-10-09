// The ground drawn from its baked material (game/render/terrain_material_cache):
// the page fields still give the shape, the water and the height - they move
// with the cut - but what the ground is made of is read from the clipmap
// instead of worked out, and only weather, light, shadow and air are live
// (terrainShade). A handful of reads a pixel where terrainSurface makes
// hundreds.
//
// Samplers: the cache's colour (t0), normal (t1), window table (t2), the
// shadow (t3), the detail ids (t4) and the ground scans' colour and normal
// (t5, t6) for the grain under the eye. The page fields are declared
// above them (terrain_page_detail.hlsli) and dropped by the compiler: below
// them, they shifted every slot after and the cache read the wrong textures.
// The material arrays and the biome tables are the bake's business.
#define TERRAIN_MATERIAL_CACHED
#define SHADOW_TEXTURE_SLOT t3
#define SHADOW_SAMPLER_SLOT s3
#include "terrain_pages.hlsl"

Texture2DArray cacheColour : register(t0, space2);
SamplerState cacheColourSampler : register(s0, space2);
Texture2DArray cacheNormal : register(t1, space2);
SamplerState cacheNormalSampler : register(s1, space2);
Texture2D<float4> cacheTable : register(t2, space2);
SamplerState cacheTableSampler : register(s2, space2);
Texture2DArray<float4> cacheDetail : register(t4, space2);
SamplerState cacheDetailSampler : register(s4, space2);
Texture2DArray groundTex : register(t5, space2);
SamplerState groundSampler : register(s5, space2);
Texture2DArray groundNormalTex : register(t6, space2);
SamplerState groundNormalSampler : register(s6, space2);
#include "terrain_layers.hlsli"   // kLayerMetres

static const int kCacheLevels = 11;          // TerrainMaterialCache::kLevels
static const float kCacheSide = 1024.0;      // kSize
static const float kCacheFinest = 0.04;      // kFinestTexel
static const float kCacheMargin = 12.0;      // texels kept from a window's edge

float3 octDecode(float2 e)
{
    e = e * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// How far inside a level's complete window p is, in texels (negative: outside).
float cacheInside(float2 p, int level)
{
    // Sampled (nearest), not loaded: a texture read without its sampler is
    // a storage texture to SDL, and this one is bound as a sampled one.
    const float4 row = cacheTable.SampleLevel(cacheTableSampler, float2((level + 0.5) / kCacheLevels, 0.5), 0);
    if (row.w < 0.5) return -1.0;
    const float2 t = p / row.z - row.xy;
    return min(min(t.x, t.y), min(kCacheSide - t.x, kCacheSide - t.y)) - kCacheMargin;
}

// The level's texel at p. The window is toroidal, so the coordinate is p over
// the level's extent, wrapped - taken as the anchor's share (exact: the anchor
// is a multiple of 64 m and every extent is 64 m times a power of two over
// 1.5625) plus the small local part, so it is exact far from the origin too.
void cacheRead(float2 anchor, float2 local, int level, out float4 colour, out float4 normal)
{
    const float span = kCacheFinest * kCacheSide * exp2((float)level);
    const float2 uv = frac(anchor * (1.0 / 64.0) * (64.0 / span)) + local / span;
    colour = cacheColour.SampleLevel(cacheColourSampler, float3(uv, level), 0);
    normal = cacheNormal.SampleLevel(cacheNormalSampler, float3(uv, level), 0);
}

// Software anisotropy: each level is a single mip, so the sampler has no
// chain to filter along and a glancing pixel - long across the ground, narrow
// along the view - was read at the level of its long side: the ground at the
// feet went to mush. The level is chosen by the narrow side (at most
// kCacheMaxAniso times finer than the long one) and up to that many taps
// cover the long side.
static const float kCacheMaxAniso = 4.0;

void cacheReadAlong(float2 anchor, float2 local, int level, float2 axis, int taps,
                    out float4 colour, out float4 normal)
{
    if (taps <= 1) {
        cacheRead(anchor, local, level, colour, normal);
        return;
    }
    colour = 0.0;
    normal = 0.0;
    [loop] for (int i = 0; i < taps; ++i) {
        float4 c, n;
        cacheRead(anchor, local + axis * ((i + 0.5) / taps - 0.5), level, c, n);
        colour += c;
        normal += n;
    }
    colour /= taps;
    normal /= taps;
}

bool cachedMaterial(float2 p, float2 anchor, float2 local, float2 dx, float2 dy, out SurfaceMaterial m)
{
    m = (SurfaceMaterial)0;
    const float lx = length(dx), ly = length(dy);
    const float major = max(lx, ly);
    const float2 axis = lx > ly ? dx : dy;
    const float footprint = max(max(min(lx, ly), major / kCacheMaxAniso), 1e-6);
    const int taps = clamp((int)ceil(major / footprint - 0.05), 1, (int)kCacheMaxAniso);
    // The level whose texel is the pixel's footprint, and the share of the next.
    const float want = log2(footprint / kCacheFinest);
    int level = clamp((int)floor(want), 0, kCacheLevels - 1);
    float toNext = saturate(want - (float)level);
    float inside = cacheInside(p, level);
    [loop] while (inside < 0.0 && level < kCacheLevels - 1) {
        ++level;
        toNext = 0.0;
        inside = cacheInside(p, level);
    }
    if (inside < 0.0) return false;
    float4 colour, normal;
    cacheReadAlong(anchor, local, level, axis, taps, colour, normal);
    // Toward the coarser level near the window's edge as well, so a level
    // ending is a blend and not a line. The coarser texel is twice the size:
    // half the taps cover the same footprint.
    toNext = max(toNext, 1.0 - saturate(inside / 24.0));
    [branch] if (toNext > 0.004 && level < kCacheLevels - 1 && cacheInside(p, level + 1) >= 0.0) {
        float4 coarseColour, coarseNormal;
        cacheReadAlong(anchor, local, level + 1, axis, (taps + 1) / 2, coarseColour, coarseNormal);
        colour = lerp(colour, coarseColour, toNext);
        normal = lerp(normal, coarseNormal, toNext);
    }
    m.colour = colour.rgb;
    m.hollow = colour.a;
    m.normal = octDecode(normal.xy);
    m.properties = float4(normal.w, normal.z, 0.5, 1.0);
    m.sandRoughness = 0.86;
    m.decalRough = -1.0;
    return true;
}

// The grain under the eye. The finest level's texel is four centimetres and a
// pixel at the feet covers a few millimetres: everything the scan has finer
// than the cache - its pebbles, blades, cracks - came out as a smooth wash.
// It is put back as the ratio of the scan at the pixel's own footprint to the
// scan at the cache's texel, from the layer the bake recorded there and at the
// size the material reads it (terrain_material.hlsli, the fine band). Where
// the footprint reaches the texel the cache holds it all and this fades out.
// It need not line up with the bake's own copy of the scan: what it adds is
// finer than anything the cache has to line up with.
static const float kDetailRepeat = 48.0;   // kMaterialRepeat over the fine band's scale

void cachedDetail(float2 anchor, float2 local, float2 dx, float2 dy, float footprint,
                  inout SurfaceMaterial m)
{
    const float fade = 1.0 - smoothstep(0.30, 0.85, footprint / kCacheFinest);
    [branch] if (fade <= 0.0 || cacheInside(anchor + local, 0) < 0.0) return;
    const float span = kCacheFinest * kCacheSide;
    const float2 at = frac(anchor * (1.0 / 64.0) * (64.0 / span)) + local / span;
    const float2 id = cacheDetail.SampleLevel(cacheDetailSampler, float3(at, 0), 0).xy * 255.0 + 0.5;
    const int layer = min((int)id.x, GROUND_LAYERS - 1), cls = min((int)id.y, 7);
    const float tile = terrainLookPS.x > 0.0 ? terrainLookPS.x : 1.0;
    const float frequency = tablePS[cls].x / 14.0 * kLayerMetres[cls] / kLayerMetres[layer] / tile * 0.5;
    const float2 uv = frameTurns(anchor, frequency, kDetailRepeat) + local * frequency;
    const float2 gx = dx * frequency, gy = dy * frequency;
    // The same footprint, no finer on either axis than the cache's texel and
    // the bilinear spread its reconstruction adds: what the cache holds.
    const float least = kCacheFinest * 1.5 * frequency;
    const float2 cx = gx * max(1.0, least / max(length(gx), 1e-7));
    const float2 cy = gy * max(1.0, least / max(length(gy), 1e-7));
    const float3 fine = groundTex.SampleGrad(groundSampler, float3(uv, layer), gx, gy).rgb;
    const float3 coarse = groundTex.SampleGrad(groundSampler, float3(uv, layer), cx, cy).rgb;
    m.colour *= lerp(1.0, clamp((fine + 0.03) / (coarse + 0.03), 0.45, 2.0), fade);
    const float2 nf = groundNormalTex.SampleGrad(groundNormalSampler, float3(uv, layer), gx, gy).xy;
    const float2 nc = groundNormalTex.SampleGrad(groundNormalSampler, float3(uv, layer), cx, cy).xy;
    m.normal = normalize(m.normal + float3((nf - nc) * 0.8 * fade, 0.0));
}

// The painted look (content/config/style/palettes.json, row "paint": amount,
// metres, how much of the shape's relief to give up, -). A painter puts down
// a field's colour as one mass, not a photograph of its grass: the colour is
// taken from a coarse level of the cache - the same material, its scan
// averaged away over `metres` - and the fine level only lends it its light
// through the normal. Never finer than the pixel's own level, so far ground
// does not alias.
void cachedPaint(float2 p, float2 anchor, float2 local, float2 dx, float2 dy, inout SurfaceMaterial m)
{
    if (!stylePresent()) return;
    const float4 paint = styleRow(5);
    [branch] if (paint.x <= 0.0) return;
    // The level: no coarser than 2.56 m a texel, the last baked from the
    // pages' fine data - coarser levels are baked from the 64 m data, and
    // their pools and borders came out as polygons a hundred metres across.
    // Wider masses are a spread of reads over `metres` instead.
    const float footprint = max(max(min(length(dx), length(dy)), max(length(dx), length(dy)) / kCacheMaxAniso), 1e-6);
    const float spread = max(paint.y, 0.0);
    const float want = max(log2(min(max(spread, kCacheFinest), 1.28) / kCacheFinest), log2(footprint / kCacheFinest));
    int level = clamp((int)floor(want), 0, kCacheLevels - 1);
    // The share of the next level, as cachedMaterial: the footprint is the
    // same over a whole triangle of the ground, and a level picked by it
    // alone changed at the triangles' edges - straight-edged polygons of one
    // colour against the next.
    float toNext = saturate(want - (float)level);
    float inside = cacheInside(p, level);
    [loop] while (inside < 0.0 && level < kCacheLevels - 1) { ++level; toNext = 0.0; inside = cacheInside(p, level); }
    if (inside < 0.0) return;
    toNext = max(toNext, 1.0 - saturate(inside / 48.0));
    if (level >= kCacheLevels - 1 || cacheInside(p, level + 1) < 0.0) toNext = 0.0;
    // Four reads over the spread, turned off the world's axes so the masses
    // have no grain of their own; fewer as the pixel itself grows past it.
    const float reach = max(spread - footprint, 0.0) * 0.5;
    const float2 o1 = float2(0.92, 0.38) * reach, o2 = float2(-0.38, 0.92) * reach;
    float4 colour = 0.0, normal = 0.0, c, n;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 at = local + (k == 0 ? o1 : k == 1 ? -o1 : k == 2 ? o2 : -o2);
        cacheRead(anchor, at, level, c, n);
        float4 c2 = c, n2 = n;
        [branch] if (toNext > 0.004) cacheRead(anchor, at, level + 1, c2, n2);
        colour += lerp(c, c2, toNext);
        normal += lerp(n, n2, toNext);
    }
    colour *= 0.25;
    normal *= 0.25;
    m.colour = lerp(m.colour, colour.rgb, paint.x);
    m.normal = normalize(lerp(m.normal, octDecode(normal.xy), paint.x * paint.z));
}

float4 TerrainCachedPS(TerrainOut input) : SV_Target0
{
    // Shape, water and height as the vertices carry them: the fine relief is
    // in the cache's normal already, and reading the page fields again for
    // every pixel was a third of this shader. A staged preview (the world
    // still being made) keeps the drawn surface's own slope, as TerrainPagePS.
    if (ringState.y >= 3.0) {
        const float3 geometric = normalize(cross(ddx(float3(input.frameXY, input.worldHeight)),
            ddy(float3(input.frameXY, input.worldHeight))));
        input.normal = geometric.z < 0.0 ? -geometric : geometric;
    } else {
        input.normal = normalize(input.normal);
    }

    const float2 anchor = frameAnchorFrom(input.worldXY, input.frameXY);
    const float2 dx = ddx(input.frameXY), dy = ddy(input.frameXY);
    SurfaceMaterial m;
    if (cachedMaterial(input.worldXY, anchor, input.frameXY, dx, dy, m)) {
        // By the narrow side: the scans' sampler is anisotropic and resolves
        // the grain along it, as the long way does - out to tens of metres.
        cachedDetail(anchor, input.frameXY, dx, dy,
                     max(min(length(dx), length(dy)), max(length(dx), length(dy)) / 8.0), m);
        cachedPaint(input.worldXY, anchor, input.frameXY, dx, dy, m);
    } else {
        // Nothing baked here yet: plain ground until the cache arrives.
        m.colour = float3(0.24, 0.27, 0.16);
        m.normal = input.normal;
        m.properties = float4(1.0, 0.85, 0.5, 1.0);
        m.sandRoughness = 0.86;
        m.decalRough = -1.0;
    }
    float4 colour = terrainShade(input, m);
    // The world editor's region grid, selection and brush, as TerrainPagePS.
    const float4 marks = editorMarks(input.worldXY, max(length(dx), length(dy)));
    colour.rgb = lerp(colour.rgb, marks.rgb, marks.a);
    return colour;
}
