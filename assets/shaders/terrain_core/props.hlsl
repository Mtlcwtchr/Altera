// Mega structures (terrain_core/props.cpp): placed models, one instanced draw
// per model and level. No texture of their own: the ground's material array
// laid on from three sides in world space, two scales of it, so a spire is
// the same rock as the canyon round it and holds up from a kilometre off and
// from its foot. Lit, shadowed and hazed exactly as the ground.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative (the light's, in the shadow pass)
    float4 wind;
    float4 eyeWrapped;
};

struct VertexIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;            // cavity occlusion, height share up the model
    float4 positionYaw : TEXCOORD3;   // instance: eye-relative origin, yaw
    float4 scale : TEXCOORD4;         // instance: scale x, y, z; material layer
    float4 anchor : TEXCOORD5;        // instance: origin x, y wrapped to 4096 m; origin z; variation
    float4 extra : TEXCOORD6;         // instance: cover (steppe on its tops), a cliff's two biomes (-1: none; -2: an A-1 slab), mirror
    float blend : TEXCOORD7;          // a cliff's vertex: how far it wears the second biome; an A-1 slab's: its _EDGE
};

struct VsOut {
    float4 position : SV_Position;
    float3 view : TEXCOORD0;          // eye-relative
    float3 normal : TEXCOORD1;
    float3 place : TEXCOORD2;         // texture space: wrapped world metres
    float2 uv : TEXCOORD3;
    nointerpolation float2 layer : TEXCOORD4;   // material layer, variation
    nointerpolation float hero : TEXCOORD5;     // 1: a megalith
    nointerpolation float cover : TEXCOORD6;    // 0..1: ground on its tops
    nointerpolation float mirror : TEXCOORD9;   // 0..1: mirrors the sky
    float blend : TEXCOORD7;                    // a cliff: the second biome's share
    nointerpolation float2 biomes : TEXCOORD8;  // a cliff: its two biomes (-1: not a cliff; -2: an A-1 slab)
    float3 local : TEXCOORD10;                  // object space, unscaled (an A-1 slab's material)
    float3 localNormal : TEXCOORD11;
};

VsOut propsVs(VertexIn input) {
    const float c = cos(input.positionYaw.w), s = sin(input.positionYaw.w);
    const float3 local = input.position * input.scale.xyz;
    const float3 turned = float3(local.x * c - local.y * s, local.x * s + local.y * c, local.z);
    const float3 n = input.normal / max(input.scale.xyz, 1e-3);
    VsOut o;
    o.view = input.positionYaw.xyz + turned;
    o.position = mul(viewProjection, float4(o.view, 1));
    o.normal = float3(n.x * c - n.y * s, n.x * s + n.y * c, n.z);
    o.place = float3(input.anchor.xy + turned.xy, input.anchor.z + turned.z);
    o.uv = input.uv;
    o.layer = float2(input.scale.w, frac(input.anchor.w));
    o.hero = input.anchor.w > 1.5 ? 1.0 : 0.0;
    o.cover = input.extra.x;
    o.mirror = input.extra.w;
    o.blend = input.blend;
    o.biomes = input.extra.yz;
    o.local = input.position;
    o.localNormal = input.normal;
    return o;
}

Texture2DArray<float4> materials : register(t0, space2);
SamplerState materialSampler : register(s0, space2);
Texture2DArray<float4> materialNormals : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
Texture2DArray<float4> materialProperties : register(t2, space2);
SamplerState propertySampler : register(s2, space2);

// A magic megalith's own tiles (A-1: hatch, grooves, moss, strokes), hard
// pixels and soft.
Texture2DArray<float4> megalithTiles : register(t4, space2);
SamplerState megalithNearest : register(s4, space2);
Texture2DArray<float4> megalithTilesSoft : register(t5, space2);
SamplerState megalithLinear : register(s5, space2);

#define LIGHT_UNIFORMS register(b0, space3)
#define SHADOW_TEXTURE register(t3, space2)
#define SHADOW_SAMPLER register(s3, space2)
#include "lighting.hlsli"

// Must match terrain_core::PropsLook.
cbuffer Rock : register(b1, space3) {
    float rockTile;          // the ground's material tile, metres
    float rockTextureScale;  // render over output pixels (0 = 1): the mip bias under DLSS
    float rockMossLayer;     // the material laid as moss
    float rockCoverLayer;    // the ground laid on tops (steppe)
};

// Must match terrain_core::Props::CliffBiomes: per biome of biomes.json, what
// its cliffs wear (tools/terrain_lab/world_cliffs.py).
cbuffer Biomes : register(b2, space3) {
    float4 biomeStone[32];   // rock layer, ground layer, ground from (normal z), rock below (normal z)
    float4 biomeTint[32];    // the biome's tint, moss on its rock
};

// Must match terrain_core::MegalithLook: A-1's knobs (megalith_a1.hlsli).
cbuffer A1 : register(b3, space3) {
    float4 a1Knobs[9];
};
#include "megalith_a1.hlsli"

struct Rock { float3 colour; float3 normal; float ao; };

// One scale of the material from three sides, normals blended the UDN way;
// `tile`: across (x, y) and up (z) - unequal stretches it along the beds.
Rock triplanar(float3 p, float3 n, float layer, float2 tile, float bias) {
    float3 w = pow(abs(n), 4.0);
    w /= dot(w, 1.0);
    const float2 uvX = p.yz / tile, uvY = p.xz / tile, uvZ = p.xy / tile.x;
    Rock r;
    r.colour = materials.SampleBias(materialSampler, float3(uvX, layer), bias).rgb * w.x +
               materials.SampleBias(materialSampler, float3(uvY, layer), bias).rgb * w.y +
               materials.SampleBias(materialSampler, float3(uvZ, layer), bias).rgb * w.z;
    const float3 tx = materialNormals.SampleBias(normalSampler, float3(uvX, layer), bias).xyz * 2.0 - 1.0;
    const float3 ty = materialNormals.SampleBias(normalSampler, float3(uvY, layer), bias).xyz * 2.0 - 1.0;
    const float3 tz = materialNormals.SampleBias(normalSampler, float3(uvZ, layer), bias).xyz * 2.0 - 1.0;
    const float3 nx = float3(tx.xy + n.zy, n.x).zyx;
    const float3 ny = float3(ty.xy + n.xz, n.y).xzy;
    const float3 nz = float3(tz.xy + n.xy, n.z);
    r.normal = normalize(nx * w.x + ny * w.y + nz * w.z);
    r.ao = materialProperties.SampleBias(propertySampler, float3(uvX, layer), bias).r * w.x +
           materialProperties.SampleBias(propertySampler, float3(uvY, layer), bias).r * w.y +
           materialProperties.SampleBias(propertySampler, float3(uvZ, layer), bias).r * w.z;
    return r;
}

// Two scales: the broad one carries the mass from afar, the near one the
// grain at the foot; the near fades out before its repeat can show.
Rock stone(float3 place, float3 n0, float layer, float distance, float bias) {
    const Rock broad = triplanar(place, n0, layer, rockTile * 9.0, bias);
    const Rock near = triplanar(place, n0, layer, rockTile * 1.5, bias);
    const float t = saturate(1.0 - distance / 900.0);
    Rock r;
    r.colour = lerp(broad.colour, near.colour * 0.5 + broad.colour * 0.5, t);
    r.normal = normalize(lerp(broad.normal, near.normal, t * 0.6));
    r.ao = lerp(broad.ao, near.ao, t);
    return r;
}

// A biome on a cliff: its rock on the walls, its ground on what faces up
// (the plateau's grass to the brink of its cliffs; on the ledges down the
// wall only in patches), in its tint. `height`: how far up the wall, 0..1.
Rock cliffGround(uint id, Rock r, float3 place, float3 n0, float height, float distance, float bias) {
    const float4 s = biomeStone[id];
    float up = smoothstep(s.w, s.z, n0.z);
    if (up > 0) {
        const float patches = materials.SampleBias(materialSampler, float3(place.xy / (rockTile * 12.0), s.y), bias + 3).g;
        // (a flat - the rim reaching out, a terrace, the scree - is the land's own ground whole)
        up *= saturate((patches - 0.4) * 3.0 + saturate((height - 0.9) * 10.0) + smoothstep(0.86, 0.94, n0.z));
    }
    if (up > 0) {
        const float2 uv = place.xy / rockTile;
        const float3 near = materials.SampleBias(materialSampler, float3(uv, s.y), bias).rgb;
        const float3 far = materials.SampleBias(materialSampler, float3(uv / 7.0, s.y), bias).rgb;
        r.colour = lerp(r.colour, lerp(near, far, saturate(distance / 1500.0)), up);
        r.normal = normalize(lerp(r.normal, n0, up));
    }
    r.colour *= biomeTint[id].rgb;
    return r;
}

Rock cliffStone(uint id, float3 place, float3 n0, float height, float distance, float bias) {
    return cliffGround(id, stone(place, n0, biomeStone[id].x, distance, bias), place, n0, height, distance, bias);
}

// Value noise (a hash on a lattice, smoothed between) for the terraces.
float hash12(float2 p) {
    float3 q = frac(p.xyx * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
float valueNoise(float2 p) {
    const float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), f.x), lerp(hash12(i + float2(0, 1)), hash12(i + 1.0), f.x), f.y);
}
float fbm3(float2 p) {
    return valueNoise(p) * 0.55 + valueNoise(p * 2.03 + 7.1) * 0.3 + valueNoise(p * 4.07 + 3.7) * 0.15;
}

// A marsh terrace's wall (tools/terrain_lab/marsh_hill_mesh.py): red bedded
// rock. The biome's rock stretched along the beds; beds a hand high, each
// its own red, rust or burgundy, rounded by the weather (their tops lean up
// to the light, their feet undercut and dark); burgundy flecks; now and then
// a dark joint down the face.
Rock terraceStone(uint id, float3 place, float3 n0, float height, float distance, float bias) {
    const float layer = biomeStone[id].x;
    const float warp = (fbm3(place.xy * 0.35) - 0.5) * 0.35;
    const float3 p = float3(place.xy, place.z + warp);
    const Rock broad = triplanar(p, n0, layer, float2(rockTile * 2.5, rockTile * 1.0), bias);
    const Rock near = triplanar(p, n0, layer, float2(rockTile * 0.6, rockTile * 0.22), bias);
    const float t = saturate(1.0 - distance / 400.0);
    Rock r;
    r.colour = lerp(broad.colour, near.colour * 0.6 + broad.colour * 0.4, t);
    r.normal = normalize(lerp(broad.normal, near.normal, t * 0.7));
    r.ao = lerp(broad.ao, near.ao, t);
    const float wall = 1.0 - smoothstep(0.55, 0.8, n0.z);
    if (wall > 0) {
        const float bedH = 0.21;
        const float z = p.z / bedH;
        const float bed = frac(z);
        const float tone = hash12(float2(floor(z), 17.0));
        float3 tint = tone < 0.55 ? float3(1.0, 1.0, 1.0) : tone < 0.8 ? float3(1.12, 0.9, 0.72) : float3(0.62, 0.36, 0.38);
        tint *= 0.9 + 0.2 * valueNoise(float2((place.x + place.y) * 0.8, floor(z)));
        // Burgundy flecks in the stone, a few dark joints down it.
        const float fleck = smoothstep(0.74, 0.8, valueNoise(place.xy * 2.7 + place.z * 3.1));
        tint = lerp(tint, float3(0.48, 0.16, 0.2), fleck * 0.8);
        const float joint = valueNoise(float2((place.x - place.y) * 1.3, place.z * 0.25));
        const float crack = (1.0 - smoothstep(0.0, 0.035, abs(joint - 0.5))) * step(0.45, valueNoise(place.xy * 0.4));
        r.colour = lerp(r.colour, r.colour * tint * (1.0 - 0.55 * crack), wall);
        const float3 tilt = float3(0, 0, 1) * (smoothstep(0.0, 1.0, bed) - 0.45) * 1.1;
        r.normal = normalize(r.normal + tilt * wall);
        r.ao *= lerp(1.0, lerp(0.6, 1.0, smoothstep(0.0, 0.3, bed)) * (1.0 - 0.4 * crack), wall);
    }
    return cliffGround(id, r, place, n0, height, distance, bias);
}

float4 propsPs(VsOut input) : SV_Target0 {
    const float3 n0 = normalize(input.normal);
    const float bias = log2(rockTextureScale > 0 ? rockTextureScale : 1.0);
    float layer = floor(input.layer.x);
    float moss = frac(input.layer.x) * 2.0;
    const float distance = length(input.view);
    float3 colour, n;
    float ao;
    const bool slab = input.biomes.x < -1.5;
    if (slab) {
        // A-1: its own material, in object space; the slab's foot sits in
        // the ground's damp (uv.y), as the moss knobs ask.
        const A1Surface a = a1Surface(input.local, normalize(input.localNormal), input.blend, input.layer.y, n0,
                                      input.view, bias);
        colour = a.colour;
        n = a.normal;
        ao = 1.0;
        moss = 0;
    } else if (input.biomes.x >= 0) {
        // A cliff: the biome on top of it down its wall, the one at its foot
        // low down, the line between them ragged.
        const uint a = uint(input.biomes.x), b = uint(input.biomes.y);
        // (a marsh terrace's mesh writes its moss weights as 2 + weight)
        const bool terrace = input.blend > 1.5;
        float wb = 0;
        if (a != b && !terrace) {
            const float patches = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 20.0) + input.place.z / (rockTile * 30.0), biomeStone[a].x), bias + 2).g;
            wb = smoothstep(0.3, 0.7, input.blend + (patches - 0.5) * 0.6);
        }
        Rock r = (Rock)0;
        if (terrace) r = terraceStone(a, input.place, n0, input.uv.y, distance, bias);
        else if (wb < 0.99) r = cliffStone(a, input.place, n0, input.uv.y, distance, bias);
        if (wb > 0.01) {
            const Rock rb = cliffStone(b, input.place, n0, input.uv.y, distance, bias);
            if (wb >= 0.99) r = rb;
            else {
                r.colour = lerp(r.colour, rb.colour, wb);
                r.normal = normalize(lerp(r.normal, rb.normal, wb));
                r.ao = lerp(r.ao, rb.ao, wb);
            }
        }
        colour = r.colour;
        n = r.normal;
        ao = r.ao;
        layer = biomeStone[wb < 0.5 ? a : b].x;
        moss = lerp(biomeTint[a].w, biomeTint[b].w, wb);
    } else {
        const Rock r = stone(input.place, n0, layer, distance, bias);
        colour = r.colour;
        n = r.normal;
        ao = r.ao;
    }

    // The painted look: broad bands of colour up the stone, as layered rock
    // reads in a painting, and a lighter weathered crust on what faces up.
    const float z = input.place.z;
    const float band = sin(z * 0.021 + input.layer.y * 6.0 + sin(z * 0.0047) * 2.0);
    if (!slab) {
        colour *= 0.9 + 0.12 * band;
        colour = lerp(colour, colour * float3(1.12, 1.06, 0.98), saturate((n0.z - 0.6) * 2.5) * 0.6);
    }
    // Moss on what faces up and in the hollows, in patches, more low down
    // where the wet stays (doc/BIOMES_RU.md, 01: by wetness and facing).
    if (moss > 0) {
        float patches = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 20.0), layer), bias + 2).g;
        const float up = saturate((n0.z + 0.15) * 1.6);
        const float wet = lerp(1.0, 0.55, saturate(input.uv.y * 1.4)) + (1.0 - input.uv.x) * 0.6;
        float grow = moss * up * wet;
        // On a cliff wall moss also runs in bands along the foot and the
        // brink (a marsh hill's mesh carries them in `blend`: 1 at a step's
        // foot and top) and hangs down the face in streaks.
        if (input.biomes.x >= 0) {
            // (the noise from the rock's height map: a red rock's green
            // channel is near black and would starve the moss)
            patches = materialProperties.SampleBias(propertySampler, float3(input.place.xy / (rockTile * 20.0), layer), bias + 2).b;
            const float edge = input.biomes.x == input.biomes.y ? input.blend : 0.0;
            const float2 down = float2((input.place.x + input.place.y) / (rockTile * 1.6), input.place.z / (rockTile * 9.0));
            const float streak = materialProperties.SampleBias(propertySampler, float3(down, layer), bias + 1).b;
            grow = max(grow, moss * (1.1 * edge + 0.8 * smoothstep(0.45, 0.65, streak)) * lerp(1.2, 0.8, saturate(input.uv.y)));
        }
        // (the height map's noise sits lower than a colour channel's)
        float m = input.biomes.x >= 0 ? smoothstep(0.28, 0.5, grow + (patches - 0.3) * 0.9)
                                     : smoothstep(0.35, 0.65, grow + (patches - 0.45) * 0.8);
        float lush = 0.5;
        if (input.blend > 1.5 && input.biomes.x >= 0) {
            // A marsh terrace: moss where it holds by nature - cushions at a
            // wall's foot in the wet, along the brink and spilling over the
            // lip, on the ledges and in the cracks and undercuts, more on
            // the side out of the sun and low down; ragged at its edge, in
            // drifts of a few metres with tufts and gaps of a hand.
            const float edge = input.blend - 2.0;
            const float upward = saturate(n0.z * 1.3 + 0.05);
            const float2 sunXY = normalize(lightSun.xy + 1e-4);
            const float shade = saturate(0.5 - 0.5 * dot(normalize(n0.xy + 1e-4), sunXY)) * (1.0 - upward);
            const float low = 1.0 - saturate(input.uv.y * 1.6);
            const float drift = fbm3(input.place.xy * 0.22 + input.place.z * 0.13);
            const float tuft = fbm3(input.place.xy * 1.9 + float2(input.place.z * 2.3, -input.place.z * 1.1));
            const float wallness = 1.0 - smoothstep(0.6, 0.85, n0.z);
            const float g = (edge * 0.85 + upward * 0.06 + (shade * 0.3 + low * 0.12 + (1.0 - ao) * 0.45) * wallness) * moss / 0.7;
            const float v = g + (drift - 0.5) * 0.6 + (tuft - 0.5) * 0.35;
            m = smoothstep(0.52, 0.6, v);
            lush = saturate((v - 0.52) * 3.0) * 0.6 + tuft * 0.4;
            n = normalize(lerp(n, normalize(n0 + float3(0, 0, 0.6)), m * 0.5));
            ao = lerp(ao, 1.0, m * 0.5);
        }
        if (m > 0) {
            const float3 green = materials.SampleBias(materialSampler, float3(input.place.xy / (rockTile * 1.5), rockMossLayer), bias).rgb;
            // Old moss deep and dark, its living edge and the tufts fresher.
            const float3 tone = lerp(float3(0.7, 0.78, 0.55), float3(1.08, 1.16, 0.72), lush);
            colour = lerp(colour, green * 0.85 * (input.blend > 1.5 ? tone : 1.0), m);
        }
    }
    // Steppe on the tops: the islands and the spires' flat crowns carry the
    // dry grass of the steppe round them, not bare rock, fading at the brink.
    if (input.cover > 0) {
        const float top = smoothstep(0.62, 0.86, n0.z) * input.cover;
        if (top > 0) {
            const float2 uv = input.place.xy / (rockTile * 1.0);
            const float3 grass = materials.SampleBias(materialSampler, float3(uv, rockCoverLayer), bias).rgb;
            const float3 grassFar = materials.SampleBias(materialSampler, float3(uv / 7.0, rockCoverLayer), bias).rgb;
            colour = lerp(colour, lerp(grass, grassFar, saturate(distance / 1500.0)), top);
        }
    }
    // Cavity from the model, darker at the foot where it meets the ground.
    const float foot = lerp(0.65, 1.0, saturate(input.uv.y * 12.0));
    const float occlusion = input.uv.x * foot * lerp(0.7, 1.0, ao);
    const float sunLight = sunShadow(input.view, n0) * cloudShadow(input.view);
    float3 lit = shadeSurface(colour, n, input.view, sunLight, occlusion, 0.0) * lerp(0.8, 1.0, input.uv.x);
    // A mirror (the desert's prism): the sky and the sun reflected off a
    // smooth face, a little of the stone's own colour left in it, and the
    // desert below it caught in the lower half of the reflection.
    if (input.mirror > 0) {
        const float3 v = normalize(input.view);
        const float3 r = reflect(v, n0);
        float3 seen = skyColour(r) + sunDisc(r) * sunLight;
        // The clouds in it: where the reflected ray meets the cloud layer.
        if (r.z > 0.02 && lightCloudCover > 0) {
            const float3 world = float3(lightEyeXY + input.view.xy, lightEyeZ + input.view.z);
            const float mid = (lightCloudBase + lightCloudTop) * 0.5;
            const float2 at = world.xy + r.xy / r.z * (mid - world.z);
            const float c = cloudCoverage(at, lightCloudCover, lightCloudDrift);
            seen = lerp(seen, lightSunColour * 0.45 + lightZenith * 0.9, saturate(c * 1.2) * 0.85);
        }
        const float3 sand = float3(0.78, 0.62, 0.38) * (lightSunColour * 0.6 + lightZenith * 0.5);
        seen = lerp(seen, sand, saturate(-r.z * 3.0));
        const float fresnel = 0.55 + 0.45 * pow(1.0 - saturate(dot(-v, n0)), 4.0);
        lit = lerp(lit, seen, input.mirror * fresnel);
    }
    // A megalith keeps its silhouette through the haze: hazed as if a third
    // as far, with a quarter of the full haze laid over that.
    if (input.hero > 0.5) {
        const float3 near = applyFog(lit, input.view * 0.35);
        const float3 far = applyFog(lit, input.view);
        return float4(lerp(near, far, 0.25), 1);
    }
    return float4(applyFog(lit, input.view), 1);
}

void propsShadowPs(VsOut input) {}
