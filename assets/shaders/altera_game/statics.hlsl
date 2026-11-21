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
    float4 fade : TEXCOORD7;       // AL-56 cutaway: vis, keep (m over the bottom), the piece's bottom
};

struct VsOut {
    float4 position : SV_Position;
    float3 rel : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 uvl : TEXCOORD2;
    float3 tint : TEXCOORD3;
    float highlight : TEXCOORD4;
    float3 fade : TEXCOORD5;       // vis, keep, height over the piece's bottom
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
    o.fade = float3(v.fade.x, v.fade.y, p.z - v.fade.z * v.scale.z);
    return o;
}

// The cutaway (src/game_view/cutaway.hpp): no alpha, a 4 x 4 ordered dither
// over the screen drops a share 1 - vis of the pixels above the kept stub.
float bayer4(float2 pixel) {
    const uint2 q = uint2(pixel) & 3u;
    static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (kBayer[q.y * 4u + q.x] + 0.5) / 16.0;
}

float4 staticsPs(VsOut i) : SV_Target0 {
    if (i.fade.x < 1 && i.fade.z > i.fade.y && i.fade.x < bayer4(i.position.xy)) discard;
    const float4 texel = materials.Sample(materialSampler, i.uvl);
    clip(texel.a - 0.5);   // leaves and grass cards cut out (opaque layers keep alpha 1)
    const float3 albedo = texel.rgb * i.tint;
    return float4(gameShade(albedo, normalize(i.normal), i.rel, i.highlight), 1);
}
