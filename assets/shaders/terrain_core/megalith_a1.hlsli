// Magic megalith A-1, the giant monolith slabs (doc/megaliths/A1_MONOLITH.md):
// the Blender material (tools/blender/megalith_a_monolith.py, stone_material)
// node for node - red hatched stone, pale hairline strokes, moss laid by noise
// on ledges and corners and seeping into the grooves, one height field for the
// relief. Everything in object space, so a slab's pattern turns and scales with
// it. The knobs come from the material manifest
// (content/config/terrain_core/megaliths/a1_monolith.material.json).
//
// Included by props.hlsl, which declares `megalithTiles` (hatch, grooves, moss,
// strokes as array layers) twice, hard and soft, and the A1 cbuffer.
#ifndef TERRAIN_CORE_MEGALITH_A1
#define TERRAIN_CORE_MEGALITH_A1

// Must match terrain_core::MegalithLook (props.hpp): the manifest's knobs in
// this order, four to a register.
#define K(i) a1Knobs[(i) / 4][(i) % 4]
#define moss_coverage      K(0)
#define moss_scale         K(1)
#define moss_softness      K(2)
#define moss_foot_m        K(3)
#define moss_foot_weight   K(4)
#define moss_ledge_weight  K(5)
#define moss_pixel_m       K(6)
#define coverage_jitter    K(7)
#define streak_strength    K(8)
#define moss_tile_m        K(9)
#define stone_saturation   K(10)
#define stone_value        K(11)
#define scratch_strength   K(12)
#define scratch_tile_m     K(13)
#define bump_strength      K(14)
#define bump_distance_m    K(15)
#define stroke_height      K(16)
#define groove_depth       K(17)
#define moss_height        K(18)
#define ao_strength        K(19)
#define ao_distance_m      K(20)
#define cavity_strength    K(21)
#define stone_roughness    K(22)
#define stroke_roughness   K(23)
#define moss_roughness     K(24)
#define a1_specular        K(25)
#define moss_streak_stretch K(26)
#define moss_edge_weight   K(27)
#define moss_groove_weight K(28)
#define groove_gate        K(29)
#define moss_fine_scale    K(30)
#define moss_ragged        K(31)
#define groove_warp        K(32)
#define stone_tile_m       K(33)

// Gradient noise in 3D, about -1..1.
float3 a1Hash3(float3 p) {
    uint3 q = uint3(int3(p) + 32768);
    q = q * uint3(1597334673u, 3812015801u, 2798796415u);
    q = (q.x ^ q.y ^ q.z) * uint3(1597334673u, 3812015801u, 2798796415u);
    return float3(q >> 8) * (1.0 / 8388608.0) - 1.0;
}

float a1Perlin(float3 p) {
    const float3 i = floor(p), f = p - i;
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n[8];
    [unroll] for (int k = 0; k < 8; ++k) {
        const float3 o = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        n[k] = dot(a1Hash3(i + o), f - o);
    }
    const float x00 = lerp(n[0], n[1], u.x), x10 = lerp(n[2], n[3], u.x);
    const float x01 = lerp(n[4], n[5], u.x), x11 = lerp(n[6], n[7], u.x);
    // (scaled to Blender's spread: a standard deviation of 0.265, measured)
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z) * 1.39;
}

// Blender's Noise Texture (fBm): `scale`, `detail` octaves, `roughness`,
// `distortion`; 0..1 round 0.5.
float a1Noise(float3 p, float scale, float detail, float roughness, float distortion) {
    p *= scale;
    if (distortion > 0)
        p += float3(a1Perlin(p + 13.5), a1Perlin(p + 47.2), a1Perlin(p - 21.9)) * distortion;
    float sum = 0, amp = 1, total = 0;
    const int octaves = int(detail);
    for (int o = 0; o <= octaves; ++o) {
        sum += a1Perlin(p) * amp;
        total += amp;
        amp *= roughness;
        p *= 2.0;
    }
    const float last = detail - float(octaves);
    if (last > 0) { sum += a1Perlin(p) * amp * last; total += amp * last; }
    return 0.5 + 0.5 * sum / total;
}

float a1Range(float v, float a, float b, float c, float d) {
    return lerp(c, d, saturate((v - a) / (b - a)));
}

float3 a1ToLinear(float3 c) { return pow(max(c, 0), 2.2); }

// Blender's box projection, blend 0.15: one face's picture, a narrow seam of
// two near the diagonals.
float3 a1BoxWeights(float3 n) {
    float3 a = abs(n);
    const float m = max(a.x, max(a.y, a.z));
    float3 w = saturate((a - (m - 0.15)) / 0.15);
    return w / max(dot(w, 1.0), 1e-4);
}

float3 a1Box(float3 p, float3 w, float layer, float bias) {
    float3 c = 0;
    if (w.x > 0) c += megalithTiles.SampleBias(megalithNearest, float3(p.yz, layer), bias).rgb * w.x;
    if (w.y > 0) c += megalithTiles.SampleBias(megalithNearest, float3(p.xz, layer), bias).rgb * w.y;
    if (w.z > 0) c += megalithTiles.SampleBias(megalithNearest, float3(p.xy, layer), bias).rgb * w.z;
    return c;
}

// The same through the soft sampler (the groove mask is read smooth).
float a1BoxSoft(float3 p, float3 w, float layer, float bias) {
    float c = 0;
    if (w.x > 0) c += megalithTilesSoft.SampleBias(megalithLinear, float3(p.yz, layer), bias).r * w.x;
    if (w.y > 0) c += megalithTilesSoft.SampleBias(megalithLinear, float3(p.xz, layer), bias).r * w.y;
    if (w.z > 0) c += megalithTilesSoft.SampleBias(megalithLinear, float3(p.xy, layer), bias).r * w.z;
    return c;
}

struct A1Surface { float3 colour; float3 normal; float roughness; };

// `p`, `n`: object space (metres, unscaled, Z up); `edge`: the `_EDGE`
// attribute; `rand`: the instance's 0..1; `nWorld`, `pView`: the shading
// normal and the eye-relative position, for the relief.
A1Surface a1Surface(float3 p, float3 n, float edge, float rand, float3 nWorld, float3 pView, float bias) {
    const float3 w = a1BoxWeights(n);
    const float3 off = float3(rand * 731.0, rand * 377.0, 0.0);
    // Stone: the hatch, streaks running down it, a slow drift, dulled to brown.
    const float3 stoneUv = p / stone_tile_m;
    const float3 hatch = a1ToLinear(a1Box(stoneUv, w, 0, bias));
    const float3 mossTex = a1ToLinear(a1Box(p / moss_tile_m, w, 2, bias));
    const float streakN = a1Noise(p * float3(0.12, 0.12, 0.006) + off, 1.0, 2.0, 0.5, 0.0);
    const float streak = a1Range(streakN, 0.52, 0.72, 0.0, 1.0) * streak_strength;
    float3 stone = lerp(hatch, hatch * float3(0.42, 0.13, 0.10), streak);
    stone *= a1Range(a1Noise(p + off, 0.012, 2.0, 0.5, 0.0), 0.3, 0.7, 0.72, 1.18);
    const float luma = dot(stone, float3(0.2126, 0.7152, 0.0722));
    stone = lerp(stone, float3(1.25, 0.82, 0.6) * luma, 1.0 - stone_saturation) * stone_value;
    // The hairlines thin to nothing in the mips; Blender's hard pixels keep
    // them. So the mip's average is lifted by how many texels it stands for.
    const float strokeLod = megalithTiles.CalculateLevelOfDetail(megalithNearest, p.xz / scratch_tile_m) + bias;
    const float strokes = saturate(a1Box(p / scratch_tile_m, w, 3, bias).r * clamp(exp2(strokeLod * 0.7), 1.0, 4.0));
    stone = lerp(stone, float3(0.46, 0.21, 0.13), saturate(strokes * scratch_strength));

    // Moss: stepped banded noise, the foot, ledges, edges, a per-slab nudge,
    // frayed; then seeping along the grooves where the noise lets it.
    const float3 shifted = p + off;
    const float3 snapped = floor(shifted / moss_pixel_m) * moss_pixel_m;
    const float mossN = a1Noise(snapped * float3(1, 1, 1.0 / moss_streak_stretch), moss_scale, 4.0, 0.55, 0.6);
    const float spread = a1Range(mossN, 0.32, 0.68, 0.0, 1.0);
    const float foot = a1Range(p.z, 0.0, moss_foot_m, 1.0, 0.0) * moss_foot_weight;
    const float ledge = a1Range(nWorld.z, 0.35, 0.9, 0.0, 1.0) * moss_ledge_weight;
    const float nudge = (rand - 0.5) * coverage_jitter;
    const float ragged = (a1Noise(snapped, moss_fine_scale, 3.0, 0.5, 1.2) - 0.5) * moss_ragged * 2.0;
    const float s = spread + foot + ledge + nudge + edge * moss_edge_weight + ragged;
    const float e = 1.15 - moss_coverage;
    float mask = a1Range(s, e - moss_softness, e + moss_softness, 0.0, 1.0);
    const float3 wobble = float3(a1Noise(shifted, 0.08, 2.0, 0.5, 0.0), a1Noise(shifted + 91.7, 0.08, 2.0, 0.5, 0.0),
                                 a1Noise(shifted - 53.1, 0.08, 2.0, 0.5, 0.0)) - 0.5;
    const float groove = a1BoxSoft(stoneUv + wobble * groove_warp, w, 1, bias);
    const float gate = a1Range(spread, groove_gate, groove_gate + 0.15, 0.0, 1.0);
    const float seep = a1Range(groove * gate + ragged, 0.45, 0.6, 0.0, 1.0) * moss_groove_weight;
    mask = max(mask, seep);
    float3 base = lerp(stone, mossTex, mask);

    // Relief: strokes proud, grooves sunk, moss a cushion; the normal from its
    // screen-space slope, as Blender's Bump node.
    const float grooveFlat = a1BoxSoft(stoneUv, w, 1, bias);
    const float hGroove = grooveFlat * (1.0 - mask) * groove_depth;
    const float mossLuma = dot(mossTex, float3(0.2126, 0.7152, 0.0722));
    const float height = (strokes * stroke_height - hGroove + mask * (0.45 + mossLuma) * moss_height) * bump_distance_m;
    const float3 dpdx = ddx(pView), dpdy = ddy(pView);
    const float dhx = ddx(height), dhy = ddy(height);
    const float3 r1 = cross(dpdy, nWorld), r2 = cross(nWorld, dpdx);
    const float det = dot(dpdx, r1);
    const float3 grad = sign(det) * (dhx * r1 + dhy * r2);
    A1Surface o;
    o.normal = normalize(lerp(nWorld, normalize(abs(det) * nWorld - grad), saturate(bump_strength)));
    if (any(isnan(o.normal))) o.normal = nWorld;
    // Hollows darkened from the relief itself (grooves, the moss's rims).
    const float rim = mask * (1.0 - mask) * 4.0;
    base *= 1.0 - (hGroove + rim * 0.35) * cavity_strength;
    o.colour = pow(base, 1.0 / 2.2);
    o.roughness = lerp(lerp(stone_roughness, stroke_roughness, strokes), moss_roughness, mask);
    return o;
}

#undef K
#endif
