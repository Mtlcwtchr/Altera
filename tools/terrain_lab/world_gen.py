"""World generator driven by rules (doc/rework/WORLD_GENERATOR_PLAN.md).

    python tools/terrain_lab/world_gen.py [--rules content/config/terrain_core/world_rules.json]
                                          [--out terrain_lab_out/world] [--size 2048] [--seed N]
                                          [--heights 2048] [--finish eroded.r16]

Layout: the continent inside the world (ocean all round, the top of the map
left for the northern isles, taken from an imported heightmap), its zones from
the rules, the special places. Writes <out>_zones.png (a picture of the
layout), <out>_biomes.png (biome ids, as the lab's --biomes reads them) and
<out>_water.png (what each zone gives to the rivers: --water-rules).
Relief: a recipe per zone, blends and steps between them, coasts by kind;
then thermal erosion (here at layout size, on the GPU for the finished map).

Coordinates: x west->east, y south->north, 0..1 over the world; pictures are
written north up. On the continent: s west->east, t south->north, 0..1 over
its box.
"""
import argparse
import json

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

COAST_BEACH, COAST_CLIFF, COAST_FLAT = 0, 1, 2


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
    "black_lands": (40, 30, 40), "isles": (110, 120, 110), "dragon_cliff": (60, 130, 60),
    "steppe": (170, 180, 90),
}


def load_isles(rules, n):
    """The northern isles from the imported heightmap: its land pieces lying
    wholly in the top `keep_top` of the picture, the biggest piece (the
    continent) apart. -> mask and heights (metres), at n*n, row 0 south."""
    cfg = rules["isles"]
    a = np.array(Image.open(cfg["source"])).astype(np.float32) / 65535.0
    h = (a - 0.13) * 6000.0
    ns = h.shape[0]
    lab, count = ndimage.label(h > 0)
    sizes = ndimage.sum(np.ones_like(h), lab, range(1, count + 1))
    main = int(np.argmax(sizes)) + 1
    keep = []
    for k, rows in enumerate(ndimage.find_objects(lab), start=1):
        if k != main and rows[0].stop / ns < cfg["keep_top"]:
            keep.append(k)
    mask = np.isin(lab, keep)
    mask = ndimage.zoom(mask.astype(np.float32), n / ns, order=1) > 0.5
    heights = ndimage.zoom(h, n / ns, order=1)
    return mask[::-1].copy(), heights[::-1].astype(np.float32).copy()


def layout(rules, n, seed):
    """-> zone (object array of names), land, box, coast (int8 kinds per pixel)."""
    world = rules["continent"]
    margin = world["margin"]
    yy, xx = (np.mgrid[0:n, 0:n] + 0.5) / n          # row = y (south first), col = x
    # The continent's box: ocean on every side, the top of the map for the isles.
    x0, x1 = margin, 1.0 - margin
    y0, y1 = margin, 1.0 - rules["isles"]["keep_top"] - 0.012
    # A rounded box, its edge wobbled: big bays and capes, and a ragged line.
    bx, by = (xx - (x0 + x1) / 2) / ((x1 - x0) / 2), (yy - (y0 + y1) / 2) / ((y1 - y0) / 2)
    # The outline: a rounded body (its width and height are the box above - the
    # continent stretches or narrows with it) whose coordinates are bent first,
    # so the coast folds into bays, capes and fjords, then noise at every scale
    # cuts it ragged. Not a box with a wobbled edge.
    e = world.get("exponent", 2.6)
    wx = bx + world.get("warp", 0.18) * fbm(n, seed + 3, octaves=4, base=4)
    wy = by + world.get("warp", 0.18) * fbm(n, seed + 4, octaves=4, base=4)
    shape = 1.0 - (np.abs(wx) ** e + np.abs(wy) ** e) ** (1.0 / e)
    shape += world["roughness"] * (fbm(n, seed + 1, octaves=3, base=2) + 0.6 * fbm(n, seed + 2, octaves=4, base=6)
                                   + 0.35 * fbm(n, seed + 5, octaves=5, base=16) + 0.15 * fbm(n, seed + 6, octaves=4, base=40))
    # The west side is cut deeper: bays and capes, not a wall of land.
    west = smoothstep(-0.1, 0.9, -bx)
    shape -= world.get("west_bite", 0.0) * west * (0.5 + 0.5 * fbm(n, seed + 8, octaves=4, base=5)
                                                  + 0.4 * fbm(n, seed + 9, octaves=4, base=11))
    land = shape > world.get("threshold", 0.10)
    land = ndimage.binary_opening(land, iterations=2)
    parts, count = ndimage.label(land)
    if count > 1:
        sizes = ndimage.sum(land, parts, range(1, count + 1))
        land = parts == (int(np.argmax(sizes)) + 1)
    land = ndimage.binary_fill_holes(land)

    warp = rules["warp"]
    # The continent's own coordinates, bent: no border runs straight.
    s = (xx - x0) / (x1 - x0) + warp["coord"] * fbm(n, seed + 31, octaves=5, base=3)
    t = (yy - y0) / (y1 - y0) + warp["coord"] * fbm(n, seed + 32, octaves=5, base=3)

    # The bands, south to north, sharing their edges: all bent together
    # widely, each wobbled on its own. Never crossing.
    bands = rules["bands"]
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
        lo = edges[k] if k > 0 else np.full((n, n), -0.2)
        hi = edges[k + 1] if k + 1 < len(bands) else np.full((n, n), 1.2)
        return lo + float(frac) * (hi - lo)

    wing = rules["east_wing"]
    wobble = fbm(n, seed + 40, octaves=4, base=6) + 0.7 * fbm(n, seed + 41, octaves=3, base=14)
    east = s + wing["tilt"] * (t - 0.5) >= wing["s"] + wing["warp"] * wobble
    for z in wing["zones"]:
        zone[land & east & (t >= place(z["from"])) & (t < place(z["to"]))] = z["name"]

    f = rules["features"]
    rng = np.random.default_rng(seed + 50)
    # The round valley with its lake, in the mountains.
    lv = f["lake_valley"]
    cx = x0 + lv["s"] * (x1 - x0)
    col = int(cx * n)
    rows = np.nonzero(zone[:, col] == lv["in"])[0]
    cy = (rows.mean() + 0.5) / n if len(rows) else y0 + 0.75 * (y1 - y0)
    r = np.hypot(xx - cx, yy - cy)
    valley = (r < lv["radius"] * (x1 - x0) * (1 + 0.06 * fbm(n, seed + 300, octaves=3, base=6))) & land
    zone[valley] = "lake_valley"

    # The rocky canyon land is cut by its network of canyons; the biggest
    # stretches of top the network leaves standing are the steppe (green
    # pasture among the stone), each walled all round by canyon.
    si = f["steppe_islands"]
    rocky = zone == si["in"]
    rr = rules["relief"][si["in"]]
    c1, _ = canyon_network(n, rules["world_km"], seed, rr)
    thr = np.percentile(c1[rocky], 100 * (1 - rr["cover"]))
    standing = (c1 < thr - 0.03) & rocky
    standing = ndimage.binary_opening(standing, iterations=max(2, n // 256))
    parts, count = ndimage.label(standing)
    if count:
        sizes = ndimage.sum(standing, parts, range(1, count + 1))
        km2 = (rules["world_km"] / n) ** 2
        order_ = np.argsort(sizes)[::-1]
        picked = [int(k) + 1 for k in order_[:si["count"]] if sizes[k] * km2 >= si["min_km2"]]
        zone[np.isin(parts, picked)] = "steppe"

    # River marshes: the marshes near the sea - the deltas.
    to_sea_km = ndimage.distance_transform_edt(land) * rules["world_km"] / n
    rm = f["river_marshes"]
    zone[(zone == rm["in"]) & (to_sea_km < rm["coast_band"] * (x1 - x0) * rules["world_km"])] = "river_marshes"
    # The valley's river runs east and ends in marshes there: its delta.
    vd = f["valley_delta"]
    zone[(zone == vd["in"]) & (s >= vd["s"]) & (to_sea_km < vd["reach"] * (x1 - x0) * rules["world_km"])] = "river_marshes"

    # Where the coast is cliff and where beach: stretches, by a slow noise
    # along it. The desert's cliff stretches are lined with a strip of green
    # cliff land under giant trees; the rest of the desert runs into the sea.
    cn = rules["coast"]
    stretch_noise = fbm(n, seed + 700, octaves=3, base=max(2, int(round(rules["world_km"] / cn["noise_km"]))))
    cliff_stretch = stretch_noise > cn["threshold"]
    dc = f["dragon_cliff"]
    # A narrow strip at the very south only: where the sea lies to the south of
    # the desert, between two places along it.
    _, near_sea = ndimage.distance_transform_edt(land, return_indices=True)
    rows_ = np.arange(n)[:, None]
    south_facing = (rows_ - near_sea[0]) > 0.7 * to_sea_km / (rules["world_km"] / n)
    along = (xx - x0) / (x1 - x0)
    stretch = (along > dc["s_from"]) & (along < dc["s_to"])
    zone[(zone == dc["in"]) & stretch & south_facing & (to_sea_km < dc["width_km"])] = "dragon_cliff"

    # Narrow sea inlets cut into the desert and the rocky steppe (the plateau
    # keeps its cliff): winding gulfs that the land runs down into smoothly.
    inl = rules.get("inlets")
    if inl:
        km_px = rules["world_km"] / n
        land_now = zone != "ocean"
        d_in = ndimage.gaussian_filter(ndimage.distance_transform_edt(land_now).astype(np.float32), 6)
        gy, gx = np.gradient(d_in)
        okz = np.isin(zone, inl["zones"])
        edge = land_now & okz & (ndimage.distance_transform_edt(land_now) < 3)
        ey, ex = np.nonzero(edge)
        irng = np.random.default_rng(seed + 90)
        taken = []
        carve = np.zeros((n, n), bool)
        yg, xg = np.mgrid[0:n, 0:n]
        tries = 0
        while len(taken) < inl["count"] and len(ey) and tries < 400:
            tries += 1
            k = irng.integers(len(ey))
            py, px = float(ey[k]), float(ex[k])
            if any(np.hypot(py - a, px - b) * km_px < inl["spacing_km"] for a, b in taken):
                continue
            taken.append((py, px))
            length = irng.uniform(*inl["length_km"]) / km_px
            width = irng.uniform(*inl["width_km"]) / km_px
            ang = np.arctan2(gy[int(py), int(px)], gx[int(py), int(px)])
            wander = 0.0
            steps = int(length)
            for i in range(steps):
                wander += irng.normal(0, 0.06) - 0.04 * wander
                a_ = ang + wander + 0.5 * np.sin(i / max(length, 1) * 7 + k)
                py += np.sin(a_); px += np.cos(a_)
                if not (3 < py < n - 3 and 3 < px < n - 3):
                    break
                if not okz[int(py), int(px)] and i > 4:
                    break
                w_ = width * (0.55 + 0.45 * np.sin(np.pi * min(i / steps, 1.0) ** 0.6)) * (0.8 + 0.2 * np.sin(i / 9.0))
                y0_, y1_, x0_, x1_ = int(py - w_) - 1, int(py + w_) + 2, int(px - w_) - 1, int(px + w_) + 2
                sub = (yg[y0_:y1_, x0_:x1_] - py) ** 2 + (xg[y0_:y1_, x0_:x1_] - px) ** 2 < w_ * w_
                carve[y0_:y1_, x0_:x1_] |= sub
            # the mouth: open it to the sea
            carve |= (np.hypot(yg - taken[-1][0], xg - taken[-1][1]) < width * 0.9)
        zone[carve & okz] = "ocean"
        to_sea_km = ndimage.distance_transform_edt(zone != "ocean") * km_px

    # The northern isles, as imported.
    isle_mask, isle_heights = load_isles(rules, n)
    zone[isle_mask & ~land] = "isles"
    all_land = zone != "ocean"

    # The coast kind of every pixel, by the zone of the land nearest to it.
    coasts = rules["transitions"]["coasts"]
    _, idx = ndimage.distance_transform_edt(~all_land, return_indices=True)
    facing = np.where(all_land, zone, zone[idx[0], idx[1]])
    coast = np.full((n, n), COAST_CLIFF, np.int8)
    for name in np.unique(facing):
        if name == "ocean":
            continue
        kind = coasts.get(name, coasts["default"])
        sel = facing == name
        if kind == "beach":
            coast[sel] = COAST_BEACH
        elif kind == "flat":
            coast[sel] = COAST_FLAT
        elif kind == "mixed":
            coast[sel] = np.where(cliff_stretch[sel], COAST_CLIFF, COAST_BEACH)
        else:
            coast[sel] = COAST_CLIFF
    return zone, all_land, (x0, x1, y0, y1), coast, (isle_mask, isle_heights)


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


def canyon_network(n, world_km, seed, r):
    """The canyon network of the rocky steppe: two crest fields (the great
    canyons and the lesser ones). Where a field is high there is canyon. The
    same in the layout (which parts stay as steppe) and in the relief."""
    c1 = warped(ridged(n, seed + 5000, r["km"], world_km, octaves=5), n, seed + 5009, r["km"] * 2, world_km, r["km"] * 0.5)
    c2 = ridged(n, seed + 5021, r["km"] / 3, world_km, octaves=4)
    return c1, c2


def ranges(zone, m, r, sd, n, world_km, xx, yy):
    """The white mountains: two long ranges running west->east that draw
    together and close into a ring around the lake valley (the caldera). Both
    fall away smoothly, on the outer sides, into the foothills and the river
    valley."""
    km_px = world_km / n
    lake = zone == "lake_valley"
    band = m | lake
    has = band.any(axis=0)
    cols = np.arange(n)
    lo = np.argmax(band, axis=0).astype(np.float32)
    hi = (n - 1 - np.argmax(band[::-1], axis=0)).astype(np.float32)
    lo = np.interp(cols, cols[has], lo[has])
    hi = np.interp(cols, cols[has], hi[has])
    sm = n / 30.0
    lo, hi = ndimage.gaussian_filter1d(lo, sm), ndimage.gaussian_filter1d(hi, sm)
    yc = (lo + hi) / 2 * km_px
    half = (hi - lo) / 2 * km_px                       # half the band's width, km
    Y, X = yy * world_km, xx * world_km
    bend = r.get("bend_km", 7.0) * fbm32(n, sd + 21, 70, world_km, 3)
    if lake.any():
        cy, cx = ndimage.center_of_mass(lake)
        cy, cx = cy * km_px, cx * km_px
        ring_r = np.sqrt(lake.sum() / np.pi) * km_px
    else:
        cy, cx, ring_r = float(yc.mean()), world_km / 2, 0.0
    # One long range along the band, bending as it goes. On it stands a great
    # peak whose top has fallen in: its slopes are the skirt, the fallen top a
    # bowl (the lake valley), the rim left broken and uneven, lower in places.
    ycol = np.broadcast_to(yc[None, :], (n, n))
    R = max(ring_r * 1.15, 1.0)
    w = r["width_km"]
    def prof(d):
        return np.exp(-(d / w) ** 2) + 0.25 * np.exp(-(d / (2.6 * w)) ** 2)
    spine = cy + (ycol + bend - cy) * smoothstep(0.0, 2.0 * R, np.abs(X - cx)) if lake.any() else ycol + bend
    line = prof(np.abs(Y - spine) / 1.25) * (0.7 + 0.3 * smoothstep(0.0, 3.0 * R, 3.0 * R - np.abs(X - cx)))
    rad = np.hypot(X - cx, Y - cy)
    skirt = 0.85 * np.exp(-(rad / (2.3 * R)) ** 2)
    crest = np.clip(np.maximum(line, skirt), 0, 1)
    detail = warped(ridged(n, sd, r["km"], world_km), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.35)
    massif = 0.6 + 0.4 * (fbm32(n, sd + 11, r["km"] * 3, world_km, 2) * 0.5 + 0.5)
    signed = (ndimage.distance_transform_edt(m | lake) - ndimage.distance_transform_edt(~(m | lake))) * km_px
    envelope = smoothstep(-r["fade_out_km"], r["fade_in_km"], signed)
    # Alpine fractal relief: ridged multifractals at three scales stretched
    # over the broad crest: main arêtes, side spurs, rock teeth.
    fine = ridged(n, sd + 3, r["km"] / 3, world_km, octaves=6)
    teeth = ridged(n, sd + 5, r["km"] / 9, world_km, octaves=4)
    # Stretched along the range (west->east): crests run lengthwise.
    gy, gx = np.mgrid[0:n, 0:n].astype(np.float32)
    def along(f, k):
        return ndimage.map_coordinates(f, [gy, (gx - n / 2) / k + n / 2], order=1, mode="reflect")
    detail, fine, teeth = along(detail, 2.4), along(fine, 1.8), along(teeth, 1.3)
    frac = 0.55 * detail ** 1.3 + 0.30 * fine ** 1.3 + 0.15 * teeth
    frac = np.clip((frac - 0.15) / 0.6, 0.0, 1.0)
    return r["base"] + (r["peak"] - r["base"]) * envelope * massif * crest ** 0.9 * (0.05 + 0.95 * frac ** 1.4)


def caldera(h, zone, r, rules, seed, n):
    """Second pass: the fallen top of the great peak. A flat floor, walls that
    climb in a fractal rim (ridged crests, notches where it slumped), merged
    into the range around it."""
    km_px = rules["world_km"] / n
    world_km = rules["world_km"]
    lake = zone == "lake_valley"
    if not lake.any():
        return h
    yy, xx = (np.mgrid[0:n, 0:n].astype(np.float32) + 0.5)
    cy, cx = ndimage.center_of_mass(lake)
    R = np.sqrt(lake.sum() / np.pi)
    d = np.hypot(yy - cy, xx - cx) / R
    ang = np.arctan2(yy - cy, xx - cx)
    warp = 0.10 * fbm32(n, seed + 601, R * km_px * 0.8, world_km, 3)
    d = d * (1.0 + warp)
    crest = ridged(n, seed + 611, 7.0, world_km, octaves=6)
    notch = fbm(n, seed + 612, octaves=3, base=3)
    rim_top = r["rim"] + r.get("rim_var", 700) * (crest - 0.5) + 350 * notch
    wall = smoothstep(0.55, 1.0, d)
    wall = wall ** 1.15 * (0.75 + 0.25 * crest)
    floor = r["floor"] + 25 * fbm32(n, seed + 613, 4, world_km, 3) + 120 * smoothstep(0.2, 0.6, d) * (crest - 0.3)
    cal = floor + (rim_top - floor) * wall
    inside = d < 1.0
    # beyond the rim: the range's own heights, lifted so that the rim sinks into them
    out = np.maximum(h, rim_top * (1.0 - smoothstep(1.0, 1.55, d)) + h * smoothstep(1.0, 1.55, d))
    out = np.where(d < 1.0, cal, out)
    return out.astype(np.float32)


def relief(zone, rules, seed, coast, isles):
    """Heights in metres over the sea, from the zones and the rules."""
    n = zone.shape[0]
    world_km = rules["world_km"]
    km_px = world_km / n
    recipes = rules["relief"]
    yy, xx = (np.mgrid[0:n, 0:n].astype(np.float32) + 0.5) / n
    land = zone != "ocean"
    names = [z for z in np.unique(zone) if z != "ocean"]
    heights, tops, masks = {}, {}, {}
    deferred = []
    for k, name in enumerate(names):
        m = zone == name
        masks[name] = m
        if name == "isles":
            heights[name] = isles[1]
            continue
        r = recipes.get(name, {"kind": "lowland", "base": 50, "rise": 0, "amp": 10, "km": 5})
        if r["kind"] == "lift_of":
            deferred.append(name)
            continue
        rows = np.nonzero(m.any(axis=1))[0]
        lo, hi = (rows.min() + 0.5) / n, (rows.max() + 0.5) / n
        progress = np.clip((yy - lo) / max(hi - lo, 1e-3), 0, 1)     # south->north across the zone
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
        elif kind == "canyons":
            # A plateau top cut by a network of canyons, most of it canyon
            # and dry stone floor: the top stays only as mesas between them.
            # (the steppe cut from it is the same land: the thresholds from both.)
            land_m = m | (zone == "steppe")
            top = r["base"] + r["rise"] * progress + r.get("bumps", 30) * fbm32(n, sd + 3, 6, world_km, 3)
            c1, c2 = canyon_network(n, world_km, seed, r)
            thr = np.percentile(c1[land_m] if land_m.any() else c1, 100 * (1 - r["cover"]))
            thr2 = np.percentile(c2[land_m] if land_m.any() else c2, 78)
            d1 = smoothstep(thr, thr + r.get("wall", 0.05), c1)
            d2 = smoothstep(thr2, thr2 + r.get("wall", 0.05), c2)
            h = top - r["depth"] * d1 - 0.35 * r["depth"] * d2 * (1 - d1) + 10 * fbm32(n, sd + 4, 2, world_km, 3) * d1
            tops[name] = top
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
            h = ranges(zone, m, r, sd, n, world_km, xx, yy)
        elif kind == "bowl":
            h = np.zeros((n, n), np.float32)        # (made later: caldera(), over the range)
        elif kind == "valley":
            h = r["floor_west"] + (r["floor_east"] - r["floor_west"]) * xx + r["amp"] * fbm32(n, sd, 4, world_km, 3)
        elif kind == "spikes":
            sharp = warped(ridged(n, sd, r["km"], world_km, octaves=7), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.3) ** 2.0
            # Rising out of the valley without a step: the envelope is a long ramp.
            inside_km = ndimage.distance_transform_edt(m) * km_px
            envelope = smoothstep(0.0, r["fade_km"], inside_km)
            h = r["base"] + (r["peak"] - r["base"]) * envelope * sharp
        else:
            raise ValueError(f"relief kind {kind}")
        heights[name] = h.astype(np.float32)
    if "lake_valley" in heights and "white_mountains" in heights:
        heights["lake_valley"] = heights["white_mountains"]
    for name in deferred:
        r = recipes[name]
        base = (tops if r.get("top") else heights).get(r["zone"])
        if base is None:
            continue
        sd = seed + 2000 + len(heights)
        heights[name] = (base + r["lift"] + r["amp"] * fbm32(n, sd, 3, world_km, 3)).astype(np.float32)

    # Blend: every zone's own heights, weighted by how far inside it a point is
    # (a zone reaches as far as its own blend distance).
    tr = rules["transitions"]
    total = np.zeros((n, n), np.float32)
    weight = np.zeros((n, n), np.float32)
    for name in names:
        if recipes.get(name, {}).get("kind") == "canyon" or name == "isles":
            continue
        sigma = tr["blend_overrides"].get(name, tr["blend_km"]) / km_px / 2.0
        w = ndimage.gaussian_filter(masks[name].astype(np.float32), sigma)
        total += w * heights[name]
        weight += w
    h = np.where(weight > 1e-6, total / np.maximum(weight, 1e-6), 0.0).astype(np.float32)
    if "isles" in masks:
        h = np.where(masks["isles"], heights["isles"], h)
    if "lake_valley" in masks:
        h = caldera(h, zone, recipes["lake_valley"], rules, seed, n)
    # Cliffs: where two zones meet in a step, each keeps its own height right
    # to the edge - no blend across it.
    band = 2.5 * tr["blend_km"] / km_px / 2.0
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

    # Coasts and the sea floor, by the kind of coast each pixel has.
    d_in = ndimage.distance_transform_edt(land) * km_px
    d_out = ndimage.distance_transform_edt(~land) * km_px
    # A beach: a gentle shore, and for a black one a quick climb behind it.
    beach = 1.5 + 15.0 * d_in + 40.0 * np.maximum(d_in - 4.0, 0.0) ** 1.6
    flat = 0.6 + 1.2 * d_in
    keep_isles = masks.get("isles", np.zeros_like(land))
    h = np.where(land & (coast == COAST_BEACH) & ~keep_isles, np.minimum(h, beach), h)
    h = np.where(land & (coast == COAST_FLAT), np.minimum(h, flat), h)
    h = np.where(land, np.maximum(h, 0.8), h)
    shelf = -(25.0 + 350.0 * smoothstep(0, 30, d_out) + 1100.0 * smoothstep(30, 140, d_out))
    steep = -(60.0 + 300.0 * smoothstep(0, 4, d_out) + 1100.0 * smoothstep(20, 120, d_out))
    sea = np.where(coast == COAST_CLIFF, steep, shelf) + 15.0 * fbm32(n, seed + 77, 20, world_km, 3)
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


def protected(zone, rules, coast, width):
    """What keeps the generator's heights: the sheer steps between zones, the
    cliff coasts, and the zones the rules keep whole (canyons wait for their
    cliff meshes; the isles come eroded)."""
    protect = np.zeros(zone.shape, bool)
    for a, b in rules["transitions"]["cliffs"]:
        ma, mb = zone == a, zone == b
        protect |= (ndimage.binary_dilation(ma, iterations=width) & mb) | (ndimage.binary_dilation(mb, iterations=width) & ma)
    land = zone != "ocean"
    protect |= (coast == COAST_CLIFF) & land & ~ndimage.binary_erosion(land, iterations=width)
    for name in rules["erosion"].get("keep_zones", []):
        protect |= zone == name
    return protect


def water_rules(zone, rules, seed):
    """What the ground gives to the rivers (R) and where deltas may form (G),
    as a picture, north up: from each zone's `rain`; none in the desert, the
    steppe and the black lands. B: the ground that swallows water (no rain)."""
    n = zone.shape[0]
    rain = {z["name"]: z["rain"] for z in rules["bands"] + rules["east_wing"]["zones"]}
    f = rules["features"]
    rain.update({"lake_valley": f["lake_valley"]["rain"], "steppe": 0.0, "steppe_canyon": 0.0,
                 "dragon_cliff": f["dragon_cliff"]["rain"], "river_marshes": 1.0,
                 "isles": rules["isles"].get("rain", 0.3), "ocean": 0.0})
    runoff = np.zeros((n, n), np.float32)
    for name, r in rain.items():
        runoff[zone == name] = r
    # Soft edges (a kilometre or two): rain does not stop at a line.
    runoff = ndimage.gaussian_filter(runoff, max(2.0, n / 512))
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
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--heights", type=int, default=0, help="also the heightmap (--size, or a multiple of it)")
    ap.add_argument("--finish", default="", help="the eroded .r16 to finish: kept zones and cliffs restored, written as <out>_final.r16")
    args = ap.parse_args()
    rules = json.load(open(args.rules, encoding="utf-8"))
    seed = rules["seed"] if args.seed is None else args.seed
    n = args.size

    zone, land, box, coast, isles = layout(rules, n, seed)
    if args.finish:
        # The eroded map (the lab's --erode output, raw 16-bit, south row first)
        # with the zones the rules keep and the cliffs put back as made.
        raw = np.fromfile(args.finish, dtype="<u2")
        big = int(round(np.sqrt(raw.size)))
        eroded = raw.reshape(big, big).astype(np.float32) / 65535.0
        eroded = (eroded - 0.13) * 6000.0
        src = np.array(Image.open(args.out + "_height.png")).astype(np.float32)[::-1] / 65535.0
        src = (src - 0.13) * 6000.0
        src_big = ndimage.zoom(src, big / src.shape[0], order=1)
        mask = protected(zone, rules, coast, 2).astype(np.float32)
        mask = ndimage.gaussian_filter(mask, 0.7)
        keep = ndimage.zoom(mask, big / mask.shape[0], order=1)
        h = eroded + (src_big - eroded) * np.clip(keep * 1.5, 0, 1)
        out = np.clip(h / 6000.0 + 0.13, 0, 1)
        (out * 65535 + 0.5).astype("<u2").tofile(args.out + "_final.r16")
        preview(h, rules, args.out + "_final_preview.png")
        print(f"finished {big}^2: {h.min():.0f} .. {h.max():.0f} m; wrote {args.out}_final.r16")
        return
    if args.heights:
        assert args.heights == n, "--heights must equal --size"
        h = relief(zone, rules, seed, coast, isles)
        protect = protected(zone, rules, coast, 2)
        h = thermal(h, zone, rules, protect, rules["erosion"].get("thermal_iterations", 60))
        sea, rng_m = 0.13, 6000.0
        v = np.clip(h / rng_m + sea, 0, 1)
        Image.fromarray((v[::-1] * 65535 + 0.5).astype(np.uint16)).save(args.out + "_height.png")
        preview(h, rules, args.out + "_height_preview.png")
        print(f"heights {n}^2: {h.min():.0f} .. {h.max():.0f} m; wrote {args.out}_height.png")

    # The layout as a picture, north up, with a legend.
    rgb = np.zeros((n, n, 3), np.uint8)
    for name, colour in ZONE_COLOURS.items():
        rgb[zone == name] = colour
    pic = Image.fromarray(rgb[::-1]).resize((1024, 1024), Image.NEAREST)
    legend = Image.new("RGB", (1024 + 230, 1024), (16, 16, 20))
    legend.paste(pic, (0, 0))
    d = ImageDraw.Draw(legend)
    for k, (name, colour) in enumerate(ZONE_COLOURS.items()):
        d.rectangle([1024 + 10, 12 + 22 * k, 1024 + 26, 28 + 22 * k], fill=colour)
        d.text((1024 + 34, 14 + 22 * k), name, fill=(230, 230, 230))
    legend.save(args.out + "_zones.png")

    Image.fromarray(water_rules(zone, rules, seed)).save(args.out + "_water.png")

    # Biome ids, as the lab reads them (north up).
    biome_of = {z["name"]: z["biome"] for z in rules["bands"] + rules["east_wing"]["zones"]}
    biome_of.update({"ocean": 0, "steppe": 3, "steppe_canyon": 3, "river_marshes": 1,
                     "dragon_cliff": rules["features"]["dragon_cliff"]["biome"],
                     "lake_valley": rules["features"]["lake_valley"]["biome"], "isles": rules["isles"]["biome"]})
    ids = np.zeros((n, n), np.uint8)
    for name, b in biome_of.items():
        ids[zone == name] = b
    Image.fromarray(ids[::-1]).save(args.out + "_biomes.png")
    print(f"land {land.mean() * 100:.0f}% of the world; wrote {args.out}_zones.png, {args.out}_biomes.png")


if __name__ == "__main__":
    main()
