// The height of the world at one texel of one clipmap level.
//
// h = macro (bicubic over the 16-bit map stretched over the world)
//   + detail (gradient-noise octaves, modulated by the macro relief).
//
// Exact on a world 2000 km across: the position comes in as an integer index
// in units of the finest spacing, and each octave's lattice cell is an integer
// shift of it. No float ever holds a world coordinate.
//
// Octaves shorter than two of the level's own texels are left out: a coarse
// level is the filtered version of a fine one, so a change of level changes
// the shape by less than the eye can tell from that distance.

Texture2D<float> macroMap : register(t0, space0);
SamplerState macroSampler : register(s0, space0);

cbuffer Generate : register(b0, space2) {
    int2 rectOrigin;        // absolute texel index (this level) of the rect's corner
    uint2 rectSize;
    uint level;             // clipmap level = layer
    uint clipSize;
    uint seed;
    uint macroSize;
    float spacing;          // metres between level-0 texels
    float worldSize;        // metres
    float macroHeightRange;
    float macroSeaLevel;
    float detailAmplitude;
    uint octaveTop;         // longest octave: wavelength spacing * 2^octaveTop
    uint macroMips;         // mip levels of the macro map
    float pad;
};

uint hashLattice(int2 c, uint octave) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ (seed + octave * 0x9e3779b9u) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float2 gradientAt(int2 c, uint octave) {
    const float a = float(hashLattice(c, octave) & 0xffffu) * (6.28318530718 / 65536.0);
    return float2(cos(a), sin(a));
}

// Gradient noise and its derivative (in cell units), quintic fade. ~[-0.7, 0.7].
float3 gradientNoise(int2 cell, float2 f, uint octave) {
    const float2 u = f * f * f * (f * (f * 6 - 15) + 10);
    const float2 du = 30 * f * f * (f * (f - 2) + 1);
    const float2 ga = gradientAt(cell, octave), gb = gradientAt(cell + int2(1, 0), octave);
    const float2 gc = gradientAt(cell + int2(0, 1), octave), gd = gradientAt(cell + int2(1, 1), octave);
    const float va = dot(ga, f), vb = dot(gb, f - float2(1, 0));
    const float vc = dot(gc, f - float2(0, 1)), vd = dot(gd, f - float2(1, 1));
    const float v = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd);
    const float2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd)
                   + du * (u.yx * (va - vb - vc + vd) + float2(vb, vc) - va);
    return float3(v, d);
}

// Catmull-Rom weights and their derivatives.
void catmullRom(float t, out float4 w, out float4 dw) {
    const float t2 = t * t, t3 = t2 * t;
    w = float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
    dw = float4(-1.5 * t2 + 2 * t - 0.5, 4.5 * t2 - 5 * t, -4.5 * t2 + 4 * t + 0.5, 1.5 * t2 - t);
}

// Macro value (0..1) and its gradient per macro texel of mip `mip`, bicubic.
float3 macroSample(float2 texel, uint mip) {
    const int size = max(1, int(macroSize >> mip));
    const float2 p = texel - 0.5;
    const float2 base = floor(p);
    const float2 t = p - base;
    float4 wx, dwx, wy, dwy;
    catmullRom(t.x, wx, dwx);
    catmullRom(t.y, wy, dwy);
    float3 result = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        float row = 0, drow = 0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 at = clamp(int2(base) + int2(i - 1, j - 1), 0, size - 1);
            const float v = macroMap.SampleLevel(macroSampler, (float2(at) + 0.5) / float(size), float(mip));
            row += wx[i] * v;
            drow += dwx[i] * v;
        }
        result.x += wy[j] * row;
        result.y += wy[j] * drow;
        result.z += dwy[j] * row;
    }
    return result;
}

// Height in metres and its gradient (dh/dx, dh/dy) at absolute texel `t` of `level`.
float3 terrainHeight(int2 t) {
    // Position in level-0 texels, exactly.
    const int2 u = t << level;

    // Macro: map texel coordinate = metres / metres-per-macro-texel. A float
    // here is fine: it only places a smooth bicubic, ~0.1 m out at the far
    // edge of the largest world, which no slope can turn into a visible step.
    //
    // A level coarser than the map reads a mip of it, not every n-th texel:
    // the far levels (a strategic view of the whole world) are the map itself,
    // and sampled sparsely it would shimmer as the window slides.
    const float levelSpacing = spacing * float(1u << level);
    const uint mip = uint(clamp(floor(log2(levelSpacing * float(macroSize) / worldSize)), 0.0, float(macroMips - 1)));
    const float macroTexel = worldSize / float(max(1u, macroSize >> mip));
    const float2 metres = float2(u) * spacing;
    const float3 m = macroSample(metres / macroTexel, mip);
    float3 h = float3((m.x - macroSeaLevel) * macroHeightRange, m.yz * macroHeightRange / macroTexel);

    // How rough the ground is here: plains are gentle, high and steep ground
    // is broken. From the macro relief, so it is the same at every level.
    const float elevation = saturate((m.x - macroSeaLevel) * 2.5);
    const float steepness = saturate(length(h.yz) * 4.0);
    // The sea floor stays smooth and below the sea: detail fades in over the
    // first ~1% of the map's range above sea level (30 m of a 3 km map).
    const float land = saturate((m.x - macroSeaLevel) * 100.0);
    const float rough = (0.25 + 1.75 * max(elevation * elevation, steepness)) * detailAmplitude * land;

    // Octaves from wavelength 2^octaveTop texels down to 2^(level+1) texels.
    float amplitude = 0.04 * spacing * float(1u << octaveTop);   // metres, 4% of the wavelength
    for (uint o = octaveTop; o >= level + 1 && o < 32; --o) {
        const int2 cell = u >> o;
        const float2 f = float2(u - (cell << o)) / float(1u << o);
        const float3 n = gradientNoise(cell, f, o);
        const float wavelength = spacing * float(1u << o);
        h.x += amplitude * rough * n.x;
        h.yz += amplitude * rough * n.yz / wavelength;
        amplitude *= 0.48;
    }
    return h;
}
