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
};

struct VsOut {
    float4 position : SV_Position;
    float3 view : TEXCOORD0;          // eye-relative
    float3 normal : TEXCOORD1;
    float3 place : TEXCOORD2;         // texture space: wrapped world metres
    float2 uv : TEXCOORD3;
    nointerpolation float2 layer : TEXCOORD4;   // material layer, variation
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
    o.layer = float2(input.scale.w, input.anchor.w);
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
    float rockPad;
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

float4 propsPs(VsOut input) : SV_Target0 {
    const float3 n0 = normalize(input.normal);
    const float bias = log2(rockTextureScale > 0 ? rockTextureScale : 1.0);
    const float layer = floor(input.layer.x);
    const float moss = frac(input.layer.x) * 2.0;
    // Two scales: the broad one carries the mass from afar, the near one the
    // grain at the foot; the near fades out before its repeat can show.
    const float distance = length(input.view);
    const Rock broad = triplanar(input.place, n0, layer, rockTile * 9.0, bias);
    const Rock near = triplanar(input.place, n0, layer, rockTile * 1.5, bias);
    const float t = saturate(1.0 - distance / 900.0);
    float3 colour = lerp(broad.colour, near.colour * 0.5 + broad.colour * 0.5, t);
    const float3 n = normalize(lerp(broad.normal, near.normal, t * 0.6));
    const float ao = lerp(broad.ao, near.ao, t);

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
    // Cavity from the model, darker at the foot where it meets the ground.
    const float foot = lerp(0.65, 1.0, saturate(input.uv.y * 12.0));
    const float occlusion = input.uv.x * foot * lerp(0.7, 1.0, ao);
    const float sunLight = sunShadow(input.view, n0) * cloudShadow(input.view);
    const float3 lit = shadeSurface(colour, n, input.view, sunLight, occlusion, 0.0) * lerp(0.8, 1.0, input.uv.x);
    return float4(applyFog(lit, input.view), 1);
}

void propsShadowPs(VsOut input) {}
