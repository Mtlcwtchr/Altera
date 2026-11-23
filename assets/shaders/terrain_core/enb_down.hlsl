// ENB chain, bloom and metering: one level of the downsample chain.
//   mode 0  the first level (half resolution) from the composite: what is over
//           the bloom threshold (soft knee), each tap weighted down by its own
//           brightness so a single hot pixel cannot flicker the whole bloom;
//           alpha the log luminance, for the eye's adaptation
//   mode 1  the next level: a 4x4 box from four bilinear taps, alpha averaged
// Works in the chain's working space (linear with `linearize`).

#include "enb.hlsli"

Texture2D<float4> source : register(t0, space2);
SamplerState sourceSampler : register(s0, space2);

float3 overThreshold(float3 c) {
    const float l = max(c.r, max(c.g, c.b));
    const float knee = max(bloomP.y * bloomP.z, 1e-4);
    float soft = clamp(l - bloomP.y + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return c * max(soft, l - bloomP.y) / max(l, 1e-4);
}

float4 downPs(VsOut input) : SV_Target0 {
    // The source is twice this target: its texel is half ours.
    const float2 t = res.zw * 0.5;
    const float2 offsets[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
    float3 sum = 0;
    float weight = 0, logLum = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float4 s = source.SampleLevel(sourceSampler, input.uv + offsets[i] * t, 0);
        if (sunColour.w < 0.5) {
            const float w = 1.0 / (1.0 + enbLuma(s.rgb));
            sum += overThreshold(s.rgb) * w;
            weight += w;
            logLum += log(enbLuma(s.rgb) + 1e-4);
        } else {
            sum += s.rgb;
            weight += 1.0;
            logLum += s.a;
        }
    }
    return float4(sum / weight, logLum * 0.25);
}
