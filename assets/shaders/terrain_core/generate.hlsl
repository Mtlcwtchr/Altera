// Fills a rectangle of one clipmap level: height and gradient per texel.
// The rect is in absolute texel indices; where it lands in the texture is
// that index modulo the clipmap size (toroidal), so a window that slides by a
// texel only ever regenerates the one strip that came into view.

#include "height.hlsli"

RWTexture2D<float4> clipLevel : register(u0, space1);

[numthreads(8, 8, 1)]
void generateCs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rectSize.x || id.y >= rectSize.y) return;
    const int2 t = rectOrigin + int2(id.xy);
    const float3 h = terrainHeight(t);
    const int n = int(clipSize);
    const int2 slot = ((t % n) + n) % n;
    clipLevel[slot] = float4(h, 0);
}
