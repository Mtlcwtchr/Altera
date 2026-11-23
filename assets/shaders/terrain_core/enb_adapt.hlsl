// ENB chain, the eye's adaptation (one texel): the frame's mean log luminance
// from the bloom chain's smallest level, metered towards the centre; the
// exposure that brings it to the key, pulled only part of the way
// (`adaptation`) and within its limits, approached over time from last
// frame's (the texel the other way round).

#include "enb.hlsli"

Texture2D<float4> smallest : register(t0, space2);
SamplerState smallestSampler : register(s0, space2);
Texture2D<float4> previous : register(t1, space2);
SamplerState previousSampler : register(s1, space2);

float4 adaptPs(VsOut input) : SV_Target0 {
    float sum = 0, weight = 0;
    [unroll] for (int y = 0; y < 8; ++y)
        [unroll] for (int x = 0; x < 8; ++x) {
            const float2 uv = (float2(x, y) + 0.5) / 8.0;
            const float w = 1.0 - 0.6 * saturate(length(uv - 0.5) * 1.6);
            sum += smallest.SampleLevel(smallestSampler, uv, 0).a * w;
            weight += w;
        }
    const float mean = exp(sum / weight);
    const float wanted = clamp(pow(expo2P.x / max(mean, 1e-4), expoP.y), expoP.z, expoP.w);
    const float before = previous.SampleLevel(previousSampler, input.uv, 0).r;
    const float k = 1.0 - exp(-camU.w * expo2P.y);
    const float now = expo2P.z > 0.5 || !(before > 0) ? wanted : lerp(before, wanted, k);
    return float4(now, mean, 0, 1);
}
