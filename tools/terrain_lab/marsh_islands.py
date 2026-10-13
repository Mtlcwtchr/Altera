#!/usr/bin/env python3
"""marsh_islands - the layout of the great marshes (doc/BIOME_PLAN.md 3.1,
marshes/MARSHES_REQUIREMENTS.md): a valley flooded with shallow red water in
irregular pools among bright grass (30-50% water by 2 km windows, nothing
deep), the giant A-1 slabs standing about the meadow, each on a flat pad just
above the water, and rare terraced hills of red mossy rock (8-25 m, 3-5
steps, a cliff side or two straight into the water, one gentle way up) with
small one-step islets between them.

    python tools/terrain_lab/marsh_islands.py                          # terrain_lab_out/world_*
    python tools/terrain_lab/marsh_islands.py --world terrain_lab_out/seed_5/world

Runs after world_gen.py --finish and before world_features.py (build_world.py
does it). Reads <world>_final.r16, <world>_zones.png, <world>_water_skeleton.png
and the rule `marsh_islands` in content/config/terrain_core/props.json; writes

- <world>_final.r16 reshaped (the heights it started from are kept as
  <world>_final_base.r16, and a re-run starts from them again);
- <world>_water_skeleton.png: the pools painted, pads and hills dry (base kept
  as <world>_water_skeleton_base.png);
- <world>_marsh_islands.json: the slabs' pads (`islands`: variant, scale,
  turn - world_megaliths.py stands a slab on each), the hills, and the water
  share check.

The water level of a painted lake is the lab's own rule (hydrology.cpp): the
lowest ground of its shore, less half a metre, on the 64 m work grid.
"""
import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

import marsh_hill_mesh
import world_features as wf

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha1(Path(path).read_bytes()).hexdigest()


def base_of(current, base, record, key):
    """The file to start from: the kept base, unless `current` is no longer
    what this script wrote last time (the generator made a new world) - then
    `current` is the new base."""
    if base.exists() and record.get(key) == digest(current):
        return base
    shutil.copyfile(current, base)
    return base


def lake_levels(h, wet):
    """Each painted lake's level on the work grid (as hydrology.cpp): parts
    8-connected, level = lowest dry neighbour - 0.5."""
    lab, n = ndimage.label(wet, structure=np.ones((3, 3)))
    ring = ndimage.binary_dilation(wet, structure=np.ones((3, 3))) & ~wet
    # Each dry ring cell belongs to the lakes next to it: take the lowest per lake.
    level = np.full(n + 1, np.inf)
    rows, cols = np.nonzero(ring)
    for dr in (-1, 0, 1):
        for dc in (-1, 0, 1):
            rr = np.clip(rows + dr, 0, wet.shape[0] - 1)
            cc = np.clip(cols + dc, 0, wet.shape[1] - 1)
            part = lab[rr, cc]
            np.minimum.at(level, part, np.where(part > 0, h[rows, cols], np.inf))
    level = np.maximum(level - 0.5, 0.5)
    level[0] = np.nan
    return lab, level


def smooth_noise(rng, k=6):
    """A wobble round a circle: a few random harmonics, about -1..1."""
    amp = rng.normal(0, 1, k) / (1 + np.arange(k))
    ph = rng.uniform(0, math.tau, k)

    def f(theta):
        return sum(a * np.cos((i + 1) * theta + p) for i, (a, p) in enumerate(zip(amp, ph))) / 1.4
    return f


def island_profile(rng, r_top, rule):
    """Treads (widths, m) and risers (heights, m) from the water up, and the
    shape: the outline's wobble, the cliff sides."""
    steps = int(rng.integers(rule["steps"][0], rule["steps"][1] + 1))
    if "riser_m" in rule:
        # Each step its own height (the user: 0.3-0.7 of a man's height).
        risers = rng.uniform(*rule["riser_m"], steps)
        treads = rng.uniform(*rule["tread_m"], steps)
        cliffs = [(rng.uniform(0, math.tau), rng.uniform(*rule["cliff_width_deg"]) * math.pi / 180)
                  for _ in range(int(rng.integers(rule["cliff_sides"][0], rule["cliff_sides"][1] + 1)))]
        return {"risers": risers, "treads": treads, "wobble": smooth_noise(rng), "cliffs": cliffs, "r_top": r_top}
    total = rng.uniform(*rule["top_m"])
    cuts = np.sort(rng.uniform(0, 1, steps - 1))
    risers = np.diff(np.concatenate([[0], cuts, [1]])) * total
    # Reserve the minimum for every riser first; rescaling after clamping
    # used to produce steps below the configured minimum.
    minimum = rule["riser_min_m"]
    total = max(total, steps * minimum)
    risers = minimum + risers / risers.sum() * (total - steps * minimum)
    treads = rng.uniform(*rule["tread_m"], steps)
    wobble = smooth_noise(rng)
    # Cliff sides: one or two sectors where the steps run together.
    cliffs = []
    for _ in range(int(rng.integers(rule["cliff_sides"][0], rule["cliff_sides"][1] + 1))):
        cliffs.append((rng.uniform(0, math.tau), rng.uniform(*rule["cliff_width_deg"]) * math.pi / 180))
    return {"risers": risers, "treads": treads, "wobble": wobble, "cliffs": cliffs, "r_top": r_top}


def cliffness(theta, cliffs):
    c = np.zeros_like(theta)
    for at, width in cliffs:
        d = np.abs((theta - at + math.pi) % math.tau - math.pi)
        c = np.maximum(c, np.clip(1.5 - 1.5 * d / (width * 0.5), 0, 1))
    return np.clip(c, 0, 1)


def raise_island(h, cell, x, y, level, prof, rule):
    """Raises the island into `h` (metres, row 0 south) round (x, y); returns
    the mask of texels it covers (its outline) and the bounding window."""
    treads, risers = prof["treads"], prof["risers"]
    reach = prof["r_top"] * (1 + rule["wobble"]) + treads.sum() + 2 * cell
    r0, r1 = int((y - reach) / cell), int((y + reach) / cell) + 2
    c0, c1 = int((x - reach) / cell), int((x + reach) / cell) + 2
    r0, c0 = max(r0, 0), max(c0, 0)
    r1, c1 = min(r1, h.shape[0]), min(c1, h.shape[1])
    yy, xx = np.mgrid[r0:r1, c0:c1]
    px, py = (xx + 0.5) * cell - x, (yy + 0.5) * cell - y
    r = np.hypot(px, py)
    theta = np.arctan2(py, px)
    shape = 1 + rule["wobble"] * prof["wobble"](theta)
    cl = cliffness(theta, prof["cliffs"])
    # On a cliff side the treads shrink to almost nothing: the risers stack.
    squeeze = 1 - cl * rule["cliff_squeeze"]
    top_r = prof["r_top"] * shape - rule.get("inset_m", 0.0)
    outer = top_r + treads.sum() * squeeze
    # Height: shelf at the water, then each riser crossed going in.
    z = np.full(r.shape, level + rule["shelf_m"], np.float64)
    radius = outer.copy()
    sharp = rule["riser_soft_m"]
    if "ramp" in prof:
        # One gentle way up (doc: every hill can be climbed): in this sector
        # each riser is spread over its whole tread.
        at, width = prof["ramp"]
        d = np.abs((theta - at + math.pi) % math.tau - math.pi)
        ramp = np.clip(1.5 - 1.5 * d / (width * 0.5), 0, 1) * (1 - cl)
        sharp = sharp + ramp * (treads.min() * 0.9 - sharp)
    for k in range(len(treads)):
        z += risers[k] * np.clip((radius - r) / sharp + 0.5, 0, 1)
        radius = radius - treads[k] * squeeze
    inside = r < outer + sharp
    # Rough treads: a little noise so a step is not a table.
    z += rule["tread_noise_m"] * np.sin(px / 23.0 + theta * 3) * np.cos(py / 31.0) * (r < outer)
    win = h[r0:r1, c0:c1]
    win[inside] = np.maximum(win[inside], z[inside])
    mask = np.zeros(h.shape, bool)
    mask[r0:r1, c0:c1] = inside
    return mask, (r0, r1, c0, c1)


NEIGHBOURS = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]


def thermal_erode(g, cell, talus, iterations, rate):
    """Thermal erosion: wherever the ground stands steeper than `talus`
    (rise over run) above a neighbour, a share `rate` of the excess slides
    down to the lower neighbours, in proportion to how much each is below."""
    H, W = g.shape
    g = g.copy()
    drop = [talus * cell * math.hypot(dr, dc) for dr, dc in NEIGHBOURS]
    for _ in range(iterations):
        p = np.pad(g, 1, mode="edge")
        ex = np.stack([np.maximum(g - p[1 + dr:1 + dr + H, 1 + dc:1 + dc + W] - d, 0)
                       for (dr, dc), d in zip(NEIGHBOURS, drop)])
        total = ex.sum(0)
        move = rate * ex.max(0)
        share = np.where(total > 0, move / np.maximum(total, 1e-9), 0)
        gain = np.zeros_like(p)
        for i, (dr, dc) in enumerate(NEIGHBOURS):
            gain[1 + dr:1 + dr + H, 1 + dc:1 + dc + W] += ex[i] * share
        g += gain[1:-1, 1:-1] - move
    return g


def raise_mound(h, cell, x, y, level, mound, erosion, rng):
    """A hill of the marshes into `h` round (x, y): a domed top of
    `top_radius_m`, a steep raw flank `flank_m` down to below the water, a
    little rough noise, then thermal erosion (`erosion`) on a fine grid slumps
    the flank into a talus apron and softens the top. Returns the mask of
    texels it raised above the water."""
    H = rng.uniform(*mound["height_m"])
    rt = rng.uniform(*mound["top_radius_m"])
    flank = rng.uniform(*mound["flank_m"])
    wob = smooth_noise(rng)
    talus = erosion["talus"]
    reach = rt * (1 + mound.get("wobble", 0.2)) + flank + H / talus + 4 * cell
    r0, r1 = max(int((y - reach) / cell), 0), min(int((y + reach) / cell) + 2, h.shape[0])
    c0, c1 = max(int((x - reach) / cell), 0), min(int((x + reach) / cell) + 2, h.shape[1])
    sub = max(int(round(cell / erosion["fine_m"])), 1)
    fine = cell / sub
    fr = r0 + (np.arange((r1 - r0) * sub) + 0.5) / sub - 0.5
    fc = c0 + (np.arange((c1 - c0) * sub) + 0.5) / sub - 0.5
    rr, cc = np.meshgrid(fr, fc, indexing="ij")
    orig = ndimage.map_coordinates(h, [rr, cc], order=1, mode="nearest")
    px, py = (cc + 0.5) * cell - x, (rr + 0.5) * cell - y
    r = np.hypot(px, py)
    theta = np.arctan2(py, px)
    edge = rt * (1 + mound.get("wobble", 0.2) * wob(theta))
    t = r / edge
    dome = mound["dome"]
    z = np.where(t <= 1, 1 - dome * t * t, (1 - dome) * np.clip(1 - (r - edge) / flank, 0, 1))
    raw = rng.normal(size=r.shape).astype(np.float32)
    bumps = ndimage.gaussian_filter(raw, mound["noise_scale_m"] / fine * 0.3)
    bumps /= max(float(np.abs(bumps).max()), 1e-6)
    # From a metre under the water (the pools round its foot stay) up to H.
    mound_z = level - 1.0 + (H + 1.0) * z + mound["noise_m"] * bumps * np.clip(z * 3, 0, 1)
    g = thermal_erode(np.maximum(orig, mound_z), fine, talus, erosion["iterations"], erosion["rate"])
    g = np.maximum(orig, g)
    coarse = g.reshape(r1 - r0, sub, c1 - c0, sub).mean(axis=(1, 3))
    win = h[r0:r1, c0:c1]
    np.maximum(win, coarse, out=win)
    mask = np.zeros(h.shape, bool)
    mask[r0:r1, c0:c1] = coarse > level + 0.2
    return mask, H


def bilinear(h, cell):
    """The heightfield's ground at world metres, as the renderer reads it."""
    def ground(x, y):
        x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
        return ndimage.map_coordinates(h, [y / cell - 0.5, x / cell - 0.5], order=1, mode="nearest")
    return ground


def terrace_rings(h, cell, x, y, reach, risers, rule, cliffs, ramp_at, rng, segments=56):
    """The terraces of a hill already in `h`: the centre moved to its peak,
    the levels from just over the peak down by `risers`, and each wall's
    contour round the peak (the outermost crossing of its level on each of
    `segments` rays), squeezed together on the cliff sides."""
    ground = bilinear(h, cell)
    # The peak, on a 2 m grid round (x, y).
    look = min(reach * 0.5, 100.0)
    gy, gx = np.mgrid[-look:look:2.0, -look:look:2.0]
    z = ground(x + gx, y + gy)
    i = int(np.argmax(z))
    x, y = x + float(gx.flat[i]), y + float(gy.flat[i])
    theta = np.linspace(0, math.tau, segments, endpoint=False)
    rs = np.arange(0, reach, 0.5)
    prof = ground(x + np.cos(theta)[:, None] * rs, y + np.sin(theta)[:, None] * rs)
    # Each ray ends at its lowest ground: past it the land rising again is
    # not this hill's (a valley's side, a neighbour).
    past = np.arange(len(rs))[None, :] > np.argmin(prof, 1)[:, None]
    prof = np.where(past, -np.inf, prof)
    top = float(prof.max()) + 0.15
    levels = top - np.concatenate([np.cumsum(risers[::-1])[::-1], [0.0]])
    n = len(risers)
    rho = np.zeros((n, segments))
    for k in range(n):
        above = prof >= levels[k]
        last = np.where(above.any(1), above.shape[1] - 1 - np.argmax(above[:, ::-1], 1), 0)
        nxt = np.minimum(last + 1, len(rs) - 1)
        a, b = prof[np.arange(segments), last], prof[np.arange(segments), nxt]
        frac = np.clip((a - levels[k]) / np.maximum(a - b, 1e-6), 0, 1)
        rho[k] = rs[last] + frac * (rs[nxt] - rs[last])
    cl = cliffness(theta, cliffs)
    rho[1:] = rho[0] - (rho[0] - rho[1:]) * (1 - cl * rule["cliff_squeeze"])
    # Treads at least `min_tread_m` (less on a cliff side), the top not to a
    # point: a wall too close to the one above moves out, downhill, where
    # the ground under its tread stays lower still.
    gap = rule["min_tread_m"] * (1 - cl) + 0.8 * cl
    rho[-1] = np.maximum(rho[-1], 3.0)
    for k in range(n - 2, -1, -1):
        rho[k] = np.maximum(rho[k], rho[k + 1] + gap)
    ramp = np.zeros(segments)
    if ramp_at is not None:
        d = np.abs((theta - ramp_at + math.pi) % math.tau - math.pi)
        ramp = np.clip(1.5 - 1.5 * d / (rule["ramp_width_deg"] * math.pi / 360), 0, 1) * (1 - cl)
    return x, y, theta, rho, levels, ramp


def reshape_marsh(world, h, skeleton, rule, rng):
    """A shallow wetland floor with moss hummocks and occasional red shelves.

    Work at the hydrology grid, blend into neighbouring biomes, and raise
    the old bowl bottoms too. A lake cut alone can only lower terrain and
    therefore cannot repair a marsh that already contains deep basins.
    """
    n = skeleton.shape[0]
    cell = world.size / n
    inside = np.isin(world.zones, rule["zones"])
    factor = n // inside.shape[0]
    marsh = np.repeat(np.repeat(inside, factor, 0), factor, 1)
    if not marsh.any():
        return skeleton, None
    depth = ndimage.distance_transform_edt(marsh) * cell
    blend = np.clip(depth / rule.get("edge_blend_m", 650), 0, 1)
    blend = blend * blend * (3 - 2 * blend)
    config = rule.get("landform", {})
    if not config:
        return skeleton, None
    def noise(km):
        raw = rng.normal(size=(n, n)).astype(np.float32)
        value = ndimage.gaussian_filter(raw, max(km * 1000 / cell * 0.18, 1))
        lo, hi = np.percentile(value[marsh], [2, 98])
        return np.clip((value - lo) / max(hi - lo, 1e-6), 0, 1)
    pools = noise(config.get("pool_km", 0.7))
    broad = noise(config.get("shelf_km", 2.0))
    threshold = float(np.percentile(pools[marsh], 100 * config.get("wet_share", 0.48)))
    wet = (pools < threshold) & marsh
    # Each separate stretch of marsh floods at its own level: the low ground
    # it already has (a river marsh up a valley lies a hundred metres above
    # the coast's), never under `water_level_m` by the sea.
    h64 = h.reshape(n, world.n // n, n, world.n // n).mean(axis=(1, 3))
    parts, count = ndimage.label(marsh)
    waterline = np.full((n, n), float(config.get("water_level_m", 14.0)), np.float32)
    for k in range(1, count + 1):
        part = parts == k
        level = max(float(np.percentile(h64[part], config.get("level_percentile", 25))), config.get("water_level_m", 14.0))
        waterline[part] = level
    # (outside, the nearest stretch's level: the blend to the land round it)
    near = ndimage.distance_transform_edt(~marsh, return_distances=False, return_indices=True)
    waterline = waterline[near[0], near[1]]
    target = waterline + 1.0 + config.get("hummock_m", 2.0) * pools
    if config.get("roll_m"):
        # Low rolling rises over the meadow, easing out towards the pools.
        roll = noise(config["roll_km"])
        shore = np.clip((pools - threshold) / 0.25, 0, 1)
        target += config["roll_m"] * roll * roll * shore * shore * (3 - 2 * shore)
    target[wet] = waterline[wet] - config.get("water_depth_m", 1.8) * (0.45 + 0.55 * (1 - pools[wet]))
    shelf = np.clip((broad - 0.70) / 0.26, 0, 0.999) * 3
    fraction = shelf - np.floor(shelf)
    ramp = np.clip((fraction - 0.86) / 0.14, 0, 1)
    terrace = (np.floor(shelf) + ramp * ramp * (3 - 2 * ramp)) / 3
    target += np.where(wet, 0, terrace * config.get("shelf_height_m", 30))
    # Upsample in strips to avoid several full 8192-square intermediates.
    f = world.n // n
    cols = (np.arange(world.n, dtype=np.float32) + 0.5) / f - 0.5
    for first in range(0, world.n, 128):
        last = min(first + 128, world.n)
        rows = (np.arange(first, last, dtype=np.float32) + 0.5) / f - 0.5
        yy, xx = np.meshgrid(rows, cols, indexing="ij")
        strength = ndimage.map_coordinates(blend, [yy, xx], order=1, mode="nearest")
        ground = ndimage.map_coordinates(target, [yy, xx], order=1, mode="nearest")
        block = h[first:last]
        block += (ground - block) * strength
    core = blend > 0.8
    result = skeleton.copy()
    result[core] = 0
    result[core & wet] = [0, 0, 255]
    print(f"marsh floor: {count} stretches, water at {np.unique(np.round(waterline[marsh])).tolist()[:6]} m, {config.get('water_depth_m', 1.8):g} m depth; "
          f"{wet[core].mean():.0%} shallow pools, terraces to {config.get('shelf_height_m', 30):g} m")
    return result, waterline


def poisson_sites(world, mask, count, gap, rng, taken=()):
    """Up to `count` points (metres) in `mask` (zone grid), none closer than
    `gap` to one another or to `taken` [(x, y, keep-off radius)]."""
    ys, xs = np.nonzero(mask)
    if not len(xs):
        return []
    out = []
    for k in rng.permutation(len(xs)):
        x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
        if any(math.hypot(x - p[0], y - p[1]) < gap for p in out):
            continue
        if any(math.hypot(x - t[0], y - t[1]) < t[2] for t in taken):
            continue
        out.append((x, y))
        if len(out) >= count:
            break
    return out


def flatten_pad(h, cell, x, y, radius, blend, z):
    """A flat meadow round a slab's foot: the ground set to `z` within
    `radius`, eased back to what it was over `blend` metres beyond."""
    reach = radius + blend + cell
    r0, r1 = max(int((y - reach) / cell), 0), min(int((y + reach) / cell) + 2, h.shape[0])
    c0, c1 = max(int((x - reach) / cell), 0), min(int((x + reach) / cell) + 2, h.shape[1])
    yy, xx = np.mgrid[r0:r1, c0:c1]
    r = np.hypot((xx + 0.5) * cell - x, (yy + 0.5) * cell - y)
    t = np.clip((r - radius) / blend, 0, 1)
    w = 1 - t * t * (3 - 2 * t)
    win = h[r0:r1, c0:c1]
    win += (z - win) * w
    mask = np.zeros(h.shape, bool)
    mask[r0:r1, c0:c1] = r < radius + blend * 0.5
    return mask


def water_windows(wet64, core64, cell64, window_m):
    """The water's share in each window of `window_m` wholly inside the core."""
    k = max(int(round(window_m / cell64)), 1)
    n = wet64.shape[0] // k * k
    w = wet64[:n, :n].reshape(n // k, k, n // k, k).mean(axis=(1, 3))
    c = core64[:n, :n].reshape(n // k, k, n // k, k).all(axis=(1, 3))
    return w[c]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", default=str(ROOT / "terrain_lab_out" / "world"))
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    stem = args.world
    rules = json.loads(wf.RULES.read_text(encoding="utf-8"))
    rule = rules["megaliths"]["marsh_islands"]
    catalogue = json.loads(wf.CATALOGUE.read_text(encoding="utf-8"))
    out_json = Path(stem + "_marsh_islands.json")
    record = json.loads(out_json.read_text(encoding="utf-8")) if out_json.exists() else {}
    final = Path(stem + "_final.r16")
    skel = Path(stem + "_water_skeleton.png")
    base_h = base_of(final, Path(stem + "_final_base.r16"), record, "final_sha1")
    base_s = base_of(skel, Path(stem + "_water_skeleton_base.png"), record, "skeleton_sha1")
    if base_h != final:
        shutil.copyfile(base_h, final)
    world = wf.World(stem, args.height_range, args.sea)   # reads the base heights now
    rng = np.random.default_rng(args.seed * 7919 + 11)

    # The shallow wetland floor and its pools, on the lab's 64 m work grid.
    skel_img = np.asarray(Image.open(base_s).convert("RGB"))
    n64 = skel_img.shape[0]
    h = world.h.copy()
    skel_south, level64 = reshape_marsh(world, h, skel_img[::-1], rule, np.random.default_rng(args.seed * 7919 + 71))
    wet64 = (skel_south[..., 2] > 200) & (skel_south[..., 0] < 80)
    f = world.n // n64
    cell64 = world.size / n64
    cell = world.size / world.n
    def waterline_at(x, y):
        return float(level64[min(int(y / cell64), n64 - 1), min(int(x / cell64), n64 - 1)])
    inside = np.isin(world.zones, rule["zones"])
    depth = ndimage.distance_transform_edt(inside) * world.cell
    core = inside & (depth > rule.get("edge_blend_m", 650))

    # The A-1 slabs: `monoliths.count` of them spread over the whole valley,
    # each on a flat meadow pad just above the water (knee-deep at most).
    mo = rule["monoliths"]
    asset = json.loads((ROOT / rule["manifest"]).read_text(encoding="utf-8"))
    names = sorted(n for n, m in catalogue.items() if m["kind"] == rule["kind"])
    order = list(rng.permutation(len(names)))
    area = inside.sum() * (world.cell / 1000) ** 2
    gap = mo.get("gap_m") or 0.8 * math.sqrt(area / mo["count"]) * 1000
    pads = []
    for k, (x, y) in enumerate(poisson_sites(world, core & (depth > mo["edge_m"]), mo["count"], gap, rng)):
        name = names[order[k % len(names)]]
        m = catalogue[name]
        s = float(rng.uniform(*mo["scale"]))
        half = 0.5 * math.hypot(m["hi"][0] - m["lo"][0], m["hi"][1] - m["lo"][1]) * s
        radius = half + rng.uniform(*mo["pad_margin_m"])
        level = waterline_at(x, y)
        z = level + mo["pad_height_m"]
        land = flatten_pad(h, cell, x, y, radius, mo["pad_blend_m"], z)
        pads.append({"x": x, "y": y, "m": name, "s": s, "yaw": float(rng.uniform(0, math.tau)),
                     "R": radius + mo["pad_blend_m"], "level": level, "top": z, "land": land})

    # The terraced hills, rare (`hills.per_km2`), and small one-step islets
    # between them; neither on a slab's pad.
    taken = [(p["x"], p["y"], p["R"] + 150) for p in pads]
    hills = []
    erosion = rule["erosion"]
    # The hills first stand in the heightfield as mounds worn down by thermal
    # erosion (`mound`, `erosion`); the terraced ones get their steps cut into
    # the worn top as meshes once the ground is final. `knolls` stay grassy.
    mounds = []
    for kind in ("hills", "knolls"):
        spec = rule[kind]
        want = int(round(area * spec["per_km2"]))
        for x, y in poisson_sites(world, core, want, spec["gap_m"], rng, taken):
            m = spec["mound"]
            R = m["top_radius_m"][1] * (1 + m.get("wobble", 0.2)) + m["flank_m"][1]
            if any(math.hypot(x - t[0], y - t[1]) < R + t[2] for t in taken):
                continue
            level = waterline_at(x, y)
            mask, H = raise_mound(h, cell, x, y, level, m, erosion, rng)
            taken.append((x, y, R + spec["gap_m"] * 0.25))
            mounds.append({"kind": kind, "x": x, "y": y, "R": R + H / erosion["talus"], "H": H, "land": mask,
                           "level": level, "spec": spec})
    for kind in ("islets",):
        spec = rule[kind]
        prule = dict(rule, **spec)
        want = int(round(area * spec["per_km2"]))
        for x, y in poisson_sites(world, core, want, spec["gap_m"], rng, taken):
            prof = island_profile(rng, rng.uniform(*spec["r_top_m"]), prule)
            if spec.get("ramp"):
                prof["ramp"] = (rng.uniform(0, math.tau), spec["ramp_width_deg"] * math.pi / 180)
            R = prof["r_top"] * (1 + prule["wobble"]) + prof["treads"].sum()
            if any(math.hypot(x - t[0], y - t[1]) < R + t[2] for t in taken):
                continue
            level = waterline_at(x, y)
            # The ground under the hill a little inside and under its mesh
            # (marsh_hill_mesh): the mesh holds the crisp steps.
            ground_rule = dict(prule, shelf_m=prule["shelf_m"] - 0.6, inset_m=rule.get("ground_inset_m", 6.0))
            mask, _ = raise_island(h, cell, x, y, level, prof, ground_rule)
            taken.append((x, y, R + spec["gap_m"] * 0.25))
            hills.append({"kind": kind, "x": x, "y": y, "R": R, "land": mask, "prof": prof, "level": level, "rule": prule,
                          "top": level + prule["shelf_m"] + float(prof["risers"].sum())})

    # Red pools still lying in the dark forest's thinned edge, fewer the
    # deeper in (doc: the water goes red - purple - blue into the forest).
    fp = rule.get("forest_pools")
    if fp:
        forest = np.isin(world.zones, fp["zones"])
        into = ndimage.distance_transform_edt(~inside) * world.cell
        zk = n64 // inside.shape[0]
        up = lambda a: np.repeat(np.repeat(a, zk, 0), zk, 1)
        h64 = h.reshape(n64, f, n64, f).mean(axis=(1, 3))
        gy, gx = np.gradient(h64, cell64)
        pools = ndimage.gaussian_filter(rng.normal(size=(n64, n64)), fp["pool_km"] * 1000 / cell64 * 0.18)
        lo, hi = np.percentile(pools, [2, 98])
        pools = np.clip((pools - lo) / (hi - lo), 0, 1)
        share = fp["wet_share"] * np.clip(1 - up(into) / fp["width_m"], 0, 1)
        forest_wet = up(forest) & (np.hypot(gx, gy) < fp["max_slope"]) & (pools < share)
        forest_wet = ndimage.binary_opening(forest_wet)
        skel_south = skel_south.copy()
        skel_south[forest_wet] = [0, 0, 255]
        print(f"forest edge: {int(forest_wet.sum())} pool cells")

    # Every 64 m cell under a pad, a hill or a knoll is dry; its texels outside
    # them are a mossy shelf just above the water, so no dry pit is left.
    land = np.zeros(h.shape, bool)
    for q in pads + hills + mounds:
        land |= q.pop("land")
    dry64 = land.reshape(n64, f, n64, f).any(axis=(1, 3))
    newly = dry64 & wet64
    for a, b in zip(*np.nonzero(newly)):
        blk = h[a * f:(a + 1) * f, b * f:(b + 1) * f]
        np.maximum(blk, level64[a, b] + 0.5 + rule["shelf_m"], out=blk)
    wet64 &= ~dry64
    # The ground as the lab will read it back (16-bit), for the terraces.
    q16 = np.clip(np.round((h / args.height_range + args.sea) * 65535.0), 0, 65535)
    h = (q16 / 65535.0 - args.sea) * args.height_range

    # The hills' meshes: the crisp red steps (marsh_hill_mesh.py) - on an
    # islet round a flat top, on a hill cut into its worn top by its contours.
    mesh_dir = Path(stem + "_marsh_hills")
    if mesh_dir.exists():
        shutil.rmtree(mesh_dir)
    mesh_dir.mkdir()
    mesh_rng = np.random.default_rng(args.seed * 7919 + 97)
    triangles = 0
    gaps, pokes, dropped = [], [], 0
    for q in mounds:
        if q["kind"] != "hills":
            continue
        spec = q["spec"]
        prule = dict(rule, **spec)
        risers = list(mesh_rng.uniform(*spec["riser_m"], int(mesh_rng.integers(spec["steps"][0], spec["steps"][1] + 1))))
        # The steps keep to the worn top: no more than `terrace_share` of the hill.
        while len(risers) > spec["steps"][0] and sum(risers) > spec["terrace_share"] * q["H"]:
            risers.pop()
        cliffs = [(mesh_rng.uniform(0, math.tau), mesh_rng.uniform(*rule["cliff_width_deg"]) * math.pi / 180)
                  for _ in range(int(mesh_rng.integers(spec["cliff_sides"][0], spec["cliff_sides"][1] + 1)))]
        ramp_at = mesh_rng.uniform(0, math.tau) if spec.get("ramp") else None
        x, y, theta, rho, levels, ramp = terrace_rings(h, cell, q["x"], q["y"], q["R"], np.array(risers), prule,
                                                       cliffs, ramp_at, mesh_rng)
        ground = bilinear(h, cell)
        tris, weights, chk = marsh_hill_mesh.build_on_ground(theta, rho, levels, ramp, prule,
                                                             lambda a, b: ground(x + a, y + b), mesh_rng)
        gap_, poke_ = float(chk["foot_gap"].max()), float(chk["tread_poke"].max())
        if gap_ > 0.05 or poke_ > 0.1:
            # Not a hill to cut (it leans on rising land, the ground breaks
            # through a tread): it stays a grassy knoll.
            q["kind"] = "knolls"
            dropped += 1
            continue
        gaps.append(gap_)
        pokes.append(poke_)
        q.update(x=x, y=y, mesh=f"hill_{len(hills):03d}", risers=risers, cliffs=cliffs, ramp=ramp_at is not None,
                 top=float(levels[-1]), terrace_from=float(levels[0]))
        hills.append(q)
        for lod in range(3):
            n_tris = marsh_hill_mesh.write(mesh_dir / f"{q['mesh']}_lod{lod}.mesh", tris, levels[0] - 1.0, weights)
        triangles += n_tris
    for k, q in enumerate(hills):
        if "prof" in q:
            tris, weights = marsh_hill_mesh.build(q["prof"], q["rule"], q["level"], mesh_rng)
            q["mesh"] = f"islet_{k:03d}"
            for lod in range(3):
                n_tris = marsh_hill_mesh.write(mesh_dir / f"{q['mesh']}_lod{lod}.mesh", tris, q["level"] - 2.0, weights)
            triangles += n_tris
    print(f"hill meshes: {len(hills)} in {mesh_dir.name}/, {triangles} triangles; terraced hills: outer foot over the "
          f"ground at most {max(gaps, default=0):+.2f} m, ground over a tread at most {max(pokes, default=0):+.2f} m")
    q16.astype("<u2").tofile(final)
    img = skel_south[::-1].copy()
    img[np.nonzero(newly[::-1])] = 0
    Image.fromarray(img).save(skel)

    # The water's share over 2 km windows (doc: 30-50%).
    zk = n64 // inside.shape[0]
    core64 = np.repeat(np.repeat(core, zk, 0), zk, 1) if zk >= 1 else core
    shares = water_windows(wet64, core64, cell64, rule["checks"]["window_m"])
    lo, hi = rule["checks"]["water_share"]
    checks = {"windows": int(len(shares)), "water_share_mean": round(float(shares.mean()), 3) if len(shares) else 0,
              "water_share_p10_p90": [round(float(np.percentile(shares, p)), 3) for p in (10, 90)] if len(shares) else [],
              "windows_in_range": round(float(((shares >= lo) & (shares <= hi)).mean()), 3) if len(shares) else 0,
              "range": [lo, hi],
              "terraces": {"hills": len(gaps), "foot_over_ground_max_m": round(max(gaps, default=0), 2),
                           "ground_over_tread_max_m": round(max(pokes, default=0), 2),
                           "left_uncut": dropped}}

    out = {"comment": "The marshes' layout (tools/terrain_lab/marsh_islands.py): `islands` the A-1 slabs' pads, which "
                      "world_megaliths.py stands a slab on; `hills` the terraced hills and islets.",
           "rows_deg": 0, "final_sha1": digest(final), "skeleton_sha1": digest(skel), "checks": checks,
           "islands": [{"x": round(q["x"], 1), "y": round(q["y"], 1), "R": round(q["R"], 1), "level": q["level"],
                        "top": round(q["top"], 1), "m": q["m"], "s": round(q["s"], 4), "yaw": round(q["yaw"], 4)}
                       for q in pads],
           "hills": [{"kind": q["kind"], "x": round(q["x"], 1), "y": round(q["y"], 1), "R": round(q["R"], 1),
                      "top": round(q["top"], 1), "steps": len(q["prof"]["risers"] if "prof" in q else q["risers"]),
                      "risers_m": [round(float(v), 1) for v in (q["prof"]["risers"] if "prof" in q else q["risers"])],
                      "cliff_sides": len(q["prof"]["cliffs"] if "prof" in q else q["cliffs"]),
                      "ramp": "ramp" in q["prof"] if "prof" in q else q["ramp"], "mesh": q["mesh"],
                      **({"hill_m": round(q["H"], 1), "terraces_from": round(q["terrace_from"], 1)} if "H" in q else {})}
                     for q in hills],
           "knolls": [{"x": round(q["x"], 1), "y": round(q["y"], 1), "R": round(q["R"], 1), "hill_m": round(q["H"], 1)}
                      for q in mounds if q["kind"] == "knolls"],
           "mesh_dir": mesh_dir.name}
    out_json.write_text(json.dumps(out, indent=1), encoding="utf-8")
    n_h = sum(q["kind"] == "hills" for q in hills)
    print(f"{len(pads)} slab pads (gap {gap:.0f} m), {n_h} terraced hills, {len(out['knolls'])} knolls, "
          f"{len(hills) - n_h} islets; "
          f"{int(newly.sum())} pool cells made dry; water {checks['water_share_mean']:.0%} on average, "
          f"{checks['windows_in_range']:.0%} of 2 km windows within {lo:.0%}-{hi:.0%} -> {out_json.name}")


if __name__ == "__main__":
    main()
