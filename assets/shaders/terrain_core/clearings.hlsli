// Clearings (src/terrain_core/clearings.hpp): discs of the world where
// plants do not grow; full growth again past the outer radius.
#ifndef CLEARINGS_REGISTER
#define CLEARINGS_REGISTER register(b1, space2)
#endif

cbuffer Clearings : CLEARINGS_REGISTER {
    float4 clearingInfo;      // count
    float4 clearingDiscs[32]; // world x, y, inner, outer
};

// 0 inside a clearing, 1 out in the wild, smooth between.
float clearingKeep(float2 world) {
    float keep = 1.0;
    const uint n = min((uint)clearingInfo.x, 32u);
    for (uint i = 0; i < n; ++i) {
        const float4 d = clearingDiscs[i];
        keep = min(keep, smoothstep(d.z, d.w, length(world - d.xy)));
    }
    return keep;
}
