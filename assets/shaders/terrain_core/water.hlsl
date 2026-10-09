// Water: the sea, lakes and rivers within the world, drawn over the terrain's
// own patches with the height of the water map instead of the ground's
// (hydrology.cpp), and the open ocean beyond the world's edge, a disc of rings
// round the eye. Transparent, after everything opaque.
//
// The depth of the water at a pixel is the water's level less the ground's,
// both exact: the ground from the same clipmap the terrain is drawn from. So
// the shallows show the bed, the deep goes blue-green, the shore foams.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative
    float eyeZ;
    float spacing;
    uint clipSize;
    uint grid;
    uint levels;
    float rangeFactor;
    float morphStart;
    float pad0;
};

cbuffer WaterView : register(b1, space1) {
    float worldSize;
    float mapSize;          // texels a side of the water map
    float eyeX;             // the eye's absolute position (floats: for the ocean's edge only)
    float eyeY;
};

Texture2D<float4> waterMapVs : register(t0, space0);
SamplerState waterVsSampler : register(s0, space0);

struct PatchIn {
    uint vertex : SV_VertexID;
    int4 origin : TEXCOORD0;
    float4 rel : TEXCOORD1;
};

struct VsOut {
    float4 position : SV_Position;
    float3 view : TEXCOORD0;               // eye to point, metres
    float level : TEXCOORD1;               // the water's surface height
    float3 flow : TEXCOORD2;               // direction x, y; strength
    float2 local : TEXCOORD3;              // the ground under it, as the terrain reads it
    nointerpolation int2 slot : TEXCOORD4;
    nointerpolation uint clipLevel : TEXCOORD5;
    float wet : TEXCOORD6;                 // 1 where there is water
    nointerpolation float ocean : TEXCOORD7;   // 1: the open ocean beyond the world
    float2 river : TEXCOORD8;              // the river's fall (m per m); how far past its water's edge (m)
};

int2 wrapSlot(int2 t, uint size) { const int n = int(size); return ((t % n) + n) % n; }

#include "water_map.hlsli"

#define RIVER_SEGMENTS register(t1, space0)
#define RIVER_TILES register(t2, space0)
#define RIVER_LIST register(t3, space0)
#include "rivers.hlsli"

VsOut waterVs(PatchIn input) {
    const uint side = grid + 1;
    const float2 local = float2(input.vertex % side, input.vertex / side);
    const uint level = uint(input.origin.z);
    const float levelSpacing = spacing * float(1u << level);
    const float2 metres = (float2(input.origin.xy) + local) * levelSpacing;
    float wet;
    float4 w = waterAt(waterMapVs, waterVsSampler, metres, worldSize, mapSize, wet);
    // A river, where this level's mesh is fine enough to hold its channel
    // (the ground is cut to the same rule): its surface over the water and
    // the channel's banks, not the whole valley - the banks rise above it,
    // so the water shows only over the bed.
    const RiverHit river = nearestRiver(metres);
    float2 riverInfo = float2(0, -1);
    if (river.found && river.distance < river.halfWidth + channelBank(river) && riverResolved(river, levelSpacing) > 0 &&
        (wet < 0.999 || river.surface > w.x)) {   // a lake's partial edge texel blends towards dry
        // Over dry ground past the water's edge the sheet only fills the
        // mesh under the banks; the pixel shader drops it wherever the
        // ground there is lower (beside a fall).
        riverInfo = float2(river.fall, wet > 0.5 ? -1.0 : river.distance - river.halfWidth);
        w = float4(river.surface, river.flow, river.along);
        wet = 1;
    }
    VsOut o;
    o.view = float3(input.rel.xy + local * levelSpacing, w.x - eyeZ);
    o.position = mul(viewProjection, float4(o.view, 1));
    o.level = w.x;
    o.flow = float3(w.zw, w.y);
    o.local = local;
    o.slot = wrapSlot(input.origin.xy, clipSize);
    o.clipLevel = level;
    o.wet = wet;
    o.ocean = 0;
    o.river = riverInfo;
    return o;
}

// The open ocean: rings round the eye out to the horizon, at sea level,
// clipped away inside the world (where the patches above draw the sea).
VsOut oceanVs(uint id : SV_VertexID) {
    const uint segments = 96;
    const uint quad = id / 6, corner = id % 6;
    const uint ring = quad / segments, segment = quad % segments;
    const uint dr = (corner == 2 || corner == 4 || corner == 5) ? 1 : 0;
    const uint ds = (corner == 1 || corner == 2 || corner == 4) ? 1 : 0;
    const float radius = 50.0 * pow(1.12, float(ring + dr)) - 50.0;
    const float angle = float(segment + ds) * 6.2831853 / float(segments);
    VsOut o;
    o.view = float3(cos(angle) * radius, sin(angle) * radius, -eyeZ);
    o.position = mul(viewProjection, float4(o.view, 1));
    o.level = 0;
    o.flow = 0;
    o.local = 0;
    o.slot = 0;
    o.clipLevel = 0;
    o.wet = 1;
    o.ocean = 1;
    o.river = float2(0, -1);
    return o;
}

Texture2DArray<float4> clipPs : register(t0, space2);
SamplerState clipPsSampler : register(s0, space2);

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t1, space2)
#define SHADOW_SAMPLER register(s1, space2)
#include "lighting.hlsli"

cbuffer WaterLook : register(b1, space3) {
    float lookWorldSize;
    float lookEyeX, lookEyeY;
    uint lookClipSize;
    float2 lookEyeWrapped;     // the eye modulo 4096 m: where the waves are
    float lookTime;
    float lookPad;
};

float groundUnder(int2 slot, float2 local, uint level) {
    const float2 i = floor(local), f = local - i;
    float h[4];
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 t = wrapSlot(slot + int2(i) + int2(k & 1, k >> 1), lookClipSize);
        h[k] = clipPs.SampleLevel(clipPsSampler, float3((float2(t) + 0.5) / float(lookClipSize), float(level)), 0).x;
    }
    return lerp(lerp(h[0], h[1], f.x), lerp(h[2], h[3], f.x), f.y);
}

// Value noise and its gradient, quintic: what breaks the waves' pattern up.
float hash2(float2 c) { return frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453); }
float3 noiseD(float2 p) {
    const float2 i = floor(p), f = p - i;
    const float2 u = f * f * f * (f * (f * 6 - 15) + 10), du = 30 * f * f * (f * (f - 2) + 1);
    const float a = hash2(i), b = hash2(i + float2(1, 0)), c = hash2(i + float2(0, 1)), d = hash2(i + 1);
    return float3(a + (b - a) * u.x + (c - a) * u.y + (a - b - c + d) * u.x * u.y,
                  du * float2(b - a + (a - b - c + d) * u.y, c - a + (a - b - c + d) * u.x));
}

// Slopes of the water's surface: a few travelling waves across the wind,
// their height swelling and falling in patches, and over them two octaves of
// drifting noise so no lattice shows; on a river, ripples running downstream.
float2 waveSlope(float2 p, float3 flow, float distance, out float patch) {
    float2 slope = 0;
    const float swell = noiseD(p / 60.0 + lookTime * 0.01).x;
    patch = swell;
    const float2 dirs[4] = {float2(0.94, 0.34), float2(-0.41, 0.91), float2(0.62, -0.78), float2(-0.88, -0.47)};
    const float lengths[4] = {23.0, 11.0, 5.3, 2.1};
    [unroll] for (int k = 0; k < 4; ++k) {
        const float kw = 6.2831853 / lengths[k];
        const float amplitude = lengths[k] * 0.008 * (0.3 + 1.4 * swell) *
                                (1.0 - saturate(distance / (lengths[k] * 120.0)));
        const float phase = dot(p, dirs[k]) * kw - lookTime * sqrt(9.81 * kw);
        slope += dirs[k] * kw * amplitude * cos(phase);
    }
    const float2 drift = float2(0.7, 0.4) * lookTime;
    const float3 n1 = noiseD((p + drift) / 6.5), n2 = noiseD((p - drift * 1.7) / 1.9 + 17.0);
    slope += n1.yz / 6.5 * 1.1 * (1.0 - saturate(distance / 900.0));
    slope += n2.yz / 1.9 * 0.35 * (1.0 - saturate(distance / 250.0));
    if (flow.z > 0) {
        const float along = dot(p, flow.xy) * 0.9 - lookTime * (2.0 + 4.0 * flow.z);
        const float across = dot(p, float2(-flow.y, flow.x)) * 0.35;
        slope += flow.xy * cos(along + sin(across) * 1.7) * 0.18 * flow.z;
    }
    return slope;
}

float4 waterPs(VsOut input) : SV_Target0 {
    if (input.ocean > 0.5) {
        // Inside the world the patches draw the sea; here only what is beyond.
        const float2 world = float2(lookEyeX, lookEyeY) + input.view.xy;
        if (all(world >= 0) && all(world <= lookWorldSize)) discard;
    } else if (input.wet < 0.5) {
        discard;
    }
    const float distance = length(input.view);
    const float depth = input.ocean > 0.5 ? 200.0 : max(input.level - groundUnder(input.slot, input.local, input.clipLevel), 0.0);
    // A river's sheet past its water's edge, over ground lower than its
    // surface: beside a fall, where the banks drop away. Not water.
    if (input.ocean < 0.5 && input.river.y > 0.0 && depth > 0.25) discard;
    const float2 place = input.view.xy + lookEyeWrapped;
    float patch;
    float2 slope = waveSlope(place, input.flow, distance, patch);
    // The river's own slope: a waterfall is a steep sheet, lit as one.
    const float fall = input.river.x;
    const float2 downstream = input.flow.xy / max(length(input.flow.xy), 1e-4);
    slope -= downstream * min(fall, 4.0);
    const float3 n = normalize(float3(-slope, 1));
    const float3 toEye = -input.view / max(distance, 1e-3);

    const float sunLight = sunShadow(input.view, float3(0, 0, 1)) * cloudShadow(input.view);
    // What the water itself sends back: dark, blue-green, lit by the sky and a little sun.
    const float3 body = float3(0.02, 0.09, 0.11) * (lerp(lightGround, lightZenith, 0.7) * 1.2 + lightSunColour * 0.25 * sunLight);
    // How much of the bed shows: thin water is clear.
    const float opacity = 1.0 - exp(-depth * 0.22);
    // Reflection: the sky (and the sun) by Fresnel.
    const float facing = saturate(dot(n, toEye));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    const float3 mirrored = reflect(-toEye, n);
    float3 reflection = skyColour(float3(mirrored.xy, abs(mirrored.z)));
    reflection += lightSunColour * pow(saturate(dot(mirrored, lightSun)), 600.0) * 30.0 * sunLight;
    // Foam: in the shallows at the shore, and where a river runs hard.
    // Broken, not a rim: a ragged band where the water thins, and streaks
    // where a river runs hard; both fade out with distance (no sparkle).
    const float churn = noiseD(place / 1.3 + float2(lookTime * 0.3, 0)).x * 0.6 + noiseD(place / 0.45).x * 0.4;
    const float near = 1.0 - saturate(distance / 1500.0);
    // A river is shallow all across: only its very edge foams.
    const float shore = saturate((1.0 - depth / (input.flow.z > 0 ? 0.2 : 0.6)) * 1.6 - (1.0 - churn) * 1.2);
    float foam = (shore * 0.7 + input.flow.z * 0.3 * smoothstep(0.55, 0.85, churn)) * (0.3 + 0.7 * near);
    // White water: rapids where the river steepens, and a fall all white -
    // streaks running down it, faster the steeper. Along the fall's own
    // length (its drop counts, not just the ground it spans), so a sheet
    // nearly vertical still streams rather than smearing into a band.
    const float rapids = smoothstep(0.02, 0.1, fall);
    const float plunge = smoothstep(0.3, 0.8, fall);
    if (rapids > 0) {
        const float across = dot(place, float2(-downstream.y, downstream.x));
        const float down = dot(place, downstream) + fmod(-input.level, 512.0);
        const float speed = 2.5 + 9.0 * plunge;
        const float streak = noiseD(float2(across / 0.8, (down - lookTime * speed) / 3.5)).x * 0.65 +
                             noiseD(float2(across / 0.3, (down - lookTime * speed * 1.3) / 1.2) + 31.0).x * 0.35;
        const float white = max(rapids * smoothstep(0.4, 0.75, churn) * 0.85, plunge * (0.55 + 0.45 * streak));
        foam = max(foam, white * (0.5 + 0.5 * near));
    }
    float3 colour = body * opacity * (1.0 - fresnel) + reflection * fresnel;
    float alpha = saturate(max(opacity, plunge * 0.9) * (1.0 - fresnel) + fresnel);
    colour = lerp(colour, float3(0.92, 0.95, 0.96) * (lightSunColour * sunLight * 0.6 + lightZenith * 0.6), foam * 0.8);
    alpha = max(alpha, foam * 0.8);
    colour = applyFog(colour / max(alpha, 1e-3), input.view);
    return float4(colour, alpha);
}
