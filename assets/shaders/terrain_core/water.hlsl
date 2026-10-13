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
// The biome ids (terrain_core::Renderer::biomeMap): which still water is marsh.
Texture2D<float> biomeVs : register(t1, space0);
SamplerState biomeVsSampler : register(s1, space0);
// The water's hue (BiomeMap::waterHue): 0 red (the marshes), 0.5 purple (the
// border with the dark forest), 1 the water's own colour.
Texture2D<float> hueVs : register(t2, space0);
SamplerState hueVsSampler : register(s2, space0);

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
    float3 kind : TEXCOORD9;               // how much of it is sea, river, marsh (the rest: a lake)
    float hue : TEXCOORD10;                // 0 red, 0.5 purple, 1 as it is
};

int2 wrapSlot(int2 t, uint size) { const int n = int(size); return ((t % n) + n) % n; }

#include "water_map.hlsli"

#define RIVER_SEGMENTS register(t3, space0)
#define RIVER_TILES register(t4, space0)
#define RIVER_LIST register(t5, space0)
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
    // What kind of water: the sea lies at level 0 in the map; still water
    // whose ground is marsh (biome 1) is marsh; a river's own channel below.
    float3 kind = float3(w.x < 0.05 ? 1.0 : 0.0, saturate(w.y * 4.0), 0.0);
    {
        float bw, bh;
        biomeVs.GetDimensions(bw, bh);
        const float2 bt = clamp(floor(metres / worldSize * bw), 0, bw - 1);
        const uint biome = uint(round(biomeVs.SampleLevel(biomeVsSampler, (bt + 0.5) / bw, 0) * 255.0));
        // (17: the marshes' edge where the dark forest closes in, biomes.json "bands")
        kind.z = (biome == 1u || biome == 17u) && kind.x < 0.5 ? 1.0 : 0.0;
    }
    const float hue = hueVs.SampleLevel(hueVsSampler, metres / worldSize, 0);
    if (river.found && river.distance < river.halfWidth + channelBank(river) && riverResolved(river, levelSpacing) > 0 &&
        (wet < 0.999 || river.surface > w.x)) {   // a lake's partial edge texel blends towards dry
        // Over dry ground past the water's edge the sheet only fills the
        // mesh under the banks; the pixel shader drops it wherever the
        // ground there is lower (beside a fall).
        riverInfo = float2(river.fall, wet > 0.5 ? -1.0 : river.distance - river.halfWidth);
        w = float4(river.surface, river.flow, river.along);
        wet = 1;
        kind = float3(0, 1, 0);
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
    o.kind = kind;
    o.hue = kind.x > 0.5 ? 1.0 : hue;
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
    o.kind = float3(1, 0, 0);
    o.hue = 1;
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

// Gradient noise as a slope, at its own scale and strength.
float2 noiseSlope(float2 p, float scale, float strength) {
    return noiseD(p / scale).yz / scale * strength;
}

// Slopes of open water: a few travelling waves across the wind, their height
// swelling and falling in patches, and two octaves of drifting noise so no
// lattice shows. `size` stretches the waves (the sea's swell is long, a
// lake's chop short), `amount` scales them (a lake is calm, a marsh still).
float2 openWaves(float2 p, float distance, float size, float amount, float drift, out float patch) {
    float2 slope = 0;
    const float swell = noiseD(p / (60.0 * size) + lookTime * 0.01).x;
    patch = swell;
    const float2 dirs[4] = {float2(0.94, 0.34), float2(-0.41, 0.91), float2(0.62, -0.78), float2(-0.88, -0.47)};
    const float lengths[4] = {23.0, 11.0, 5.3, 2.1};
    [unroll] for (int k = 0; k < 4; ++k) {
        const float len = lengths[k] * size;
        const float kw = 6.2831853 / len;
        const float amplitude = len * 0.008 * amount * (0.3 + 1.4 * swell) * (1.0 - saturate(distance / (len * 120.0)));
        const float phase = dot(p, dirs[k]) * kw - lookTime * sqrt(9.81 * kw);
        slope += dirs[k] * kw * amplitude * cos(phase);
    }
    const float2 d = float2(0.7, 0.4) * lookTime * drift;
    slope += noiseSlope(p + d, 6.5, 1.1 * amount) * (1.0 - saturate(distance / 900.0));
    slope += noiseSlope(p - d * 1.7 + 17.0, 1.9, 0.35 * amount) * (1.0 - saturate(distance / 250.0));
    return slope;
}

// A river's surface: its ripples carried downstream by the current (two
// phases of a flow map, cross-faded so the stretch never shows), faster where
// the river is big or falls steeply, with the slow wobble of big eddies.
float2 currentSlope(float2 p, float2 downstream, float strength, float fall, float distance) {
    const float speed = 1.2 + 3.5 * strength + 14.0 * min(fall, 0.3);
    const float cycle = 2.5;
    const float t0 = frac(lookTime / cycle), t1 = frac(lookTime / cycle + 0.5);
    const float blend = abs(1.0 - 2.0 * t0);
    const float2 across = float2(-downstream.y, downstream.x);
    // (squeezed along the current: streaks, not round ripples)
    const float2 q = float2(dot(p, downstream) * 0.45, dot(p, across));
    const float2 q0 = q - float2(t0 * cycle * speed * 0.45, 0);
    const float2 q1 = q - float2(t1 * cycle * speed * 0.45, 0) + 7.3;
    const float2 a = noiseD(q0 / 1.6).yz / 1.6 + noiseD(q0 / 0.55 + 3.1).yz / 0.55 * 0.35;
    const float2 b = noiseD(q1 / 1.6).yz / 1.6 + noiseD(q1 / 0.55 + 3.1).yz / 0.55 * 0.35;
    const float2 local = lerp(a, b, blend) * (0.35 + 0.5 * strength) * (1.0 - saturate(distance / 700.0));
    // back to the world: x along the current, y across it
    float2 slope = downstream * local.x * 0.45 + across * local.y;
    slope += noiseSlope(p - downstream * lookTime * speed * 0.6, 9.0, 0.5 * strength);
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
    // The kinds, as weights: sea, river, marsh; a lake is what is left.
    const float sea = saturate(input.kind.x);
    const float riverW = saturate(input.kind.y) * (1.0 - sea);
    const float marsh = saturate(input.kind.z) * (1.0 - sea) * (1.0 - riverW);
    const float lake = saturate(1.0 - sea - riverW - marsh);

    // The surface. The sea: a long swell and a chop; a lake: a light chop,
    // nearly a mirror; a marsh: all but still; a river: its current.
    float patch;
    const float2 downstream = input.flow.xy / max(length(input.flow.xy), 1e-4);
    const float fall = input.river.x;
    const float waveAmount = sea * 1.25 + lake * 0.16 + marsh * 0.05 + riverW * 0.12;
    float2 slope = openWaves(place, distance, lerp(1.0, 2.2, sea), waveAmount, lerp(0.35, 1.0, sea), patch);
    if (riverW > 0) slope += currentSlope(place, downstream, input.flow.z, fall, distance) * riverW;
    // The river's own slope: a waterfall is a steep sheet, lit as one.
    slope -= downstream * min(fall, 4.0) * riverW;
    const float3 n = normalize(float3(-slope, 1));
    const float3 toEye = -input.view / max(distance, 1e-3);

    const float sunLight = sunShadow(input.view, float3(0, 0, 1)) * cloudShadow(input.view);
    const float3 ambient = lerp(lightGround, lightZenith, 0.7) * 1.2 + lightSunColour * 0.25 * sunLight;
    // What the water itself sends back, by kind: the sea deep blue going
    // turquoise over the shallows, a lake dark and clear, a river a little
    // green and silty, a marsh brown-green and murky.
    const float3 seaBody = lerp(float3(0.040, 0.190, 0.190), float3(0.010, 0.055, 0.105), saturate(depth / 18.0));
    float3 own = seaBody * sea + float3(0.015, 0.070, 0.085) * lake + float3(0.035, 0.085, 0.070) * riverW +
                 float3(0.045, 0.050, 0.020) * marsh;
    // The marshes' water is red (doc/BIOME_PLAN.md 3.1), going purple at the
    // dark forest and the forest's blue past it.
    const float hue = saturate(input.hue);
    if (hue < 0.999) {
        const float3 red = float3(0.620, 0.070, 0.050), purple = float3(0.260, 0.070, 0.330);
        own = hue < 0.5 ? lerp(red, purple, hue * 2.0) : lerp(purple, own, (hue - 0.5) * 2.0);
    }
    const float3 body = own * ambient;
    // How much of the bed shows: thin water is clear, a marsh's is not.
    // (the red water is murky wherever it is, a lake's too)
    const float murk = lerp(1.2, 0.22 * sea + 0.18 * lake + 0.35 * riverW + 0.9 * marsh, hue);
    const float opacity = 1.0 - exp(-depth * murk);
    // Reflection: the sky (and the sun) by Fresnel; a lake the sharpest mirror.
    const float facing = saturate(dot(n, toEye));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    const float3 mirrored = reflect(-toEye, n);
    float3 reflection = skyColour(float3(mirrored.xy, abs(mirrored.z)));
    const float glint = lerp(600.0, 1400.0, lake);
    reflection += lightSunColour * pow(saturate(dot(mirrored, lightSun)), glint) * 30.0 * sunLight * (1.0 - 0.7 * marsh);
    // Foam: in the shallows at the shore, and where a river runs hard.
    // Broken, not a rim: a ragged band where the water thins, and streaks
    // where a river runs hard; both fade out with distance (no sparkle).
    const float churn = noiseD(place / 1.3 + float2(lookTime * 0.3, 0)).x * 0.6 + noiseD(place / 0.45).x * 0.4;
    const float near = 1.0 - saturate(distance / 1500.0);
    // A river is shallow all across: only its very edge foams; a lake's
    // shore barely, a marsh's not at all.
    const float shore = saturate((1.0 - depth / (input.flow.z > 0 ? 0.2 : 0.6)) * 1.6 - (1.0 - churn) * 1.2) *
                        (1.0 - 0.9 * marsh - 0.5 * lake);
    float foam = (shore * 0.7 + input.flow.z * 0.3 * smoothstep(0.55, 0.85, churn) * riverW) * (0.3 + 0.7 * near);
    // The surf: breakers rolling in to the sea's shore. Crests are lines of
    // equal depth running shorewards as time goes; each breaks white as the
    // water under it thins, and the wash spreads ragged behind it.
    if (sea > 0 && depth < 9.0) {
        const float wobble = noiseD(place / 14.0).x * 4.0 + noiseD(place / 4.0 + 5.0).x * 1.2;
        const float crest = frac(depth * 0.36 + lookTime * 0.13 + wobble * 0.2);
        // (a crest's face steep, its back a long fading wash)
        const float breaking = smoothstep(0.62, 0.9, crest) * (1.0 - smoothstep(0.93, 1.0, crest)) * smoothstep(0.45, 0.7, churn + 0.25);
        const float shoal = smoothstep(9.0, 2.0, depth);
        const float wash = smoothstep(1.4, 0.2, depth) * smoothstep(0.35, 0.75, churn);
        foam = max(foam, saturate(breaking * shoal * 1.1 + wash * 0.7) * sea * (0.5 + 0.5 * near));
    }
    // White water: rapids where the river steepens, and a fall all white -
    // streaks running down it, faster the steeper. Along the fall's own
    // length (its drop counts, not just the ground it spans), so a sheet
    // nearly vertical still streams rather than smearing into a band.
    const float rapids = smoothstep(0.02, 0.1, fall) * riverW;
    const float plunge = smoothstep(0.3, 0.8, fall) * riverW;
    if (rapids > 0) {
        const float across = dot(place, float2(-downstream.y, downstream.x));
        const float down = dot(place, downstream) + fmod(-input.level, 512.0);
        const float speed = 2.5 + 9.0 * plunge;
        const float streak = noiseD(float2(across / 0.8, (down - lookTime * speed) / 3.5)).x * 0.65 +
                             noiseD(float2(across / 0.3, (down - lookTime * speed * 1.3) / 1.2) + 31.0).x * 0.35;
        const float white = max(rapids * smoothstep(0.4, 0.75, churn) * 0.85, plunge * (0.55 + 0.45 * streak));
        foam = max(foam, white * (0.5 + 0.5 * near));
    }
    // (the red water is dull: less of the sky in it)
    reflection *= 0.2 + 0.8 * hue;
    float3 colour = body * opacity * (1.0 - fresnel) + reflection * fresnel;
    float alpha = saturate(max(opacity, plunge * 0.9) * (1.0 - fresnel) + fresnel);
    // A marsh's moss creeping off its banks onto the water, a ragged fringe in
    // the shallows only: no duckweed on the open red water (doc/BIOME_PLAN.md
    // 3.1). Matte, it hides the reflection.
    if (marsh > 0) {
        const float2 slow = float2(0.05, 0.03) * lookTime;
        const float raft = noiseD((place + slow) / 6.0).x * 0.55 + noiseD((place - slow) / 1.8 + 9.0).x * 0.3 +
                           noiseD(place / 0.7 + 21.0).x * 0.15;
        const float weed = smoothstep(0.62, 0.7, raft + 0.4 * smoothstep(0.9, 0.1, depth) - 0.15) * marsh * 0.95;
        // (moss: a deep green skin, freckled close by, even far off - no shimmer)
        const float freckle = lerp(noiseD(place / 0.35).x, 0.5, saturate(distance / 60.0));
        const float3 green = float3(0.07, 0.13, 0.03) * (0.75 + 0.5 * freckle) *
                             (lightSunColour * sunLight * 0.7 + ambient * 0.9);
        colour = lerp(colour, green, weed);
        alpha = lerp(alpha, 1.0, weed);
    }
    colour = lerp(colour, float3(0.92, 0.95, 0.96) * (lightSunColour * sunLight * 0.6 + lightZenith * 0.6), foam * 0.8);
    alpha = max(alpha, foam * 0.8);
    colour = applyFog(colour / max(alpha, 1e-3), input.view);
    return float4(colour, alpha);
}
