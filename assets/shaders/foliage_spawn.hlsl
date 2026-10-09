// Where the ground cover stands, decided on the card every frame: no
// candidate is placed, kept or uploaded by the CPU.
//
// Nested square windows of world cells around the eye, one level a doubling
// of the cell (2 m, 4 m, 8 m ...), each `side` cells across. A cell's root is
// a point fixed to the world - a hash of its integer cell - so a window that
// moves only changes which cells are in it, never where any root stands.
//
// The levels nest: a level's root IS one of the four roots of the level
// under it (the descendant its hash picks, down to the 2 m cell), so where
// one level hands over to the next, a quarter of the roots carry on - grown
// to stand for the four - and the other three fade out where they stand. No
// spacing that jumps, no field that is rebuilt.
//
// Written into the 32-byte root layout the cull and the plant shaders already
// read (world::PageGrassRoot): position, then z again, the fade, the scale and
// the upright, then the run code (level, dataset, log2 cell). A cell with
// nothing under it is pushed far below the world, where the cull drops it.

// The page fields' lookup (terrain_pages.hlsli) reads these from the ground
// draw's block; here they come from this dispatch's own.
static const float4 morphing = 0.0;
static const float4 morphReplacement = 0.0;
static const float4 envSwitches = 0.0;
cbuffer Spawn : register(b0, space2)
{
    float4 morphWindow;    // z: height of nought, w: metres a unit
    float4 morphSettings;  // xy: page table size
    float4 centre;         // xy: the eye; z: the finest level; w: how far the finest has faded (0..1)
    float4 shape;          // x: levels, y: side (cells), z: first slot, w: -
};
#include "terrain_pages.hlsli"

RWByteAddressBuffer roots : register(u0, space1);

uint spawnHash(uint x, uint y)
{
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

// Which of the four children of a level's cell the level above keeps.
uint2 keptChild(int2 parent, int level)
{
    const uint h = spawnHash(uint(parent.x) ^ (uint(level) * 0x9e3779b9u), uint(parent.y) ^ 0x51ed270bu);
    return uint2(h & 1u, (h >> 1) & 1u);
}

// The world position of a level's cell's root: the 2 m root of the descendant
// the levels keep, jittered inside its own 2 m cell.
float2 rootOf(int2 cell, int level)
{
    int2 c = cell;
    for (int k = level; k > 0; --k) c = c * 2 + int2(keptChild(c, k));
    const uint h = spawnHash(uint(c.x) ^ 0x3c6ef372u, uint(c.y) ^ 0xa54ff53au);
    const float2 jitter = float2(h & 0xffffu, h >> 16) / 65535.0;
    return (float2(c) + 0.1 + 0.8 * jitter) * 2.0;
}

// The size a card at this spacing stands at: one at 2 m, wider with the cell
// so the cover keeps its look as the roots thin (the coarse cards' old rule).
float spacingScale(float cellMetres) { return max(1.0, min(cellMetres, 64.0) * 0.32); }

void writeRoot(uint slot, float3 p, float fade, float scale, float upright, float run)
{
    roots.Store4(slot * 32, asuint(float4(p, p.z)));
    roots.Store4(slot * 32 + 16, asuint(float4(fade, scale, upright, run)));
}

[numthreads(64, 1, 1)]
void SpawnCS(uint3 id : SV_DispatchThreadID)
{
    const uint side = uint(shape.y), perLevel = side * side;
    const uint levels = uint(shape.x);
    if (id.x >= levels * perLevel) return;
    const uint slot = uint(shape.z) + id.x;
    const uint band = id.x / perLevel, local = id.x % perLevel;
    const int finest = int(centre.z);
    const int level = finest + int(band);
    const float cellMetres = 2.0 * exp2(float(level));
    const int2 origin = int2(floor(centre.xy / cellMetres)) - int(side / 2);
    const int2 cell = origin + int2(local % side, local / side);
    const float2 p = rootOf(cell, level);
    const float3 hidden = float3(p, -1.0e9);

    // How far out this root is, as the windows measure it (they are squares),
    // and the band each level owns: out to its reach, from three quarters of
    // the reach of the level under it (the hand-over, where both draw).
    const float2 d2 = abs(p - centre.xy);
    const float d = max(d2.x, d2.y);
    const float reach = (float(side / 2) - 1.0) * cellMetres;
    const float underReach = reach * 0.5;
    if (d >= reach) { writeRoot(slot, hidden, 0, 0, 1, 0); return; }

    // Does this root carry on into the level above?
    const int2 parent = int2(floor(float2(cell) * 0.5));
    const bool carries = all(uint2(cell - parent * 2) == keptChild(parent, level + 1));
    // Out across this level's own hand-over to the level above, the roots that
    // carry on are the level above's to draw; the others fade.
    const float handOver = smoothstep(reach * 0.75, reach, d);
    float fade = 1.0;
    float scale = spacingScale(cellMetres);
    const bool top = band + 1 >= levels;
    if (!top && d >= reach * 0.75) {
        if (carries) { writeRoot(slot, hidden, 0, 0, 1, 0); return; }
        fade = 1.0 - handOver;
    }
    // The last level: the cover thins out to nothing at its reach, not an edge.
    if (top) fade = 1.0 - handOver;
    if (band > 0) {
        // Inside the level under it this root is that level's.
        if (d < underReach * 0.75) { writeRoot(slot, hidden, 0, 0, 1, 0); return; }
        // Grown from the size of the roots it stood among to its own.
        // Next to the finest level, as far grown as the finest has thinned, so
        // when the finest steps up a level the two agree on every size.
        float grown = smoothstep(underReach * 0.75, underReach, d);
        if (band == 1) grown = max(grown, centre.w);
        scale = lerp(spacingScale(cellMetres * 0.5), scale, grown);
    } else {
        // The finest level as the eye rises: the roots that will not carry on
        // thin out, the ones that do grow, until the next level is the finest.
        if (!carries) fade *= 1.0 - centre.w;
        else scale = lerp(scale, spacingScale(cellMetres * 2.0), centre.w);
    }
    if (fade <= 0.001) { writeRoot(slot, hidden, 0, 0, 1, 0); return; }

    // The ground under it: the finest page data resident there.
    int data = -1;
    [loop] for (int k = 0; k < kPageDatasets; ++k)
        if (pageAddress(p, k).z != 0.0) { data = k; break; }
    if (data < 0) { writeRoot(slot, hidden, 0, 0, 1, 0); return; }
    const float z = pageHeight(p, data);
    const float upright = pageNormal(p, data, max(2.0, pageStepOf(data))).z;
    uint logCell = 0;
    while ((2u << logCell) <= uint(cellMetres) && logCell < 14u) ++logCell;
    // decodeGrassRun (foliage_pages.hlsl): dataset, parent dataset, log2 cell, morph.
    const uint run = uint(data) | (uint(data) << 3) | (logCell << 6);
    writeRoot(slot, float3(p, z), fade, scale, upright, float(run));
}
