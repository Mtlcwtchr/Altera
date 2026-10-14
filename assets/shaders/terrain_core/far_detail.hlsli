// The far ground's painted detail. Past a hundred and fifty metres or so the
// mesh is a few metres a quad and the ground's 4 m textures have mipped down
// to their mean: mountains go soft and fields go flat. What is lost there is
// painted back here, as colour and as slope for the light - nothing has to
// agree with the geometry or from frame to frame, it only has to read:
//
//   erosion  gullies cut straight down the slope, in three sizes, crests worn
//            light and grooves dark (waves across the fall line, Gabor-like,
//            blended over neighbouring cells so no grid shows);
//   strata   bedding bands in steep rock, by height, wandering;
//   cavity   the clipmap's own curvature at the drawn level: hollows dark,
//            crests pale - the shapes the mesh has, made legible;
//   mottle   a coarse unevenness of colour over hundreds of metres;
//   blades   grass near the eye (nearBlades, called on its own): single
//            blades, tufts and combed clumps as strokes, each size fading to
//            its mean before it could shimmer - there are no grass plants;
//   meadow   grass further off, from its own nearer distance: tuft
//            grain, pale tips along the ground and
//            dark feet and soil from above, gusts rolling across it with the
//            wind, the low sun through the blades;
//   canopy   forest as crowns on the ground wherever the tree layers grow -
//            the same biome rules, cluster noise, cells and hashes as
//            foliage_spawn.hlsl, so a crown is painted where a tree stands,
//            and the forest carries on past the trees' range. Crowns under a
//            pixel become their cover, thicker the flatter the look.
//
// Included by terrain.hlsl after ground.hlsli (biome map, GroundPoint) and
// its Look uniforms (bilinear over the clipmap for the curvature).

// Must match terrain_core::FarUniforms.
cbuffer Far : register(b3, space3) {
    float4 farRange;              // start m, full m; the meadow's start m, full m
    float4 farStrength;           // erosion, strata, cavity, mottle
    float4 farStrength2;          // meadow, canopy, canopy layer count, near blades
    float4 canopyColour[4];       // rgb, min normal z
    float4 canopyShape[4];        // candidate cell (m), crown radius (m), -, -
    float4 canopyRules[4 * 32];   // per layer and biome: density, log2 cluster cell, cover, seed (bits)
};

static const float kTau = 6.2831853;

// Waves across the fall line in cells of 2^log2Cell level-0 texels:
// `freq` waves a cell. x: -1..1, yz: its gradient, per cell.
float3 farGullies(GroundPoint p, int log2Cell, float2 across, float freq, uint seed) {
    int2 c;
    float2 f;
    cellOf(p, log2Cell, c, f);
    float3 sum = 0;
    float weights = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) {
            const uint h = groundHash(c + int2(i, j), seed);
            const float2 d = f - (float2(i, j) + float2(h & 0xffffu, h >> 16) / 65536.0);
            const float w = exp(-dot(d, d) * 2.5);
            const float phase = (dot(d, across) * freq + float(groundHash(c + int2(i, j), seed + 1u) & 0xffu) / 256.0) * kTau;
            sum += w * float3(cos(phase), -sin(phase) * freq * kTau * across);
            weights += w;
        }
    return sum / max(weights, 1e-4);
}

// The clipmap's curvature at the drawn level round `local` (Laplacian over
// one texel of that level, divided by its spacing: about the slope change
// across one quad - so it always shows shapes a few pixels big).
float farCurvature(int2 slot, float2 local, uint level, float height, float levelSpacing) {
    const float sum = bilinear(clipPs, clipPsSampler, lookClipSize, slot, local + float2(1, 0), level).x +
                      bilinear(clipPs, clipPsSampler, lookClipSize, slot, local - float2(1, 0), level).x +
                      bilinear(clipPs, clipPsSampler, lookClipSize, slot, local + float2(0, 1), level).x +
                      bilinear(clipPs, clipPsSampler, lookClipSize, slot, local - float2(0, 1), level).x;
    return (sum - 4.0 * height) / levelSpacing;
}

struct FarCanopy {
    float cover;      // 0..1, how much of the pixel is crowns
    float3 colour;
    float2 slope;     // the crowns' domes, dz/dx dz/dy
};

// Forest of one tree layer at p: chance per candidate cell as the spawn has it.
float canopyChance(GroundPoint p, float4 rule) {
    float chance = rule.x;
    if (rule.y > 0) {
        GroundPoint q = p;
        q.footprint = 0;
        const uint seed = asuint(rule.w);
        const float n = cellNoise(q, int(rule.y), seed + 7u) * 0.7 + cellNoise(q, int(rule.y) - 2, seed + 11u) * 0.3;
        const float mask = smoothstep(1.0 - rule.z - 0.08, 1.0 - rule.z + 0.08, n);
        // Clusters under a pixel are their share of the ground.
        chance *= lerp(mask, rule.z, noiseFade(p, int(rule.y) - 1));
    }
    return chance;
}

FarCanopy farCanopy(GroundPoint p, float normalZ, float3 toEye, float pixelMetres, float distance) {
    FarCanopy o;
    o.cover = 0; o.colour = 0; o.slope = 0;
    const uint count = uint(farStrength2.z);
    if (count == 0 || farStrength2.y <= 0) return o;
    BiomeMapInfo info;
    info.size = float(groundSizes.x);
    info.worldSize = groundWorld.x;
    info.spacing = groundWorld.y;
    info.tileLog2 = int(groundGlobals.w);
    const uint biome = min(dominantBiome(biomeMap, biomeSampler, info, p), 31u);
    // Metres from the world's corner: exact enough in a float on a map of a
    // hundred-odd kilometres (a hundredth of a metre).
    const float2 metres = (float2(p.base) + p.sub) * groundWorld.y;
    // Looked at along the ground, the crowns hide the gaps between them.
    const float slant = 1.0 / max(abs(toEye.z), 0.12);
    [loop] for (uint k = 0; k < count; ++k) {
        const float4 rule = canopyRules[k * 32 + biome];
        if (rule.x <= 0) continue;
        const float steep = smoothstep(canopyColour[k].w - 0.06, canopyColour[k].w + 0.02, normalZ);
        if (steep <= 0) continue;
        const float chance = canopyChance(p, rule) * steep;
        if (chance <= 0.001) continue;
        const float cell = canopyShape[k].x, radius = canopyShape[k].y;
        const uint seed = asuint(rule.w);
        // Statistical cover: crowns of this size at this chance, seen slant.
        const float area = chance * 3.14159 * radius * radius / (cell * cell);
        const float cover = 1.0 - exp(-area * 1.3 * slant);
        float3 colour = canopyColour[k].rgb * 0.85;
        float2 slope = 0;
        float crowns = cover;
        // The crowns themselves while they are a few pixels across: the
        // candidates of the cells round the point, as the spawn places them.
        const float resolved = saturate(radius / max(pixelMetres, 1e-3) * 0.5 - 0.5);
        if (resolved > 0) {
            const float2 at = metres / cell;
            const int2 c0 = int2(floor(at));
            float hit = 0;
            float3 crownColour = 0;
            float2 crownSlope = 0;
            [unroll] for (int j = -1; j <= 1; ++j)
                [unroll] for (int i = -1; i <= 1; ++i) {
                    const int2 c = c0 + int2(i, j);
                    if (float(groundHash(c, seed + 101u) & 0xffffu) / 65536.0 >= chance) continue;
                    const uint h = groundHash(c, seed);
                    const float2 centre = float2(c) + float2(h & 0xffffu, h >> 16) / 65536.0;
                    const uint h3 = groundHash(c, seed + 202u);
                    const float r = radius * (0.75 + 0.5 * float((h3 >> 8) & 0xffu) / 255.0);
                    const float2 d = (at - centre) * cell / r;
                    const float dd = dot(d, d);
                    if (dd >= 1.0) continue;
                    const float dome = sqrt(1.0 - dd);
                    const float edge = smoothstep(1.0, 0.8, sqrt(dd));
                    if (edge > hit) {
                        hit = edge;
                        // Each tree its own shade; the dome dark at its foot.
                        const float shade = 0.82 + 0.36 * float(h3 & 0xffu) / 255.0;
                        crownColour = canopyColour[k].rgb * shade * (0.7 + 0.4 * dome);
                        crownSlope = d / max(dome, 0.25) * 0.6;
                    }
                }
            crowns = lerp(cover, hit, resolved);
            colour = lerp(colour, hit > 0 ? crownColour : colour, resolved);
            slope = crownSlope * resolved;
        } else {
            // Clumps of crowns: the forest's roof is not one flat colour.
            const float clump = cellNoise(p, 5, seed + 31u) * 0.6 + cellNoise(p, 7, seed + 37u) * 0.4;
            colour *= 0.8 + 0.4 * clump;
        }
        if (crowns > o.cover) {
            o.colour = colour;
            o.slope = slope;
        }
        o.cover = max(o.cover, crowns);
    }
    o.cover *= farStrength2.y;
    return o;
}

// Grass past the blades (nearBlades), as paint. What
// makes a far meadow read as grass rather than green ground: grain of tufts
// and clumps; along the ground only the pale tips show, from above the dark
// feet and the soil between; the wind combs it in gusts that roll across;
// against a low sun the blades glow. `grass` is how much of the ground is
// grass (the material's grass share).
void farMeadow(GroundPoint p, float3 normal, float3 toEye, float distance, float grass,
               inout float3 colour, inout float2 slope) {
    const float on = smoothstep(farRange.z, max(farRange.w, farRange.z + 1.0), distance);
    const float g = grass * on * farStrength2.x * smoothstep(0.6, 0.75, normal.z);
    if (g <= 0) return;
    const float grazing = 1.0 - saturate(dot(toEye, normal) * 2.5);
    float shade = lerp(0.88, 1.1, grazing);
    // Tufts and clumps, one to thirty metres; each size fades to its mean
    // (cellNoise) before it could shimmer.
    shade *= 0.84 + 0.32 * (cellNoise(p, 2, 721u) * 0.3 + cellNoise(p, 4, 727u) * 0.35 + cellNoise(p, 6, 701u) * 0.35);
    // Gusts: broad bands rolling downwind, a finer ripple in them, the
    // combed blades turning their pale side up.
    const float2 metres = (float2(p.base) + p.sub) * groundWorld.y;
    const float2 wind = dot(lightWindDirection, lightWindDirection) > 1e-4 ? normalize(lightWindDirection) : float2(1, 0);
    const float along = dot(metres, wind), across = dot(metres, float2(-wind.y, wind.x));
    const float gust = sin(along * 0.045 - lightTime * 1.6 + sin(across * 0.013) * 2.0) * 0.5 + 0.5;
    const float ripple = sin(along * 0.12 - lightTime * 2.7 + across * 0.035) * 0.5 + 0.5;
    const float pixelMetres = max(p.footprint * groundWorld.y, 1e-3);
    const float rippleFade = saturate(52.0 / (pixelMetres * 6.0) - 1.0);
    const float wave = gust * 0.7 + (ripple - 0.5) * 0.3 * rippleFade;
    shade *= 1.0 + 0.16 * (wave - 0.5) * (0.4 + 0.6 * saturate(lightWind));
    // Seen along it, the grass is all blade: richer, and the stones and soil
    // patches of the material under it half hidden.
    const float luma = dot(colour, float3(0.3, 0.5, 0.2));
    float3 sward = lerp(luma.xxx, colour, 1.0 + 0.35 * grazing);
    // Against a low sun the blades glow at their edges.
    const float back = pow(saturate(dot(-toEye, lightSun)), 6.0) * grazing * saturate(lightSunAbove);
    sward *= 1.0 + back * float3(0.35, 0.45, 0.12);
    colour = lerp(colour, sward * shade, g);
    // Blades for the light: a grain of tilts, gone where it is under a pixel.
    slope += (float2(cellNoise(p, 1, 731u), cellNoise(p, 1, 737u)) - 0.5) * 0.5 * g;
}

// Grass near the eye, as paint too: there are no grass plants, the ground is
// the grass. Seen from above, blades in three sizes - single blades a few
// centimetres long, the tufts they stand in, the combed clumps - each a stroke
// leaning downwind and swaying with it, pale towards its tip, the shaded
// blades under them dark between; the gaps close as the look flattens (along
// the ground only blades show). Each size fades to its mean before it could
// shimmer, so further off this is the ground's own colour and the meadow
// above takes over.

// Cells of 1/k level-0 texel at p: exact integer cell and fraction.
void bladeCell(GroundPoint p, int k, out int2 c, out float2 f) {
    const float2 s = p.sub * float(k);
    const float2 w = floor(s);
    c = p.base * k + int2(w);
    f = s - w;
}

// Two blades a cell of 1/k texel, each at most a cell long (so the 3x3 cells
// round hold every blade that reaches this one): x how much of the pixel the
// top blade covers, y how far up it (0 foot, 1 tip), zw its direction.
float4 bladeField(GroundPoint p, int k, uint seed, float2 wind, float sway, float aa) {
    int2 c;
    float2 f;
    bladeCell(p, k, c, f);
    float4 best = float4(0, 0, wind);
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
            [unroll] for (int b = 0; b < 2; ++b) {
                const uint h = groundHash(c + int2(i, j), seed + uint(b) * 131u);
                const float2 root = float2(i, j) + float2(h & 0xffu, (h >> 8) & 0xffu) / 256.0;
                float sn, cs;
                sincos((float((h >> 16) & 0xffu) / 256.0 - 0.5) * 2.4 + sway, sn, cs);
                const float2 dir = wind * cs + float2(-wind.y, wind.x) * sn;
                const float len = 0.6 + 0.4 * float(h >> 24) / 256.0;
                const float2 q = f - root;
                const float t = dot(q, dir) / len;
                const float halfWidth = 0.2 * (1.0 - 0.75 * saturate(t));
                const float cover = saturate((halfWidth - abs(dot(q, float2(-dir.y, dir.x)))) / aa + 0.5) *
                                    saturate(t * len / aa + 0.5) * saturate((1.0 - t) * len / aa + 0.5);
                if (cover > best.x) best = float4(cover, saturate(t), dir);
            }
    return best;
}

void nearBlades(GroundPoint p, float3 normal, float3 view, float grass, inout float3 colour, inout float2 slope) {
    const float g = grass * farStrength2.w * smoothstep(0.6, 0.75, normal.z);
    const float pixelMetres = max(p.footprint * groundWorld.y, 1e-4);
    if (g <= 0 || pixelMetres > 0.25) return;
    const float distance = length(view);
    const float3 toEye = -view / max(distance, 1e-3);
    const float grazing = 1.0 - saturate(dot(toEye, normal) * 2.5);
    const float2 wind = dot(lightWindDirection, lightWindDirection) > 1e-4 ? normalize(lightWindDirection) : float2(1, 0);
    const float2 metres = (float2(p.base) + p.sub) * groundWorld.y;
    const float along = dot(metres, wind), across = dot(metres, float2(-wind.y, wind.x));
    const float sway = 0.35 * sin(along * 0.8 - lightTime * 2.2 + across * 0.2) * (0.3 + 0.7 * saturate(lightWind));
    // Between the top blades: the shaded ones under them, closing up as the
    // look flattens.
    const float gap = lerp(0.56, 0.92, grazing);
    const float mean = 0.4 * 1.0 + 0.6 * gap;
    float shade = 1.0, tips = 0;
    // Blades (3 cm cells), tufts (12.5 cm), combed clumps (50 cm).
    const int sizes[3] = {16, 4, 1};
    const float contrast[3] = {1.0, 0.7, 0.45};
    [unroll] for (int s = 0; s < 3; ++s) {
        const float cellMetres = groundWorld.y / float(sizes[s]);
        const float pixelCells = pixelMetres / cellMetres;
        const float fade = saturate((pixelCells - 0.12) / 0.35);
        if (fade >= 1.0) continue;
        const float4 blade = bladeField(p, sizes[s], 811u + uint(s) * 37u, wind, sway, max(pixelCells, 0.02));
        const float lit = 0.82 + 0.38 * blade.y;
        const float v = lerp(gap, lit, blade.x) / mean;
        shade *= lerp(1.0, v, contrast[s] * (1.0 - fade));
        if (s == 0) tips = blade.x * blade.y * blade.y * (1.0 - fade);
        // The blades lean: a grain of tilts for the light.
        slope += blade.zw * (blade.y - 0.5) * 0.7 * blade.x * contrast[s] * (1.0 - fade) * g;
    }
    // Clumps a little apart in hue, the tips going straw-pale.
    const float hue = cellNoise(p, 0, 743u);
    float3 grassColour = colour * lerp(float3(0.95, 1.02, 1.04), float3(1.05, 0.99, 0.90), hue) * shade;
    grassColour = lerp(grassColour, grassColour * float3(1.10, 1.07, 0.80), tips * 0.6);
    colour = lerp(colour, grassColour, g);
}

// What it paints: `colour` and the ground's gradient `slope` (dz/dx, dz/dy,
// added to the mesh's). `curvature` from farCurvature; `grass` how much of the
// ground is grass.
void farDetail(GroundPoint p, float height, float3 normal, float3 view, float curvature, float grass,
               inout float3 colour, inout float2 slope) {
    const float distance = length(view);
    const float3 toEye = -view / max(distance, 1e-3);
    farMeadow(p, normal, toEye, distance, grass, colour, slope);
    const float on = smoothstep(farRange.x, max(farRange.y, farRange.x + 1.0), distance);
    if (on <= 0) return;
    const float pixelMetres = max(p.footprint * groundWorld.y, 1e-3);
    const float steepness = saturate((1.0 - normal.z) * 3.0);

    // Gullies down the fall line, three sizes; each fades as its waves near
    // a few pixels, before they could shimmer.
    if (farStrength.x > 0 && steepness > 0) {
        const float2 fall = normal.xy / max(length(normal.xy), 1e-4);   // downhill
        const float2 across = float2(-fall.y, fall.x);
        const int tile = int(groundGlobals.w);
        float2 grad = 0;
        float value = 0;
        // log2 cell (level-0 texels), waves a cell, depth as a share of a wave
        const float3 octaves[3] = {float3(10, 3, 0.05), float3(8, 3, 0.06), float3(6, 3, 0.07)};
        [unroll] for (int o = 0; o < 3; ++o) {
            const float cellMetres = float(1 << int(octaves[o].x)) * groundWorld.y;
            const float wave = cellMetres / octaves[o].y;
            const float fade = saturate(wave / (pixelMetres * 6.0) - 1.0);
            if (fade <= 0) continue;
            const float3 g = farGullies(p, int(octaves[o].x), across, octaves[o].y, 401u + uint(o) * 13u);
            // Grooves sharpened into a V, crests left round.
            const float v = g.x;
            const float shaped = v > 0 ? v : -sqrt(-v);
            const float dShaped = v > 0 ? 1.0 : 0.5 / sqrt(max(-v, 1e-3));
            const float depth = wave * octaves[o].z * fade;
            value += shaped * fade * (0.5 + 0.25 * float(o));
            grad += g.yz / cellMetres * depth * min(dShaped, 3.0);
        }
        const float k = farStrength.x * on * steepness;
        slope += grad * k;
        colour *= 1.0 + 0.16 * value * k;
    }

    // Bedding in the steep rock: bands of two thicknesses by height, the
    // band line wandering a few tens of metres.
    if (farStrength.y > 0) {
        const float cliff = smoothstep(0.85, 0.6, normal.z);
        if (cliff > 0) {
            const float wander = (cellNoise(p, 9, 503u) - 0.5) * 30.0 + (cellNoise(p, 7, 509u) - 0.5) * 8.0;
            const float h = height + wander;
            const float fine = saturate(7.0 / (pixelMetres * 4.0) - 1.0);
            const float coarse = saturate(23.0 / (pixelMetres * 4.0) - 1.0);
            const float bands = sin(h / 7.0 * kTau) * 0.4 * fine + sin(h / 23.0 * kTau + 1.3) * 0.6 * coarse;
            const float k = farStrength.y * on * cliff;
            colour *= 1.0 + 0.14 * bands * k;
            // The beds stand out a little: a ledge along each band.
            const float dBands = cos(h / 23.0 * kTau + 1.3) * coarse * 0.6 * kTau / 23.0;
            slope += -normal.xy / max(normal.z, 0.3) * dBands * 2.0 * k;
        }
    }

    // Hollows dark, crests pale.
    if (farStrength.z > 0) {
        const float c = clamp(curvature * 4.0, -1.0, 1.0);
        const float k = farStrength.z * on;
        colour *= 1.0 - 0.28 * max(c, 0.0) * k + 0.14 * max(-c, 0.0) * k;
    }

    // Mottle over hundreds of metres, a little warmer and cooler by turns.
    if (farStrength.w > 0) {
        const float a = cellNoise(p, 9, 601u), b = cellNoise(p, 11, 607u), d = cellNoise(p, 7, 613u);
        const float m = (a * 0.45 + b * 0.35 + d * 0.2) - 0.5;
        const float k = farStrength.w * on;
        colour *= 1.0 + 0.3 * m * k;
        colour *= lerp(1.0, lerp(float3(1.05, 1.0, 0.92), float3(0.95, 1.0, 1.05), b), 0.6 * k);
    }

    // The forest's roof over everything.
    const FarCanopy canopy = farCanopy(p, normal.z, toEye, pixelMetres, distance);
    if (canopy.cover > 0) {
        const float k = canopy.cover * on;
        // Between the crowns: the forest floor, in their shade.
        const float3 under = colour * lerp(1.0, 0.55, saturate(k * 2.0));
        colour = lerp(under, canopy.colour, k);
        slope = lerp(slope, slope * 0.3 + canopy.slope, k);
    }
}
