// Noise on the ground, from exact positions: an integer texel index and a
// small float offset (see ground.hlsli). No resources: shared by the
// fragment stage (the ground's colour) and compute (where foliage grows).
#ifndef TERRAIN_CORE_GROUND_NOISE
#define TERRAIN_CORE_GROUND_NOISE

struct GroundPoint {
    int2 base;          // level-0 texel index of the patch origin
    float2 sub;         // offset from it, level-0 texels
    float footprint;    // level-0 texels one pixel covers
};

// How far a noise of cells `cell` texels wide has to be faded to its mean
// here: a pixel that covers half a cell or more can only alias it into grain.
float noiseFade(GroundPoint p, int log2Cell) {
    return saturate(p.footprint / float(1 << log2Cell) * 2.0 - 0.5);
}

uint groundHash(int2 c, uint seed) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

// Position in cells of 2^log2Cell level-0 texels: integer cell and fraction.
void cellOf(GroundPoint p, int log2Cell, out int2 cell, out float2 f) {
    const int size = 1 << log2Cell;
    const float2 inCell = (float2(p.base & (size - 1)) + p.sub) / float(size);
    const float2 whole = floor(inCell);
    cell = (p.base >> log2Cell) + int2(whole);
    f = inCell - whole;
}

// Value noise, 0..1, cells of 2^log2Cell level-0 texels.
float cellNoise(GroundPoint p, int log2Cell, uint seed) {
    int2 c;
    float2 f;
    cellOf(p, log2Cell, c, f);
    const float2 u = f * f * (3 - 2 * f);
    const float a = float(groundHash(c, seed) & 0xffffu), b = float(groundHash(c + int2(1, 0), seed) & 0xffffu);
    const float d = float(groundHash(c + int2(0, 1), seed) & 0xffffu), e = float(groundHash(c + int2(1, 1), seed) & 0xffffu);
    return lerp(lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y) / 65535.0, 0.5, noiseFade(p, log2Cell));
}

#endif
