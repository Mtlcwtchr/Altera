// The ENB-like frame chain (terrain_core::Enb, src/terrain_core/enb.hpp): what
// its passes share. One uniform block for every pass (b0 of the fragment
// stage), filled once a frame on the CPU; `res` and `mode` change per pass.
//
// The scene arrives in a half-float target: the same values the scene shaders
// always wrote (display-coded, roughly 0..1), but no longer clipped at 1, so a
// sun disc, a sun glint on water or the sky by the sun keep their strength
// for the bloom and the tone curve. With `linearize` the chain works in linear
// light (value^2.2) and writes the display code back at the end.
#ifndef TERRAIN_CORE_ENB
#define TERRAIN_CORE_ENB

// Must match terrain_core::EnbUniforms.
cbuffer Enb : register(b0, space3) {
    float4 camF;        // xyz forward; w near plane (distance = near / depth)
    float4 camR;        // xyz right * tan(fov/2) * aspect; w seconds
    float4 camU;        // xyz up * tan(fov/2); w frame seconds (dt)
    float4 res;         // this pass's target: w, h, 1/w, 1/h
    float4 src;         // the scene: w, h, 1/w, 1/h
    float4 sun;         // xyz towards the sun; w 0 under the horizon .. 1 above
    float4 sunColour;   // rgb; w the pass's mode
    float4 zenith;      // rgb; w fog density per metre at sea level
    float4 horizon;     // rgb; w fog falloff per metre of height
    float4 sunUv;       // xy the sun on the screen; z how much of it to use; w the eye's height
    float4 aoP;         // strength, radius m, distance m, power
    float4 contactP;    // strength, length m, distance m, darkness
    float4 atmosP;      // sun scatter, phase g, aerial strength, aerial distance m
    float4 aerialTint;  // rgb; w sun halo in the sky
    float4 bloomP;      // strength, threshold, knee, radius (how much of the coarser level)
    float4 raysP;       // strength, decay, density; w the point lights' glow in the air
    float4 expoP;       // exposure, adaptation 0..1, min, max
    float4 expo2P;      // key, speed (1/s), reset (1: no history), linearize
    float4 toneP;       // tonemap (0 clamp, 1 ACES, 2 AgX), contrast, saturation, vibrance
    float4 liftP;       // rgb lift; w pivot of the contrast
    float4 gammaP;      // rgb gamma; w white point (ACES input scale)
    float4 gainP;       // rgb gain; w fog extra density
    float4 shadowTint;  // rgb; w split-tone strength
    float4 highTint;    // rgb; w balance (-1 .. 1: more of the picture counts as highlight)
    float4 lensP;       // vignette, reach, grain, grain size (pixels)
    float4 flagsP;      // clouds on, grain on, vignette colour (0 black .. 1 umber), point lights in use
    float4 lightPos[8]; // warm point lights: xyz eye-relative, w radius
    float4 lightCol[8]; // rgb, w flicker phase
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut enbVs(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float enbLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float enbHash(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}

// Interleaved gradient noise: a rotation per pixel that a small blur removes.
float enbIgn(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

// The view ray through `uv` scaled to depth 1 along the forward axis.
float3 enbRay(float2 uv) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return camF.xyz + ndc.x * camR.xyz + ndc.y * camU.xyz;
}

// Eye-relative metres of what is at `uv` with stored (reversed, infinite) depth `d`.
float enbDistance(float d) { return d > 0 ? camF.w / d : 1e7; }
float3 enbPosition(float2 uv, float d) { return enbRay(uv) * enbDistance(d); }

// Where an eye-relative point lands on the screen; z its forward distance.
float3 enbProject(float3 p) {
    const float z = dot(p, camF.xyz);
    const float x = dot(p, camR.xyz) / (dot(camR.xyz, camR.xyz) * z);
    const float y = dot(p, camU.xyz) / (dot(camU.xyz, camU.xyz) * z);
    return float3(x * 0.5 + 0.5, 0.5 - y * 0.5, z);
}

// The sky's colour the way lighting.hlsli has it (a copy, so this chain does
// not need the scene's light block bound).
float3 enbSky(float3 dir) {
    const float up = saturate(dir.z);
    float3 sky = lerp(horizon.rgb, zenith.rgb, pow(up, 0.55));
    sky = lerp(sky, horizon.rgb * 0.9, saturate(-dir.z * 4.0));
    return sky;
}

// The same height fog integral as lighting.hlsli's applyFog: how much of the
// way to `rel` is fog, for a density `scale` times the scene's.
float enbFog(float3 rel, float scale) {
    const float distance = length(rel);
    const float k = horizon.w;
    const float atEye = zenith.w * scale * exp(-k * max(sunUv.w, 0.0));
    const float rise = rel.z * k;
    const float through = abs(rise) > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0;
    return saturate(1.0 - exp(-atEye * distance * through));
}

// Henyey-Greenstein: how much light a haze throws forward along the sun.
float enbPhase(float cosine, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5));
}

#endif
