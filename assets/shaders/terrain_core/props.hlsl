// Mega structures (terrain_core/props.cpp): placed models, one instanced draw
// per model and level. No texture of their own: the ground's material array
// laid on from three sides in world space, two scales of it, so a spire is
// the same rock as the canyon round it and holds up from a kilometre off and
// from its foot. Lit, shadowed and hazed exactly as the ground.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative (the light's, in the shadow pass)
    float4 wind;
    float4 eyeWrapped;
};

struct VertexIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;            // cavity occlusion, height share up the model
    float4 positionYaw : TEXCOORD3;   // instance: eye-relative origin, yaw
    float4 scale : TEXCOORD4;         // instance: scale x, y, z; material layer
    float4 anchor : TEXCOORD5;        // instance: origin x, y wrapped to 4096 m; origin z; variation
    float4 extra : TEXCOORD6;         // instance: cover (steppe on its tops), a cliff's two biomes (-1: none), mirror
    float blend : TEXCOORD7;          // a cliff's vertex: how far it wears the second biome
};

struct VsOut {
    float4 position : SV_Position;
    float3 view : TEXCOORD0;          // eye-relative
    float3 normal : TEXCOORD1;
    float3 place : TEXCOORD2;         // texture space: wrapped world metres
    float2 uv : TEXCOORD3;
    nointerpolation float2 layer : TEXCOORD4;   // material layer, variation
    nointerpolation float hero : TEXCOORD5;     // 1: a megalith
    nointerpolation float cover : TEXCOORD6;    // 0..1: ground on its tops
    nointerpolation float mirror : TEXCOORD9;   // 0..1: mirrors the sky
    float blend : TEXCOORD7;                    // a cliff: the second biome's share
    nointerpolation float2 biomes : TEXCOORD8;  // a cliff: its two biomes (-1: not a cliff)
};

VsOut propsVs(VertexIn input) {
    const float c = cos(input.positionYaw.w), s = sin(input.positionYaw.w);
    const float3 local = input.position * input.scale.xyz;
    const float3 turned = float3(local.x * c - local.y * s, local.x * s + local.y * c, local.z);
    const float3 n = input.normal / max(input.scale.xyz, 1e-3);
    VsOut o;
    o.view = input.positionYaw.xyz + turned;
    o.position = mul(viewProjection, float4(o.view, 1));
    o.normal = float3(n.x * c - n.y * s, n.x * s + n.y * c, n.z);
    o.place = float3(input.anchor.xy + turned.xy, input.anchor.z + turned.z);
    o.uv = input.uv;
    o.layer = float2(input.scale.w, frac(input.anchor.w));
    o.hero = input.anchor.w > 1.5 ? 1.0 : 0.0;
    o.cover = input.extra.x;
    o.mirror = input.extra.w;
    o.blend = input.blend;
    o.biomes = input.extra.yz;
    return o;
}

Texture2DArray<float4> materials : register(t0, space2);
SamplerState materialSampler : register(s0, space2);
Texture2DArray<float4> materialNormals : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
Texture2DArray<float4> materialProperties : register(t2, space2);
SamplerState propertySampler : register(s2, space2);

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t3, space2)
#define SHADOW_SAMPLER register(s3, space2)
#include "lighting.hlsli"

// Must match terrain_core::PropsLook.
cbuffer Rock : register(b1, space3) {
    float rockTile;          // the ground's material tile, metres
    float rockTextureScale;  // render over output pixels (0 = 1): the mip bias under DLSS
    float rockMossLayer;     // the material laid as moss
    float rockCoverLayer;    // the ground laid on tops (steppe)
};

// Must match terrain_core::Props::CliffBiomes: per biome of biomes.json, what
// its cliffs wear (tools/terrain_lab/world_cliffs.py).
cbuffer Biomes : register(b2, space3) {
    float4 biomeStone[32];   // rock layer, ground layer, ground from (normal z), rock below (normal z)
    float4 biomeTint[32];    // the biome's tint, moss on its rock
};

struct Rock { float3 colour; float3 normal; float ao; };

// One scale of the material from three sides, normals blended the UDN way.
Rock triplanar(float3 p, float3 n, float layer, float tile, float bias) {
    float3 w = pow(abs(n), 4.0);
    w /= dot(w, 1.0);
    const float2 uvX = p.yz / tile, uvY = p.xz / tile, uvZ = p.xy / tile;
    Rock r;
    r.colour = materials.SampleBias(materialSampler, float3(uvX, layer), bias).rgb * w.x +
               materials.SampleBias(materialSampler, float3(uvY, layer), bias).rgb * w.y +
               materials.SampleBias(materialSampler, float3(uvZ, layer), bias).rgb * w.z;
    const float3 tx = materialNormals.SampleBias(normalSampler, float3(uvX, layer), bias).xyz * 2.0 - 1.0;
    const float3 ty = materialNormals.SampleBias(normalSampler, float3(uvY, layer), bias).xyz * 2.0 - 1.0;
    const float3 tz = materialNormals.SampleBias(normalSampler, float3(uvZ, layer), bias).xyz * 2.0 - 1.0;
    const float3 nx = float3(tx.xy + n.zy, n.x).zyx;
    const float3 ny = float3(ty.xy + n.xz, n.y).xzy;
    const float3 nz = float3(tz.xy + n.xy, n.z);
    r.normal = normalize(nx * w.x + ny * w.y + nz * w.z);
    r.ao = materialProperties.SampleBias(propertySampler, float3(uvX, layer), bias).r * w.x +
           materialProperties.SampleBias(propertySampler, float3(uvY, layer), bias).r * w.y +
           materialProperties.SampleBias(propertySampler, float3(uvZ, layer), bias).r * w.z;
    return r;
}

// Two scales: the broad one carries the mass from afar, the near one the
// grain at the foot; the near fades out before its repeat can show.
Rock stone(float3 place, float3 n0, float layer, float distance, float bias) {
    const Rock broad = triplanar(place, n0, layer, rockTile * 9.0, bias);
    const Rock near = triplanar(place, n0, layer, rockTile * 1.5, bias);
    const float t = saturate(1.0 - distance / 900.0);
    Rock r;
    r.colour = lerp(broad.colour, near.colour * 0.5 + broad.colour * 0.5, t);
    r.normal = normalize(lerp(broad.normal, near.normal, t * 0.6));
    r.ao = lerp(broad.ao, near.ao, t);
    return r;
}

// A biome on a cliff: its rock on the walls, its ground on what faces up
// (the plateau's grass to the brink of its cliffs; on the ledges down the
// wall only in patches), in its tint. `height`: how far up the wall, 0..1.
Rock cliffStone(uint id, float3 place, float3 n0, float height, float distance, float bias) {
    const float4 s = biomeStone[id];
    Rock r = stone(place, n0, s.x, distance, bias);
    float up = smoothstep(s.w, s.z, n0.z);
    if (up > 0) {
        const float patches = materials.SampleBias(materialSampler, float3(place.xy / (rockTile * 12.0), s.y), bias + 3).g;
        up *= saturate((patches - 0.4) * 3.0 + saturate((height - 0.9) * 10.0));
    }
    if (up > 0) {
        const float2 uv = place.xy / rockTile;
        const float3 near = materials.SampleBias(materialSampler, float3(uv, s.y), bias).rgb;
        const float3 far = materials.SampleBias(materialSampler, float3(uv / 7.0, s.y), bias).rgb;
        r.colour = lerp(r.colour, lerp(near, far, saturate(distance / 1500.0)), up);
        r.normal = normalize(lerp(r.normal, n0, up));
    }
    r.colour *= biomeTint[id].rgb;
    return r;
}

float4 propsPs(VsOut input) : SV_Target0 {
    const float3 n0 = normalize(input.normal);
    const float bias = log2(rockTextureScale > 0 ? rockTextureScale : 1.0);
    float layer = floor(input.layer.x);
    float moss = frac(input.layer.x) * 2.0;
    const float distance = length(input.view);
    float3 colour, n;
    float ao;
    if (input.biomes.x >= 0) {
        // A cliff: the biome on top of it down its wall, the one at its foot
        // low down, the line between them ragged.
        const uint a = uint(input.biomes.x), b = uint(input.biomes.y);
        float wb = 0;
        if (a != b) {
            const float patches = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 20.0) + input.place.z / (rockTile * 30.0), biomeStone[a].x), bias + 2).g;
            wb = smoothstep(0.3, 0.7, input.blend + (patches - 0.5) * 0.6);
        }
        Rock r = (Rock)0;
        if (wb < 0.99) r = cliffStone(a, input.place, n0, input.uv.y, distance, bias);
        if (wb > 0.01) {
            const Rock rb = cliffStone(b, input.place, n0, input.uv.y, distance, bias);
            if (wb >= 0.99) r = rb;
            else {
                r.colour = lerp(r.colour, rb.colour, wb);
                r.normal = normalize(lerp(r.normal, rb.normal, wb));
                r.ao = lerp(r.ao, rb.ao, wb);
            }
        }
        colour = r.colour;
        n = r.normal;
        ao = r.ao;
        layer = biomeStone[wb < 0.5 ? a : b].x;
        moss = lerp(biomeTint[a].w, biomeTint[b].w, wb);
    } else {
        const Rock r = stone(input.place, n0, layer, distance, bias);
        colour = r.colour;
        n = r.normal;
        ao = r.ao;
    }

    // The painted look: broad bands of colour up the stone, as layered rock
    // reads in a painting, and a lighter weathered crust on what faces up.
    const float z = input.place.z;
    const float band = sin(z * 0.021 + input.layer.y * 6.0 + sin(z * 0.0047) * 2.0);
    colour *= 0.9 + 0.12 * band;
    colour = lerp(colour, colour * float3(1.12, 1.06, 0.98), saturate((n0.z - 0.6) * 2.5) * 0.6);
    // Moss on what faces up and in the hollows, in patches, more low down
    // where the wet stays (doc/BIOMES_RU.md, 01: by wetness and facing).
    if (moss > 0) {
        const float patches = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 20.0), layer), bias + 2).g;
        const float up = saturate((n0.z + 0.15) * 1.6);
        const float wet = lerp(1.0, 0.55, saturate(input.uv.y * 1.4)) + (1.0 - input.uv.x) * 0.6;
        const float m = smoothstep(0.35, 0.65, moss * up * wet + (patches - 0.45) * 0.8);
        if (m > 0) {
            const float3 green = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 1.5), rockMossLayer), bias).rgb;
            colour = lerp(colour, green * 0.85, m);
        }
    }
    // Steppe on the tops: the islands and the spires' flat crowns carry the
    // dry grass of the steppe round them, not bare rock, fading at the brink.
    if (input.cover > 0) {
        const float top = smoothstep(0.62, 0.86, n0.z) * input.cover;
        if (top > 0) {
            const float2 uv = input.place.xy / (rockTile * 1.0);
            const float3 grass = materials.SampleBias(materialSampler, float3(uv, rockCoverLayer), bias).rgb;
            const float3 grassFar = materials.SampleBias(materialSampler, float3(uv / 7.0, rockCoverLayer), bias).rgb;
            colour = lerp(colour, lerp(grass, grassFar, saturate(distance / 1500.0)), top);
        }
    }
    // Cavity from the model, darker at the foot where it meets the ground.
    const float foot = lerp(0.65, 1.0, saturate(input.uv.y * 12.0));
    const float occlusion = input.uv.x * foot * lerp(0.7, 1.0, ao);
    const float sunLight = sunShadow(input.view, n0) * cloudShadow(input.view);
    float3 lit = shadeSurface(colour, n, input.view, sunLight, occlusion, 0.0) * lerp(0.8, 1.0, input.uv.x);
    // A mirror (the desert's prism): the sky and the sun reflected off a
    // smooth face, a little of the stone's own colour left in it, and the
    // desert below it caught in the lower half of the reflection.
    if (input.mirror > 0) {
        const float3 v = normalize(input.view);
        const float3 r = reflect(v, n0);
        float3 seen = skyColour(r) + sunDisc(r) * sunLight;
        // The clouds in it: where the reflected ray meets the cloud layer.
        if (r.z > 0.02 && lightCloudCover > 0) {
            const float3 world = float3(lightEyeXY + input.view.xy, lightEyeZ + input.view.z);
            const float mid = (lightCloudBase + lightCloudTop) * 0.5;
            const float2 at = world.xy + r.xy / r.z * (mid - world.z);
            const float c = cloudCoverage(at, lightCloudCover, lightCloudDrift);
            seen = lerp(seen, lightSunColour * 0.45 + lightZenith * 0.9, saturate(c * 1.2) * 0.85);
        }
        const float3 sand = float3(0.78, 0.62, 0.38) * (lightSunColour * 0.6 + lightZenith * 0.5);
        seen = lerp(seen, sand, saturate(-r.z * 3.0));
        const float fresnel = 0.55 + 0.45 * pow(1.0 - saturate(dot(-v, n0)), 4.0);
        lit = lerp(lit, seen, input.mirror * fresnel);
    }
    // A megalith keeps its silhouette through the haze: hazed as if a third
    // as far, with a quarter of the full haze laid over that.
    if (input.hero > 0.5) {
        const float3 near = applyFog(lit, input.view * 0.35);
        const float3 far = applyFog(lit, input.view);
        return float4(lerp(near, far, 0.25), 1);
    }
    return float4(applyFog(lit, input.view), 1);
}

void propsShadowPs(VsOut input) {}
