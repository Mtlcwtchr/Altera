// The village's buildings and works (src/game_view/statics.cpp): static meshes
// baked from the kit (tools/game_assets/bake_buildings.py), one texture array
// of repeating 128 px materials, instanced per piece. Positions eye-relative.

#include "game_light.hlsli"

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;
};

Texture2DArray<float4> materials : register(t0, space2);
SamplerState materialSampler : register(s0, space2);

struct VsIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint layer : TEXCOORD3;
    float4 placeYaw : TEXCOORD4;   // eye-relative origin, yaw
    float4 scale : TEXCOORD5;      // xyz scale, w highlight
    float4 tint : TEXCOORD6;       // rgb multiplier
};

struct VsOut {
    float4 position : SV_Position;
    float3 rel : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 uvl : TEXCOORD2;
    float3 tint : TEXCOORD3;
    float highlight : TEXCOORD4;
};

VsOut staticsVs(VsIn v) {
    const float c = cos(v.placeYaw.w), s = sin(v.placeYaw.w);
    const float3 p = v.position * v.scale.xyz;
    const float3 rel = v.placeYaw.xyz + float3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
    VsOut o;
    o.position = mul(viewProjection, float4(rel, 1));
    o.rel = rel;
    o.normal = float3(v.normal.x * c - v.normal.y * s, v.normal.x * s + v.normal.y * c, v.normal.z);
    o.uvl = float3(v.uv, v.layer);
    o.tint = v.tint.rgb;
    o.highlight = v.scale.w;
    return o;
}

float4 staticsPs(VsOut i) : SV_Target0 {
    const float4 texel = materials.Sample(materialSampler, i.uvl);
    clip(texel.a - 0.5);   // leaves and grass cards cut out (opaque layers keep alpha 1)
    const float3 albedo = texel.rgb * i.tint;
    return float4(gameShade(albedo, normalize(i.normal), i.rel, i.highlight), 1);
}
