// ENB chain, last pass (scene resolution, 8-bit out): the lens and the grade.
//   bloom + light shafts added in the working space
//   the eye's exposure (enb_adapt.hlsl) times the preset's own
//   the tone curve: 0 none (a clamp, the old picture), 1 ACES (fitted RRT+ODT),
//   2 AgX (minimal fit) - highlights roll off instead of clipping
//   the grade in display space: lift / gamma / gain, contrast round a pivot,
//   saturation and vibrance, split toning (shadow and highlight tints, chosen
//   by the time of day on the CPU), a vignette, film grain, a dither

#include "enb.hlsli"

Texture2D<float4> lit : register(t0, space2);
SamplerState litSampler : register(s0, space2);
Texture2D<float4> bloom : register(t1, space2);
SamplerState bloomSampler : register(s1, space2);
Texture2D<float4> rays : register(t2, space2);
SamplerState raysSampler : register(s2, space2);
Texture2D<float4> adapted : register(t3, space2);
SamplerState adaptedSampler : register(s3, space2);

// Stephen Hill's fit of the ACES reference rendering and output transforms.
float3 aces(float3 c) {
    const float3x3 into = float3x3(0.59719, 0.35458, 0.04823,
                                   0.07600, 0.90834, 0.01566,
                                   0.02840, 0.13383, 0.83777);
    const float3x3 outOf = float3x3(1.60475, -0.53108, -0.07367,
                                    -0.10208, 1.10813, -0.00605,
                                    -0.00327, -0.07276, 1.07602);
    c = mul(into, c);
    const float3 a = c * (c + 0.0245786) - 0.000090537;
    const float3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    return saturate(mul(outOf, a / b));
}

// AgX, the minimal polynomial fit (Wrensch): out in display code.
float3 agx(float3 c) {
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(c, inset);
    c = clamp(log2(max(c, 1e-10)), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    const float3 x2 = c * c, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    return saturate(mul(c, outset));
}

float4 finalPs(VsOut input) : SV_Target0 {
    const float2 uv = input.uv;
    float3 c = lit.SampleLevel(litSampler, uv, 0).rgb;
    c += bloom.SampleLevel(bloomSampler, uv, 0).rgb * bloomP.x;
    c += rays.SampleLevel(raysSampler, uv, 0).r * raysP.x * sunColour.rgb * sun.w;

    const float eye = max(adapted.SampleLevel(adaptedSampler, float2(0.5, 0.5), 0).r, 1e-3);
    c *= expoP.x * (expoP.y > 0 ? eye : 1.0);

    const int curve = int(toneP.x + 0.5);
    const bool linearLight = expo2P.w > 0.5;
    if (curve == 1) c = aces(c);
    else if (curve == 2) c = agx(c);
    else c = saturate(c);
    // ACES gives linear light; AgX and the plain clamp give display code
    // (the clamp only if the chain did not linearize).
    if (linearLight && curve != 2) c = pow(saturate(c), 1.0 / 2.2);

    // The grade.
    c = gainP.rgb * (c + liftP.rgb * (1.0 - c));
    c = pow(max(c, 0.0), 1.0 / max(gammaP.rgb, 0.05));
    c = (c - liftP.w) * toneP.y + liftP.w;
    {
        const float l = enbLuma(c);
        const float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        c = lerp(float3(l, l, l), c, toneP.z * (1.0 + toneP.w * (1.0 - saturate(chroma * 2.0))));
    }
    if (shadowTint.w > 0) {
        const float l = saturate(enbLuma(c));
        const float high = smoothstep(0.08, 0.85, l + highTint.w * 0.3);
        c *= lerp(float3(1, 1, 1), lerp(shadowTint.rgb, highTint.rgb, high), shadowTint.w);
    }
    if (lensP.x > 0) {
        const float2 d = (uv - 0.5) * float2(res.x * res.w, 1.0);
        const float v = 1.0 - lensP.x * smoothstep(lensP.y * 0.35, lensP.y, length(d));
        const float3 umber = float3(0.06, 0.035, 0.02);
        c = lerp(c * v, lerp(umber, c, v), flagsP.z);
    }
    if (flagsP.y > 0.5 && lensP.z > 0) {
        const float2 cell = floor(input.position.xy / max(lensP.w, 1.0));
        const float grain = enbHash(cell + frac(camR.w * 7.13) * 409.0) + enbHash(cell * 1.37 + frac(camR.w * 3.71) * 211.0) - 1.0;
        c += grain * lensP.z * (1.0 - saturate(enbLuma(c)) * 0.6);
    }
    c += (enbHash(input.position.xy + 17.0) - 0.5) / 255.0;
    return float4(saturate(c), 1);
}
