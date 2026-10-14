"""The canyon steppe's relief cut into mesas (doc/BIOME_PLAN.md 3.3, the
user's references doc/NewRef/steppe/): flat tops in tiers, sheer risers
between them, a flat dry floor at the foot of the walls.

    python tools/terrain_lab/steppe_relief.py [--world terrain_lab_out/world] [--seed N]

Run after the generator (and the marshes), before steppe_ground.py and
world_features.py: the walls become cliff meshes there (risers are steeper
than the cliffs' threshold), the floor and the tops get their own biomes.

How: inside the steppe and the stone desert (<world>_biomes.png ids 3, 16 and
the steppe's own 19-21) the heights are smoothed (the eroded lumps go). The
floor is the region's low ground (`window_m`) with dry washes; the tops are
the smoothed land cut into flat tiers `tier_m` high over it, the tier levels
drifting slowly so a step is not a contour line. The lowest ground is floor;
a network of canyons (ridges of a warped noise: trunks and branches) takes
`cover` of the rest down to the floor between sheer walls `wall_m`
wide, with scree banked at their foot. The change eases in over `feather_m`
from the region's edge.

Re-run safe: the heights it replaced are kept in <world>_steppe_relief.npz
with a digest of what it wrote; a re-run on its own output starts from
them, on anything else (a new world) takes the current heights. The
marshes' base copy (<world>_final_base.r16) gets the same change when it is
in step with the heights, so a marsh re-run keeps the mesas.
"""

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as nd

ROOT = Path(__file__).resolve().parents[2]
RULES = ROOT / "content" / "config" / "terrain_core" / "steppe_ground.json"
WORLD_RULES = ROOT / "content" / "config" / "terrain_core" / "world_rules.json"


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def noise(shape, cell_px, rng, octaves=3):
    out = np.zeros(shape, np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        scale = cell_px / (2 ** o)
        gh, gw = int(np.ceil(shape[0] / scale)) + 3, int(np.ceil(shape[1] / scale)) + 3
        grid = rng.random((gh, gw)).astype(np.float32)
        y = np.arange(shape[0], dtype=np.float32) / scale
        x = np.arange(shape[1], dtype=np.float32) / scale
        yy, xx = np.meshgrid(y, x, indexing="ij")
        out += amp * nd.map_coordinates(grid, (yy, xx), order=1, mode="nearest")
        total += amp
        amp *= 0.5
    return out / total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", default="terrain_lab_out/world")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--stats", action="store_true", help="print how the region spreads between its low and high ground")
    args = ap.parse_args()
    r = json.loads(RULES.read_text(encoding="utf-8"))["relief"]
    world_m = json.loads(WORLD_RULES.read_text(encoding="utf-8"))["world_km"] * 1000.0

    final = Path(args.world + "_final.r16")
    raw = np.fromfile(final, "<u2")
    n = int(np.sqrt(raw.size))
    q16 = raw.reshape(n, n).copy()
    cell = world_m / n

    biomes = np.array(Image.open(args.world + "_biomes.png"))
    k = n // biomes.shape[0]
    region64 = np.isin(biomes, r["biomes"])
    # The heights' rows run south to north, the biome picture's north to south.
    region64 = region64[::-1]
    rows = np.nonzero(region64.any(axis=1))[0]
    cols = np.nonzero(region64.any(axis=0))[0]
    pad = int(np.ceil((r["window_m"] + r["feather_m"]) / cell / k)) + 2
    b0, b1 = max(rows[0] - pad, 0), min(rows[-1] + pad + 1, biomes.shape[0])
    c0, c1 = max(cols[0] - pad, 0), min(cols[-1] + pad + 1, biomes.shape[1])
    y0, y1, x0, x1 = b0 * k, b1 * k, c0 * k, c1 * k

    before_sha = hashlib.sha1(raw.tobytes()).hexdigest()
    keep = Path(args.world + "_steppe_relief.npz")
    if keep.exists():
        # Our own last output: put back what it replaced.
        z = np.load(keep)
        ky0, ky1, kx0, kx1 = (int(v) for v in z["box"])
        if str(z["wrote"]) == hashlib.sha1(q16[ky0:ky1, kx0:kx1].tobytes()).hexdigest():
            q16[ky0:ky1, kx0:kx1] = z["original"]
    original = q16[y0:y1, x0:x1].copy()

    h = (original.astype(np.float32) / 65535.0 - args.sea) * args.height_range
    m = np.repeat(np.repeat(region64[b0:b1, c0:c1], k, 0), k, 1)
    inside = nd.distance_transform_edt(m) * cell
    weight = smoothstep(0.0, r["feather_m"], inside)

    rng = np.random.default_rng(args.seed * 7919 + 3)
    s = nd.gaussian_filter(h, r["smooth_m"] / cell)
    win = max(3, int(round(r["window_m"] / cell)) | 1)
    # (the filters on a 4x coarser grid: a few kilometres of window at 16 m)
    sk = 4
    sc = s[::sk, ::sk]
    # Low and high ground of the region itself: the plain above it and the
    # desert below are not its tops and floors.
    mc = m[::sk, ::sk]
    lo = nd.minimum_filter(np.where(mc, sc, np.inf), win // sk | 1)
    hi = nd.maximum_filter(np.where(mc, sc, -np.inf), win // sk | 1)
    lo = np.where(np.isfinite(lo), lo, sc)
    hi = np.where(np.isfinite(hi), hi, sc)
    lo = nd.gaussian_filter(lo, r["window_m"] / cell / sk / 3)
    hi = nd.gaussian_filter(hi, r["window_m"] / cell / sk / 3)
    zoom = (s.shape[0] / sc.shape[0], s.shape[1] / sc.shape[1])
    lo = nd.zoom(lo, zoom, order=1)[: s.shape[0], : s.shape[1]]
    hi = nd.zoom(hi, zoom, order=1)[: s.shape[0], : s.shape[1]]
    span = np.maximum(hi - lo, r["min_span_m"])
    rel = np.clip((s - lo) / span, 0.0, 1.0)
    if args.stats:
        w5 = weight > 0.5
        print("rel percentiles 10..90:", np.percentile(rel[w5], [10, 25, 50, 75, 90]).round(2))

    # The floor: the region's low ground, flat but for dry washes.
    wash = noise(h.shape, r["wash_km"] * 1000 / cell, rng, 4)
    floor = lo + r["wash_m"] * smoothstep(0.35, 0.65, wash)
    # The tops: the smoothed land at least `min_drop_m` over the floor, cut
    # into flat tiers; the levels drift slowly so a step is not a contour.
    top = np.maximum(nd.gaussian_filter(s, r["top_smooth_m"] / cell), floor + r["min_drop_m"])
    drift = (noise(h.shape, r["drift_km"] * 1000 / cell, rng, 2) - 0.5) * r["drift"]
    qt = (top - floor) / r["tier_m"] + drift
    step = np.floor(qt) + smoothstep(1.0 - r["riser"], 1.0, qt - np.floor(qt))
    top = floor + np.maximum((step - drift) * r["tier_m"], r["min_drop_m"])
    roll = (noise(h.shape, r["roll_km"] * 1000 / cell, rng, 3) - 0.5) * r["roll_m"]
    top = top + roll
    # The canyons: ridges of a warped noise at two sizes (trunks and their
    # branches) plus the region's lowest ground, taking `cover` of it.
    warp = (noise(h.shape, 2500 / cell, rng, 2) - 0.5) * (1500 / cell)
    yy, xx = np.mgrid[0:h.shape[0], 0:h.shape[1]].astype(np.float32)
    def ridges(km, salt):
        g = np.random.default_rng(args.seed * 31 + salt)
        base = noise(h.shape, km * 1000 / cell, g, 3)
        warped = nd.map_coordinates(base, (yy + warp, xx - warp), order=1, mode="nearest")
        return 1.0 - np.abs(warped * 2.0 - 1.0)
    c = np.maximum(ridges(r["trunk_km"], 1), ridges(r["branch_km"], 2) * r["branch"])
    region = weight > 0.5
    low = rel < r["low_rel"]
    upland = region & ~low
    canyon = low | (c > np.quantile(c[upland], 1.0 - r["cover"]))
    canyon = nd.binary_opening(canyon, iterations=1)
    # Sheer walls: across `wall_m`, by the signed distance to the canyon's edge;
    # a bank of scree at the foot.
    sd = (nd.distance_transform_edt(~canyon) - nd.distance_transform_edt(canyon)) * cell
    up = smoothstep(-r["wall_m"] * 0.5, r["wall_m"] * 0.5, sd)
    scree = r["scree_m"] * np.exp(-np.maximum(-sd, 0.0) / r["scree_reach_m"]) * (sd < 0)
    new = floor + scree + (top - floor - scree) * up
    if args.stats:
        print(f"canyon share {canyon[region].mean():.2f}, top over floor median {np.median((top - floor)[region]):.0f} m, "
              f"span median {np.median(span[region]):.0f} m")
    out = h + (new - h) * weight
    q = np.clip(np.round((out / args.height_range + args.sea) * 65535.0), 0, 65535).astype(np.uint16)

    q16[y0:y1, x0:x1] = q
    q16.astype("<u2").tofile(final)
    np.savez_compressed(keep, box=np.array([y0, y1, x0, x1]), original=original,
                        wrote=hashlib.sha1(q.tobytes()).hexdigest())
    # The marshes' base copy, when it is in step with the heights they wrote.
    marsh_json = Path(args.world + "_marsh_islands.json")
    marsh_base = Path(args.world + "_final_base.r16")
    synced = False
    if marsh_json.exists() and marsh_base.exists():
        record = json.loads(marsh_json.read_text(encoding="utf-8"))
        if record.get("final_sha1") == before_sha:
            b = np.fromfile(marsh_base, "<u2").reshape(n, n)
            b[y0:y1, x0:x1] = q
            b.astype("<u2").tofile(marsh_base)
            record["final_sha1"] = hashlib.sha1(final.read_bytes()).hexdigest()
            marsh_json.write_text(json.dumps(record, indent=1), encoding="utf-8")
            synced = True

    moved = np.abs(out - h)[weight > 0.5]
    gy, gx = np.gradient(out, cell)
    sl = np.hypot(gx, gy)[weight > 0.99]
    print(f"steppe relief: box {y1 - y0}x{x1 - x0} cells, span median {np.median(span[weight > 0.5]):.0f} m, "
          f"moved median {np.median(moved):.0f} m (95% {np.percentile(moved, 95):.0f} m), "
          f"walls (slope > 0.7) {np.mean(sl > 0.7) * 100:.0f}%, flat (< 0.1) {np.mean(sl < 0.1) * 100:.0f}%, "
          f"marsh base synced {synced}")


if __name__ == "__main__":
    main()
