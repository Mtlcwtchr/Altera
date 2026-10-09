// Foliage instances from foliage_spawn.hlsl: one model mesh per indirect
// draw, the instance data at instance rate. Alpha-tested; lit like the ground
// and hazed the same, so it sinks into the distance with the ground.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative
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
};

VsOut foliageVs(VertexIn input) {
    const float c = cos(input.positionYaw.w), s = sin(input.positionYaw.w);
    // Grows out of the ground over the last stretch of its range rather than
    // appearing whole.
    const float scale = input.extra.x * lerp(0.2, 1.0, saturate(input.extra.z * 4.0));
    const float3 local = input.position * scale;
    const float3 turned = float3(local.x * c - local.y * s, local.x * s + local.y * c, local.z);
    VsOut o;
    o.view = input.positionYaw.xyz + turned;
    o.position = mul(viewProjection, float4(o.view, 1));
    o.uv = input.uv;
    o.layer = input.extra.y;
    o.normal = float3(input.normal.x * c - input.normal.y * s, input.normal.x * s + input.normal.y * c, input.normal.z);
    return o;
}

Texture2DArray<float4> foliageTextures : register(t0, space2);
SamplerState foliageSampler : register(s0, space2);

cbuffer Look : register(b0, space3) {
    float3 sunDirection;
    float hazeDistance;
    float3 skyColour;
    uint debugLevels;
    uint lookClipSize;
    uint lookLevels;
    uint lookFlags;      // 1: plain ground (the materials module off)
    float lookPad;
};

float4 foliagePs(VsOut input, bool front : SV_IsFrontFace) : SV_Target0 {
    const float4 albedo = foliageTextures.Sample(foliageSampler, float3(input.uv, input.layer));
    clip(albedo.a - 0.5);
    // Cards have no real normal: lean every one towards the sky, so a crown is
    // lit as a crown and not as a set of planes that flash as they turn.
    float3 n = normalize(front ? input.normal : -input.normal);
    n = normalize(lerp(n, float3(0, 0, 1), 0.55));
    const float sun = saturate(dot(n, sunDirection)) * 0.75 + 0.25;
    const float3 lit = albedo.rgb * (sun * float3(1.0, 0.95, 0.85) * 1.05 + 0.35 * skyColour);
    const float haze = 1.0 - exp(-length(input.view) / hazeDistance);
    return float4(lerp(lit, skyColour, haze), 1);
}
