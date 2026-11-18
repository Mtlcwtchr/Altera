// The PSX look (terrain_core::Psx), drawn into the low-resolution picture:
// one full-screen triangle, a pixel per block of `scale` x `scale` frame pixels.
// The steps and units are those of assets_catalog/tools/psx_filter.py:
//   mean of the block (the preview's bilinear shrink), + Bayer 4x4 * dither,
//   + gaussian grain, then floor to `bits` per channel. Values in 0..255.
// The frame is UNORM with sRGB-coded values, as the previews' PNGs are.

Texture2D<float4> frame : register(t0, space2);
SamplerState frameSampler : register(s0, space2);

cbuffer Psx : register(b0, space3) {
    float2 source;     // the frame's size, pixels
    uint scale;
    uint bits;
    float dither;
    float grain;
    float time;        // 0: grain fixed
    float psxPad;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut psxVs(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float hash(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}

static const float kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float4 psxPs(VsOut input) : SV_Target0 {
    const int2 low = int2(input.position.xy);
    const int2 origin = low * int(scale);
    const int2 last = int2(source) - 1;
    float3 sum = 0;
    for (uint y = 0; y < scale; ++y)
        for (uint x = 0; x < scale; ++x)
            sum += frame.Load(int3(min(origin + int2(x, y), last), 0)).rgb;
    float3 c = sum / float(scale * scale) * 255.0;

    const float d = kBayer[(low.y & 3) * 4 + (low.x & 3)] / 16.0 - 0.5;
    c += d * dither;
    // Gaussian grain (Box-Muller), its own value per channel, as the preview's.
    const float2 seed = float2(low) + frac(time * 0.731) * 1013.0;
    const float r = sqrt(-2.0 * log(max(hash(seed + 0.5), 1e-6)));
    const float a = 6.2831853 * hash(seed + 71.3);
    const float a2 = 6.2831853 * hash(seed + 29.7);
    c += grain * r * float3(cos(a), sin(a), cos(a2));

    const float levelStep = exp2(8.0 - float(bits));
    c = floor(clamp(c, 0.0, 255.0) / levelStep) * levelStep;
    return float4(c / 255.0, 1);
}
