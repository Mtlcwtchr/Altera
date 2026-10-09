// The final pass over the frame (terrain_core::Post), one full-screen
// triangle: the clouds laid over it, then a picture style. Three looks, for judging the world as the game will show it rather
// than as a render:
//
//   1  dark fantasy   drained, heavy, cold in the shadows, film grain, a vignette
//                     that closes the picture in
//   2  Frazetta       paint: a Kuwahara filter turns the picture into strokes;
//                     earth palette (umber, ochre), deep chiaroscuro, warm lights,
//                     edges sinking into dark brown, a little canvas
//   3  Vallejo        airbrush: a soft Kuwahara and a blur smooth the surfaces;
//                     saturated, glossy, blue-violet shadows, warm skin-like
//                     lights, a glow round everything bright

Texture2D<float4> frame : register(t0, space2);
SamplerState frameSampler : register(s0, space2);
Texture2D<float4> clouds : register(t1, space2);      // clouds.hlsl, half resolution
SamplerState cloudSampler : register(s1, space2);

cbuffer Post : register(b0, space3) {
    uint mode;
    float time;
    float2 resolution;
    float cloudsOn;
    float3 postPad;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut postVs(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// The frame with the clouds over it: what every style starts from.
float3 at(float2 uv) {
    float3 c = frame.SampleLevel(frameSampler, uv, 0).rgb;
    if (cloudsOn > 0.5) {
        const float4 cloud = clouds.SampleLevel(cloudSampler, uv, 0);
        c = c * cloud.a + cloud.rgb;
    }
    return c;
}
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float hash(float2 p) {
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac((p.x + p.y) * p.x);
}

// Kuwahara: the mean of whichever of the four quadrants round the pixel varies
// least. Edges stay, flat areas flatten into strokes.
float3 kuwahara(float2 uv, int radius) {
    const float2 texel = 1.0 / resolution;
    float3 best = at(uv);
    float bestVariance = 1e9;
    [unroll] for (int q = 0; q < 4; ++q) {
        const int2 dir = int2(q & 1 ? 1 : -1, q & 2 ? 1 : -1);
        float3 sum = 0, sum2 = 0;
        float n = 0;
        for (int y = 0; y <= radius; ++y)
            for (int x = 0; x <= radius; ++x) {
                const float3 c = at(uv + float2(x * dir.x, y * dir.y) * texel);
                sum += c;
                sum2 += c * c;
                n += 1;
            }
        const float3 mean = sum / n;
        const float3 v = sum2 / n - mean * mean;
        const float variance = v.r + v.g + v.b;
        if (variance < bestVariance) { bestVariance = variance; best = mean; }
    }
    return best;
}

float3 blur(float2 uv, float radius) {
    const float2 texel = radius / resolution;
    float3 sum = 0;
    [unroll] for (int k = 0; k < 12; ++k) {
        const float a = float(k) * 0.5235988;
        sum += at(uv + float2(cos(a), sin(a)) * texel * (k & 1 ? 1.0 : 0.55));
    }
    return sum / 12.0;
}

float vignette(float2 uv, float strength, float reach) {
    const float2 d = (uv - 0.5) * float2(resolution.x / resolution.y, 1.0);
    return 1.0 - strength * smoothstep(reach * 0.35, reach, length(d));
}

float3 darkFantasy(float2 uv) {
    float3 c = at(uv);
    const float l = luma(c);
    c = lerp(float3(l, l, l), c, 0.45);                            // drained
    c = pow(saturate(c), 1.35) * 0.95;                             // heavier, crushed
    const float3 shadow = float3(0.80, 0.95, 1.00), light = float3(1.05, 0.97, 0.85);
    c *= lerp(shadow, light, smoothstep(0.05, 0.6, l));            // cold below, dull warm above
    c = (c - 0.5) * 1.15 + 0.47;                                   // contrast round a low middle
    c *= vignette(uv, 0.7, 1.15);
    const float grain = hash(uv * resolution + frac(time) * 371.0) - 0.5;
    c += grain * 0.09 * (1.0 - l * 0.5);
    return saturate(c);
}

float3 frazetta(float2 uv) {
    float3 c = kuwahara(uv, 4);
    const float l = luma(c);
    // Earth palette: map the picture's value onto umber -> ochre -> warm light,
    // keeping some of its own hue.
    const float3 umber = float3(0.16, 0.09, 0.05), ochre = float3(0.62, 0.42, 0.20), light = float3(1.0, 0.86, 0.62);
    const float3 earth = l < 0.45 ? lerp(umber, ochre, l / 0.45) : lerp(ochre, light, (l - 0.45) / 0.55);
    c = lerp(earth, c * float3(1.10, 0.98, 0.80), 0.62);
    // Chiaroscuro: an S curve, darks pushed down hard.
    c = saturate(c);
    c = c * c * (3.0 - 2.0 * c);
    c = pow(c, 1.1);
    // Edges sink into dark brown, as his canvases do - softly, not a porthole.
    const float v = vignette(uv, 0.85, 1.25);
    c = lerp(float3(0.06, 0.035, 0.02), c, v);
    // Canvas weave.
    const float2 px = uv * resolution;
    const float weave = (sin(px.x * 1.7) * sin(px.y * 1.9)) * 0.5 + 0.5;
    c *= 0.96 + 0.06 * weave;
    return saturate(c);
}

float3 vallejo(float2 uv) {
    const float3 smooth = lerp(kuwahara(uv, 2), blur(uv, 1.5), 0.4);
    float3 c = smooth;
    const float l = luma(c);
    c = lerp(float3(l, l, l), c, 1.5);                             // saturated
    c = pow(saturate(c), 0.85);                                    // shadows open, airbrushed not inked
    // Shadows blue-violet, lights warm and glossy.
    c *= lerp(float3(0.78, 0.80, 1.18), float3(1.10, 1.00, 0.88), smoothstep(0.15, 0.75, l));
    c = saturate((c - 0.5) * 1.25 + 0.52);
    // Glow round the bright: a wide blur of what is over the threshold.
    const float3 wide = blur(uv, 9.0) + blur(uv, 18.0);
    const float3 glow = max(wide * 0.5 - 0.55, 0.0) * 1.6;
    c += glow * float3(1.0, 0.85, 0.7);
    c *= vignette(uv, 0.25, 1.3);
    return saturate(c);
}

float4 postPs(VsOut input) : SV_Target0 {
    float3 c;
    if (mode == 1) c = darkFantasy(input.uv);
    else if (mode == 2) c = frazetta(input.uv);
    else if (mode == 3) c = vallejo(input.uv);
    else c = at(input.uv);
    // A half-step of noise: soft gradients (sky, vignette, glow) otherwise
    // come out as rings of 8-bit steps.
    c += (hash(input.uv * resolution + 17.0) - 0.5) / 255.0;
    return float4(c, 1);
}
