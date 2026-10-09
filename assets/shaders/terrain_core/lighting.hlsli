// Light, shared by everything drawn in the world (ground, grass, trees, sky):
// the sun and the sky, height fog, cascaded sun shadows, the clouds' shadows.
// One function each, so a tuft of grass and the ground under it are lit by
// exactly the same arithmetic and cannot drift apart.
//
// The including shader says where its uniforms and shadow map are bound:
//   #define LIGHT_UNIFORMS register(bN, space3)
//   #define SHADOW_TEXTURE register(tN, space2)
//   #define SHADOW_SAMPLER register(sN, space2)
// or #define NO_SUN_SHADOW: no shadow map at all, sunShadow() is 1 (a
// compute pass that only wants the clouds' shadow; a declared texture it never
// binds would still take a slot on some back ends).
// Positions are eye-relative metres; the eye's own absolute position is here.
#ifndef TERRAIN_CORE_LIGHTING
#define TERRAIN_CORE_LIGHTING

#include "clouds.hlsli"

// Must match terrain_core::LightUniforms.
cbuffer Light : LIGHT_UNIFORMS {
    float3 lightSun;            // towards the sun
    float lightSunAbove;        // 0 with the sun under the horizon, 1 above
    float3 lightSunColour;      // what the sun adds to a face turned to it
    float lightTime;            // seconds, for wind and clouds
    float3 lightZenith;
    float lightFogDensity;      // per metre at sea level
    float3 lightHorizon;
    float lightFogFalloff;      // per metre of height
    float3 lightGround;         // light from below (bounce)
    float lightEyeZ;            // the eye's absolute height
    float2 lightEyeXY;          // the eye's absolute position (floats: for clouds and wind only)
    float lightCloudCover;
    float lightCloudBase;
    float lightCloudTop;
    float lightShadows;         // 1: sample the shadow map
    float lightCloudShadows;    // 1: the clouds shade the ground
    float lightWind;            // strength, 0..1
    float2 lightWindDirection;
    float2 lightCloudDrift;     // metres the clouds have moved
    row_major float4x4 shadowMatrix[3];   // eye-relative -> cascade clip (x, y in -1..1, z reversed depth)
    float4 cascadeFar;          // where cascades 0, 1, 2 end (m); w: shadow atlas texel (1/width)
};

#ifndef NO_SUN_SHADOW
Texture2D<float> shadowAtlas : SHADOW_TEXTURE;
SamplerState shadowSampler : SHADOW_SAMPLER;
#endif

float3 skyColour(float3 dir) {
    const float up = saturate(dir.z);
    float3 sky = lerp(lightHorizon, lightZenith, pow(up, 0.55));
    // Below the horizon, the haze of the distance.
    sky = lerp(sky, lightHorizon * 0.9, saturate(-dir.z * 4.0));
    const float toSun = saturate(dot(dir, lightSun));
    sky += lightSunColour * (pow(toSun, 8.0) * 0.18 + pow(toSun, 64.0) * 0.35) * lightSunAbove;
    return sky;
}

float3 sunDisc(float3 dir) {
    return lightSunColour * smoothstep(0.9995, 0.99975, dot(dir, lightSun)) * 8.0 * lightSunAbove;
}

// Height fog: denser low, thinner high, integrated along the ray.
float3 applyFog(float3 colour, float3 rel) {
    const float distance = length(rel);
    const float k = lightFogFalloff;
    const float atEye = lightFogDensity * exp(-k * max(lightEyeZ, 0.0));
    const float rise = rel.z * k;
    const float through = abs(rise) > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0;
    const float amount = 1.0 - exp(-atEye * distance * through);
    const float3 dir = rel / max(distance, 1e-3);
    return lerp(colour, skyColour(float3(dir.xy, max(dir.z, 0.0) * 0.3)), saturate(amount));
}

// How much of the sun gets to a point past the shadow map, 0..1.
float sunShadow(float3 rel, float3 normal) {
#ifdef NO_SUN_SHADOW
    return 1.0;
#else
    if (lightShadows < 0.5) return 1.0;
    const float distance = length(rel);
    const int cascade = distance < cascadeFar.x ? 0 : distance < cascadeFar.y ? 1 : distance < cascadeFar.z ? 2 : 3;
    if (cascade > 2) return 1.0;
    // Pushed off the surface a little, more where the sun grazes it.
    const float3 offset = normal * (0.04 + 0.12 * float(cascade)) * (1.5 - saturate(dot(normal, lightSun)));
    const float4 clip = mul(shadowMatrix[cascade], float4(rel + offset, 1));
    const float2 tile = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (any(tile < 0.002) || any(tile > 0.998)) return 1.0;
    const float2 uv = float2((tile.x + float(cascade)) / 3.0, tile.y);
    const float texel = cascadeFar.w;
    float lit = 0;
    [unroll] for (int k = 0; k < 5; ++k) {
        const float2 o = k == 0 ? float2(0, 0) : float2((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0) * 0.75;
        const float stored = shadowAtlas.SampleLevel(shadowSampler, uv + o * float2(texel, texel * 3.0), 0);
        // Reversed depth: what is nearer the sun is larger.
        lit += clip.z >= stored - 1e-5 ? 1.0 : 0.0;
    }
    lit /= 5.0;
    // Fade out towards the far end of the last cascade.
    return lerp(lit, 1.0, saturate((distance - cascadeFar.z * 0.85) / (cascadeFar.z * 0.15)));
#endif
}

float cloudShadow(float3 rel) {
    if (lightCloudShadows < 0.5 || lightCloudCover <= 0) return 1.0;
    const float3 world = float3(lightEyeXY + rel.xy, lightEyeZ + rel.z);
    const float mid = (lightCloudBase + lightCloudTop) * 0.5;
    const float2 under = world.xy + lightSun.xy / max(lightSun.z, 0.15) * (mid - world.z);
    return 1.0 - 0.72 * cloudCoverage(under, lightCloudCover, lightCloudDrift);
}

// A surface lit by the sun and the sky. `sunLight` is the shadowing already
// worked out (sun and cloud shadows); `ao` the surface's own occlusion;
// `translucent` how much sun comes through it from behind (leaves, grass).
float3 shadeSurface(float3 albedo, float3 normal, float3 rel, float sunLight, float ao, float translucent) {
    const float facing = dot(normal, lightSun);
    float3 light = lightSunColour * saturate(facing) * sunLight;
    // Through the leaf: sun behind it, seen against it.
    if (translucent > 0) {
        const float3 view = normalize(rel);
        const float back = pow(saturate(dot(view, lightSun)), 3.0);
        light += lightSunColour * translucent * (back * 1.4 + saturate(-facing) * 0.35) * sunLight;
    }
    const float3 sky = lerp(lightGround, lightZenith, normal.z * 0.5 + 0.5);
    light += sky * ao * 0.75;
    return albedo * light;
}

#endif
