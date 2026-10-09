// The cloud layer's shape, shared by the cloud march (clouds.hlsl) and by the
// shadows the clouds throw on the ground (lighting.hlsli): one function, so a
// cloud's shadow lies under the cloud.
//
// Absolute world metres in floats: on a 2000 km world that is a quarter of a
// metre out, which no cloud a kilometre across can show.
#ifndef TERRAIN_CORE_CLOUDS
#define TERRAIN_CORE_CLOUDS

uint cloudHash(int3 c) {
    uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^ uint(c.z) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float cloudValue3(float3 p) {
    const float3 i = floor(p), f = p - i;
    const float3 u = f * f * (3 - 2 * f);
    const int3 c = int3(i);
    float v[8];
    [unroll] for (int k = 0; k < 8; ++k)
        v[k] = float(cloudHash(c + int3(k & 1, (k >> 1) & 1, k >> 2)) & 0xffffu) / 65535.0;
    return lerp(lerp(lerp(v[0], v[1], u.x), lerp(v[2], v[3], u.x), u.y),
                lerp(lerp(v[4], v[5], u.x), lerp(v[6], v[7], u.x), u.y), u.z);
}

// Where there are clouds at all, 0..1: a slow 2D field, thresholded by the
// cover (0 clear sky .. 1 overcast). Drifts with the wind.
float cloudCoverage(float2 xy, float cover, float2 drift) {
    const float2 p = (xy + drift) / 9000.0;
    const float n = cloudValue3(float3(p, 0.5)) * 0.55 + cloudValue3(float3(p * 2.7, 3.5)) * 0.3 +
                    cloudValue3(float3(p * 7.1, 7.5)) * 0.15;
    return saturate((n - (1.0 - cover)) / max(cover * 0.6, 0.05));
}

// Density at a point of the layer: the coverage, rounded off towards the base
// and the top, eaten into by a billowing 3D noise. `level` 0..1 through the layer.
float cloudDensity(float3 world, float level, float cover, float2 drift, int octaves) {
    const float coverage = cloudCoverage(world.xy, cover, drift);
    if (coverage <= 0) return 0;
    // Flat bottoms, rounded tops.
    const float profile = saturate(level * 6.0) * saturate((1.0 - level) * 2.0);
    float shape = coverage * profile;
    if (shape <= 0) return 0;
    const float3 p = float3(world.xy + drift * 1.4, world.z) / 1100.0;
    float billow = cloudValue3(p) * 0.6;
    if (octaves > 1) billow += cloudValue3(p * 2.9 + 11.0) * 0.28;
    if (octaves > 2) billow += cloudValue3(p * 8.3 + 37.0) * 0.12;
    return saturate((shape - billow * 0.55) * 2.2);
}

#endif
