// The desert mirage round the gate A-3 (doc/megaliths/A3_DESERT_GATE.md):
// seen at a grazing look from far enough off, the flat sand round the gate
// turns to "water" - an inferior mirage: the ground shows the sky just above
// the horizon and, between the eye and the gate, the gate itself upside down,
// all of it trembling in the heat. Nearer, or looking down more steeply, the
// sand is sand again (the mirage recedes as you walk on).
//
// Needs lighting.hlsli (skyColour, the light, lightTime). The gate comes from
// the props (Props::placed, the instance with "look": "a3_desert_gate"),
// eye-relative, through the terrain's Look uniforms.

#ifndef MIRAGE_HLSLI
#define MIRAGE_HLSLI

// The gate's box and arch in its own frame (metres at scale 1; front = -y),
// from tools/blender/megalith_a3_desert_gate.py V1: 36 x 30 x 190, the left
// wall flaring and the right side slanting out low (a box round both), the
// arch 21 m wide with its centre 1.1 m right, springing at 169.6 m.
static const float3 kGateLo = float3(-24.0, -15.0, 0.0);
static const float3 kGateHi = float3(26.0, 15.0, 190.0);
static const float kArchX = 1.08, kArchR = 10.44, kArchSpring = 169.5;

static const float kMirageRadius = 4500.0;   // round the gate, metres
static const float kMirageNear = 45.0;       // nearer than this it is sand (an eye 1.8 m up: 2.3 deg below the horizon)
static const float kMirageGrazing = 0.06;    // below this sine of the look down (3.4 deg), it is water

// Colour of the gate seen along `dir` from `origin` (both in the gate's frame,
// metres at scale 1); false when the ray misses it.
bool mirageGateHit(float3 origin, float3 dir, out float3 colour) {
    colour = 0;
    const float3 inv = 1.0 / select(abs(dir) > 1e-6, dir, float3(1e-6, 1e-6, 1e-6));
    const float3 t0 = (kGateLo - origin) * inv, t1 = (kGateHi - origin) * inv;
    const float3 tmin = min(t0, t1), tmax = max(t0, t1);
    const float enter = max(max(tmin.x, tmin.y), tmin.z), leave = min(min(tmax.x, tmax.y), tmax.z);
    if (leave < max(enter, 0.0)) return false;
    const float3 hit = origin + dir * enter;
    // which face: its normal, for the light
    float3 n = float3(0, 0, 1);
    if (enter == tmin.x) n = float3(-sign(dir.x), 0, 0);
    else if (enter == tmin.y) n = float3(0, -sign(dir.y), 0);
    const bool front = n.y < -0.5;
    const float ax = hit.x - kArchX;
    const bool arch = front && abs(ax) < kArchR &&
                      hit.z < kArchSpring + sqrt(max(kArchR * kArchR - ax * ax, 0.0));
    colour = arch ? float3(0.02, 0.02, 0.025) : float3(0.72, 0.71, 0.68);
    if (!arch) colour *= 0.55 * lerp(lightGround, lightZenith, 0.6) + lightSunColour * 0.8 * saturate(dot(n, lightSun) * 0.8 + 0.2);
    return true;
}

// `view`: eye to ground point (metres); `metres`: its world xy (for the
// shimmer); `up`: the ground's normal z; `gate`: the gate's origin, eye-
// relative (xyz) and its turn (w); `scale`: its scale, 0 when there is none.
float3 applyMirage(float3 lit, float3 view, float2 metres, float up, float4 gate, float scale) {
    if (scale <= 0.0) return lit;
    const float dist = length(view.xy);
    const float around = 1.0 - smoothstep(0.6 * kMirageRadius, kMirageRadius, length(view.xy - gate.xy));
    if (around <= 0.0 || dist < kMirageNear) return lit;
    const float3 v = view / max(length(view), 1e-3);
    // The heat: slow ripples running across the band and quick trembling.
    const float t = lightTime;
    const float ripple = sin(metres.x * 0.021 + metres.y * 0.013 + t * 1.3 + 2.0 * sin(metres.y * 0.007 - t * 0.6))
                       + 0.5 * sin(metres.x * 0.09 - metres.y * 0.07 + t * 4.1);
    const float down = -v.z + 0.0025 * ripple;
    const float water = (1.0 - smoothstep(0.55 * kMirageGrazing, kMirageGrazing, down))
                      * smoothstep(kMirageNear, 3.0 * kMirageNear, dist + 25.0 * ripple) * around * smoothstep(0.82, 0.94, up)
                      * smoothstep(-0.004, 0.002, down);   // only below the horizon: not up a dune's face
    if (water <= 0.0) return lit;
    // What the "water" shows: the ray turned up at the ground (bent a touch,
    // trembling), the gate where it meets it, else the sky low over the horizon.
    float3 r = normalize(float3(v.xy + 0.002 * ripple * float2(-v.y, v.x), max(abs(v.z), 0.003) + 0.0015 * ripple));
    // (the sky a little higher than the bend alone gives: the painted mirages read as blue water)
    float3 seen = skyColour(float3(r.xy, 0.3 + r.z));
    const float c = cos(gate.w), s = sin(gate.w);
    const float3 rel = (view - gate.xyz) / scale;           // the point in the gate's frame, turned below
    const float3 o = float3(rel.x * c + rel.y * s, -rel.x * s + rel.y * c, rel.z);
    const float3 d = float3(r.x * c + r.y * s, -r.x * s + r.y * c, r.z);
    float3 gateColour;
    if (mirageGateHit(o, d, gateColour)) seen = gateColour;
    // a bright, slightly paler sheet, as the painted mirages are
    seen = lerp(seen, lightHorizon * 1.05, 0.08);
    return lerp(lit, seen, saturate(water) * 0.92);
}

#endif
