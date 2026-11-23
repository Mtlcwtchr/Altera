// ENB chain, pass 2 (scene resolution, half float): everything that changes
// the scene's own light before the lens.
//   - the clouds laid over the frame (as post.hlsl did)
//   - ambient occlusion and contact shadows from enb_ao.hlsl, blurred by depth
//   - atmosphere: more height fog where wanted, the sun's light thrown
//     forward by the haze (Mie), a cool aerial perspective on the far land,
//     a halo round the sun in the sky
//   - warm point lights (fires): lit by the depth's normals, a glow in the air
// Out: the working space of the rest of the chain - linear light with
// `linearize`, the display code as it came otherwise.

#include "enb.hlsli"

Texture2D<float4> scene : register(t0, space2);
SamplerState sceneSampler : register(s0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
SamplerState depthSampler : register(s1, space2);
Texture2D<float4> clouds : register(t2, space2);
SamplerState cloudSampler : register(s2, space2);
Texture2D<float4> occlusion : register(t3, space2);
SamplerState occlusionSampler : register(s3, space2);

// The half-resolution AO round `uv`, each tap weighted by how near its
// distance is to this pixel's: the noise goes, the edges stay.
float2 occlusionAt(float2 uv, float z) {
    const float2 texel = 1.0 / max(res.xy * 0.5, 1.0);
    float2 sum = 0;
    float weight = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float4 s = occlusion.SampleLevel(occlusionSampler, uv + float2(x, y) * texel, 0);
            const float w = exp(-abs(s.b - z) / (0.03 * z + 0.05)) * (x == 0 && y == 0 ? 2.0 : 1.0);
            sum += s.rg * w;
            weight += w;
        }
    return weight > 1e-4 ? sum / weight : float2(1, 1);
}

float3 positionAt(float2 uv) { return enbPosition(uv, sceneDepth.SampleLevel(depthSampler, uv, 0)); }

// The surface normal from the depth (the nearer neighbour each way).
float3 normalAt(float2 uv, float3 p) {
    const float2 t = src.zw;
    const float3 px0 = positionAt(uv - float2(t.x, 0)), px1 = positionAt(uv + float2(t.x, 0));
    const float3 py0 = positionAt(uv - float2(0, t.y)), py1 = positionAt(uv + float2(0, t.y));
    const float3 dx = abs(dot(px1 - p, camF.xyz)) < abs(dot(p - px0, camF.xyz)) ? px1 - p : p - px0;
    const float3 dy = abs(dot(py1 - p, camF.xyz)) < abs(dot(p - py0, camF.xyz)) ? py1 - p : p - py0;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n;
}

// Fires: what they light (a surface of the picture's own hue, at a middle
// value - the frame is dark where they matter) and their glow in the air
// between the eye and whatever stands behind them.
float3 pointLights(float3 c, float2 uv, float3 p, float z, float3 dir, bool sky) {
    const int count = int(flagsP.w + 0.5);
    if (count <= 0) return c;
    const float l = enbLuma(c);
    const float3 albedo = saturate(c / max(l, 0.03) * 0.30);
    const float3 n = sky ? float3(0, 0, 1) : normalAt(uv, p);
    float3 add = 0;
    [loop] for (int i = 0; i < count; ++i) {
        const float3 at = lightPos[i].xyz;
        const float radius = lightPos[i].w;
        const float phase = lightCol[i].w;
        const float flicker = 0.85 + 0.1 * sin(camR.w * 9.1 + phase) + 0.05 * sin(camR.w * 23.7 + phase * 1.7);
        const float3 colour = lightCol[i].rgb * flicker;
        if (!sky) {
            const float3 v = at - p;
            const float d = length(v);
            if (d < radius) {
                const float fall = saturate(1.0 - (d * d) / (radius * radius));
                const float atten = fall * fall / (1.0 + d * d * 0.08);
                const float facing = saturate(dot(n, v / max(d, 1e-3)) * 0.8 + 0.2);
                add += albedo * colour * atten * facing;
            }
        }
        // The glow: how near the view ray passes the light, if the light is
        // in front of what this pixel shows.
        const float along = dot(at, dir);
        if (along > 0 && along < (sky ? 1e7 : length(p)) + 0.5) {
            const float miss = length(at - dir * along);
            const float glow = raysP.w * exp(-miss * miss * 1.6) / (1.0 + along * 0.02);
            add += colour * glow * 0.35;
        }
    }
    return c + add;
}

float4 compositePs(VsOut input) : SV_Target0 {
    const float2 uv = input.uv;
    float3 c = scene.SampleLevel(sceneSampler, uv, 0).rgb;
    const float d = sceneDepth.SampleLevel(depthSampler, uv, 0);
    const float z = enbDistance(d);
    const float3 ray = enbRay(uv);
    const float3 dir = normalize(ray);
    const bool sky = d <= 0;

    if (!sky) {
        // Occlusion darkens what the sky and the bounce light, a little less
        // what the sun lights head-on (it is already bright and directional).
        const float2 o = occlusionAt(uv, z);
        const float ao = lerp(1.0, o.x, 0.85 + 0.15 * saturate(1.0 - enbLuma(c)));
        c *= ao;
        c *= lerp(1.0, contactP.w, (1.0 - o.y) * contactP.x);
    }

    const float cosSun = dot(dir, sun.xyz);
    if (!sky) {
        const float3 rel = ray * z;
        // More of the scene's fog, towards the same sky it already fades to.
        if (gainP.w > 0) {
            const float extra = enbFog(rel, gainP.w);
            c = lerp(c, enbSky(float3(dir.xy, max(dir.z, 0.0) * 0.3)), extra);
        }
        // Aerial perspective: the far land cools and pales into the air.
        const float aerial = atmosP.z * (1.0 - exp(-z / max(atmosP.w, 1.0)));
        const float l = enbLuma(c);
        c = lerp(c, lerp(float3(l, l, l), c, 0.6) * 0.5 + aerialTint.rgb * enbSky(dir) * 0.6, saturate(aerial));
        // The sun's light thrown forward by the haze between here and the eye.
        const float haze = enbFog(rel, 1.0 + gainP.w);
        c += sunColour.rgb * sun.w * haze * atmosP.x * (enbPhase(cosSun, atmosP.y) + 0.08);
    } else {
        // The sky round the sun: a broad glow the haze makes of it.
        c += sunColour.rgb * sun.w * aerialTint.w * enbPhase(cosSun, atmosP.y) * 0.25;
    }

    c = pointLights(c, uv, ray * z, z, dir, sky);

    if (flagsP.x > 0.5) {
        const float4 cloud = clouds.SampleLevel(cloudSampler, uv, 0);
        c = c * cloud.a + cloud.rgb;
    }
    c = max(c, 0.0);
    if (expo2P.w > 0.5) c = pow(c, 2.2);
    return float4(c, 1);
}
