// Volumetric clouds: a ray march through the cloud layer, at half resolution,
// into a texture the final pass lays over the frame (rgb: light the clouds
// send to the eye, a: how much of what is behind still shows).
//
// Each pixel's ray is cut by the scene's depth, so a mountain stands in front
// of a cloud behind it; through the layer the density is cloudDensity
// (clouds.hlsli), lit by a short march towards the sun (the cloud shading
// itself) with a forward-scattering phase (the silver edge against the sun)
// and the sky's light, and fogged with distance like everything else.

Texture2D<float> sceneDepth : register(t0, space0);   // read-only storage: the frame's depth, reversed
RWTexture2D<float4> cloudsOut : register(u0, space1);

cbuffer CloudCamera : register(b0, space2) {
    float4 cameraForward;
    float4 cameraRight;   // scaled by tan(fov/2) * aspect
    float4 cameraUp;      // scaled by tan(fov/2)
    float4 frameInfo;     // near plane, full width, full height, frame number
};

#define LIGHT_UNIFORMS register(b1, space2)
#define SHADOW_TEXTURE register(t5, space0)
#define SHADOW_SAMPLER register(s5, space0)
#include "lighting.hlsli"

float noiseAt(uint2 p, float frame) {
    return frac(52.9829189 * frac(dot(float2(p) + frame * float2(5.588238, 4.371), float2(0.06711056, 0.00583715))));
}

float phase(float cosine, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (12.566 * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-3), 1.5));
}

[numthreads(8, 8, 1)]
void cloudsCs(uint3 id : SV_DispatchThreadID) {
    const uint2 size = uint2(frameInfo.yz) / 2;
    if (id.x >= size.x || id.y >= size.y) return;
    const uint2 full = id.xy * 2;
    const float2 ndc = float2((float(full.x) + 1.0) / frameInfo.y * 2.0 - 1.0, 1.0 - (float(full.y) + 1.0) / frameInfo.z * 2.0);
    const float3 dir = normalize(cameraForward.xyz + ndc.x * cameraRight.xyz + ndc.y * cameraUp.xyz);

    // How far the scene is along this ray (reversed depth: near / distance).
    const float depth = sceneDepth.Load(int3(full, 0));
    const float sceneT = depth > 0 ? frameInfo.x / depth / max(dot(dir, cameraForward.xyz), 1e-3) : 1e9;

    // The ray's stretch inside the layer.
    const float z = lightEyeZ, base = lightCloudBase, top = lightCloudTop;
    float t0, t1;
    if (abs(dir.z) < 1e-4) { t0 = (z > base && z < top) ? 0 : 1e9; t1 = 60000; }
    else {
        const float ta = (base - z) / dir.z, tb = (top - z) / dir.z;
        t0 = max(min(ta, tb), 0.0);
        t1 = max(ta, tb);
    }
    t1 = min(min(t1, sceneT), t0 + 45000.0);
    if (lightCloudCover <= 0 || t1 <= t0) { cloudsOut[id.xy] = float4(0, 0, 0, 1); return; }

    const int steps = 36;
    const float stepLength = (t1 - t0) / float(steps);
    const float sigma = 0.0032;   // extinction per metre at density 1
    float t = t0 + stepLength * noiseAt(id.xy, frameInfo.w);
    float transmittance = 1.0;
    float3 light = 0;
    float firstHit = -1;
    const float cosine = dot(dir, lightSun);
    const float ph = lerp(phase(cosine, 0.65), phase(cosine, -0.15), 0.35);
    const float3 ambient = lerp(lightHorizon, lightZenith, 0.55);
    [loop] for (int i = 0; i < steps && transmittance > 0.02; ++i, t += stepLength) {
        const float3 world = float3(lightEyeXY + dir.xy * t, z + dir.z * t);
        const float level = (world.z - base) / (top - base);
        const float density = cloudDensity(world, level, lightCloudCover, lightCloudDrift, 3);
        if (density <= 0.01) continue;
        if (firstHit < 0) firstHit = t;
        // Towards the sun: how much of it reaches this point through the cloud.
        float towards = 0;
        [unroll] for (int k = 1; k <= 5; ++k) {
            const float3 q = world + lightSun * (180.0 * k);
            towards += cloudDensity(q, (q.z - base) / (top - base), lightCloudCover, lightCloudDrift, 2);
        }
        // Light that has come through the cloud towards the sun is scattered
        // forward mostly: it dims slower than the eye's own path would.
        const float sunReach = exp(-towards * sigma * 180.0 * 0.45) * (1.0 - exp(-density * 2.0) * 0.5);   // powder
        const float3 scattered = lightSunColour * sunReach * (ph * 18.0 + 0.55) + ambient * (0.75 + 0.45 * level);
        const float extinction = density * sigma;
        const float through = exp(-extinction * stepLength);
        light += transmittance * scattered * (1.0 - through);
        transmittance *= through;
    }
    if (firstHit >= 0) {
        // Far clouds sink into the haze like the far ground.
        const float far = 1.0 - exp(-firstHit / 70000.0);
        light = lerp(light, skyColour(float3(dir.xy, max(dir.z, 0.05))) * (1.0 - transmittance), far);
    }
    cloudsOut[id.xy] = float4(light, transmittance);
}
