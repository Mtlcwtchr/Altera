// The per-model instance counters of foliage_spawn.hlsl: zeroed before the
// spawn, turned into indexed indirect draw arguments after it.
// (Each entry point here binds from slot 0 of its own: SDL counts bindings,
// it does not read register numbers.)

StructuredBuffer<uint4> modelSlots : register(t0, space0);   // instance offset, capacity, first index, index count
RWStructuredBuffer<uint> counters : register(u0, space1);
RWStructuredBuffer<uint> drawArgs : register(u1, space1);

cbuffer Count : register(b0, space2) {
    uint models;
    uint3 countPad;
};

[numthreads(64, 1, 1)]
void resetCs(uint3 id : SV_DispatchThreadID) {
    if (id.x < models) counters[id.x] = 0;
}

[numthreads(64, 1, 1)]
void argsCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= models) return;
    const uint4 slots = modelSlots[id.x];
    const uint i = id.x * 5;
    drawArgs[i + 0] = slots.w;                          // indices per instance
    drawArgs[i + 1] = min(counters[id.x], slots.y);     // instances
    drawArgs[i + 2] = slots.z;                          // first index
    drawArgs[i + 3] = 0;                                // vertex offset (indices are absolute)
    drawArgs[i + 4] = slots.x;                          // first instance
}
