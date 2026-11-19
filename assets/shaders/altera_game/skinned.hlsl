// The game's people and beasts (src/game_view/skinned.cpp): skinned meshes,
// four joints a vertex, the joints' matrices for this frame in a storage buffer
// (three rows each, armature space of the bake: z up, facing -y). One indexed
// draw per figure; its instance row places it (eye-relative metres, yaw facing
// +x as the sim's headings do) and tints skin, hair and cloth.

#include "game_light.hlsli"

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;
};

StructuredBuffer<float4> palette : register(t0, space0);

Texture2D<float4> atlas : register(t0, space2);
SamplerState atlasSampler : register(s0, space2);

struct VsIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    uint4 joints : TEXCOORD3;
    float4 weights : TEXCOORD4;
    uint klass : TEXCOORD5;
    float4 placeYaw : TEXCOORD6;   // eye-relative origin, yaw
    float4 scale : TEXCOORD7;      // xyz scale, w highlight
    float4 skin : TEXCOORD8;       // rgb tint, w first palette row (joint index * 3)
    float4 hair : TEXCOORD9;       // rgb tint, w alpha (1 drawn)
    float4 cloth : TEXCOORD10;     // rgb tint
};

struct VsOut {
    float4 position : SV_Position;
    float3 rel : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 tint : TEXCOORD3;
    float highlight : TEXCOORD4;
};

float3x4 jointMatrix(uint base, uint j) {
    const uint r = base + j * 3;
    return float3x4(palette[r], palette[r + 1], palette[r + 2]);
}

VsOut skinnedVs(VsIn v) {
    const uint base = (uint)v.skin.w;
    float3x4 m = jointMatrix(base, v.joints.x) * v.weights.x;
    m += jointMatrix(base, v.joints.y) * v.weights.y;
    m += jointMatrix(base, v.joints.z) * v.weights.z;
    m += jointMatrix(base, v.joints.w) * v.weights.w;
    const float3 p = mul(m, float4(v.position, 1)) * v.scale.xyz;
    const float3 n0 = mul((float3x3)m, v.normal);
    // Bake space faces -y; the sim's yaw 0 faces +x.
    const float yaw = v.placeYaw.w + 1.5707963;
    const float c = cos(yaw), s = sin(yaw);
    const float3 rel = v.placeYaw.xyz + float3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
    VsOut o;
    o.position = mul(viewProjection, float4(rel, 1));
    o.rel = rel;
    o.normal = float3(n0.x * c - n0.y * s, n0.x * s + n0.y * c, n0.z);
    o.uv = v.uv;
    o.tint = v.klass == 1 ? v.skin.rgb : v.klass == 2 ? v.hair.rgb : v.klass == 3 ? v.cloth.rgb : float3(1, 1, 1);
    o.highlight = v.scale.w;
    return o;
}

float4 skinnedPs(VsOut i) : SV_Target0 {
    const float4 tex = atlas.Sample(atlasSampler, i.uv);
    clip(tex.a - 0.5);
    const float3 albedo = tex.rgb * i.tint;   // as the foliage cards: texels are albedo as they are
    return float4(gameShade(albedo, normalize(i.normal), i.rel, i.highlight), 1);
}
