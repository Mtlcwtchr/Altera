// Foliage instances from foliage_spawn.hlsl: one model mesh per indirect
// draw, the instance data at instance rate. Alpha-tested; lit like the ground
// and hazed the same, so it sinks into the distance with the ground.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative (the light's, in the shadow pass)
    float4 wind;                         // direction x, y; strength; seconds
    float4 eyeWrapped;                   // the eye's x, y modulo 4096 m: a place for the gusts
};

struct VertexIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 positionYaw : TEXCOORD3;   // instance: eye-relative base, yaw
    float4 extra : TEXCOORD4;         // instance: scale, texture layer, fade, model
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float layer : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float3 view : TEXCOORD3;
    float up : TEXCOORD4;             // 0 at the foot, 1 at the top
};

VsOut foliageVs(VertexIn input) {
    const float c = cos(input.positionYaw.w), s = sin(input.positionYaw.w);
    // Grows out of the ground over the last stretch of its range rather than
    // appearing whole.
    const float scale = input.extra.x * lerp(0.2, 1.0, saturate(input.extra.z * 4.0));
    const float3 local = input.position * scale;
    float3 turned = float3(local.x * c - local.y * s, local.x * s + local.y * c, local.z);
    // Wind: the whole plant leans with the gusts, more the higher up, and the
    // crown flutters. A gust is a wave rolling over the land along the wind.
    const float lever = max(local.z, 0.0);
    const float2 place = input.positionYaw.xy + eyeWrapped.xy;
    const float gust = 0.55 + 0.45 * sin(dot(place, wind.xy) * 0.018 - wind.w * 1.3 + input.positionYaw.w);
    const float bend = wind.z * gust * pow(lever / 10.0, 1.6) * 0.9;
    const float flutter = sin(wind.w * 6.3 + input.positionYaw.w * 13.0 + dot(local, float3(2.1, 1.7, 0.9))) * 0.06 * wind.z * lever / 6.0;
    turned.xy += wind.xy * (bend + flutter);
    turned.z -= bend * bend * 0.08;
    VsOut o;
    o.view = input.positionYaw.xyz + turned;
    o.up = saturate(local.z / max(input.extra.w, 0.5));
    o.position = mul(viewProjection, float4(o.view, 1));
    o.uv = input.uv;
    o.layer = input.extra.y;
    o.normal = float3(input.normal.x * c - input.normal.y * s, input.normal.x * s + input.normal.y * c, input.normal.z);
    return o;
}

Texture2DArray<float4> foliageTextures : register(t0, space2);
SamplerState foliageSampler : register(s0, space2);

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t1, space2)
#define SHADOW_SAMPLER register(s1, space2)
#include "lighting.hlsli"


float4 foliagePs(VsOut input, bool front : SV_IsFrontFace) : SV_Target0 {
    const float4 albedo = foliageTextures.Sample(foliageSampler, float3(input.uv, input.layer));
    clip(albedo.a - 0.5);
    // Cards have no real normal: lean every one towards the sky, so a crown is
    // lit as a crown and not as a set of planes that flash as they turn.
    float3 n = normalize(front ? input.normal : -input.normal);
    n = normalize(lerp(n, float3(0, 0, 1), 0.55));
    const float sunLight = sunShadow(input.view, float3(0, 0, 1)) * cloudShadow(input.view);
    // Deeper in the crown and lower down, darker; light comes through leaves.
    const float ao = lerp(0.45, 1.0, input.up);
    const float3 lit = shadeSurface(albedo.rgb, n, input.view, sunLight, ao, 0.55) * lerp(0.75, 1.0, input.up);
    return float4(applyFog(lit, input.view), 1);
}

// Depth only, into the shadow atlas: the cards' holes let the sun through.
void foliageShadowPs(VsOut input) {
    clip(foliageTextures.Sample(foliageSampler, float3(input.uv, input.layer)).a - 0.5);
}
