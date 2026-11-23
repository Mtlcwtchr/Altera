// ENB chain, pass 1 (half resolution): ambient occlusion and contact shadows
// from the scene's depth alone.
//   r  how open the point is (1 open .. 0 shut in)
//   g  1 lit, 0 a short screen-space ray towards the sun hits something
//      (the shadow map is too coarse for a foot on the ground, a stone in grass)
//   b  the distance, for the composite's depth-aware blur
// The AO is the cosine-weighted share of a ring of samples round the point
// that stand above its surface (scalable ambient obscurance, simplified).

#include "enb.hlsli"

Texture2D<float> sceneDepth : register(t0, space2);
SamplerState depthSampler : register(s0, space2);

float depthAt(float2 uv) { return sceneDepth.SampleLevel(depthSampler, uv, 0); }

float4 aoPs(VsOut input) : SV_Target0 {
    const float2 uv = input.uv;
    const float d = depthAt(uv);
    if (d <= 0) return float4(1, 1, 1e7, 1);   // the sky
    const float z = enbDistance(d);
    const float3 p = enbPosition(uv, d);

    // The surface's normal from the depth: of the two neighbours each way, the
    // nearer one, so an edge does not bend the normal of what is in front.
    const float2 t = src.zw;
    const float3 px0 = enbPosition(uv - float2(t.x, 0), depthAt(uv - float2(t.x, 0)));
    const float3 px1 = enbPosition(uv + float2(t.x, 0), depthAt(uv + float2(t.x, 0)));
    const float3 py0 = enbPosition(uv - float2(0, t.y), depthAt(uv - float2(0, t.y)));
    const float3 py1 = enbPosition(uv + float2(0, t.y), depthAt(uv + float2(0, t.y)));
    const float3 dx = abs(dot(px1 - p, camF.xyz)) < abs(dot(p - px0, camF.xyz)) ? px1 - p : p - px0;
    const float3 dy = abs(dot(py1 - p, camF.xyz)) < abs(dot(p - py0, camF.xyz)) ? py1 - p : p - py0;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0) n = -n;

    const float noise = enbIgn(input.position.xy);
    float ao = 1.0;
    const float aoFade = 1.0 - smoothstep(aoP.z * 0.6, aoP.z, z);
    if (aoP.x > 0 && aoFade > 0) {
        const float radius = aoP.y * (1.0 + z * 0.004);   // a little wider far away, where a pixel is metres
        // Metres at distance z -> uv: the screen is 2 tan(fov/2) z tall.
        const float uvRadius = clamp(radius / (2.0 * length(camU.xyz) * z), 3.0 * src.w, 0.12);
        const float aspect = src.y * src.z;   // h / w: the ring round in pixels
        const int kSamples = 12;
        float occluded = 0;
        [unroll] for (int i = 0; i < kSamples; ++i) {
            const float f = (float(i) + noise) / float(kSamples);
            const float angle = (float(i) * 2.39996323 + noise * 6.2831853);
            const float2 o = float2(cos(angle) * aspect, sin(angle)) * uvRadius * sqrt(f);
            const float2 s = uv + o;
            const float ds = depthAt(s);
            const float3 v = enbPosition(s, ds) - p;
            const float vv = dot(v, v);
            const float r2 = radius * radius;
            const float falloff = saturate(1.0 - vv / (r2 * 1.5));
            occluded += max(0.0, dot(v, n) * rsqrt(vv + 1e-4) - 0.08) * falloff;
        }
        ao = saturate(1.0 - occluded / float(kSamples) * 2.2 * aoP.x);
        ao = pow(ao, aoP.w);
        ao = lerp(1.0, ao, aoFade);
    }

    float lit = 1.0;
    const float facing = dot(n, sun.xyz);
    if (contactP.x > 0 && z < contactP.z && facing > 0.02 && sun.w > 0) {
        // March towards the sun in the scene's depth: a step's point behind
        // what the depth holds there, by less than a thin object's thickness,
        // is in that object's shadow.
        const int kSteps = 10;
        const float stride = contactP.y * (1.0 + z * 0.02) / float(kSteps);
        const float3 start = p + n * (0.02 + z * 0.0015);
        float hit = 0;
        [unroll] for (int i = 1; i <= kSteps; ++i) {
            const float3 q = start + sun.xyz * stride * (float(i) - noise * 0.8);
            const float3 sq = enbProject(q);
            if (sq.z <= 0 || any(sq.xy < 0) || any(sq.xy > 1)) break;
            const float zs = enbDistance(depthAt(sq.xy));
            const float behind = sq.z - zs;
            if (behind > 0.02 + sq.z * 0.002 && behind < 0.5 + sq.z * 0.01) {
                hit = 1.0 - float(i - 1) / float(kSteps);
                break;
            }
        }
        const float fade = 1.0 - smoothstep(contactP.z * 0.6, contactP.z, z);
        lit = 1.0 - hit * fade * saturate(facing * 4.0);
    }
    return float4(ao, lit, z, 1);
}
