// Grass tufts from grass_spawn.hlsl: two crossed cards each, made from the
// vertex index - no mesh. The sprite is white and shaded; the colour is the
// ground's, lit as the ground is, so a tuft's foot is the ground under it and
// its tip a little lighter.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative
    float4 wind;                         // direction x, y; strength; seconds
    float4 eyeWrapped;                   // the eye's x, y modulo 4096 m
};

struct TuftIn {
    uint vertex : SV_VertexID;
    float4 positionYaw : TEXCOORD0;
    float4 shape : TEXCOORD1;            // width, height, sprite, packed colour
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float sprite : TEXCOORD1;
    nointerpolation float4 colour : TEXCOORD2;   // rgb, a: the clouds' shadow here
    float3 view : TEXCOORD3;
    nointerpolation float3 normal : TEXCOORD4;
};

VsOut grassVs(TuftIn input) {
    // Two quads, six vertices each: (0,0) (1,0) (1,1) (0,0) (1,1) (0,1).
    const uint corner = input.vertex % 6u;
    const uint card = input.vertex / 6u;
    const float2 c = float2(corner == 1 || corner == 2 || corner == 4 ? 1.0 : 0.0,
                            corner == 2 || corner == 4 || corner == 5 ? 1.0 : 0.0);
    const float yaw = input.positionYaw.w + float(card) * 1.5707963;
    const float2 across = float2(cos(yaw), sin(yaw)) * (c.x - 0.5) * input.shape.x;
    // Seen from above a card is an edge - a dark scratch on the ground. The
    // steeper the eye looks down at a tuft, the lower it lies, until from
    // overhead it is gone and the ground under it, the same colour, is all
    // there is.
    const float down = saturate(-normalize(input.positionYaw.xyz).z);
    const float height = input.shape.y * (1.0 - smoothstep(0.45, 0.8, down));
    // Wind: the tips swing with gusts rolling across the field, along the wind.
    const float2 place = input.positionYaw.xy + eyeWrapped.xy;
    const float wave = sin(dot(place, wind.xy) * 0.32 - wind.w * 3.1) * 0.5 + 0.5;
    const float ripple = sin(dot(place, float2(-wind.y, wind.x)) * 0.9 + wind.w * 4.7 + input.positionYaw.w * 7.0);
    const float swing = wind.z * (0.25 + 0.75 * wave) + ripple * 0.12 * wind.z;
    const float2 tip = wind.xy * swing * height * 0.75 * c.y * c.y;
    VsOut o;
    o.view = input.positionYaw.xyz + float3(across + tip, c.y * height * (1.0 - 0.25 * swing * swing));
    o.position = mul(viewProjection, float4(o.view, 1));
    o.uv = float2(c.x, 1.0 - c.y);
    const uint bits = asuint(input.shape.z);
    o.sprite = float(bits & 255u);
    const float2 nxy = float2((bits >> 8) & 255u, (bits >> 16) & 255u) / 255.0 * 2.0 - 1.0;
    o.normal = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
    const uint packed = asuint(input.shape.w);
    o.colour = float4(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u, packed >> 24) / 255.0;
    return o;
}

Texture2DArray<float4> sprites : register(t0, space2);
SamplerState spriteSampler : register(s0, space2);

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t1, space2)
#define SHADOW_SAMPLER register(s1, space2)
#include "lighting.hlsli"


float4 grassPs(VsOut input) : SV_Target0 {
    const float4 s = sprites.Sample(spriteSampler, float3(input.uv, input.sprite));
    clip(s.a - 0.45);
    // Foot darker, tip lighter - but on average exactly the ground's own
    // brightness (the 1.12), and flat with distance, so where the tufts end
    // the ground goes on in the same colour and there is no edge to find.
    const float up = 1.0 - input.uv.y;
    const float shade = lerp(0.72, 1.12, up) * lerp(0.85, 1.05, s.r) * 1.12;
    const float3 albedo = input.colour.rgb * lerp(shade, 1.0, saturate(length(input.view) / 70.0));
    // Lit with the ground's own normal - the ground under it gets the same
    // light - and a little sun through the blades from behind, fading with
    // distance so the far grass is the ground's colour exactly.
    const float sunLight = sunShadow(input.view, input.normal) * input.colour.a;
    const float through = 0.3 * (1.0 - saturate(length(input.view) / 70.0)) * up;
    const float3 lit = shadeSurface(albedo, input.normal, input.view, sunLight, 1.0, through);
    return float4(applyFog(lit, input.view), 1);
}
