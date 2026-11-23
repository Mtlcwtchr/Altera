// AL-78: the trees the game holds (terrain_core::forestHold, every one a tree
// the people can fell), appended to the foliage's instances after the
// scattering - which leaves the held layers out inside the game's rect. One
// thread a tree: out of range, out of the frustum or under a pixel and a half
// on screen it is dropped, as a scattered one would be; else it is drawn by
// the same model, wind, light and shadow.

struct Instance {
    float4 positionYaw;   // eye-relative position (m), yaw
    float4 extra;         // scale, texture layer, fade, height (m)
};

StructuredBuffer<float4> held : register(t0, space0);        // two a tree: x, y, z, yaw; model, scale, range, -
StructuredBuffer<float4> modelInfo : register(t1, space0);   // radius, height, texture layer, -
StructuredBuffer<uint4> modelSlots : register(t2, space0);   // instance offset, capacity, first index, index count
RWStructuredBuffer<Instance> instances : register(u0, space1);
RWStructuredBuffer<uint> counters : register(u1, space1);

cbuffer Hold : register(b0, space2) {
    float4 planes[6];   // eye-relative frustum, inside >= 0
    float4 eye;         // world metres
    uint count;
    float pixelScale;
    float minPixels;
    float holdPad;
};

bool sphereOutside(float3 centre, float radius) {
    [unroll] for (int k = 0; k < 6; ++k)
        if (dot(planes[k].xyz, centre) + planes[k].w < -radius) return true;
    return false;
}

[numthreads(64, 1, 1)]
void holdCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    const float4 a = held[id.x * 2], b = held[id.x * 2 + 1];
    const uint model = uint(b.x);
    const float scale = b.y, range = b.z;
    const float3 base3 = a.xyz - eye.xyz;
    const float distance = length(base3.xy);
    if (distance > range) return;
    const float fade = 1.0 - smoothstep(0.7 * range, range, distance);
    const float4 info4 = modelInfo[model];
    const float radius = info4.x * scale, height = info4.y * scale;
    if (sphereOutside(base3 + float3(0, 0, height * 0.5), radius)) return;
    if (radius * pixelScale / max(distance, 1.0) < minPixels) return;
    uint slot;
    InterlockedAdd(counters[model], 1u, slot);
    const uint4 slots = modelSlots[model];
    if (slot >= slots.y) return;
    Instance o;
    o.positionYaw = float4(base3, a.w);
    o.extra = float4(scale, info4.z, fade, height);
    instances[slots.x + slot] = o;
}
