// The sky behind everything: drawn first in the frame's pass, a full-screen
// triangle the ground then covers. Colour from lighting.hlsli (gradient, sun
// glow, sun disc), so the fog the ground fades into is this same sky.

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t0, space2)
#define SHADOW_SAMPLER register(s0, space2)
#include "lighting.hlsli"

cbuffer SkyCamera : register(b1, space3) {
    float4 cameraForward;
    float4 cameraRight;   // scaled by tan(fov/2) * aspect
    float4 cameraUp;      // scaled by tan(fov/2)
};

struct VsOut {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VsOut skyVs(uint id : SV_VertexID) {
    VsOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * float2(2, -2) + float2(-1, 1);
    o.position = float4(o.ndc, 0, 1);
    return o;
}

float4 skyPs(VsOut input) : SV_Target0 {
    const float3 dir = normalize(cameraForward.xyz + input.ndc.x * cameraRight.xyz + input.ndc.y * cameraUp.xyz);
    return float4(skyColour(dir) + sunDisc(dir), 1);
}
