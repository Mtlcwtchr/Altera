// The ground, drawn from the clipmap with no mesh: every vertex is a point of
// one shared G x G grid, placed by its patch (instance) and lifted by the
// height the generator wrote. Near the end of its level's range a vertex slides
// onto the next level's grid and takes that level's height (CDLOD morph), so a
// patch meets its coarser neighbour exactly and nothing pops.
//
// Lit by lighting.hlsli (sun, sky, shadows, cloud shadows, fog); the ground's
// colour is the biome's materials (ground.hlsli). shadowPs: the depth-only
// pass into the sun's shadow atlas - the same vertices, the light's matrix.

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

#define LIGHT_UNIFORMS register(b2, space3)
#define SHADOW_TEXTURE register(t5, space2)
#define SHADOW_SAMPLER register(s5, space2)
#include "lighting.hlsli"

#define RIVER_SEGMENTS register(t6, space2)
#define RIVER_TILES register(t7, space2)
#define RIVER_LIST register(t8, space2)
#include "rivers.hlsli"

#define RELIEF_PATCHES register(t9, space2)
#define RELIEF_TEXELS register(t10, space2)
#include "relief.hlsli"

cbuffer Look : register(b0, space3) {
    float3 lookUnusedSun;      // the light is lighting.hlsli's now
    float lookUnusedHaze;
    float3 lookUnusedSky;
    uint debugLevels;
    // The vertex stage's View is not visible here: each stage has its own uniforms.
    uint lookClipSize;
    uint lookLevels;
    uint lookFlags;      // 1: plain ground (the materials module off)
    float lookPad;
};

#include "far_detail.hlsli"

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
    GroundPoint p;
    p.base = input.origin << input.level;
    p.sub = input.local * float(1u << input.level);
    p.footprint = max(length(ddx(p.sub)), length(ddy(p.sub)));
    {
        // The fine relief (a marsh hill's low terraces) where this level's
        // mesh is too coarse to hold it: its slopes - and so its rock and its
        // light - in the shading, filtered over the pixel.
        const float levelSpacing = groundWorld.y * float(1u << input.level);
        float weight, texel;
        const float3 fine = reliefShade(p.base, p.sub, groundWorld.y, p.footprint * groundWorld.y, weight, texel);
        const float fade = weight * saturate(levelSpacing / texel - 0.5);
        if (fade > 0) s.yz = lerp(s.yz, fine.yz, fade);
    }
    float3 normal = normalize(float3(-s.y, -s.z, 1));
    const float3 surfaceNormal = normal;
    float3 colour;
    if ((lookFlags & 1u) != 0 && debugLevels != 5) {
        // The ground module off: colour by height and slope only.
        const float slope = 1.0 - normal.z;
        if (height < 0.0) colour = lerp(float3(0.06, 0.14, 0.26), float3(0.16, 0.34, 0.42), saturate(1.0 + height / 60.0));
        else {
            colour = lerp(float3(0.30, 0.42, 0.18), float3(0.52, 0.50, 0.48), saturate(height / 2500.0));
            colour = lerp(colour, float3(0.40, 0.36, 0.32), saturate((slope - 0.15) * 5.0));
        }
    } else {
        groundGradient = s.yz;
        groundHeight = height;
        const Surface surface = groundSurface(p, height, normal.z, debugLevels == 5);
        // Surface maps (the material's normal and AO) unless switched off (flag 2).
        const bool maps = (lookFlags & 2u) == 0;
        colour = surface.colour * (maps ? lerp(1.0, surface.ao, 0.8) : 1.0);
        // The material's own relief on the terrain's: slopes add. Faded out
        // where a texel of it is smaller than a pixel (the mips flatten it anyway).
        const float relief = maps ? 1.0 - saturate(p.footprint / 24.0) : 0.0;
        normal = normalize(float3(-s.y + surface.slope.x * relief, -s.z + surface.slope.y * relief, 1));
        // Past a hundred and fifty metres or so: what the mesh and the
        // textures can no longer show, painted (far_detail.hlsli).
        if (debugLevels != 5 && dot(input.view, input.view) > min(farRange.x, farRange.z) * min(farRange.x, farRange.z)) {
            const float levelSpacing = groundWorld.y * float(1u << input.level);
            const float curvature = farCurvature(input.slots.xy, input.local, input.level, height, levelSpacing);
            float2 farSlope = 0;
            farDetail(p, height, surfaceNormal, input.view, curvature, surface.grass, colour, farSlope);
            normal = normalize(normal / normal.z + float3(-farSlope, 0));
        }
    }
    // The river graph: banks damp near the water; and a river this level's
    // mesh is too coarse to cut (the generator leaves it) painted instead,
    // never thinner than about a pixel if it is a big one - so rivers read
    // from far off, the way they do on a map.
    const float2 metres = (float2(p.base) + p.sub) * groundWorld.y;
    const RiverHit river = nearestRiver(metres);
    float riverCover = 0;
    if (river.found) {
        const float resolved = riverResolved(river, groundWorld.y * float(1u << input.level));
        const float damp = 1.0 - saturate((river.distance - river.halfWidth) / max(channelBank(river) * 0.5, 1.0));
        colour *= 1.0 - 0.3 * damp * resolved;
        const float pixel = max(p.footprint * groundWorld.y, 1e-3);   // metres a pixel spans
        // Never thinner than its rank allows: the biggest river two pixels
        // across, a tributary one, a brook only as wide as it really is (and
        // so fading out as it gets under a pixel).
        const float rank = smoothstep(0.2, 0.9, river.flow);
        const float halfWidth = max(river.halfWidth, pixel * rank);
        const float strength = max(saturate(2.0 * river.halfWidth / pixel), smoothstep(0.2, 0.5, river.flow));
        riverCover = saturate((halfWidth - river.distance) / pixel + 0.5) * (1.0 - resolved) * strength;
    }
    if (debugLevels == 5) return float4(colour, 1);
    if (debugLevels == 1) colour = lerp(colour, levelTint(input.level), 0.45);
    if (debugLevels == 2) return float4(normal * 0.5 + 0.5, 1);
    if (debugLevels == 3) return float4(input.morph, frac(float(input.level) * 0.37), 1.0 - input.morph, 1);
    if (debugLevels == 4) return float4(frac(height / 50.0), frac(height / 500.0), saturate(height / 3000.0), 1);

    const float sunLight = sunShadow(input.view, surfaceNormal) * cloudShadow(input.view);
    float3 lit = shadeSurface(colour, normal, input.view, sunLight, 1.0, 0.0);
    if (riverCover > 0) {
        // Flat water: the sky by Fresnel over the water's own dark colour.
        const float3 toEye = -normalize(input.view);
        const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(toEye.z), 5.0);
        const float3 body = float3(0.02, 0.09, 0.11) * (lerp(lightGround, lightZenith, 0.7) * 1.2 + lightSunColour * 0.25 * sunLight);
        lit = lerp(lit, lerp(body, skyColour(float3(-toEye.xy, abs(toEye.z))), fresnel), riverCover);
    }
    return float4(applyFog(lit, input.view), 1);
}

// Depth only, into the shadow atlas.
void shadowPs(VsOut input) {}
