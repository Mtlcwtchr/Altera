#!/usr/bin/env python3
"""Lays the biomes of a world over a height map, by their requirements, and
writes the world's authoring package.

    python tools/biome_layout.py [--heights PNG] [--config JSON] [--out DIR] [--seed N]

The biomes are templates (content/config/biomes/fallen_gods.json): on which
landmasses each may stand, where on it it wants to be and what ground it
wants, as curves over fields of the height map. At every point the biome
that wants it most takes it, after a seeded wander of the borders; the areas
are then balanced to each biome's share, the result smoothed into masses, and
the spec's transitions (`borders`) checked. A new seed gives another world
of the same design.

Then the biomes shape the ground they stand on - the requirements they put on
the terrain: how the land meets the sea (a marsh over many kilometres, a
floating coast as a cliff), a marsh pressed flat and low, mountains raised,
dunes smoothed - and each turns into terrain categories by the ground inside
it, and into control maps.

  height.png      u16          metres, LOW_M .. HIGH_M, the open sea at -60
  water.png       flags        lake bit
  control_0.png   rgba8        moisture_bias, forest_bias, mountain_strength, erosion_strength
  control_1.png   rgba8        moisture_target, moisture_hold (the rain a biome holds its ground to), temperature_bias
  ground.png      categorical  terrain categories (content/config/terrain/categories.json)
  biomes.png      u8           the biome ids (for the tools; not imported)
  preview.png                  the biome map and the categories, with names and a 50 km grid

Then:  world_load_probe --import DIR 2x2 --sample 64 --water --save worlds/fallen_gods.json
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parents[1]
FINE = 4096                 # samples a side of the package: 64 m
SAMPLE = 64.0
COARSE = 1024               # the layout's grid: 256 m
CS = SAMPLE * FINE / COARSE
LOW_M, HIGH_M, SEA_M = -300.0, 3300.0, -60.0
RAW_SEA, RAW_LAND, RAW_TOP = 1500.0, 20000.0, 62000.0


def smooth(t):
    t = np.clip(t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def curve(x, points):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return np.interp(x, xs, ys)


def noise(n, sigma, seed):
    a = ndi.gaussian_filter(np.random.default_rng(seed).standard_normal((n, n)), sigma, mode="wrap")
    return a / (a.std() + 1e-9)


def down(a, k):
    n = a.shape[0] // k
    return a[:n * k, :n * k].reshape(n, k, n, k).mean(axis=(1, 3))


# --- the height map ---------------------------------------------------------

def load(path):
    raw = np.asarray(Image.open(path), np.float32)
    assert raw.shape == (FINE, FINE), raw.shape
    return raw


def water_of(raw):
    """Sea: the low ground reaching the edge of the map. Lake: low ground
    inside the land, big enough to be water (the crater)."""
    low = raw < RAW_LAND - 2000.0
    labels, n = ndi.label(low)
    edge = np.unique(np.concatenate((labels[0], labels[-1], labels[:, 0], labels[:, -1])))
    sea = np.isin(labels, edge[edge > 0])
    lake = np.zeros_like(sea)
    sizes = ndi.sum(np.ones_like(raw), labels, index=np.arange(1, n + 1))
    for k in range(1, n + 1):
        if k in edge or sizes[k - 1] < 3000:      # 12 km2
            continue
        lake |= labels == k
    lake = ndi.binary_opening(lake, iterations=2)
    # The world decides sea and land by cells of 512 m (world_map_gen): a
    # coast drawn finer than that - the map's fjords and one-pixel capes -
    # stood up as walls along the cells' edges where a cell said sea and the
    # heights said land. The coast is rounded to the cell's scale and land
    # smaller than a few cells goes under.
    land = ~sea
    land = ndi.binary_closing(land, iterations=6)
    land = ndi.binary_opening(land, iterations=8)
    land = ndi.gaussian_filter(land.astype(np.float32), 6.0) > 0.5
    labels, n = ndi.label(land)
    if n:
        sizes = ndi.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
        land &= np.isin(labels, 1 + np.nonzero(sizes >= 1200)[0])     # 5 km2
    sea = ~land
    lake &= land
    return sea, lake


def base_height(raw, sea, lake):
    # 2400 m at the top: the map's slopes are drawn for it. At 3300 m one
    # pixel in ten of the land was steeper than 45 degrees and the ridges
    # were walls a few hundred metres high.
    u = np.clip((ndi.gaussian_filter(raw, 1.2) - RAW_LAND) / (RAW_TOP - RAW_LAND), 0.0, 1.0)
    h = 12.0 + 2400.0 * u ** 1.15
    return np.where(sea, SEA_M, h).astype(np.float32)


# --- fields -----------------------------------------------------------------

def fields_of(h, land, lake, landmass_of):
    """The layout's fields on the coarse grid."""
    hc = down(np.where(land, h, 0.0), FINE // COARSE)
    landc = down(land.astype(np.float32), FINE // COARSE) > 0.5
    lakec = down(lake.astype(np.float32), FINE // COARSE) > 0.5
    gy, gx = np.gradient(h, SAMPLE)
    slope = np.clip(np.degrees(np.arctan(np.hypot(gx, gy))) / 45.0, 0.0, 1.0)
    f = {"h": hc, "land": landc, "lake_mask": lakec}
    f["slope"] = down(slope, FINE // COARSE)
    km = 1000.0 / CS
    mean = ndi.gaussian_filter(hc, 1.5 * km)
    f["rugged"] = np.clip(np.sqrt(np.maximum(ndi.gaussian_filter(hc * hc, 1.5 * km) - mean * mean, 0.0)) / 350.0, 0, 1)
    f["basin"] = np.clip(0.5 + (ndi.gaussian_filter(hc, 8.0 * km) - hc) / 500.0, 0.0, 1.0)
    f["coast"] = ndi.distance_transform_edt(landc | lakec) * CS / 1000.0
    f["lake"] = ndi.distance_transform_edt(~lakec) * CS / 1000.0 if lakec.any() else np.full_like(hc, 1e4)
    # elevation, x and y within each landmass
    elevation = np.zeros_like(hc)
    x = np.zeros_like(hc)
    y = np.zeros_like(hc)
    yy, xx = np.mgrid[0:COARSE, 0:COARSE].astype(np.float32)
    for mask in landmass_of.values():
        if not mask.any():
            continue
        lo, hi = np.percentile(hc[mask], 1), np.percentile(hc[mask], 99)
        elevation[mask] = np.clip((hc[mask] - lo) / max(hi - lo, 1.0), 0, 1)
        ys, xs = np.nonzero(mask)
        x[mask] = (xx[mask] - xs.min()) / max(xs.max() - xs.min(), 1)
        y[mask] = (yy[mask] - ys.min()) / max(ys.max() - ys.min(), 1)
    f["elevation"], f["x"], f["y"] = elevation, x, y
    # Patches of colour inside a biome (ground rules: "patch": [lo, hi]):
    # masses a kilometre or two across with a ragged edge, ranked so a range
    # is a share of the ground - [0.6, 1] is two fifths of it.
    p = noise(COARSE, 1500.0 / CS, 9091) + 0.45 * noise(COARSE, 450.0 / CS, 9092)
    f["patch"] = (np.argsort(np.argsort(p.ravel())).reshape(p.shape) / p.size).astype(np.float32)
    return f


def shapes_of(config):
    """The author's shapes (config "shapes"): lines through points of the
    1024 map - a ridge's crest, a valley's axis, a strip of coast - and the
    distance to each, in km, as a field ("shape:NAME") a biome can want to
    be near. The drawing says where; the ground and the seed still say how
    the border runs."""
    out = {}
    for name, points in config.get("shapes", {}).items():
        if name.startswith("_"):
            continue
        im = Image.new("L", (COARSE, COARSE), 0)
        d = ImageDraw.Draw(im)
        # One line, or several (a list of lines): a mountain range of three
        # ridges is one shape.
        lines = points if points and isinstance(points[0][0], list) else [points]
        for line in lines:
            pts = [tuple(p) for p in line]
            if len(pts) == 1:
                d.point(pts, fill=255)
            else:
                d.line(pts, fill=255, width=1)
        mask = np.asarray(im) > 0
        out["shape:" + name] = ndi.distance_transform_edt(~mask) * CS / 1000.0
    return out


def landmasses(landc, config):
    labels, n = ndi.label(landc)
    sizes = ndi.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
    order = list(np.argsort(-sizes) + 1)
    centres = ndi.center_of_mass(np.ones_like(labels), labels, index=np.arange(1, n + 1))
    named = {}
    taken = set()
    for spec in config["landmasses"]:
        if "rank" in spec:
            k = order[spec["rank"]]
            named[spec["name"]] = labels == k
            taken.add(k)
    for spec in config["landmasses"]:
        if "box" not in spec:
            continue
        x0, y0, x1, y1 = spec["box"]
        mask = np.zeros_like(landc)
        for k in range(1, n + 1):
            cy, cx = centres[k - 1]
            if k not in taken and x0 <= cx / COARSE <= x1 and y0 <= cy / COARSE <= y1:
                mask |= labels == k
                taken.add(k)
        named[spec["name"]] = mask
    rest = landc & ~np.isin(labels, list(taken))
    return named, rest


# --- the layout ---------------------------------------------------------------

def layout(f, named, rest, config, seed, penalty=None):
    biomes = config["biomes"]
    penalty = penalty or {}
    ids = np.zeros((COARSE, COARSE), np.uint8)
    wander = config.get("wander", {})
    sigma = wander.get("km", 9.0) * 1000.0 / CS
    amplitude = wander.get("amplitude", 0.3)
    for name, mask in named.items():
        here = [b for b in biomes if name in b["landmasses"]]
        if not here or not mask.any():
            continue
        scores = []
        for i, b in enumerate(here):
            total, weights = np.zeros(mask.sum(), np.float32), 0.0
            gate = np.ones(mask.sum(), np.float32)
            for term in b["want"]:
                if term.get("landmass", name) != name:
                    continue
                value = curve(f[term["field"]][mask], term["curve"])
                # A required term is a condition, not a wish: it scales the
                # rest, so a biome cannot stand where it fails however well
                # the rest fits (a forest cliff on every coast, because it
                # also wanted a coast).
                if term.get("require"):
                    gate *= value
                    continue
                total += term["weight"] * value
                weights += term["weight"]
            s = (total / weights if weights > 0 else np.ones_like(total)) * gate
            s += amplitude * noise(COARSE, sigma, seed * 101 + b["id"])[mask] * 0.5
            if b["id"] in penalty:
                s -= penalty[b["id"]][mask]
            # Where a required term fails, the biome is out of the running:
            # the area balance below may not buy it ground there.
            s = np.where(gate < 0.03, -100.0, s)
            scores.append(s)
        scores = np.stack(scores)
        target = np.array([b["share"].get(name, 0.0) for b in here])
        target = target / max(target.sum(), 1e-6)
        bias = np.zeros(len(here))
        for _ in range(60):
            pick = np.argmax(scores + bias[:, None], axis=0)
            share = np.bincount(pick, minlength=len(here)) / pick.size
            bias = np.clip(bias + 0.6 * (target - share), -2.0, 2.0)
        pick = np.argmax(scores + bias[:, None], axis=0)
        ids[mask] = np.array([b["id"] for b in here], np.uint8)[pick]
    ids = masses(ids, f["land"], config)
    # land no landmass named: the nearest biome
    if rest.any():
        far = ids == 0
        idx = ndi.distance_transform_edt(far, return_distances=False, return_indices=True)
        ids = np.where(rest, ids[tuple(idx)], ids)
    ids[~f["land"]] = 0
    return ids


def apart(f, named, rest, config, seed, names):
    """The layout, with the `never` pairs pushed apart: each biome of a pair
    that touches wants less of the ground near the other, so a third biome
    takes the seam. A few rounds; what still touches is reported."""
    penalty = {}
    ids = layout(f, named, rest, config, seed)
    for _ in range(4):
        touching = False
        for x, y in config["borders"].get("never", []):
            a, b = names.get(x), names.get(y)
            if a is None or b is None:
                continue
            near_a = ndi.binary_dilation(ids == a, iterations=2)
            if not (near_a & (ids == b)).any():
                continue
            touching = True
            for p, q in ((a, b), (b, a)):
                d = ndi.distance_transform_edt(ids != q) * CS / 1000.0
                penalty[p] = penalty.get(p, 0.0) + 0.5 * np.exp(-d / 6.0)
        if not touching:
            break
        ids = layout(f, named, rest, config, seed, penalty)
    return ids


def masses(ids, land, config):
    """Smoothed into masses, and patches below the smallest area given to their
    neighbours."""
    present = [k for k in np.unique(ids) if k > 0]
    sigma = config.get("smooth_km", 1.5) * 1000.0 / CS
    stack = np.stack([ndi.gaussian_filter((ids == k).astype(np.float32), sigma) for k in present])
    ids = np.where(land, np.array(present, np.uint8)[np.argmax(stack, axis=0)], 0).astype(np.uint8)
    smallest = config.get("min_patch_km2", 6.0) * 1e6 / (CS * CS)
    for k in present:
        labels, n = ndi.label(ids == k)
        if n == 0:
            continue
        sizes = ndi.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
        for j in np.nonzero(sizes < smallest)[0]:
            ids[labels == j + 1] = 255
    hole = ids == 255
    if hole.any():
        idx = ndi.distance_transform_edt(hole | (ids == 0) & land, return_distances=False, return_indices=True)
        ids = np.where(hole, ids[tuple(idx)], ids)
        ids[(ids == 255)] = 0
    return np.where(land, ids, 0).astype(np.uint8)


def borders(ids, config, names):
    report = []
    pairs = set()
    for a, b in ((ids[:, :-1], ids[:, 1:]), (ids[:-1, :], ids[1:, :])):
        m = (a != b) & (a > 0) & (b > 0)
        for p, q in zip(a[m], b[m]):
            pairs.add((min(p, q), max(p, q)))
    for kind in ("must", "never"):
        for x, y in config["borders"].get(kind, []):
            p, q = names.get(x), names.get(y)
            if p is None or q is None:
                continue
            meet = (min(p, q), max(p, q)) in pairs
            ok = meet if kind == "must" else not meet
            report.append(("ok  " if ok else "FAIL") + " %s %s - %s" % (kind, x, y))
    return report


# --- the biomes shape their ground --------------------------------------------

RELIEF = {
    # how the land meets the sea (km to full height; small is a cliff), and what
    # the biome does to the heights inside it
    "flat_channels": {"coast_km": 16.0, "flatten": 1.0},
    "rolling_roots": {"coast_km": 3.0},
    "canyons_spires_bridges": {"coast_km": 2.0},
    "dunes_buried_ruins": {"coast_km": 9.0, "soften": 1.0},
    "sea_cliffs_ledges": {"coast_km": 0.35},
    "tilted_plateau_scarp": {"coast_km": 2.5},
    "bowl_to_lake": {"coast_km": 3.0},
    "alpine_ridge_gnome_holes": {"coast_km": 1.5, "raise": 0.12},
    "long_valley_one_river": {"coast_km": 6.0},
    "black_spires_rifts": {"coast_km": 0.8, "raise": 0.1},
    "lush_hills_lagoons": {"coast_km": 4.0},
}


def shape(h, sea, lake, ids, config):
    biomes = {b["id"]: b for b in config["biomes"]}
    k = FINE // COARSE
    land = ~sea
    weights = {}
    for i, b in biomes.items():
        w = ndi.gaussian_filter((ids == i).astype(np.float32), 2.0 * 1000.0 / CS)
        weights[i] = np.kron(w, np.ones((k, k), np.float32))
    total = sum(weights.values()) + 1e-6
    for i in weights:
        weights[i] /= total
    def blend(key, default):
        return sum(weights[i] * RELIEF.get(b["terrain"]["relief"], {}).get(key, default) for i, b in biomes.items())
    d_in = ndi.distance_transform_edt(land) * SAMPLE
    reach = blend("coast_km", 3.0) * 1000.0
    ramp = smooth(d_in / np.maximum(reach, 200.0)) ** 1.25
    out = 3.0 + (h - 3.0) * ramp
    flatten = blend("flatten", 0.0)
    out = out + (2.0 + 30.0 * smooth(out / 1500.0) - out) * flatten
    soft = blend("soften", 0.0)
    out = out + (ndi.gaussian_filter(out, 6.0) - out) * soft
    lift = blend("raise", 0.0)
    out = out * (1.0 + lift * smooth((out - 600.0) / 900.0))
    out = np.maximum(out, 1.5 + 6.0 * smooth(d_in / 500.0))
    d_out = ndi.distance_transform_edt(sea) * SAMPLE
    out = np.where(sea, -(8.0 + 240.0 * smooth(d_out / 16000.0)), out)
    if lake.any():
        shore = ndi.binary_dilation(lake, iterations=3) & ~lake & land
        level = np.percentile(out[shore], 10)
        d_lake = ndi.distance_transform_edt(lake) * SAMPLE
        out = np.where(lake, level - 3.0 - 45.0 * smooth(d_lake / 2500.0), out)
    return out.astype(np.float32)


def ground(ids, f, config, registry):
    out = np.zeros_like(ids)
    for b in config["biomes"]:
        mask = ids == b["id"]
        left = mask.copy()
        for rule in b["ground"]:
            hit = left.copy()
            for field, (lo, hi) in rule.get("when", {}).items():
                hit &= (f[field] >= lo) & (f[field] <= hi)
            out[hit] = registry[rule["category"]]
            left &= ~hit
    out[f["lake_mask"]] = registry["lake"]
    return out


def climate_hold(ids, config):
    """control_1: the rain each biome holds its ground to (moisture_target)
    and how firmly (moisture_hold): the world's rain model ranks a place
    against all the land, and alone it parched an inland meadow."""
    rgba = np.zeros((COARSE, COARSE, 4), np.float32)
    for b in config["biomes"]:
        t = b["terrain"]
        rgba[ids == b["id"]] = (t["moisture"], t.get("moisture_hold", 0.75), t.get("temperature", 0.5), 0.0)
    rgba[ids == 0, 2] = 0.5
    for c in range(3):
        rgba[..., c] = ndi.gaussian_filter(rgba[..., c], 3000.0 / CS)
    return (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8)


def controls(ids, config):
    rgba = np.zeros((COARSE, COARSE, 4), np.float32)
    rgba[...] = (0.5, 0.5, 0.0, 0.5)
    for b in config["biomes"]:
        t = b["terrain"]
        rgba[ids == b["id"]] = (t["moisture"], t["forest"], t["mountain"], t["erosion"])
    for c in range(4):
        rgba[..., c] = ndi.gaussian_filter(rgba[..., c], 1500.0 / CS)
    return (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8)


# --- pictures -------------------------------------------------------------------

def preview(h, ids, ground_ids, config, registry):
    hc = down(h, FINE // COARSE)
    gy, gx = np.gradient(hc, CS)
    shade = np.clip(0.8 - (gx + gy) * 1.6, 0.45, 1.25)[..., None]
    colours = {b["id"]: b["colour"] for b in config["biomes"]}
    a = np.zeros((COARSE, COARSE, 3), np.float32)
    a[...] = (25, 50, 95)
    for k, c in colours.items():
        a[ids == k] = c
    a = np.where((ids > 0)[..., None], a * shade, a)
    rng = np.random.default_rng(3)
    palette = {v: rng.integers(40, 230, 3) for v in registry.values()}
    g = np.zeros_like(a)
    g[...] = (25, 50, 95)
    for v, c in palette.items():
        g[ground_ids == v] = c
    g = np.where((ids > 0)[..., None], g * shade, (25, 50, 95))
    left = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    right = Image.fromarray(np.clip(g, 0, 255).astype(np.uint8))
    for im, names in ((left, {b["id"]: b["title"] for b in config["biomes"]}),
                      (right, {v: k for k, v in registry.items()})):
        d = ImageDraw.Draw(im)
        try:
            font = ImageFont.truetype("arial.ttf", 13)
        except OSError:
            font = ImageFont.load_default()
        km = 1000.0 / CS
        for k in range(0, 263, 50):
            d.line([(k * km, 0), (k * km, COARSE)], fill=(255, 255, 255), width=1)
            d.line([(0, k * km), (COARSE, k * km)], fill=(255, 255, 255), width=1)
        source = ids if im is left else ground_ids
        for v, name in names.items():
            labels, n = ndi.label(source == v)
            for j in range(1, n + 1):
                m = labels == j
                if m.sum() < 900:
                    continue
                dist = ndi.distance_transform_edt(np.pad(m, 1))[1:-1, 1:-1]
                y, x = np.unravel_index(np.argmax(dist), dist.shape)
                w = d.textlength(name, font=font)
                d.text((x - w / 2 + 1, y - 6), name, fill=(0, 0, 0), font=font)
                d.text((x - w / 2, y - 7), name, fill=(255, 255, 255), font=font)
    sheet = Image.new("RGB", (COARSE * 2 + 8, COARSE), (0, 0, 0))
    sheet.paste(left, (0, 0))
    sheet.paste(right, (COARSE + 8, 0))
    return sheet


def views(ids, config, out):
    """A place deep inside every biome to look at it from (shot lists):
    the point of each biome's largest patch farthest from its edge, in
    world metres."""
    lines = []
    for b in config["biomes"]:
        labels, n = ndi.label(ids == b["id"])
        if n == 0:
            continue
        sizes = ndi.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
        m = labels == (1 + int(np.argmax(sizes)))
        dist = ndi.distance_transform_edt(np.pad(m, 1))[1:-1, 1:-1]
        y, x = np.unravel_index(np.argmax(dist), dist.shape)
        lines.append("%02d_%s %d %d" % (b["id"], b["name"], int((x + 0.5) * CS), int((y + 0.5) * CS)))
    (out / "views.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def sites(ids, f, config, seed):
    """The biomes' landmarks (config biome "sites": [{"painted": NAME,
    "per_1000km2": N}]): a site is a small painted patch of the 'features'
    layer (content/config/environment/painted.json names it) where that
    name's recipe (content/config/environment/recipes/landmarks.json) sets
    its stones. Deep inside the biome (1 km from its edge), on gentle ground,
    never nearer than 4 km to another site."""
    legend = json.loads((ROOT / "content/config/environment/painted.json").read_text(encoding="utf-8"))["ids"]
    by_name = {v: int(k) for k, v in legend.items()}
    out = np.zeros_like(ids)
    rng = np.random.default_rng(seed * 7919 + 13)
    taken = []
    spacing = 4000.0 / CS
    for b in config["biomes"]:
        wanted = b.get("sites", [])
        if not wanted:
            continue
        inside = ndi.distance_transform_edt(ids == b["id"]) * CS >= 1000.0
        inside &= f["slope"] < 0.35
        ys, xs = np.nonzero(inside)
        if len(xs) == 0:
            continue
        area_km2 = (ids == b["id"]).sum() * (CS / 1000.0) ** 2
        for rule in wanted:
            sid = by_name[rule["painted"]]
            count = max(1, int(round(area_km2 * rule["per_1000km2"] / 1000.0)))
            order = rng.permutation(len(xs))
            placed = 0
            for k in order:
                x, y = int(xs[k]), int(ys[k])
                if any((x - tx) ** 2 + (y - ty) ** 2 < spacing ** 2 for tx, ty in taken):
                    continue
                out[max(0, y - 1):y + 2, max(0, x - 1):x + 2] = sid
                taken.append((x, y))
                placed += 1
                if placed >= count:
                    break
            print("%-18s %-16s %d sites" % (b["name"], rule["painted"], placed))
    return out, {str(v): k for k, v in by_name.items()}


def world_json(registry, used, feature_ids=None):
    return {
        "format": "campfire.world-authoring", "version": 1, "stage": "water",
        "world": {"width_m": FINE * SAMPLE, "height_m": FINE * SAMPLE, "sample_m": int(SAMPLE), "chunk_m": 32768},
        "origin_m": [0.0, 0.0], "size_m": [FINE * SAMPLE, FINE * SAMPLE], "vectors": {},
        "rasters": {
            "height": {"file": "raster/height.png", "type": "u16", "kind": "height", "min_m": LOW_M, "max_m": HIGH_M,
                       "default_m": SEA_M, "interpolation": "bicubic", "optional": False, "version": 1},
            "water": {"file": "raster/water.png", "type": "u8", "kind": "flags", "default": 0, "bits": {"lake": 0},
                      "optional": True, "version": 1},
            "control_0": {"file": "raster/control_0.png", "type": "rgba8", "kind": "control", "version": 1, "channels": {
                "R": {"name": "moisture_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["climate", "ecology"]},
                "G": {"name": "forest_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["ecology", "vegetation"]},
                "B": {"name": "mountain_strength", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True, "affects": ["terrain", "drainage"]},
                "A": {"name": "erosion_strength", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["terrain"]}}},
            "control_1": {"file": "raster/control_1.png", "type": "rgba8", "kind": "control", "version": 1, "channels": {
                "R": {"name": "moisture_target", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["climate", "ecology"]},
                "G": {"name": "moisture_hold", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True, "affects": ["climate", "ecology"]},
                "B": {"name": "temperature_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["climate"]},
                "A": {"name": "reserved_a", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True}}},
            **({"features": {"file": "raster/features.png", "type": "u8", "kind": "categorical", "default": 0,
                             "optional": True, "version": 1, "ids": feature_ids}} if feature_ids else {}),
            "ground": {"file": "raster/ground.png", "type": "u8", "kind": "categorical", "default": 0, "optional": True,
                       "version": 1, "ids": {str(v): k for k, v in registry.items() if v in used or v == 0}},
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--heights", default=str(ROOT / "doc/NewRef/world_heightmap_v5_4096_gray16.png"))
    ap.add_argument("--config", default=str(ROOT / "content/config/biomes/fallen_gods.json"))
    ap.add_argument("--out", default=str(ROOT / ".cache/worlds/fallen_gods"))
    ap.add_argument("--seed", type=int, default=None, help="another world of the same design (default: the config's)")
    args = ap.parse_args()
    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    seed = config.get("seed", 1) if args.seed is None else args.seed
    registry = {c["name"]: c["id"] for c in json.loads(
        (ROOT / "content/config/terrain/categories.json").read_text(encoding="utf-8"))["categories"]}

    raw = load(args.heights)
    sea, lake = water_of(raw)
    h0 = base_height(raw, sea, lake)
    land = ~sea & ~lake
    landc = down(land.astype(np.float32), FINE // COARSE) > 0.5
    named, rest = landmasses(landc, config)
    f = fields_of(h0, land, lake, named)
    f.update(shapes_of(config))
    names = {b["name"]: b["id"] for b in config["biomes"]}
    ids = apart(f, named, rest, config, seed, names)
    h = shape(h0, sea, lake, ids, config)
    f2 = fields_of(h, land, lake, named)
    f2.update(shapes_of(config))
    ground_ids = ground(ids, f2, config, registry)

    for line in borders(ids, config, names):
        print(line)
    landsum = max(int(f["land"].sum()), 1)
    for b in config["biomes"]:
        print("%-18s %5.1f%% of the land" % (b["name"], 100.0 * (ids == b["id"]).sum() / landsum))

    out = Path(args.out)
    (out / "raster").mkdir(parents=True, exist_ok=True)
    stored = np.where(sea, SEA_M, np.maximum(h, 1.0))
    u16 = np.clip(np.round((stored - LOW_M) / (HIGH_M - LOW_M) * 65535.0), 0, 65535).astype(np.uint16)
    # the sea exactly at default_m: not stored
    u16[sea] = np.uint16(round((SEA_M - LOW_M) / (HIGH_M - LOW_M) * 65535.0))
    Image.fromarray(u16).save(out / "raster/height.png")
    Image.fromarray((lake.astype(np.uint8))).save(out / "raster/water.png")
    Image.fromarray(controls(ids, config), "RGBA").save(out / "raster/control_0.png")
    Image.fromarray(climate_hold(ids, config), "RGBA").save(out / "raster/control_1.png")
    Image.fromarray(ground_ids).save(out / "raster/ground.png")
    Image.fromarray(ids).save(out / "biomes.png")
    used = set(np.unique(ground_ids).tolist())
    features, feature_ids = sites(ids, f2, config, seed)
    Image.fromarray(features).save(out / "raster/features.png")
    (out / "world.json").write_text(json.dumps(world_json(registry, used, feature_ids), indent=1), encoding="utf-8")
    preview(h, ids, ground_ids, config, registry).save(out / "preview.png")
    views(ids, config, out)
    print("wrote", out)


if __name__ == "__main__":
    main()
