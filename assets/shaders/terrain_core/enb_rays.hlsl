// ENB chain, light shafts (quarter resolution): the bright sky near the sun
// smeared along the lines towards it, stopped wherever the ground, a tree or
// a roof stands in front - god rays through the branches and over ridges.
// Out: the shafts' light (working space), before the sun's colour.

#include "enb.hlsli"

Texture2D<float4> lit : register(t0, space2);
SamplerState litSampler : register(s0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
SamplerState depthSampler : register(s1, space2);

float4 raysPs(VsOut input) : SV_Target0 {
    if (sunUv.z <= 0 || raysP.x <= 0) return float4(0, 0, 0, 1);
    const int kSamples = 40;
    const float2 toSun = sunUv.xy - input.uv;
    const float2 stride = toSun * raysP.z / float(kSamples);
    const float jitter = enbIgn(input.position.xy);
    float2 at = input.uv + stride * jitter;
    const float2 aspect = float2(src.x * src.w, 1.0);
    float sum = 0, decay = 1.0;
    [unroll] for (int i = 0; i < kSamples; ++i) {
        if (all(at >= 0) && all(at <= 1)) {
            const bool open = sceneDepth.SampleLevel(depthSampler, at, 0) <= 0;
            // Only the sky lets light through: its own brightness, softly
            // capped, the more of it the nearer the sun it is.
            const float l = enbLuma(lit.SampleLevel(litSampler, at, 0).rgb);
            const float bySun = saturate(1.0 - length((at - sunUv.xy) * aspect) * 1.4);
            sum += open ? min(l, 4.0) * bySun * bySun * decay : 0.0;
        }
        decay *= raysP.y;
        at += stride;
    }
    // Strongest looking near the sun, gone looking away from it.
    const float near = saturate(1.0 - length(toSun * aspect) * 0.9);
    return float4(sum / float(kSamples) * near * near * sunUv.z, 0, 0, 1);
}
