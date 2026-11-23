// ENB chain, bloom: one level of the upsample chain - this level's own
// downsample plus the coarser level, spread by a 3x3 tent, weighted by the
// bloom's radius (more of the coarse levels: a wider, softer glow).

#include "enb.hlsli"

Texture2D<float4> coarse : register(t0, space2);
SamplerState coarseSampler : register(s0, space2);
Texture2D<float4> level : register(t1, space2);
SamplerState levelSampler : register(s1, space2);

float4 upPs(VsOut input) : SV_Target0 {
    const float2 t = res.zw;   // this level's texel; the coarse one is twice it, bilinear covers the gap
    float3 sum = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            sum += coarse.SampleLevel(coarseSampler, input.uv + float2(x, y) * t, 0).rgb * w;
        }
    const float3 own = level.SampleLevel(levelSampler, input.uv, 0).rgb;
    return float4(own + sum / 16.0 * bloomP.w, 1);
}
