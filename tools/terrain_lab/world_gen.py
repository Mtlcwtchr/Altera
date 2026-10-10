"""World generator driven by rules (doc/rework/WORLD_GENERATOR_PLAN.md).

    python tools/terrain_lab/world_gen.py [--rules content/config/terrain_core/world_rules.json]
                                          [--out terrain_lab_out/world] [--size 1024] [--seed N]

Stage 1 - layout: the continent inside the world (ocean all round, room for
the northern isles), its zones from the rules, the special places, the isles.
Writes <out>_zones.png (a picture of the layout) and <out>_biomes.png (biome
ids, as the lab's --biomes reads them).

Coordinates: x west->east, y south->north, 0..1 over the world; pictures are
written north up. On the continent: s west->east, t south->north, 0..1 over
its box.
"""
import argparse
import json

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage


def fbm(n, seed, octaves=5, base=4, persistence=0.5):
    """Smooth noise in about -1..1 on an n*n grid; `base` cells across at the coarsest."""
    rng = np.random.default_rng(seed)
    out = np.zeros((n, n))
    amp, total = 1.0, 0.0
    cells = base
    for _ in range(octaves):
        coarse = rng.standard_normal((cells + 3, cells + 3))
        layer = ndimage.zoom(coarse, (n + 2 * n / cells) / (cells + 3), order=3)
        off = int(n / cells)
        out += amp * layer[off:off + n, off:off + n]
        total += amp
        amp *= persistence
        cells *= 2
    out /= total
    return out / max(np.abs(out).max(), 1e-9)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


ZONE_COLOURS = {
    "ocean": (24, 46, 82), "golden_desert": (226, 190, 98), "canyon_steppe": (196, 140, 70),
    "steppe_canyon": (120, 64, 34), "green_plateau": (120, 176, 80), "great_marshes": (64, 120, 100),
    "river_marshes": (40, 140, 150), "dark_forest": (30, 70, 50), "foothills": (150, 160, 110),
    "white_mountains": (232, 232, 236), "lake_valley": (220, 120, 140), "river_valley": (110, 190, 160),
    "black_lands": (40, 30, 40), "isles": (110, 120, 110), "cliff_coast": (250, 220, 150),
    "steppe": (170, 180, 90),
}


def layout(rules, n, seed):
    world = rules["continent"]
    margin = world["margin"]
    yy, xx = (np.mgrid[0:n, 0:n] + 0.5) / n          # row = y (south first), col = x
    # The continent's box: ocean on every side, more in the north for the isles.
    isle_band = rules["isles"]["band"]
    x0, x1 = margin, 1.0 - margin
    y0, y1 = margin, 1.0 - isle_band[1] - 0.01
    # A rounded box, its edge wobbled: big bays and capes, and a ragged line.
    bx, by = (xx - (x0 + x1) / 2) / ((x1 - x0) / 2), (yy - (y0 + y1) / 2) / ((y1 - y0) / 2)
    shape = 1.0 - (np.abs(bx) ** 4 + np.abs(by) ** 4) ** 0.25
    shape += world["roughness"] * (fbm(n, seed + 1, octaves=4, base=3) + 0.35 * fbm(n, seed + 2, octaves=5, base=12))
    land = shape > 0.08
    parts, count = ndimage.label(land)
    if count > 1:
        sizes = ndimage.sum(land, parts, range(1, count + 1))
        land = parts == (int(np.argmax(sizes)) + 1)
    land = ndimage.binary_fill_holes(land)

    s = (xx - x0) / (x1 - x0)
    t = (yy - y0) / (y1 - y0)

    # The bands, south to north, sharing their edges: each edge a curve, all
    # bent together widely (so the bands swing as one) and each wobbled on
    # its own (so their widths vary). Never crossing.
    bands = rules["bands"]
    warp = rules["warp"]
    wide = warp["large"] * fbm(n, seed + 10, octaves=3, base=2)
    edges = [np.full((n, n), -10.0)]
    at = 0.0
    for k in range(1, len(bands)):
        at += bands[k - 1]["size"]
        e = at + wide + warp["edge"] * fbm(n, seed + 20 + k, octaves=4, base=4)
        edges.append(np.maximum(e, edges[-1] + 0.01))
    edges.append(np.full((n, n), 10.0))
    index = np.zeros((n, n), int)
    for k in range(1, len(bands)):
        index += (t >= edges[k]).astype(int)
    zone = np.full((n, n), "ocean", dtype=object)
    for k, b in enumerate(bands):
        zone[land & (index == k)] = b["name"]
    order = {b["name"]: k for k, b in enumerate(bands)}

    def place(spec):
        """'band:fraction' -> where that is in t, per point (the bands' own curves)."""
        name, frac = spec.split(":")
        k = order[name]
        lo = edges[k] if k > 0 else np.zeros((n, n))
        hi = edges[k + 1] if k + 1 < len(bands) else np.ones((n, n))
        return lo + float(frac) * (hi - lo)

    wing = rules["east_wing"]
    east = s >= wing["s"] + wing["warp"] * fbm(n, seed + 40, octaves=4, base=3)
    for z in wing["zones"]:
        zone[land & east & (t >= place(z["from"])) & (t < place(z["to"]))] = z["name"]

    f = rules["features"]
    rng = np.random.default_rng(seed + 50)
    # The round valley with its lake, in the mountains.
    lv = f["lake_valley"]
    k = order[lv["in"]]
    cx = x0 + lv["s"] * (x1 - x0)
    col = int(cx * n)
    rows = np.nonzero(zone[:, col] == lv["in"])[0]
    cy = (rows.mean() + 0.5) / n if len(rows) else y0 + 0.75 * (y1 - y0)
    r = np.hypot(xx - cx, yy - cy)
    valley = (r < lv["radius"] * (x1 - x0) * (1 + 0.06 * fbm(n, seed + 300, octaves=3, base=6))) & land
    zone[valley] = "lake_valley"

    # The rocky canyon land holds islands of steppe, each in a ring of canyon.
    si = f["steppe_islands"]
    rocky = zone == si["in"]
    cand = np.argwhere(ndimage.binary_erosion(rocky, iterations=int(0.03 * n)))
    centres = []
    for _ in range(400):
        if len(centres) >= si["count"] or len(cand) == 0:
            break
        cyx = cand[rng.integers(len(cand))] / n
        size = rng.uniform(*si["size"]) * (x1 - x0)
        if all(np.hypot(*(cyx - c[0])) > (size + c[1]) * 2.2 for c in centres):
            centres.append((cyx, size))
    ring_px = max(1, int(round(si["canyon_width"] * (x1 - x0) * n)))
    for i, (cyx, size) in enumerate(centres):
        stretch = rng.uniform(1.0, 1.8)
        ang = rng.uniform(0, np.pi)
        dx, dy = xx - cyx[1], yy - cyx[0]
        u = (dx * np.cos(ang) + dy * np.sin(ang)) / (size * stretch)
        v = (-dx * np.sin(ang) + dy * np.cos(ang)) / size
        blob = ((u * u + v * v) < (1.0 + 0.4 * fbm(n, seed + 400 + i, octaves=4, base=6))) & rocky
        ring = ndimage.binary_dilation(blob, iterations=ring_px) & ~blob & rocky
        zone[blob] = "steppe"
        zone[ring] = "steppe_canyon"

    # River marshes: the marshes near the sea - the deltas.
    to_sea_n = ndimage.distance_transform_edt(land) / n
    rm = f["river_marshes"]
    zone[(zone == rm["in"]) & (to_sea_n < rm["coast_band"] * (x1 - x0))] = "river_marshes"
    # The valley's river runs east and ends in marshes there: its delta.
    vd = f["valley_delta"]
    zone[(zone == vd["in"]) & (s >= vd["s"]) & (to_sea_n < vd["reach"] * (x1 - x0))] = "river_marshes"
    # The cut cliff coast of the desert.
    cc = f["cliff_coast"]
    zone[(zone == cc["in"]) & (to_sea_n * rules["world_km"] < cc["width_km"])] = "cliff_coast"

    # The northern isles: tilted plateaus out in the northern ocean, clear
    # of the continent.
    isles = rules["isles"]
    near = ndimage.binary_dilation(land, iterations=int(isles["clearance"] * n))
    made = 0
    for i in range(200):
        if made >= isles["count"]:
            break
        ix = rng.uniform(0.1, 0.9)
        iy = 1.0 - rng.uniform(*isles["band"])
        size = rng.uniform(*isles["size"])
        stretch = rng.uniform(1.3, 2.2)
        ang = rng.uniform(0, np.pi)
        dx, dy = xx - ix, yy - iy
        u = (dx * np.cos(ang) + dy * np.sin(ang)) / (size * stretch)
        v = (-dx * np.sin(ang) + dy * np.cos(ang)) / size
        blob = (u * u + v * v) < (1.0 + 0.35 * fbm(n, seed + 600 + i, octaves=4, base=8))
        # Clear of the world's edge too: no island cut off by it.
        e = isles["edge"]
        if (blob & ((xx < e) | (xx > 1 - e) | (yy > 1 - e))).any():
            continue
        if (blob & (near | (zone == "isles"))).any() or blob.sum() < 50:
            continue
        zone[blob] = "isles"
        made += 1
    return zone, land | (zone == "isles"), (x0, x1, y0, y1)


def fbm32(n, seed, km, world_km, octaves=5, persistence=0.5):
    """fbm with its coarsest wavelength about `km` kilometres; -1..1, float32."""
    base = max(2, int(round(world_km / km)))
    return fbm(n, seed, octaves=octaves, base=base, persistence=persistence).astype(np.float32)


def ridged(n, seed, km, world_km, octaves=6):
    """Ridged multifractal: sharp crests, each octave weighted by the last."""
    out = np.zeros((n, n), np.float32)
    weight = np.ones((n, n), np.float32)
    amp, total = 1.0, 0.0
    base = max(2, int(round(world_km / km)))
    for o in range(octaves):
        layer = fbm(n, seed + 7 * o, octaves=1, base=base * 2 ** o).astype(np.float32)
        r = (1.0 - np.abs(layer)) ** 2
        r *= weight
        weight = np.clip(r * 1.6, 0.0, 1.0)
        out += amp * r
        total += amp
        amp *= 0.5
    out /= total
    # Spread to 0..1 by its own range, so a "peak" means the peak height.
    lo, hi = np.percentile(out, 2), np.percentile(out, 99.5)
    return np.clip((out - lo) / max(hi - lo, 1e-6), 0.0, 1.0)


def warped(field, n, seed, km, world_km, strength_km):
    """`field` resampled through a smooth warp of `strength_km`: straight
    features bend, round ones smear into crests."""
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    px = strength_km / (world_km / n)
    wy = px * fbm32(n, seed, km, world_km, 3)
    wx = px * fbm32(n, seed + 1, km, world_km, 3)
    return ndimage.map_coordinates(field, [yy + wy, xx + wx], order=1, mode="nearest").astype(np.float32)


def relief(zone, rules, seed):
    """Heights in metres over the sea, from the zones and the rules."""
    n = zone.shape[0]
    world_km = rules["world_km"]
    km_px = world_km / n
    recipes = rules["relief"]
    yy, xx = (np.mgrid[0:n, 0:n].astype(np.float32) + 0.5) / n
    land = zone != "ocean"
    names = [z for z in np.unique(zone) if z != "ocean"]
    heights, masks = {}, {}
    for k, name in enumerate(names):
        r = recipes.get(name, {"kind": "lowland", "base": 50, "rise": 0, "amp": 10, "km": 5})
        if r["kind"] == "same_as":
            r = recipes[r["zone"]]
        m = zone == name
        masks[name] = m
        rows = np.nonzero(m.any(axis=1))[0]
        lo, hi = (rows.min() + 0.5) / n, (rows.max() + 0.5) / n
        progress = np.clip((yy - lo) / max(hi - lo, 1e-3), 0, 1)     # south->north across the zone
        inside_km = ndimage.distance_transform_edt(m) * km_px            # how deep inside it
        sd = seed + 1000 + 37 * k
        kind = r["kind"]
        if kind == "dunes":
            def dune_field(km, wobble, sd_):
                phase = yy * world_km / km + wobble * fbm32(n, sd_, 20, world_km, 3)
                f = phase - np.floor(phase)
                return np.where(f < 0.75, f / 0.75, (1 - f) / 0.25)      # long windward, steep lee
            strength = 0.4 + 0.6 * (fbm32(n, sd + 1, 12, world_km, 3) * 0.5 + 0.5)
            # Draa (great ridges of sand) and the dunes riding on them.
            h = (r["base"] + r["rise"] * progress + r.get("draa_m", 0) * dune_field(r.get("draa_km", 5), 2.0, sd + 3)
                 + r["dune_m"] * dune_field(r["dune_km"], 3.0, sd) * strength + 25 * fbm32(n, sd + 2, 30, world_km, 3))
        elif kind == "mesas":
            q = (fbm32(n, sd, r["km"], world_km, 4) * 0.5 + 0.5) * r["levels"]
            step = np.floor(q) + smoothstep(0.82, 1.0, q - np.floor(q))
            h = r["base"] + r["rise"] * progress + r["step_m"] * step
        elif kind == "table":
            h = r["base"] + r["amp"] * fbm32(n, sd, r["km"], world_km, 3)
        elif kind == "canyon":
            h = np.zeros((n, n), np.float32)       # cut into its neighbours below
        elif kind == "hills":
            bumps = (fbm32(n, sd, r["km"], world_km, 4) * 0.5 + 0.5) ** 1.4
            small = fbm32(n, sd + 5, r["km"] / 3.0, world_km, 3)
            h = r["base"] + r["rise"] * progress + r["amp"] * bumps + 0.25 * r["amp"] * small
        elif kind == "lowland":
            h = r["base"] + r["rise"] * progress + r["amp"] * fbm32(n, sd, r["km"], world_km, 3)
        elif kind == "ridges":
            ridge = warped(ridged(n, sd, r["km"], world_km), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.35)
            # The range's height swells and sinks along it: massifs and saddles.
            massif = 0.65 + 0.35 * (fbm32(n, sd + 11, r["km"] * 3, world_km, 2) * 0.5 + 0.5)
            envelope = smoothstep(0.0, 14.0, inside_km)
            h = r["base"] + (r["peak"] - r["base"]) * envelope * massif * ridge ** 1.5
        elif kind == "bowl":
            cy, cx = ndimage.center_of_mass(m)
            rad = np.sqrt(m.sum() / np.pi)
            d = np.hypot(yy * n - cy, xx * n - cx) / rad
            h = r["floor"] + (r["rim"] - r["floor"]) * np.clip(d, 0, 1.3) ** 2.4 + 20 * fbm32(n, sd, 4, world_km, 3)
        elif kind == "valley":
            h = r["floor_west"] + (r["floor_east"] - r["floor_west"]) * xx + r["amp"] * fbm32(n, sd, 4, world_km, 3)
        elif kind == "spikes":
            sharp = warped(ridged(n, sd, r["km"], world_km, octaves=7), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.3) ** 2.0
            envelope = smoothstep(0.0, 8.0, inside_km)
            h = r["base"] + (r["peak"] - r["base"]) * envelope * sharp
        elif kind == "tilted":
            h = np.zeros((n, n), np.float32)
            parts, count = ndimage.label(m)
            rng = np.random.default_rng(sd)
            for i in range(1, count + 1):
                one = parts == i
                cy, cx = ndimage.center_of_mass(one)
                ang = rng.uniform(0, 2 * np.pi)
                proj = ((xx * n - cx) * np.cos(ang) + (yy * n - cy) * np.sin(ang))
                span = max(np.abs(proj[one]).max(), 1.0)
                q = np.clip(proj / span * 0.5 + 0.5, 0, 1) * r["terraces"]
                stepped = (np.floor(q) + smoothstep(0.85, 1.0, q - np.floor(q))) / r["terraces"]
                h = np.where(one, r["low"] + (r["high"] - r["low"]) * stepped, h)
        else:
            raise ValueError(f"relief kind {kind}")
        heights[name] = h.astype(np.float32)

    # Blend: every zone's own heights, weighted by how far inside it a point is.
    tr = rules["transitions"]
    sigma = tr["blend_km"] / km_px / 2.0
    total = np.zeros((n, n), np.float32)
    weight = np.zeros((n, n), np.float32)
    for name in names:
        if recipes.get(name, {}).get("kind") == "canyon":
            continue
        w = ndimage.gaussian_filter(masks[name].astype(np.float32), sigma)
        total += w * heights[name]
        weight += w
    h = np.where(weight > 1e-6, total / np.maximum(weight, 1e-6), 0.0).astype(np.float32)
    # Cliffs: where two zones meet in a step, each keeps its own height right
    # to the edge - no blend across it.
    band = 2.5 * sigma
    for a, b in tr["cliffs"]:
        if a not in masks or b not in masks:
            continue
        da = ndimage.distance_transform_edt(~masks[a])
        db = ndimage.distance_transform_edt(~masks[b])
        near = np.maximum(da, db) < band
        hard = np.where(masks[a], heights[a], np.where(masks[b], heights[b], h))
        fade = 1.0 - smoothstep(0.4 * band, band, np.maximum(da, db))
        h = np.where(near & (masks[a] | masks[b]), h + (hard - h) * fade, h)
    # Canyons: cut down into whatever is around, U-shaped across.
    for name in names:
        r = recipes.get(name, {})
        if r.get("kind") != "canyon":
            continue
        m = masks[name]
        depth_in = ndimage.distance_transform_edt(m)
        width = max(depth_in.max(), 1.0)
        floor = smoothstep(0.0, 0.6 * width, depth_in)
        around = ndimage.grey_dilation(np.where(m, -1e9, h), size=5)
        level = np.where(m, np.maximum(around, h), h)
        h = np.where(m, level - r["depth"] * floor, h)

    # Coasts and the sea floor, by the kind of coast each zone has.
    coasts = tr["coasts"]
    d_in = ndimage.distance_transform_edt(land) * km_px
    d_out, idx = ndimage.distance_transform_edt(~land, return_indices=True)
    d_out *= km_px
    nearest = zone[idx[0], idx[1]]                  # the land zone each sea point faces
    kinds = {z: coasts.get(z, coasts["default"]) for z in np.unique(nearest)}
    facing = np.where(land, zone, nearest)
    coast_land = np.empty((n, n), dtype=object)
    for z, kd in kinds.items():
        coast_land[facing == z] = kd
    # A black shore: a narrow beach, then straight up into the land.
    beach = 1.5 + 15.0 * d_in + 500.0 * np.maximum(d_in - 1.5, 0.0)
    flat = 0.6 + 1.2 * d_in
    h = np.where(land & (coast_land == "beach"), np.minimum(h, beach), h)
    h = np.where(land & (coast_land == "flat"), np.minimum(h, flat), h)
    h = np.where(land, np.maximum(h, 0.8), h)
    shelf = -(25.0 + 350.0 * smoothstep(0, 30, d_out) + 1100.0 * smoothstep(30, 140, d_out))
    steep = -(60.0 + 300.0 * smoothstep(0, 4, d_out) + 1100.0 * smoothstep(20, 120, d_out))
    sea = np.where(coast_land == "cliff", steep, shelf) + 15.0 * fbm32(n, seed + 77, 20, world_km, 3)
    h = np.where(land, h, np.minimum(sea, -2.0))
    return h.astype(np.float32)


def thermal(h, zone, rules, protect, iterations):
    """Thermal erosion (Jacobi, eight neighbours): wherever ground stands
    steeper than its zone's angle of repose, the excess slides down - peaks
    are cut, scree aprons form, slopes settle at the angle. `protect`: cells
    that keep their height (cliffs that must stay sheer)."""
    n = h.shape[0]
    cell = rules["world_km"] * 1000.0 / n
    er = rules["erosion"]
    angles = er.get("talus_deg", {})
    talus = np.full(h.shape, np.tan(np.radians(angles.get("default", 34.0))), np.float32)
    for name, deg in angles.items():
        if name != "default":
            talus[zone == name] = np.tan(np.radians(deg))
    rate = np.float32(er.get("thermal_rate", 0.25))
    steps = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
             (1, 1, 1.4142), (1, -1, 1.4142), (-1, 1, 1.4142), (-1, -1, 1.4142)]
    land = zone != "ocean"
    keep = protect | ~land
    h = h.astype(np.float32).copy()
    for _ in range(iterations):
        out = np.zeros_like(h)
        moved = np.zeros_like(h)
        excesses = []
        for dy, dx, d in steps:
            lower = np.roll(np.roll(h, dy, axis=0), dx, axis=1)
            excess = np.maximum(h - lower - talus * cell * d, 0.0)
            excesses.append(excess)
            out += excess
        total = np.maximum(out, 1e-6)
        # Move a share of the biggest excess, split by each neighbour's share.
        biggest = np.max(np.stack(excesses), axis=0)
        give = np.where(keep, 0.0, rate * biggest * 0.5)
        h -= give
        for (dy, dx, d), excess in zip(steps, excesses):
            share = give * excess / total
            moved += np.roll(np.roll(share, -dy, axis=0), -dx, axis=1)
        h += np.where(keep, 0.0, moved)
    return h


def preview(h, rules, path):
    # Hillshade (light from the north-west, slopes exaggerated) over a
    # colour by height: what the relief does, at a glance.
    cell = rules["world_km"] * 1000.0 / h.shape[0]
    gy, gx = np.gradient(h.astype(np.float64), cell)
    nx_, ny_, nz_ = -gx * 3.0, -gy * 3.0, np.ones_like(gx)
    norm = np.sqrt(nx_ ** 2 + ny_ ** 2 + 1.0)
    light = np.array([-0.5, 0.5, 0.7]); light /= np.linalg.norm(light)
    shade = np.clip((nx_ * light[0] + ny_ * light[1] + nz_ * light[2]) / norm, 0.0, 1.0)
    stops = np.array([0, 200, 600, 1200, 2000, 3000, 4500])
    cols = np.array([[70, 110, 60], [150, 150, 90], [190, 160, 110], [160, 140, 110], [140, 130, 120], [200, 200, 200], [250, 250, 255]], float)
    hc = np.clip(h, 0, 4500)
    colour = np.stack([np.interp(hc, stops, cols[:, c]) for c in range(3)], -1) / 255.0
    rgb = colour * (0.35 + 0.85 * shade[..., None])
    rgb[h <= 0] = (0.12, 0.2, 0.38)
    Image.fromarray((np.clip(rgb, 0, 1)[::-1] * 255).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS).save(path)


def protected(zone, rules, width):
    """Cliffs that must stay sheer: the steps between zones and the cut coasts."""
    protect = np.zeros(zone.shape, bool)
    for a, b in rules["transitions"]["cliffs"]:
        ma, mb = zone == a, zone == b
        protect |= (ndimage.binary_dilation(ma, iterations=width) & mb) | (ndimage.binary_dilation(mb, iterations=width) & ma)
    land = zone != "ocean"
    coasts = rules["transitions"]["coasts"]
    cut = np.zeros(zone.shape, bool)
    for name in np.unique(zone):
        if name != "ocean" and coasts.get(name, coasts["default"]) == "cliff":
            cut |= zone == name
    protect |= cut & ~ndimage.binary_erosion(land, iterations=width)
    return protect


def water_rules(zone, rules, seed):
    """What the ground gives to the rivers (R) and where deltas may form (G),
    as a picture, north up: from each zone's `rain`; none in the desert, the
    steppe and the black lands. B: the ground that swallows water (no rain)."""
    n = zone.shape[0]
    rain = {z["name"]: z["rain"] for z in rules["bands"] + rules["east_wing"]["zones"]}
    f = rules["features"]
    rain.update({"lake_valley": f["lake_valley"]["rain"], "steppe": 0.0, "steppe_canyon": 0.0, "cliff_coast": 0.0,
                 "river_marshes": 1.0, "isles": rules["isles"].get("rain", 0.3), "ocean": 0.0})
    runoff = np.zeros((n, n), np.float32)
    for name, r in rain.items():
        runoff[zone == name] = r
    # Soft edges (a kilometre or two): rain does not stop at a line.
    runoff = ndimage.gaussian_filter(runoff, 2.0)
    delta = ndimage.binary_dilation(zone == "river_marshes", iterations=max(2, n // 128))
    rgb = np.zeros((n, n, 3), np.uint8)
    rgb[..., 0] = np.clip(runoff * 255 + 0.5, 0, 255).astype(np.uint8)
    rgb[..., 1] = np.where(delta, 255, 0).astype(np.uint8)
    # Ground that swallows water: where the rules give no rain at all.
    dry = np.zeros((n, n), bool)
    for name, r in rain.items():
        if r <= 0 and name != "ocean":
            dry |= zone == name
    rgb[..., 2] = np.where(dry, 255, 0).astype(np.uint8)
    return rgb[::-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rules", default="content/config/terrain_core/world_rules.json")
    ap.add_argument("--out", default="terrain_lab_out/world")
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--heights", type=int, default=0, help="also the heightmap, this many texels a side (a multiple of --size)")
    ap.add_argument("--finish", default="", help="the eroded .r16 to finish: cliffs restored, written as <out>_final.r16")
    args = ap.parse_args()
    rules = json.load(open(args.rules, encoding="utf-8"))
    seed = rules["seed"] if args.seed is None else args.seed
    n = args.size

    zone, land, box = layout(rules, n, seed)
    if args.finish:
        # The eroded map (the lab's --erode output, raw 16-bit, south row first)
        # with the cliffs put back as the generator made them.
        raw = np.fromfile(args.finish, dtype="<u2")
        big = int(round(np.sqrt(raw.size)))
        eroded = raw.reshape(big, big).astype(np.float32) / 65535.0
        eroded = (eroded - 0.13) * 6000.0
        src = np.array(Image.open(args.out + "_height.png")).astype(np.float32)[::-1] / 65535.0
        src = (src - 0.13) * 6000.0
        src_big = ndimage.zoom(src, big / src.shape[0], order=1)
        # The cliffs' mask at twice the layout's resolution, then stretched.
        zone2 = np.repeat(np.repeat(zone, 2, axis=0), 2, axis=1)
        mask2 = ndimage.gaussian_filter(protected(zone2, rules, 2).astype(np.float32), 0.7)
        keep = ndimage.zoom(mask2, big / mask2.shape[0], order=1)
        h = eroded + (src_big - eroded) * np.clip(keep * 1.5, 0, 1)
        out = np.clip(h / 6000.0 + 0.13, 0, 1)
        (out * 65535 + 0.5).astype("<u2").tofile(args.out + "_final.r16")
        preview(h, rules, args.out + "_final_preview.png")
        print(f"finished {big}^2: {h.min():.0f} .. {h.max():.0f} m; wrote {args.out}_final.r16")
        return
    if args.heights:
        big = args.heights
        f = big // n
        zone_big = np.repeat(np.repeat(zone, f, axis=0), f, axis=1)
        h = relief(zone_big, rules, seed)
        protect = protected(zone_big, rules, 2)
        h = thermal(h, zone_big, rules, protect, rules["erosion"].get("thermal_iterations", 60))
        sea, rng_m = 0.13, 6000.0
        v = np.clip(h / rng_m + sea, 0, 1)
        Image.fromarray((v[::-1] * 65535 + 0.5).astype(np.uint16)).save(args.out + "_height.png")
        preview(h, rules, args.out + "_height_preview.png")
        print(f"heights {big}^2: {h.min():.0f} .. {h.max():.0f} m; wrote {args.out}_height.png")

    # The layout as a picture, north up, with a legend.
    rgb = np.zeros((n, n, 3), np.uint8)
    for name, colour in ZONE_COLOURS.items():
        rgb[zone == name] = colour
    pic = Image.fromarray(rgb[::-1])
    legend = Image.new("RGB", (n + 230, n), (16, 16, 20))
    legend.paste(pic, (0, 0))
    d = ImageDraw.Draw(legend)
    for k, (name, colour) in enumerate(ZONE_COLOURS.items()):
        d.rectangle([n + 10, 12 + 22 * k, n + 26, 28 + 22 * k], fill=colour)
        d.text((n + 34, 14 + 22 * k), name, fill=(230, 230, 230))
    legend.save(args.out + "_zones.png")

    Image.fromarray(water_rules(zone, rules, seed)).save(args.out + "_water.png")

    # Biome ids, as the lab reads them (north up).
    biome_of = {z["name"]: z["biome"] for z in rules["bands"] + rules["east_wing"]["zones"]}
    biome_of.update({"ocean": 0, "steppe": 3, "steppe_canyon": 3, "river_marshes": 1, "cliff_coast": 4,
                     "lake_valley": rules["features"]["lake_valley"]["biome"], "isles": rules["isles"]["biome"]})
    ids = np.zeros((n, n), np.uint8)
    for name, b in biome_of.items():
        ids[zone == name] = b
    Image.fromarray(ids[::-1]).save(args.out + "_biomes.png")
    print(f"land {land.mean() * 100:.0f}% of the world; wrote {args.out}_zones.png, {args.out}_biomes.png")


if __name__ == "__main__":
    main()
