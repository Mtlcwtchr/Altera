// The grass cover's counter: zeroed before the spawn, turned into the
// arguments of its one indirect draw after it (12 vertices a tuft: two
// crossed cards).

RWStructuredBuffer<uint> tuftCount : register(u0, space1);
RWStructuredBuffer<uint> drawArgs : register(u1, space1);

cbuffer Capacity : register(b0, space2) {
    uint capacity;
    uint3 capacityPad;
};

[numthreads(1, 1, 1)]
void resetCs(uint3 id : SV_DispatchThreadID) {
    tuftCount[0] = 0;
}

[numthreads(1, 1, 1)]
void argsCs(uint3 id : SV_DispatchThreadID) {
    drawArgs[0] = 12;                               // vertices per instance
    drawArgs[1] = min(tuftCount[0], capacity);      // instances
    drawArgs[2] = 0;                                // first vertex
    drawArgs[3] = 0;                                // first instance
}
