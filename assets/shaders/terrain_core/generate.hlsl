// Fills a rectangle of one clipmap level: height and gradient per texel.
// The rect is in absolute texel indices; where it lands in the texture is
// that index modulo the clipmap size (toroidal), so a window that slides by a
// texel only ever regenerates the one strip that came into view.
//
// Then the rivers (rivers.hlsli): within reach of the river graph the ground
// becomes a bed under the water's surface and banks rising from it, cut at
// this level's own resolution - a brook only where a texel is small enough
// to hold it (further out the ground's shader paints it instead).

#include "height.hlsli"

#define RIVER_SEGMENTS register(t1, space0)
#define RIVER_TILES register(t2, space0)
#define RIVER_LIST register(t3, space0)
#include "rivers.hlsli"

RWTexture2D<float4> clipLevel : register(u0, space1);

// The ground `h` (height, dh/dx, dh/dy) at `p` with any river's bed and
// valley cut into it. Across the water a parabola down to the depth (where
// this level can hold the channel; a coarser one keeps a flat floor at the
// water, which the ground's shader paints); then the sides rising at 1:3
// from the water's edge, blended back into the ground over the valley's
// width. Only ever down: the graph laid the water under the ground all
// along the river (hydrology.cpp), so ground lower than the cut is left as it
// is rather than built up into a bank the water stands on.
float3 carveRiver(float3 h, float2 p, float levelSpacing) {
    const RiverHit r = nearestRiver(p);
    if (!r.found || r.distance >= r.halfWidth + r.bank) return h;
    const float valley = valleyResolved(r, levelSpacing);
    if (valley <= 0) return h;
    const float channel = riverResolved(r, levelSpacing);
    float3 cut;
    if (r.distance < r.halfWidth) {
        const float x = r.distance / max(r.halfWidth, 1e-3);
        const float depth = r.depth * channel;
        cut = float3(r.surface - depth * (1.0 - x * x), r.away * (depth * 2.0 * x / max(r.halfWidth, 1e-3)));
    } else {
        const float beyond = r.distance - r.halfWidth;
        const float x = beyond / r.bank;
        const float b = x * x * (3.0 - 2.0 * x), db = 6.0 * x * (1.0 - x) / r.bank;
        const float rise = r.surface + beyond * 0.33;
        cut = float3(lerp(rise, h.x, b), (1.0 - b) * 0.33 * r.away + b * h.yz + (h.x - rise) * db * r.away);
    }
    if (cut.x >= h.x) return h;
    return lerp(h, cut, valley);
}

[numthreads(8, 8, 1)]
void generateCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rectSize.x || id.y >= rectSize.y) return;
    const int2 t = rectOrigin + int2(id.xy);
    float3 h = terrainHeight(t);
    const float levelSpacing = spacing * float(1u << level);
    h = carveRiver(h, float2(t << level) * spacing, levelSpacing);
    const int n = int(clipSize);
    const int2 slot = ((t % n) + n) % n;
    clipLevel[slot] = float4(h, 0);
}
