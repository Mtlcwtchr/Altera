// The water map (src/terrain_core/hydrology.cpp), read by hand: RGBA32F need
// not filter, and a dry texel must not drag the surface to -30 km. Per texel:
// level (m), flow, direction x, y; a dry one counts as water 60 m under sea
// level, under any bank, so the bilinear surface dips into the ground there.
// `wet` is how much of the four texels hold water at all.
//
// Shared by the water's draw and the plants' spawns (no tree in a lake).

float4 waterAt(Texture2D<float4> map, SamplerState pointSampler, float2 metres, float worldSize, float mapSize,
               out float wet) {
    const float2 p = metres / worldSize * mapSize - 0.5;
    const float2 i = floor(p), f = p - i;
    float4 v[4];
    wet = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 t = clamp(i + float2(k & 1, k >> 1), 0, mapSize - 1);
        v[k] = map.SampleLevel(pointSampler, (t + 0.5) / mapSize, 0);
        const float w = (k & 1 ? f.x : 1 - f.x) * (k & 2 ? f.y : 1 - f.y);
        wet += v[k].x > -1000 ? w : 0;
        if (v[k].x < -1000) v[k] = float4(-60, 0, 0, 0);
    }
    return lerp(lerp(v[0], v[1], f.x), lerp(v[2], v[3], f.x), f.y);
}
