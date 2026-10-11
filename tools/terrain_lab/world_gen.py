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
import heapq
import json
from pathlib import Path

import numpy as np
import world_zones
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


def load_shape(rules, n):
    """The continent from a picture (north up, land where it is lighter than the
    threshold): its biggest piece is the continent, the others the isles, which
    keep the picture's brightness as their height. -> land, (isle mask, heights)."""
    cfg = rules["shape"]
    a = np.array(Image.open(cfg["image"]).convert("L")).astype(np.float32)
    big = ndimage.zoom(a, n / a.shape[0], order=1)[::-1]
    mask = ndimage.gaussian_filter(big, 1.2) > cfg.get("threshold", 12)
    mask = ndimage.binary_fill_holes(mask)
    lab, count = ndimage.label(mask)
    sizes = ndimage.sum(mask, lab, range(1, count + 1))
    main = int(np.argmax(sizes)) + 1
    land = lab == main
    isles = mask & ~land
    if cfg.get("coast_km"):
        # The picture's coast is a row of hexagon scallops: the continent's signed
        # distance, blurred (the scallops melt), then pushed by a fractal noise, so
        # the shore is ragged at every scale like a real one - bays, capes, coves.
        # (the isles keep the picture's outline: their islets are too small for it)
        km_px = rules["world_km"] / n
        sd = (ndimage.distance_transform_edt(land) - ndimage.distance_transform_edt(~land)) * km_px
        sd = ndimage.gaussian_filter(sd, cfg["coast_km"] / km_px)
        amp = cfg.get("coast_wander_km", 1.5)
        noise = 0.6 * fbm(n, rules["seed"] + 901, octaves=6, base=max(2, int(rules["world_km"] / 30.0)))
        noise += 0.4 * fbm(n, rules["seed"] + 902, octaves=4, base=max(2, int(rules["world_km"] / 5.0)))
        unit = 0.5 * (noise - noise.mean()) / max(float(np.std(noise)), 1e-6)
        moved = sd + amp * unit > 0
        lab2, _ = ndimage.label(moved)
        sizes2 = np.bincount(lab2.ravel()); sizes2[0] = 0
        land = ndimage.binary_fill_holes(lab2 == int(np.argmax(sizes2))) & ~isles
    heights = np.where(isles, np.maximum(big / 255.0 * cfg.get("isle_peak_m", 2600.0), 6.0), 0.0).astype(np.float32)
    return land, (isles, heights)


def pick(rng, spec):
    """A rule's number, or a range [lo, hi] the seed picks from."""
    if isinstance(spec, (list, tuple)):
        return float(rng.uniform(spec[0], spec[1]))
    return spec


def split_body(land, frac=0.05):
    """The land with its narrow tongues taken off (an opening by a big disk)."""
    r_open = frac * land.shape[0]
    core = ndimage.distance_transform_edt(land) > r_open
    return (ndimage.distance_transform_edt(~core) <= r_open) & land


def tongue_score(land):
    """The area of land that juts out south of the body as a narrow tongue."""
    n = land.shape[0]
    body = split_body(land)
    rows = np.nonzero(body.any(axis=1))[0]
    if not len(rows):
        return 0.0
    south = rows[0] + 0.04 * n
    tongue = land & ~body
    tongue[int(south):] = False
    parts, count = ndimage.label(tongue)
    if not count:
        return 0.0
    return float(ndimage.sum(tongue, parts, range(1, count + 1)).max()) / n ** 2


def continent(n, seed, rs, land_share, y_top):
    """The continent's shape, on its own: a broad oval of the seed's size, tilt and
    stretch, whose coast is cut by noise at every scale - the noise is warped by
    itself, so bays curl and capes hook. The land is whatever stands above a level
    chosen so that it fills the share of the world asked for."""
    yy, xx = (np.mgrid[0:n, 0:n] + 0.5) / n
    cy0 = 0.5 * y_top + 0.03
    ax, ay = rs.uniform(0.30, 0.37), rs.uniform(0.22, 0.28) * y_top / 0.76
    rot = rs.uniform(-0.3, 0.3)
    # the noise bends the space it is read in
    qx = xx + 0.10 * fbm(n, seed + 3, octaves=4, base=2) + 0.04 * fbm(n, seed + 4, octaves=4, base=6)
    qy = yy + 0.10 * fbm(n, seed + 5, octaves=4, base=2) + 0.04 * fbm(n, seed + 6, octaves=4, base=6)
    dx, dy = qx - 0.5, qy - cy0
    u = dx * np.cos(rot) + dy * np.sin(rot)
    v = -dx * np.sin(rot) + dy * np.cos(rot)
    oval = 1.0 - np.sqrt((u / ax) ** 2 + (v / ay) ** 2)
    amp = rs.uniform(0.50, 0.65)
    noise = (fbm(n, seed + 1, octaves=3, base=2) + 0.75 * fbm(n, seed + 2, octaves=4, base=4)
             + 0.45 * fbm(n, seed + 7, octaves=5, base=9) + 0.25 * fbm(n, seed + 8, octaves=5, base=22)
             + 0.12 * fbm(n, seed + 9, octaves=4, base=55))
    field = oval + amp * noise
    edge = np.minimum(np.minimum(xx, 1 - xx), np.minimum(yy, 1 - yy))
    field -= 2.5 * (1.0 - smoothstep(0.0, 0.08, edge)) + 2.5 * smoothstep(y_top, y_top + 0.03, yy)
    thr = float(np.quantile(field, 1.0 - land_share))
    land = (field > thr) & (edge > 0.03)
    land = ndimage.binary_opening(land, iterations=max(1, n // 512))
    parts, count = ndimage.label(land)
    if count > 1:
        sizes = ndimage.sum(land, parts, range(1, count + 1))
        land = parts == (int(np.argmax(sizes)) + 1)
    return ndimage.binary_fill_holes(land)


# The water painted on the imported zones picture (lakes, rivers), if any.
PICTURE_WATER = None
BOG_WATER = None     # the bog's pools (relief(), great marshes with "bog"), painted as lakes


def smooth_zones(zone, land, rules, seed):
    """The picture's borders follow its hexagon cells: a saw of teeth a few km
    wide. Each zone's share is blurred (the teeth melt) and read through a
    gentle warp (the border wanders like a natural one), the biggest share wins."""
    n = zone.shape[0]
    km_px = rules["world_km"] / n
    cfg = rules["shape"].get("smooth", {})
    sigma = cfg.get("blur_km", 1.3) / km_px
    amp = cfg.get("wander_km", 1.6) / km_px
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    base = max(2, int(round(rules["world_km"] / cfg.get("wander_scale_km", 14.0))))
    wy = yy + amp * fbm(n, seed + 811, octaves=5, base=base).astype(np.float32)
    wx = xx + amp * fbm(n, seed + 812, octaves=5, base=base).astype(np.float32)
    # (and a finer wander on top: the border ragged at a km or two, not a smooth wave)
    fine = max(2, int(round(rules["world_km"] / 3.5)))
    wy += 0.45 * amp * fbm(n, seed + 813, octaves=3, base=fine).astype(np.float32)
    wx += 0.45 * amp * fbm(n, seed + 814, octaves=3, base=fine).astype(np.float32)
    names = [z for z in np.unique(zone[land]) if z != "ocean"]
    best = np.full((n, n), -1.0, np.float32)
    out = zone.copy()
    for name in names:
        share = ndimage.gaussian_filter((zone == name).astype(np.float32), sigma)
        share = ndimage.map_coordinates(share, [wy, wx], order=1, mode="nearest")
        win = land & (share > best)
        out[win] = name
        best = np.where(win, share, best)
    return out


def layout(rules, n, seed):
    """-> zone (object array of names), land, box, coast (int8 kinds per pixel)."""
    world = rules["continent"]
    # The rules say what the world has and how much; the seed makes the shape:
    # how stretched the continent is, how it is bent, how ragged and bitten.
    rs = np.random.default_rng(seed + 1)
    yy, xx = (np.mgrid[0:n, 0:n] + 0.5) / n          # row = y (south first), col = x
    y_top = 1.0 - rules["isles"]["keep_top"] - 0.012
    # 1. The continent first, on its own: where its land is, by no zone's rules.
    isle_pieces = None
    if rules.get("shape"):
        land, isle_pieces = load_shape(rules, n)
    else:
        # (Several continents come out of the seed's noise; the one with the best
        # southern tongue of land - where the desert can lie washed by the sea on both
        # sides - is kept. The shape itself is nobody's design: it is noise.)
        best, best_score = 0, -1.0
        for c in range(14):
            cand = continent(512, seed + 101 * c, np.random.default_rng(seed * 7919 + c), world["land_share"], y_top)
            # (a continent pressed against the world's edge or the isles' band is no good)
            yy5, xx5 = (np.mgrid[0:512, 0:512] + 0.5) / 512
            wall = (xx5 < 0.09) | (xx5 > 0.91) | (yy5 < 0.09) | (yy5 > y_top - 0.03)
            # a whole, not too bitten shape (its area against what a big closing makes of it)
            r_ = int(0.12 * 512)
            dil = ndimage.distance_transform_edt(~cand) <= r_
            closed = ndimage.distance_transform_edt(dil) > r_
            solidity = float(cand.sum()) / max(float(closed.sum()), 1.0)
            sc = 40.0 * min(tongue_score(cand), 0.012) + 3.0 * solidity - 4.0 * float((cand & wall).sum()) / 512 ** 2
            if sc > best_score:
                best, best_score = c, sc
        land = continent(n, seed + 101 * best, np.random.default_rng(seed * 7919 + best), world["land_share"], y_top)
    # 2. Read the shape: its extent, then patches of natural outline laid over
    # it, and the rules of place give the patches their biomes (world_zones.py).
    cols = np.nonzero(land.any(axis=0))[0]
    x0, x1 = (cols[0] + 0.5) / n, (cols[-1] + 0.5) / n
    rows_any = np.nonzero(land.any(axis=1))[0]
    y0, y1 = (rows_any[0] + 0.5) / n, (rows_any[-1] + 0.5) / n
    s = (xx - x0) / (x1 - x0)
    zone, info = world_zones.place_zones(land, rules, n, seed, rs, fbm)
    print("patches:", info)
    zi = (rules.get("shape") or {}).get("zones_image")
    if zi and Path(zi).exists():
        pal = {k: v for k, v in rules["shape"]["palette"].items() if not k.startswith("_")}
        pal["_water"] = rules["shape"]["palette"].get("_water", [30, 110, 240])
        pal_zone = {k: v for k, v in pal.items() if k != "_water"}
        zone = world_zones.import_zones(zi, {**pal_zone, "_water": pal["_water"]}, land, n)
        zone = smooth_zones(zone, land, rules, seed)
        km_px_ = rules["world_km"] / n
        print("zones imported from", zi)
        global PICTURE_WATER
        PICTURE_WATER = world_zones.import_zones.water
        # (its lakes are drawn in hexagons too: straight edges and corners; the
        # shores are rounded and made ragged like the coast's)
        pw = PICTURE_WATER
        sdw = (ndimage.distance_transform_edt(pw) - ndimage.distance_transform_edt(~pw)) * km_px_
        sdw = ndimage.gaussian_filter(sdw, 0.4 / km_px_) - 0.2
        wn = fbm(n, seed + 905, octaves=4, base=max(2, int(rules["world_km"] / 3.0)))
        PICTURE_WATER = (sdw + 0.2 * (wn - wn.mean()) / max(float(np.std(wn)), 1e-6) > 0) & land

    f = rules["features"]
    rng = np.random.default_rng(seed + 50)
    # The round valley with its lake, in the mountains.
    lv = f["lake_valley"]
    have_lake = (zone == "lake_valley").any()
    white = zone == lv["in"]
    km_px_ = rules["world_km"] / n
    depth_km = ndimage.distance_transform_edt(white) * km_px_
    # deep inside the range, towards the marshes' side (as the sketch has it)
    score = depth_km * (1.0 + 0.8 * np.clip(info["marsh_side"] * (xx - 0.5), -0.5, 0.5))
    iy, ix = np.unravel_index(np.argmax(np.where(white, score, -1.0)), score.shape)
    cy, cx = (iy + 0.5) / n, (ix + 0.5) / n
    lv_radius_km = min(pick(rs, lv["radius_km"]), 0.8 * float(depth_km[iy, ix]))
    r = np.hypot(xx - cx, yy - cy)
    valley = (r < lv_radius_km / rules["world_km"] * (1 + 0.06 * fbm(n, seed + 300, octaves=3, base=6))) & land
    if not have_lake:
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
    y_south_km = float(np.nonzero(land.any(axis=1))[0][0]) * rules["world_km"] / n
    stretch = yy * rules["world_km"] < y_south_km + pick(rs, dc["tip_km"])
    strip = (zone == dc["in"]) & stretch & south_facing & (to_sea_km < dc["width_km"])
    # (one band along the shore, not teeth where the facing flips pixel by pixel)
    strip = (ndimage.gaussian_filter(strip.astype(np.float32), 0.8 / (rules["world_km"] / n)) > 0.45) & (zone == dc["in"])
    zone[strip] = "dragon_cliff"

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
                # (a gulf winds: a slow swing and a drift, never a straight canal)
                wander += irng.normal(0, 0.12) - 0.03 * wander
                a_ = ang + wander + 0.9 * np.sin(i / max(length, 1) * 9 + k) + 0.35 * np.sin(i / max(length, 1) * 23 + 2 * k)
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
    if isle_pieces is not None:
        isle_mask, isle_heights = isle_pieces
    else:
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
    # The spine lies a little off the band's middle (a seed's choice) and
    # bends along its length; the great peak sits on it.
    shift = np.random.default_rng(sd).uniform(-0.12, 0.12)
    ycol = np.broadcast_to((yc + shift * half)[None, :], (n, n))
    R = max(ring_r * 1.15, 1.0)
    w = r.get("width_km", 14.0)
    spine = cy + (ycol + bend - cy) * smoothstep(0.0, 2.0 * R, np.abs(X - cx)) if lake.any() else ycol + bend
    d = np.abs(Y - spine)
    # Cross-section: a sharp crest with long heavy skirts, which are the
    # foothills (no separate belt): they sink into the plains over tens of km.
    prof = 0.78 / (1.0 + (d / w) ** 2) ** 1.1 + 0.22 * np.exp(-(d / (5.0 * w)) ** 2)
    # Along the range: massifs and saddles, slow; the great peak is the highest.
    along_h = 0.72 + 0.28 * (fbm32(n, sd + 11, 150, world_km, 3) * 0.5 + 0.5)
    rad = np.hypot(X - cx, Y - cy)
    peak = 0.30 * np.exp(-(rad / (2.6 * R)) ** 2)
    shape = np.clip(prof * along_h * (1.0 + 0.0) + peak, 0.0, 1.0)
    signed = (ndimage.distance_transform_edt(m | lake) - ndimage.distance_transform_edt(~(m | lake))) * km_px
    envelope = smoothstep(-r["fade_out_km"], r["fade_in_km"], signed)
    # Alpine fractal relief rides on that body: arêtes, spurs, rock teeth,
    # stretched along the range. Strong high up, nearly none low down.
    gy, gx = np.mgrid[0:n, 0:n].astype(np.float32)
    def along(f, k):
        return ndimage.map_coordinates(f, [gy, (gx - n / 2) / k + n / 2], order=1, mode="reflect")
    detail = along(warped(ridged(n, sd, r["km"], world_km), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.35), 2.4)
    fine = along(ridged(n, sd + 3, r["km"] / 3, world_km, octaves=6), 1.8)
    teeth = along(ridged(n, sd + 5, r["km"] / 9, world_km, octaves=4), 1.3)
    frac = np.clip((0.55 * detail ** 1.3 + 0.30 * fine ** 1.3 + 0.15 * teeth - 0.3) / 0.45, -0.5, 1.0)
    rugged = 0.06 + 0.40 * smoothstep(0.12, 0.55, shape)
    foot = 150.0 * fbm32(n, sd + 14, 9, world_km, 4) * (1.0 - smoothstep(0.1, 0.4, shape))
    span = r["peak"] - r["base"]
    top = span * shape * (1.0 - 0.6 * rugged + rugged * frac) + foot
    return r["base"] + envelope * np.clip(top, 0.0, span)


def massif(m, r, sd, n, world_km):
    """Mountains as the island's: height grows with the distance from the edge of the
    zone (so the crest runs along the middle of it, wherever it bends), and a
    dendritic fractal - ridged noise bent hard - cuts it into spurs and valleys.
    The edge itself lies low, so it meets its neighbours without a step."""
    km_px = world_km / n
    d = ndimage.distance_transform_edt(m) * km_px
    size = max(3, int(30.0 / km_px))
    local = ndimage.gaussian_filter(ndimage.grey_dilation(d, size=(size, size)), 12.0 / km_px)
    q = np.clip(ndimage.gaussian_filter(d, 1.5 / km_px) / np.maximum(local, 4.0), 0.0, 1.0)
    # (q ** power alone stands up vertically at the edge: a crease along the
    # ragged border; the smoothstep lays the foot down flat into the neighbour)
    body = 0.10 + 0.90 * q ** r.get("power", 0.9) * smoothstep(0.0, 0.3, q)
    ridge = warped(ridged(n, sd, r["km"], world_km, octaves=7), n, sd + 9, r["km"] * 2.0, world_km, r["km"] * 0.9)
    fine = ridged(n, sd + 3, r["km"] / 3.0, world_km, octaves=6)
    frac = np.clip((0.6 * ridge ** 1.2 + 0.4 * fine ** 1.4 - 0.1) / 0.8, 0.0, 1.0)
    # the height swells and sinks along the range: massifs and saddles
    lo_s = r.get("swell_low", 0.7)
    # (stretched to its full range: deep saddles and high massifs, not a ripple)
    sw = smoothstep(-0.6, 0.6, fbm32(n, sd + 11, r["km"] * r.get("swell_km", 5.0), world_km, 3) * r.get("swell_contrast", 1.0))
    swell = lo_s + (1.0 - lo_s) * sw
    # lone great summits standing over the crest line
    if r.get("summits"):
        swell = swell + r["summits"] * smoothstep(0.35, 0.8, fbm32(n, sd + 12, r["km"] * 1.3, world_km, 2))
    span = r["peak"] - r["base"]
    h = r["base"] + span * body * swell * (0.22 + 0.78 * frac)
    if r.get("foot_m"):
        # a belt of foothills before the range: hills and spurs, not a smooth apron
        hills = warped(ridged(n, sd + 13, r["km"] / 2.5, world_km, octaves=5), n, sd + 14, r["km"], world_km, r["km"] * 0.3)
        h = h + r["foot_m"] * hills * smoothstep(0.0, 0.08, q) * (1.0 - smoothstep(0.2, 0.5, q))
    if r.get("cap"):
        # The highest crests bend under a ceiling instead of being cut flat by it
        # (the map's range ends at about 5.2 km): they keep their shapes, pressed.
        knee, cap = r.get("knee", 0.8 * r["cap"]), r["cap"]
        h = np.where(h > knee, knee + (cap - knee) * np.tanh((h - knee) / (cap - knee)), h)
    return h


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
    # (the crest's own height, which the outer flank starts from: no step at the rim)
    rim_eff = floor + (rim_top - floor) * (0.75 + 0.25 * crest)
    # Beyond the rim: the volcano's outer flank, concave, cut by radial gullies
    # (ridged noise stretched along the rays: sampled at a radius squeezed 4x,
    # so nothing seams), sinking into the range's own heights.
    rr = np.maximum(np.hypot(yy - cy, xx - cx), 1e-3)
    rad = R + (rr - R) / 4.0
    gullies = ndimage.map_coordinates(ridged(n, seed + 621, 2.5, world_km, octaves=5),
                                      [cy + (yy - cy) / rr * rad, cx + (xx - cx) / rr * rad], order=1, mode="reflect")
    s = smoothstep(1.0, 2.0, d)
    flank = rim_eff * (1.0 - s) ** 1.5 * (1.0 + (0.35 * (gullies - 0.5) - 0.05) * smoothstep(1.0, 1.25, d)) + h * s
    out = np.maximum(h, flank)
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
            # (the picture gives them a smooth body only: crests and gullies
            # ride on it, stronger the higher the ground, the shore left alone)
            ir = recipes.get("isles", {})
            body = isles[1]
            if ir.get("detail"):
                rid = warped(ridged(n, seed + 1700, ir.get("km", 7.0), world_km, octaves=6), n, seed + 1701,
                             ir.get("km", 7.0) * 2, world_km, ir.get("km", 7.0) * 0.4)
                up = smoothstep(30.0, 600.0, body)
                body = np.where(m, body * (1.0 + ir["detail"] * up * (rid - 0.45) * 2.0), body)
            if ir.get("shore_km"):
                # Their shores: here a cliff straight out of the sea, there a slope
                # of a few km down to a beach (a slow noise along the coast says which).
                inside = ndimage.distance_transform_edt(m) * km_px
                lo_, hi_ = ir["shore_km"]
                reach = lo_ + (hi_ - lo_) * smoothstep(-0.3, 0.5, fbm32(n, seed + 1710, 30, world_km, 3))
                body = np.where(m, np.maximum(body * smoothstep(0.0, 1.0, inside / reach) ** 0.8, 3.0), body)
            heights[name] = body.astype(np.float32)
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
            if r.get("tip_rise"):
                # low where it meets the canyon, climbing smoothly to the nose, where it ends in the cliff
                canyon = (zone == "canyon_steppe") | (zone == "steppe")
                far = ndimage.distance_transform_edt(~canyon) * km_px if canyon.any() else np.full((n, n), 1.0)
                progress = np.clip(far / max(float(np.percentile(far[m], 96)) if m.any() else 1.0, 1.0), 0.0, 1.0) ** 1.4
            h = (r["base"] + r["rise"] * progress + r.get("draa_m", 0) * dune_field(r.get("draa_km", 5), 2.0, sd + 3)
                 + r["dune_m"] * dune_field(r["dune_km"], 3.0, sd) * strength + 25 * fbm32(n, sd + 2, 30, world_km, 3))
        elif kind == "canyons":
            # A plateau top cut by a network of canyons, most of it canyon
            # and dry stone floor: the top stays only as mesas between them.
            # (the steppe cut from it is the same land: the thresholds from both.)
            land_m = m | (zone == "steppe")
            # The whole zone is one wide canyon between the desert and the plain:
            # its floor at the desert's level, its mesas and the steppe islands
            # at the plain's level, the walls between them sheer, spires rising
            # from the floor.
            # (a mesa's top rolls a little: no table plate tens of km^2 dead flat)
            top = r["base"] + r["rise"] * progress + r.get("bumps", 30) * fbm32(n, sd + 3, 6, world_km, 5)
            floor = r["floor"] + r["floor_rise"] * progress + r.get("floor_bumps", 14) * fbm32(n, sd + 6, 3, world_km, 4)
            c1, c2 = canyon_network(n, world_km, seed, r)
            thr = np.percentile(c1[land_m] if land_m.any() else c1, 100 * (1 - r["cover"]))
            thr2 = np.percentile(c2[land_m] if land_m.any() else c2, 78)
            d1 = smoothstep(thr, thr + r.get("wall", 0.03), c1)
            d2 = smoothstep(thr2, thr2 + r.get("wall", 0.03), c2)
            sp = fbm32(n, sd + 7, 3.0, world_km, 4)
            spires = r.get("spires", 0) * smoothstep(0.30, 0.46, sp) * (0.45 + 0.55 * ridged(n, sd + 8, 1.5, world_km, octaves=3))
            if r.get("tiers"):
                # Mesas at several levels, of every size: the standing ground steps
                # down in tiers (a slow noise, terraced), the lesser canyons cut it
                # into islands a tier or two lower; the floor is broken by dry
                # washes and rises in scree at the foot of every wall.
                k = r["tiers"]
                lvl = 0.5 + 0.5 * fbm32(n, sd + 9, r["km"] * 0.7, world_km, 3) * 1.6
                q = np.clip(lvl, 0.0, 0.999) * k
                step = (np.floor(q) + smoothstep(0.82, 1.0, q - np.floor(q))) / k
                tier = r.get("tier_low", 0.3) + (1.0 - r.get("tier_low", 0.3)) * step
                stand = floor + (top - floor) * tier * (1.0 - r.get("cut", 0.6) * d2)
                washes = ridged(n, sd + 11, r["km"] / 4.0, world_km, octaves=4)
                low = floor - r.get("wash_m", 25) * smoothstep(0.55, 0.9, washes)
                scree = ndimage.gaussian_filter(1.0 - d1, 0.35 / km_px)
                low = low + r.get("scree_m", 60) * scree * (1.0 - scree)
                h = low + (stand - low) * (1.0 - d1) + d1 * spires
            else:
                h = top - (top - floor) * d1 - 0.25 * (top - floor) * d2 * (1 - d1) + d1 * spires
            tops[name] = top
        elif kind == "table":
            h = r["base"] + r["amp"] * fbm32(n, sd, r["km"], world_km, 3)
        elif kind == "canyon":
            h = np.zeros((n, n), np.float32)       # cut into its neighbours below
        elif kind == "hills":
            bumps = (fbm32(n, sd, r["km"], world_km, 4) * 0.5 + 0.5) ** 1.4
            small = fbm32(n, sd + 5, r["km"] / 3.0, world_km, 3)
            h = r["base"] + r["rise"] * progress + r["amp"] * bumps + 0.25 * r["amp"] * small
            if r.get("ridge_m"):
                # spurs and gullies: a ridged layer, so a hill is cut, not a cushion
                h = h + r["ridge_m"] * (warped(ridged(n, sd + 9, r["km"] / 2.0, world_km, octaves=5), n, sd + 10,
                                                  r["km"], world_km, r["km"] * 0.3) - 0.45) * (0.4 + bumps)
        elif kind == "lowland":
            h = r["base"] + r["rise"] * progress + r["amp"] * fbm32(n, sd, r["km"], world_km, 3)
            if r.get("pools"):
                # Shallow marsh waters of their own: many small pools just under the sea's level.
                if r.get("bog"):
                    # A bog: standing water everywhere, broken into moss islands
                    # and spits of every size (the picture's lakes join them).
                    f = 0.6 * fbm32(n, sd + 21, r.get("bog_km", 0.6), world_km, 4) + 0.4 * fbm32(n, sd + 22, r.get("bog_km", 0.6) / 3.0, world_km, 3)
                    thr_b = np.percentile(f[m], 100 * (1.0 - r["bog"])) if m.any() else 0.0
                    bog = smoothstep(thr_b, thr_b + 0.05, f)
                else:
                    bog = 0.0
                if PICTURE_WATER is not None and (PICTURE_WATER & m).any():
                    # the lakes the zones picture paints there, their shores
                    # ragged by a little noise, shelving gently (no deep bowls)
                    wet = ndimage.gaussian_filter((PICTURE_WATER & m).astype(np.float32), 0.6 / km_px)
                    wet = wet + 0.25 * fbm32(n, sd + 5, r.get("pool_km", 1.4), world_km, 4)
                    pool = smoothstep(0.35, 0.65, wet)
                else:
                    f = fbm32(n, sd + 5, r.get("pool_km", 1.4), world_km, 4)
                    thr = np.percentile(f[m], 100 * (1.0 - r["pools"])) if m.any() else 0.0
                    pool = smoothstep(thr, thr + 0.10, f)
                # (the bed sinks below the marsh, but stays above the shore's sand:
                # under the water lies mud, dark, not a pale beach)
                pool = np.maximum(pool, bog)
                bed = np.maximum(h - r.get("pool_depth", 6.0), r.get("pool_bed_min", 4.5))
                h = h + (bed - h) * pool
                if r.get("bog"):
                    global BOG_WATER
                    BOG_WATER = (pool > 0.5) & m
        elif kind == "ridges":
            h = ranges(zone, m, r, sd, n, world_km, xx, yy)
        elif kind == "massif":
            mm = m | (zone == "lake_valley") if name == "white_mountains" else m
            h = massif(mm, r, sd, n, world_km)
        elif kind == "bowl":
            h = np.zeros((n, n), np.float32)        # (made later: caldera(), over the range)
        elif kind == "valley":
            # No carved trench of its own: made after the ranges (below), from their edges.
            h = np.zeros((n, n), np.float32)
        elif kind == "spikes":
            sharp = warped(ridged(n, sd, r["km"], world_km, octaves=7), n, sd + 9, r["km"] * 2, world_km, r["km"] * 0.3) ** 2.0
            # Rising out of the valley without a step: the envelope is a long ramp.
            inside_km = ndimage.distance_transform_edt(m) * km_px
            envelope = smoothstep(0.0, r["fade_km"], inside_km)
            # The black peaks stand along the valley; beyond them the black lands
            # run on, falling into dry hills and plains toward the sea.
            far = smoothstep(r.get("peaks_share", 0.4), 1.0, progress)
            h = r["base"] + (r["peak"] - r["base"]) * envelope * sharp * (1.0 - 0.8 * far) - r.get("lowering", 350) * far
        else:
            raise ValueError(f"relief kind {kind}")
        heights[name] = h.astype(np.float32)
    if "lake_valley" in heights and "white_mountains" in heights:
        heights["lake_valley"] = heights["white_mountains"]
    # The valley is where the slopes of the two ranges (the white mountains' north side,
    # the black peaks' south side) meet. From the river in its middle the ground
    # climbs, concave, over up to `slope_km` (never past the middle), to the height
    # the range has along its edge: no step.
    for name in names:
        r = recipes.get(name, {})
        if r.get("kind") != "valley":
            continue
        floor = r["floor_west"] + (r["floor_east"] - r["floor_west"]) * xx
        L = r.get("slope_km", 14)
        sides = []
        for other in ("white_mountains", "black_lands"):
            om = (zone == other) | ((zone == "lake_valley") if other == "white_mountains" else False)
            if not om.any() or other not in heights:
                continue
            # The range's edge height, averaged along the edge by a normalized blur
            # (no nearest-edge lookup: that smears every edge bump into a ray).
            band = (om & (ndimage.distance_transform_edt(om) * km_px < 4.0)).astype(np.float32)
            edge_h = np.zeros((n, n), np.float32)
            got = np.zeros((n, n), np.float32)
            for s_km in (6.0, 24.0):
                wsum = ndimage.gaussian_filter(band, s_km / km_px)
                hsum = ndimage.gaussian_filter(band * heights[other], s_km / km_px)
                avg = hsum / np.maximum(wsum, 1e-6)
                take = np.clip(wsum / 0.05, 0.0, 1.0) * (1.0 - got)
                edge_h += take * np.where(wsum > 1e-6, avg, 0.0)
                got += take
            edge_h = np.where(got > 1e-3, edge_h / np.maximum(got, 1e-3), floor)
            # (blurred: a ragged edge creases the distance field, and every crease
            # would run down the slope as a line)
            sides.append((ndimage.gaussian_filter(ndimage.distance_transform_edt(~om) * km_px, 3.0 / km_px), edge_h))
        walls = np.zeros((n, n), np.float32)
        for k, (dist, edge_h) in enumerate(sides):
            other_d = sides[1 - k][0] if len(sides) == 2 else np.full_like(dist, 1e9)
            reach = np.clip(0.5 * (dist + other_d), 1.0, L)
            walls = np.maximum(walls, (edge_h - floor) * (1.0 - smoothstep(0.0, reach, dist)) ** 1.8)
        heights[name] = (floor + np.maximum(walls, 0.0) + r["amp"] * fbm32(n, seed + 1900, 4, world_km, 3)).astype(np.float32)
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
    # (the caps below follow the distance to the sea: blurred, more so further in,
    # or every bay of the ragged coast creases them into flat facets)
    d_near = ndimage.gaussian_filter(d_in, 1.5 / km_px)
    d_in = np.where(land, d_near + (ndimage.gaussian_filter(d_in, 5.0 / km_px) - d_near) * smoothstep(3.0, 10.0, d_near), d_in)
    beach = 1.5 + 15.0 * d_in + 60.0 * np.maximum(d_in - 3.0, 0.0) ** 1.6
    # (a flat coast is flat near the sea only: further in the ground is free to rise;
    # the cap lets go by a blend, not by a steep ramp under min(), whose crossing
    # with the ground would be a crease)
    flat = 0.6 + 3.0 * d_in
    flat_free = smoothstep(6.0, 30.0, d_in) ** 1.5
    keep_isles = masks.get("isles", np.zeros_like(land))
    # The coast kinds fade into each other over a few km (where two kinds meet,
    # on land and on the sea floor alike, a hard switch is a step or a fan of rays).
    def kind_weight(kind, km=2.5):
        return ndimage.gaussian_filter((coast == kind).astype(np.float32), km / km_px)
    w_beach = kind_weight(COAST_BEACH) * ~keep_isles
    w_flat = kind_weight(COAST_FLAT)
    # (the cap keeps the ground's own small relief: a mountain coming down to a
    # beach stays cut by its gullies, not planed smooth)
    detail = h - ndimage.gaussian_filter(h, 1.5 / km_px)
    def soft_min(a, b, k):
        # (min() with its corner rounded over `k` metres: no crease where they cross)
        return 0.5 * (a + b - np.sqrt((a - b) ** 2 + k * k))
    h = np.where(land, h + (soft_min(h, beach + 0.8 * detail * smoothstep(1.0, 4.0, d_in), 40.0) - h) * w_beach, h)
    flat_h = np.minimum(h, flat)
    h = np.where(land, h + (flat_h + (h - flat_h) * flat_free - h) * w_flat, h)
    h = np.where(land, np.maximum(h, 0.8), h)
    # (off a beach the floor shelves from the waterline: shallows where the surf breaks)
    shelf = -(1.5 + 22.0 * smoothstep(0, 1.5, d_out) + 350.0 * smoothstep(0, 30, d_out) + 1100.0 * smoothstep(30, 140, d_out))
    steep = -(60.0 + 300.0 * smoothstep(0, 4, d_out) + 1100.0 * smoothstep(20, 120, d_out))
    sea = shelf + (steep - shelf) * kind_weight(COAST_CLIFF, 4.0) + 15.0 * fbm32(n, seed + 77, 20, world_km, 3)
    h = np.where(land, h, np.minimum(sea, -2.0))
    return h.astype(np.float32)


def drain(h, mask, slope=0.004):
    """The ground in `mask` made to run out: every hollow in it filled up to its
    spill point, with a slight fall towards the outlet (`slope` metres a cell),
    so a river crossing it runs on instead of ending in a lake (priority flood,
    over the mask's box; the box's edge and the sea are the outlets)."""
    ys, xs = np.nonzero(mask)
    if not len(ys):
        return h
    pad = 4
    y0, y1 = max(ys.min() - pad, 0), min(ys.max() + pad + 1, h.shape[0])
    x0, x1 = max(xs.min() - pad, 0), min(xs.max() + pad + 1, h.shape[1])
    sub = h[y0:y1, x0:x1].astype(np.float64)
    inside = ndimage.binary_dilation(mask[y0:y1, x0:x1], iterations=pad)
    ny, nx = sub.shape
    out = sub.copy()
    done = ~inside | (sub <= 0.0)
    done[0, :] = done[-1, :] = done[:, 0] = done[:, -1] = True
    # (the outlets: every done cell next to one that is not)
    edge = done & ndimage.binary_dilation(~done)
    queue = [(float(sub[y, x]), int(y), int(x)) for y, x in zip(*np.nonzero(edge))]
    heapq.heapify(queue)
    while queue:
        z, y, x = heapq.heappop(queue)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            v, u = y + dy, x + dx
            if 0 <= v < ny and 0 <= u < nx and not done[v, u]:
                done[v, u] = True
                out[v, u] = max(sub[v, u], z + slope)
                heapq.heappush(queue, (out[v, u], v, u))
    res = h.copy()
    res[y0:y1, x0:x1] = np.where(mask[y0:y1, x0:x1], out, sub).astype(h.dtype)
    return res


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
        # The river valley runs out to the sea: no hollow on its floor (where the two
        # ranges' skirts meet they would dam it into a chain of lakes).
        h = drain(h, zone == "river_valley")
        protect = protected(zone, rules, coast, 2)
        h = thermal(h, zone, rules, protect, rules["erosion"].get("thermal_iterations", 60))
        sea, rng_m = 0.13, 6000.0
        v = np.clip(h / rng_m + sea, 0, 1)
        Image.fromarray((v[::-1] * 65535 + 0.5).astype(np.uint16)).save(args.out + "_height.png")
        preview(h, rules, args.out + "_height_preview.png")
        print(f"heights {n}^2: {h.min():.0f} .. {h.max():.0f} m; wrote {args.out}_height.png")

    if PICTURE_WATER is not None:
        # The lakes the zones picture paints (round patches; the long lines are
        # its rivers, left to the generated ones), as the lab's painted water
        # framework (--water-skeleton): blue lakes, north up.
        parts, count = ndimage.label(PICTURE_WATER, np.ones((3, 3)))
        lakes = np.zeros((n, n), bool)
        for k, sl in enumerate(ndimage.find_objects(parts), start=1):
            piece = parts[sl] == k
            span = max(piece.shape)
            # (a marsh's pools are lakes whatever their outline: arms and all)
            marshy = (zone[sl][piece] == "great_marshes").mean() > 0.5
            # (the picture's river down the valley breaks into beads on a small map:
            # they are that river, which the lab makes itself - not a chain of lakes)
            valley = np.isin(zone[sl][piece], ["river_valley", "river_marshes"]).mean() > 0.3
            if piece.sum() >= 12 and (span * span < 10.0 * piece.sum() or marshy) and not valley:
                lakes[sl] |= piece
        if BOG_WATER is not None:
            # the bog's own water: painted too, every pool, or the lab drops them as dents
            lakes |= BOG_WATER
        pic = np.zeros((n, n, 3), np.uint8)
        pic[lakes] = (0, 0, 255)
        Image.fromarray(pic[::-1]).save(args.out + "_water_skeleton.png")
        print(f"{int(ndimage.label(lakes)[1])} painted lakes; wrote {args.out}_water_skeleton.png")

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
    if args.heights:
        # One brush for the height and the biome: ground whose height falls
        # outside its zone's range takes the biome of the nearest ground that fits.
        fit = np.ones((n, n), bool)
        for name, span_ in rules.get("biome_height", {}).items():
            if name == "comment":
                continue
            lo_, hi_ = span_
            sel = zone == name
            # (shores and the sea floor keep their own biome: only dry ground is judged)
            fit[sel] = ((h[sel] >= lo_) & (h[sel] <= hi_)) | (h[sel] < 3.0)
        if not fit.all() and fit.any():
            # (from another zone's ground: a marsh climbing a hill takes the
            # forest's biome, not its own from the foot of the hill)
            # (and from ground at its own level: a mesa top above the canyon takes
            # the plain's biome, not the desert's sand from the foot of the cliff)
            src = ids.copy()
            spans = {k: v for k, v in rules.get("biome_height", {}).items() if k != "comment"}
            for name in np.unique(zone[~fit]):
                lo_, hi_ = spans.get(name, (-1e9, 1e9))
                for side, level in (("high", h > hi_), ("low", h < lo_)):
                    sel = (zone == name) & ~fit & level
                    if not sel.any():
                        continue
                    donor = fit & (zone != name) & (zone != "ocean") & (h > hi_ - 150.0 if side == "high" else h < lo_ + 150.0)
                    if not donor.any():
                        continue
                    dist, near = ndimage.distance_transform_edt(~donor, return_indices=True)
                    # ... and only from ground close by: no wedges borrowed from far zones
                    close = sel & (dist * (rules["world_km"] / n) < 6.0)
                    ids = np.where(close, src[near[0], near[1]], ids)
        # The mountains' lower skirts are the foothills: grass and stone, not snow.
        # (not along one contour line: the edge climbs and falls with a noise)
        foot = 2300.0 + 300.0 * fbm(n, seed + 1801, octaves=5, base=max(2, int(rules["world_km"] / 12.0)))
        ids[(zone == "white_mountains") & (h < foot)] = 5
        # The canyon's mesas and plateaus, at whatever tier: steppe on their tops
        # (flat ground well above the floor), stone on the floor and the walls.
        rc = rules["relief"].get("canyon_steppe", {})
        if rc.get("kind") == "canyons":
            cell_m = rules["world_km"] * 1000.0 / n
            gy_, gx_ = np.gradient(h, cell_m)
            flat = np.hypot(gx_, gy_) < np.tan(np.radians(18.0))
            above = h > rc["floor"] + rc["floor_rise"] + rc.get("steppe_above_m", 150.0)
            ids[np.isin(zone, ["canyon_steppe", "steppe"]) & above & flat] = 3
    Image.fromarray(ids[::-1]).save(args.out + "_biomes.png")
    print(f"land {land.mean() * 100:.0f}% of the world; wrote {args.out}_zones.png, {args.out}_biomes.png")


if __name__ == "__main__":
    main()
