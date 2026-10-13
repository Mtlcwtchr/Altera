// Magic megalith A-2, the carved stele (doc/megaliths/A2_CARVED_STELE.md):
// baked maps on the model's own UVs (one 2048 atlas for the stone and its
// fallen pieces), hard pixels. Layers of the array (import_megalith.py):
// 0 basecolor (as baked, sRGB values), 1 normal (OpenGL tangent space),
// 2 surface (R = AO, G = roughness).
//
// The mesh has no tangents: the frame comes from the screen derivatives of
// the position and the texcoord (a cotangent frame), which is exact on a flat
// shaded low poly mesh.

#ifndef MEGALITH_A2_HLSLI
#define MEGALITH_A2_HLSLI

struct A2Surface { float3 colour; float3 normal; float ao; float roughness; };

// The maps were made under Blender's brighter studio light: in the game's
// light the stone keeps its look (dark, the relief still read) at this gain.
static const float kA2AlbedoGain = 2.0;

// `uv`: atlas texcoord (glTF: v down); `p`: any position the derivatives are
// taken of (eye-relative, world orientation); `n0`: the face normal.
A2Surface a2Surface(Texture2DArray<float4> maps, SamplerState hard, float2 uv, float3 p, float3 n0, float bias) {
    A2Surface o;
    o.colour = maps.SampleBias(hard, float3(uv, 0), bias).rgb * kA2AlbedoGain;
    const float3 t = maps.SampleBias(hard, float3(uv, 1), bias).rgb * 2.0 - 1.0;
    const float2 s = maps.SampleBias(hard, float3(uv, 2), bias).rg;
    o.ao = s.r;
    o.roughness = s.g;
    // Cotangent frame (Schueler): T along +u, B along -v (the map's +Y is up the
    // picture, the texcoord's v runs down it).
    const float3 dp1 = ddx(p), dp2 = ddy(p);
    const float2 du1 = ddx(uv), du2 = ddy(uv);
    const float3 dp2perp = cross(dp2, n0), dp1perp = cross(n0, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    const float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    T *= inv;
    B *= -inv;
    const float3 n = normalize(T * t.x + B * t.y + n0 * max(t.z, 0.05));
    // (a degenerate frame - a sliver seen edge on - keeps the face's normal)
    o.normal = all(isfinite(n)) ? n : n0;
    return o;
}

#endif
