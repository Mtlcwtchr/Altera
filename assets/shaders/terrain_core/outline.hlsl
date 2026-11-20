// Ink outline (terrain_core::Outline): dark, uneven lines on silhouettes and
// creases, found in the frame's depth. One full-screen triangle over a copy of
// the finished picture, drawn back into the frame before the PSX look.
//
// Depth is reversed and infinite (Camera::viewProjection): d = near / z, so the
// distance along the view axis is near / d, and 0 is the sky.
//   silhouette: a neighbour `width` pixels away is farther by more than
//               `depthEdge` of this pixel's distance - the line sits on the
//               near object, so it hugs the figure, not the ground behind;
//   crease:     the second difference of distance across the pixel, relative
//               to the distance - a fold in a cliff, a roof's ridge.
// The hand-drawn part: the width breathes with a slow noise, the stroke wobbles
// and breaks in small cells (`roughness`), and fades with distance down to
// `farStrength`, so far ridges keep a thin line against the sky.

Texture2D<float4> frame : register(t0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
SamplerState frameSampler : register(s0, space2);
SamplerState depthSampler : register(s1, space2);

cbuffer Outline : register(b0, space3) {
    float2 source;        // the frame's size, pixels
    float nearPlane;
    float width;          // line half-width, pixels
    float depthEdge;      // relative distance step that makes a silhouette
    float creaseEdge;     // relative second difference that makes a crease
    float creaseStrength;
    float opacity;
    float fadeStart;      // metres: full strength nearer than this
    float fadeEnd;        // metres: `farStrength` beyond this
    float farStrength;
    float roughness;      // 0: clean lines, 1: very broken and wobbly
    float4 ink;           // rgb: the line's colour (sRGB-coded, as the frame), a: how much of the picture shows through
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut outlineVs(uint id : SV_VertexID) {
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

float valueNoise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float distanceAt(float2 uv) {
    const float d = sceneDepth.SampleLevel(depthSampler, uv, 0);
    return d > 0 ? nearPlane / d : 1e7;
}

float4 outlinePs(VsOut input) : SV_Target0 {
    const float2 pixel = input.uv * source;
    const float2 texel = 1.0 / source;
    const float3 colour = frame.SampleLevel(frameSampler, input.uv, 0).rgb;

    // The stroke's wobble and breathing width, in screen space (a pen, not a decal).
    const float2 wobble = float2(valueNoise(pixel / 9.0), valueNoise(pixel / 9.0 + 37.1)) - 0.5;
    const float2 uv = input.uv + wobble * roughness * 1.5 * texel;
    const float breathe = lerp(1.0, 0.45 + 1.1 * valueNoise(pixel / 23.0 + 11.3), roughness);
    const float r = max(0.75, width * breathe);

    const float z = distanceAt(uv);
    if (z > 1e6) return float4(colour, 1);   // the sky draws no line of its own

    const float2 dx = float2(r * texel.x, 0), dy = float2(0, r * texel.y);
    const float zl = distanceAt(uv - dx), zr = distanceAt(uv + dx);
    const float zu = distanceAt(uv - dy), zd = distanceAt(uv + dy);
    const float za = distanceAt(uv + (dx + dy) * 0.7071), zb = distanceAt(uv + (dx - dy) * 0.7071);
    const float zc = distanceAt(uv - (dx - dy) * 0.7071), ze = distanceAt(uv - (dx + dy) * 0.7071);

    // Silhouette: how much farther the farthest neighbour is, relative to here.
    const float farthest = max(max(max(zl, zr), max(zu, zd)), max(max(za, zb), max(zc, ze)));
    const float jump = (min(farthest, z * 50.0) - z) / z;
    const float silhouette = saturate((jump - depthEdge) / max(depthEdge, 1e-4));

    // Crease: the bend across the pixel, only where both sides are the same surface.
    float crease = 0;
    {
        const float bx = abs(zl + zr - 2.0 * z) / z, by = abs(zu + zd - 2.0 * z) / z;
        const float same = step(max(max(abs(zl - z), abs(zr - z)), max(abs(zu - z), abs(zd - z))) / z, depthEdge * 4.0);
        crease = saturate((max(bx, by) - creaseEdge) / max(creaseEdge, 1e-4)) * same * creaseStrength;
    }

    const float near = 1.0 - saturate((z - fadeStart) / max(fadeEnd - fadeStart, 1.0));
    float line = max(silhouette, crease) * lerp(farStrength, 1.0, near);
    // Broken in small cells, the dry-brush part.
    const float gap = hash(floor(pixel / 2.0) + 7.7);
    line *= lerp(1.0, 0.35 + 0.65 * step(roughness * 0.3, gap), roughness);
    line = saturate(line * opacity);

    // Ink darkens what is under it rather than painting over: a trace of the colour stays.
    const float3 inked = lerp(ink.rgb, colour * ink.rgb * 4.0, ink.a);
    return float4(lerp(colour, min(inked, colour), line), 1);
}
