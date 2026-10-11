// The river graph (src/terrain_core/hydrology.cpp, rivers.cpp): the nearest
// river to a point, from the segments listed in the point's tile.
//
// The includer names the three buffers' registers:
//   RIVER_SEGMENTS  three float4 a segment: (x0 y0 x1 y1) (surface0 surface1
//                   width0 width1) (depth0 depth1 flow bank), metres
//   RIVER_TILES     uint2 per tile (offset, count) after a header element
//                   (tile size as float bits, tiles a side)
//   RIVER_LIST      segment indices

StructuredBuffer<float4> riverSegments : RIVER_SEGMENTS;
StructuredBuffer<uint2> riverTiles : RIVER_TILES;
StructuredBuffer<uint> riverList : RIVER_LIST;

struct RiverHit {
    bool found;
    float distance;    // from the centre line, metres
    float halfWidth;   // of the water
    float surface;     // the water's height
    float depth;       // at the middle
    float bank;        // how far past the water the banks are shaped
    float flow;        // 0 a brook .. 1 the biggest river
    float fall;        // how fast the surface drops downstream (m per m): rapids, a waterfall
    float2 along;      // downstream, unit
    float2 away;       // from the centre line towards the point, unit
};

RiverHit noRiver() {
    RiverHit r;
    r.found = false;
    r.distance = 1e9;
    r.halfWidth = 0;
    r.surface = 0;
    r.depth = 0;
    r.bank = 0;
    r.flow = 0;
    r.fall = 0;
    r.along = float2(1, 0);
    r.away = float2(0, 1);
    return r;
}

// The point's tile of the river graph: (offset, count) into riverList, or
// none off the map.
uint2 riverSpan(float2 p) {
    const uint2 header = riverTiles[0];
    const float tile = asfloat(header.x);
    const int side = int(header.y);
    if (side == 0) return uint2(0, 0);
    const int2 t = int2(floor(p / tile));
    if (any(t < 0) || any(t >= side)) return uint2(0, 0);
    return riverTiles[1 + uint(t.y * side + t.x)];
}

// Segment `index` seen from `p`; `u` is where along it p falls, unclamped
// (0 its start, 1 its end).
RiverHit riverSegment(uint index, float2 p, out float u) {
    const uint s = index * 3;
    const float4 a = riverSegments[s], b = riverSegments[s + 1], c = riverSegments[s + 2];
    const float2 e = a.zw - a.xy;
    u = dot(p - a.xy, e) / max(dot(e, e), 1e-6);
    const float uc = saturate(u);
    const float2 q = a.xy + e * uc;
    RiverHit r;
    r.found = true;
    r.distance = length(p - q);
    r.halfWidth = lerp(b.z, b.w, uc) * 0.5;
    r.surface = lerp(b.x, b.y, uc);
    r.depth = lerp(c.x, c.y, uc);
    r.bank = c.w;
    r.flow = c.z;
    r.fall = max(b.x - b.y, 0.0) / max(length(e), 1e-3);
    r.along = e / max(length(e), 1e-4);
    r.away = r.distance > 1e-4 ? (p - q) / r.distance : float2(-r.along.y, r.along.x);
    return r;
}

// Nearest by the edge of the water, not the centre line: where a brook joins
// a big river, a point in the big river belongs to it.
RiverHit nearestRiver(float2 p) {
    RiverHit r = noRiver();
    const uint2 span = riverSpan(p);
    float best = 1e9;
    for (uint k = 0; k < span.y; ++k) {
        float u;
        const RiverHit h = riverSegment(riverList[span.x + k], p, u);
        if (h.distance - h.halfWidth >= best || h.distance > h.halfWidth + h.bank) continue;
        best = h.distance - h.halfWidth;
        r = h;
    }
    return r;
}

// The banks of the channel itself (hydrology.cpp's formula). `bank` in a
// hit is more: as far as the valley the river cuts reaches, three to one
// from the water, where it had to cut through a rise.
float channelBank(RiverHit r) {
    return max(6.0, 2.0 * r.halfWidth) + 4.0 * r.depth;
}

// How much of a river's channel a grid of `spacing` metres can hold: none
// when the water and its banks are under a texel across, all from two.
float riverResolved(RiverHit r, float spacing) {
    return saturate((r.halfWidth + channelBank(r)) / spacing - 1.0);
}

// The same for the valley it cuts.
float valleyResolved(RiverHit r, float spacing) {
    return saturate((r.halfWidth + r.bank) / spacing - 1.0);
}
