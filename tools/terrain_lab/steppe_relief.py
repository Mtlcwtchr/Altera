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
wide, with scree banked at their foot. Buttes (`buttes`): columns of the land
on the canyon floor, sheer all round, some stepped once, some taller than the
mesas; their walls too become cliff meshes. The change eases in over
`feather_m` from the region's edge.

Re-run safe: the heights it replaced are kept in <world>_steppe_relief.npz
with a digest of what it wrote (the buttes' places in <world>_steppe_relief.json); a re-run on its own output starts from
them, on anything else (a new world) takes the current heights. The
marshes' base copy (<world>_final_base.r16) gets the same change when it is
in step with the heights, so a marsh re-run keeps the mesas.
"""

import argparse
import hashlib
import heapq
import math
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as nd
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[2]
RULES = ROOT / "content" / "config" / "terrain_core" / "steppe_ground.json"
WORLD_RULES = ROOT / "content" / "config" / "terrain_core" / "world_rules.json"


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def unit(a):
    """A noise stretched over 0..1 by its own 2nd and 98th percentiles."""
    lo_, hi_ = np.percentile(a, [2, 98])
    return np.clip((a - lo_) / max(hi_ - lo_, 1e-6), 0.0, 1.0)


def noise(shape, cell_px, rng, octaves=3, order=1):
    """Value noise; `order` 3 for one whose contours are drawn (thresholds,
    steps): the bilinear one's run straight along its grid."""
    out = np.zeros(shape, np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        scale = cell_px / (2 ** o)
        gh, gw = int(np.ceil(shape[0] / scale)) + 3, int(np.ceil(shape[1] / scale)) + 3
        grid = rng.random((gh, gw)).astype(np.float32)
        y = np.arange(shape[0], dtype=np.float32) / scale
        x = np.arange(shape[1], dtype=np.float32) / scale
        yy, xx = np.meshgrid(y, x, indexing="ij")
        out += amp * nd.map_coordinates(grid, (yy, xx), order=order, mode="nearest")
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

    # --- old rivers: the canyons are their dry valleys -------------------
    # Water ran off the smoothed land (on the coarse grid) to the region's
    # edge: a priority flood from the edge fills the pits and gives each cell
    # the one it drains to; the area draining through a cell is summed down.
    ny, nx = sc.shape
    rv = r["rivers"]
    # (the land a little rumpled first: D8 on a smooth slope runs in straight
    # parallel lines; on a rumpled one the rivers wander and gather)
    g0 = np.random.default_rng(args.seed * 7919 + 5)
    rumple = (noise(sc.shape, rv["route_noise_km"] * 1000 / (cell * sk), g0, 4) - 0.5) * 2.0 * rv["route_noise_m"]
    filled = (sc + rumple).astype(np.float64)
    recv = np.full(ny * nx, -1, np.int64)
    seen = ~mc.copy()
    edge_ = seen & nd.binary_dilation(mc, structure=np.ones((3, 3)))
    # Ties on the filled flats broken by a slow noise, so a river crossing a
    # filled pit bends with it instead of running a straight line.
    tie = noise(sc.shape, rv["route_noise_km"] * 500 / (cell * sk), g0, 2).ravel()
    heap = [(float(filled.flat[i_]), float(tie[i_]), int(i_)) for i_ in np.flatnonzero(edge_)]
    heapq.heapify(heap)
    order = []
    nbr = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    while heap:
        z_, _t, i_ = heapq.heappop(heap)
        yy_, xx_ = divmod(i_, nx)
        for dy, dx in nbr:
            y_, x_ = yy_ + dy, xx_ + dx
            if 0 <= y_ < ny and 0 <= x_ < nx and not seen[y_, x_]:
                seen[y_, x_] = True
                j_ = y_ * nx + x_
                filled.flat[j_] = max(filled.flat[j_], z_ + 1e-3)
                recv[j_] = i_
                order.append(j_)
                heapq.heappush(heap, (float(filled.flat[j_]), float(tie[j_]), j_))
    cell_c = cell * sk
    acc = np.full(ny * nx, cell_c * cell_c / 1e6)          # km^2
    for j_ in reversed(order):
        if recv[j_] >= 0:
            acc[recv[j_]] += acc[j_]
    acc = acc.reshape(ny, nx)
    chan = mc & (acc >= rv["min_km2"])
    span_c = span[::sk, ::sk][:ny, :nx]
    depth = np.minimum(rv["depth_m"] + rv["depth_per_doubling_m"] * np.log2(1.0 + acc), rv["depth_of_span"] * span_c)
    bed = filled.reshape(ny, nx) - rumple - depth
    # A river cuts no deeper than the region's low ground: where it comes out
    # onto the low plain its valley floor is that plain.
    bed = np.maximum(bed, lo[::sk, ::sk][:ny, :nx])
    # The floor between channels blended smoothly from their beds (a nearest
    # bed would step where two valleys of different depth meet).
    wgt = nd.gaussian_filter(chan.astype(np.float64), rv["blend_m"] / cell_c)
    bed_s = nd.gaussian_filter(np.where(chan, bed, 0.0), rv["blend_m"] / cell_c) / np.maximum(wgt, 1e-9)
    bed_s = np.where(wgt > 1e-4, bed_s, bed)
    bed_f = nd.zoom(bed_s, (h.shape[0] / ny, h.shape[1] / nx), order=1)[: h.shape[0], : h.shape[1]]
    width = np.clip(rv["width_per_sqrt_km2_m"] * np.sqrt(acc), *rv["width_m"])
    # Down to the fine grid: each point takes the nearest channel cell's bed
    # and width; it is valley floor within half that width (wandering).
    cm = np.zeros(h.shape, bool)
    sub = cm[sk // 2::sk, sk // 2::sk]
    sub[:] = chan[: sub.shape[0], : sub.shape[1]]
    dist, (iy, ix) = nd.distance_transform_edt(~cm, return_indices=True)
    # The valleys bent: the lookup pushed about by a noise, so no stretch of
    # a river runs ruler-straight along the grid.
    wy = (noise(h.shape, rv["bend_km"] * 1000 / cell, rng, 3) - 0.5) * 2.0 * rv["bend_m"] / cell
    wx = (noise(h.shape, rv["bend_km"] * 1000 / cell, rng, 3) - 0.5) * 2.0 * rv["bend_m"] / cell
    gy_, gx_ = np.mgrid[0:h.shape[0], 0:h.shape[1]].astype(np.float32)
    at = (np.clip(gy_ + wy, 0, h.shape[0] - 1), np.clip(gx_ + wx, 0, h.shape[1] - 1))
    dist = nd.map_coordinates(dist.astype(np.float32), at, order=1) * cell
    iy = nd.map_coordinates(iy, at, order=0)
    ix = nd.map_coordinates(ix, at, order=0)
    del gy_, gx_, at
    cy_, cx_ = np.clip(iy // sk, 0, ny - 1), np.clip(ix // sk, 0, nx - 1)
    meander = 1.0 + rv["wander"] * (noise(h.shape, rv["wander_km"] * 1000 / cell, rng, 3) - 0.5) * 2.0
    half_w = 0.5 * width[cy_, cx_] * meander
    region = weight > 0.5
    low = rel < r["low_rel"]
    valley = (dist < half_w) & bool(cm.any())
    canyon = valley | low
    canyon = nd.binary_opening(canyon, iterations=2)
    # Edges rounded off the coarse grid's stairs, then roughened a little.
    rough_ = (noise(h.shape, rv["edge_km"] * 1000 / cell, rng, 3) - 0.5) * rv["edge_rough"]
    canyon = nd.gaussian_filter(canyon.astype(np.float32), rv["edge_smooth_m"] / cell) + rough_ > 0.5
    floor = np.where(valley, bed_f, lo)
    floor = np.minimum(floor, lo + span)          # never over the high ground
    floor = nd.gaussian_filter(floor, rv["floor_smooth_m"] / cell)
    # The tops: the smoothed land at least `min_drop_m` over the floor, cut
    # into flat tiers; the levels drift slowly so a step is not a contour.
    # A tier's riser is a cliff `riser_cells` wide, not a ramp (user 14.10:
    # sharp stone terraces instead of gentle rises).
    top = np.maximum(nd.gaussian_filter(s, r["top_smooth_m"] / cell), floor + r["min_drop_m"])
    drift = (noise(h.shape, r["drift_km"] * 1000 / cell, rng, 2) - 0.5) * r["drift"]
    tier = nd.gaussian_filter(np.floor((top - floor) / r["tier_m"] + drift), r["riser_cells"])
    top = floor + np.maximum((tier - drift) * r["tier_m"], r["min_drop_m"])
    roll = (noise(h.shape, r["roll_km"] * 1000 / cell, rng, 3) - 0.5) * r["roll_m"]
    top = top + roll

    # --- the drought: the high ground crumbled into islands ---------------
    # (user 14.10) After the water cut the valleys, the dry broke the banks
    # up: cracks along a warped Voronoi net split the tops into islands, the
    # small net only near the canyons, where the crumbling began.
    cr = r["cracks"]
    gk = np.random.default_rng(args.seed * 7919 + 11)
    to_canyon = nd.distance_transform_edt(~canyon) * cell
    yy0, xx0 = np.mgrid[0:h.shape[0], 0:h.shape[1]].astype(np.float32)
    crack = np.zeros(h.shape, bool)
    for lv in cr["levels"]:
        cpx = lv["cell_m"] / cell
        gh_, gw_ = int(h.shape[0] / cpx) + 3, int(h.shape[1] / cpx) + 3
        jy = (np.arange(gh_)[:, None] - 1 + gk.uniform(0.15, 0.85, (gh_, gw_))) * cpx
        jx = (np.arange(gw_)[None, :] - 1 + gk.uniform(0.15, 0.85, (gh_, gw_))) * cpx
        tree = cKDTree(np.stack([jy.ravel(), jx.ravel()], 1))
        wp = lv["warp_m"] / cell
        qy = yy0 + (noise(h.shape, lv["cell_m"] * 0.7 / cell, gk, 3, 3) - 0.5) * 2.0 * wp
        qx = xx0 + (noise(h.shape, lv["cell_m"] * 0.7 / cell, gk, 3, 3) - 0.5) * 2.0 * wp
        dd, _ = tree.query(np.stack([qy.ravel(), qx.ravel()], 1), k=2, workers=-1)
        border = ((dd[:, 1] - dd[:, 0]) * 0.5 * cell).reshape(h.shape).astype(np.float32)
        del dd, qy, qx
        wn = unit(noise(h.shape, lv["cell_m"] * 0.5 / cell, gk, 2, 3))
        half = 0.5 * (lv["width_m"][0] + (lv["width_m"][1] - lv["width_m"][0]) * wn)
        gn = noise(h.shape, lv["cell_m"] * 1.3 / cell, gk, 2, 3)
        gate = gn < np.quantile(gn[region], lv["open"])
        near = to_canyon < lv.get("near_m", 1e9) * (0.6 + 0.8 * wn)
        crack |= (border < half) & gate & near
    del yy0, xx0
    crack = nd.binary_opening(crack & ~canyon, iterations=1)      # no one-cell slits
    low_all = canyon | crack

    # --- the islands, by what the crumbling left of them ------------------
    # Wide enough to walk on: a mesa (terraced, the wall stepped by a bench);
    # narrower: a stone column, or worn to a peak; the crumbs: a peak or a
    # heap of rubble. (A piece of the box's own edge stays a mesa.)
    ic = r["islands"]
    isl = ~low_all
    lab_, nis = nd.label(isl)
    din = (nd.distance_transform_edt(isl) * cell).astype(np.float32)
    ids = np.arange(1, nis + 1)
    Ri = np.zeros(nis + 1, np.float32)
    Ri[1:] = nd.maximum(din, lab_, ids)
    area = np.zeros(nis + 1)
    area[1:] = nd.sum(isl, lab_, ids) * cell * cell
    Hm = np.zeros(nis + 1, np.float32)
    Hm[1:] = nd.mean(top - floor, lab_, ids)
    touch = np.zeros(nis + 1, bool)
    touch[np.unique(lab_[isl & nd.binary_dilation(~region, iterations=2)])] = True
    gi = np.random.default_rng(args.seed * 104729 + 23)
    cls = np.zeros(nis + 1, np.int8)        # 1 mesa, 2 column, 3 peak, 4 rubble
    rise = np.ones(nis + 1, np.float32)
    lift_t = np.zeros(nis + 1, np.float32)
    for i in range(1, nis + 1):
        if touch[i] or Ri[i] >= ic["mesa_r_m"]:
            cls[i] = 1
            # each island its own height: tiers up or down (user 14.10)
            if not touch[i]:
                lift_t[i] = gi.choice(ic["tier_shift"], p=ic["tier_shift_p"]) * r["tier_m"]
        elif Ri[i] >= ic["column_r_m"]:
            cls[i] = 3 if gi.random() < ic["peak_share"] else 2
            rise[i] = gi.uniform(*(ic["peak_rise"] if cls[i] == 3 else ic["column_rise"]))
        else:
            cls[i] = 3 if gi.random() < ic["crumb_peak_share"] else 4
            rise[i] = gi.uniform(*ic["peak_rise"]) if cls[i] == 3 else gi.uniform(*ic["rubble_m"])
    c_ = cls[lab_]
    t_ = np.clip(din / np.maximum(Ri[lab_], 1.0), 0.0, 1.0)
    # A mesa's wall stepped once by a bench where its island is big enough:
    # two sheer risers with a ledge between, the ledge broken here and there.
    bw = ic["bench_m"][0] + (ic["bench_m"][1] - ic["bench_m"][0]) * unit(noise(h.shape, 300 / cell, rng, 3, 3))
    bn = noise(h.shape, 900 / cell, rng, 2, 3)
    bgate = bn < np.quantile(bn[region], ic["bench_share"])
    bh = ic["bench_rise"][0] + (ic["bench_rise"][1] - ic["bench_rise"][0]) * unit(noise(h.shape, 1200 / cell, rng, 2, 3))
    bench = (c_ == 1) & (Ri[lab_] >= ic["bench_min_r_m"]) & bgate & (din < bw)
    bench = nd.binary_opening(bench, iterations=1)
    top = np.where(c_ == 1, np.maximum(top + lift_t[lab_], floor + r["min_drop_m"]), top)
    zi = np.where(bench, floor + bh * (top - floor), top)
    zi = np.where(c_ == 2, floor + Hm[lab_] * rise[lab_], zi)
    zi = np.where(c_ == 3, floor + Hm[lab_] * rise[lab_] * t_ ** ic["peak_pow"], zi)
    zi = np.where(c_ == 4, floor + rise[lab_] * np.sqrt(t_), zi)
    zi = np.where(isl, zi, floor)
    # (a wall's top read from its own island, so the riser stays sheer)
    _, (ey, ex) = nd.distance_transform_edt(~isl, return_indices=True)
    zext = zi[ey, ex]
    del ey, ex

    # --- the canyon's floor: stony, with peaks --------------------------
    # Round the walls' feet the blocks that fell: low ledges a step or two
    # high; peaks of every size standing alone and in small groups, more of
    # them near the walls; the dry bed of the main valley kept clear.
    fl = r["floor"]
    dwall = (nd.distance_transform_edt(low_all) * cell).astype(np.float32)
    clear = smoothstep(0.0, fl["clear_m"], dist - half_w * 0.35)
    amp = fl["block_m"] * np.exp(-dwall / fl["block_reach_m"]) + fl["block_far_m"]
    lift = amp * unit(noise(h.shape, fl["block_cell_m"] / cell, rng, 3, 3)) * clear
    lift = np.floor(lift / fl["step_m"]) * fl["step_m"]
    # (the small steps a little softer than the walls: a step of a few metres
    # sharper than a cell speckles the shading as the eye moves)
    lift = nd.gaussian_filter(lift, fl["step_soft_cells"])
    pk = r["peaks"]
    wall_h = np.maximum(zext - floor, 0.0)
    cand = low_all & region & (dwall >= pk["wall_m"][0]) & (dwall <= pk["wall_m"][1]) & (clear > 0.9)
    ys_, xs_ = np.nonzero(cand)
    want = int(pk["per_km2"] * (low_all & region).sum() * cell * cell / 1e6)
    gp = np.random.default_rng(args.seed * 104729 + 31)
    wgt_ = np.exp(-dwall[ys_, xs_] / pk["near_m"])
    order_ = gp.choice(len(ys_), size=min(len(ys_), want * 4), replace=False, p=wgt_ / wgt_.sum()) if len(ys_) else []
    peaks, taken = [], []
    gcell = pk["gap_m"] / cell
    grid_ = {}
    def free(y, x, gap):
        gy0, gx0 = int(y // gcell), int(x // gcell)
        for a_ in range(gy0 - 1, gy0 + 2):
            for b_ in range(gx0 - 1, gx0 + 2):
                for (qy_, qx_) in grid_.get((a_, b_), ()):
                    if (qy_ - y) ** 2 + (qx_ - x) ** 2 < gap * gap:
                        return False
        return True
    for k_ in order_:
        if len(peaks) >= want:
            break
        y, x = int(ys_[k_]), int(xs_[k_])
        for j_ in range(int(gp.integers(pk["group"][0], pk["group"][1] + 1))):
            if j_:
                a_ = gp.uniform(0, math.tau)
                y = int(ys_[k_] + math.sin(a_) * gp.uniform(1.0, 2.2) * pk["radius_m"][1] / cell)
                x = int(xs_[k_] + math.cos(a_) * gp.uniform(1.0, 2.2) * pk["radius_m"][1] / cell)
                if not (0 <= y < h.shape[0] and 0 <= x < h.shape[1]) or not cand[y, x]:
                    continue
            if not free(y, x, gcell * (0.5 if j_ else 1.0)):
                continue
            grid_.setdefault((int(y // gcell), int(x // gcell)), []).append((y, x))
            rad = gp.uniform(*pk["radius_m"]) / cell
            tall = min(gp.uniform(*pk["rise_m"]), max(pk["of_wall"] * float(wall_h[y, x]), pk["rise_m"][0]))
            w = int(rad * 1.4) + 2
            y0_, y1_, x0_, x1_ = max(y - w, 0), min(y + w + 1, h.shape[0]), max(x - w, 0), min(x + w + 1, h.shape[1])
            yy_, xx_ = np.mgrid[y0_:y1_, x0_:x1_]
            ang = np.arctan2(yy_ - y, xx_ - x)
            lob = 1.0
            for k2 in (2, 3, 5):
                lob = lob + gp.uniform(0.08, 0.3) * np.sin(k2 * ang + gp.uniform(0, math.tau))
            d = np.hypot(yy_ - y, xx_ - x) / (rad * lob)
            # (as a mesh, `mesh`: the ground keeps only its foot of broken rock)
            prof = tall * (pk["foot"] if pk.get("mesh") else 1.0) * np.clip(1.0 - d, 0.0, 1.0) ** pk["pow"]
            np.maximum(lift[y0_:y1_, x0_:x1_], prof, out=lift[y0_:y1_, x0_:x1_])
            peaks.append({"x": round((x0 + x + 0.5) * cell, 1), "y": round((y0 + y + 0.5) * cell, 1),
                          "radius_m": round(rad * cell, 1), "rise_m": round(float(tall), 1),
                          "floor_m": round(float(floor[y, x]), 1), "top_m": round(float(floor[y, x] + tall), 1),
                          "room_m": round(rad * cell * pk.get("room", 1.0), 1)})
    scree = r["scree_m"] * np.exp(-dwall / r["scree_reach_m"]) * low_all
    base = floor + np.maximum(lift, scree)
    # Sheer walls: across `wall_m`, by the signed distance to the islands' edge.
    sd = (nd.distance_transform_edt(isl) - nd.distance_transform_edt(~isl)) * cell
    up = smoothstep(-r["wall_m"] * 0.5, r["wall_m"] * 0.5, sd)
    new = base + np.maximum(zext - base, 0.0) * up

    # --- the last streams: dry washes and their mouths -------------------
    # The rain that still falls ran off the finished ground (D8 on the coarse
    # grid again); where enough of it gathered on the floor a shallow wash
    # is cut; where a wash leaves a narrow cleft for the open floor, or runs
    # into the old river's bed, its last load spread as a fan: the dried-up
    # mouths of the last streams (user 14.10).
    ws = r["washes"]
    nc = new[sk // 2::sk, sk // 2::sk][:ny, :nx].astype(np.float64)
    nyc, nxc = nc.shape
    regc = region[sk // 2::sk, sk // 2::sk][:nyc, :nxc]
    seen = ~regc.copy()
    edge_ = seen & nd.binary_dilation(regc, structure=np.ones((3, 3)))
    tie = noise(nc.shape, 4.0, g0, 2).ravel()
    filled = nc.copy()
    heap = [(float(filled.flat[i_]), float(tie[i_]), int(i_)) for i_ in np.flatnonzero(edge_)]
    heapq.heapify(heap)
    recv2 = np.full(nyc * nxc, -1, np.int64)
    order = []
    while heap:
        z_, _t, i_ = heapq.heappop(heap)
        yy_, xx_ = divmod(i_, nxc)
        for dy, dx in nbr:
            y_, x_ = yy_ + dy, xx_ + dx
            if 0 <= y_ < nyc and 0 <= x_ < nxc and not seen[y_, x_]:
                seen[y_, x_] = True
                j_ = y_ * nxc + x_
                filled.flat[j_] = max(filled.flat[j_], z_ + 1e-3)
                recv2[j_] = i_
                order.append(j_)
                heapq.heappush(heap, (float(filled.flat[j_]), float(tie[j_]), j_))
    acc2 = np.full(nyc * nxc, cell_c * cell_c / 1e6)
    for j_ in reversed(order):
        if recv2[j_] >= 0:
            acc2[recv2[j_]] += acc2[j_]
    lowc = low_all[sk // 2::sk, sk // 2::sk][:nyc, :nxc]
    wash = (acc2.reshape(nyc, nxc) >= ws["min_km2"]) & lowc & regc
    # The mouths: a wash cell whose water leaves a cleft (the cracks) for open
    # floor, or reaches the old river's bed.
    crc = crack[sk // 2::sk, sk // 2::sk][:nyc, :nxc]
    chc = chan[:nyc, :nxc]
    fans = []
    gf = np.random.default_rng(args.seed * 104729 + 41)
    for j_ in np.flatnonzero(wash.ravel()):
        rcv = recv2[j_]
        if rcv < 0:
            continue
        a2 = acc2[j_]
        yj, xj = divmod(j_, nxc)
        yr, xr = divmod(rcv, nxc)
        out_of_cleft = crc[yj, xj] and not crc[yr, xr]
        into_bed = (not chc[yj, xj]) and chc[yr, xr] and a2 >= ws["mouth_min_km2"]
        if out_of_cleft or into_bed:
            fans.append((yj, xj, yr - yj, xr - xj, a2))
    fan_lift = np.zeros(h.shape, np.float32)
    kept_fans = []
    for yj, xj, dy, dx, a2 in fans:
        cy, cx = (yj + 0.5) * sk, (xj + 0.5) * sk
        if any((cy - qy) ** 2 + (cx - qx) ** 2 < (ws["fan_gap_m"] / cell) ** 2 for qy, qx in kept_fans):
            continue
        kept_fans.append((cy, cx))
        rad = float(np.clip(ws["fan_per_sqrt_km2_m"] * math.sqrt(a2), *ws["fan_r_m"])) / cell
        tall = gf.uniform(*ws["fan_h_m"])
        w = int(rad) + 2
        y0_, y1_, x0_, x1_ = max(int(cy) - w, 0), min(int(cy) + w + 1, h.shape[0]), max(int(cx) - w, 0), min(int(cx) + w + 1, h.shape[1])
        yy_, xx_ = np.mgrid[y0_:y1_, x0_:x1_].astype(np.float32)
        vy, vx = yy_ - cy, xx_ - cx
        dn = np.hypot(vy, vx)
        cosang = (vy * dy + vx * dx) / np.maximum(dn * math.hypot(dy, dx), 1e-6)
        prof = tall * np.clip(1.0 - dn / rad, 0.0, 1.0) ** 1.5 * smoothstep(-0.35, 0.35, cosang)
        np.maximum(fan_lift[y0_:y1_, x0_:x1_], prof, out=fan_lift[y0_:y1_, x0_:x1_])
    new = new + fan_lift * (1.0 - up)
    # The washes on the fine grid: bent like the valleys, shallow, wider and
    # deeper downstream.
    wm = np.zeros(h.shape, bool)
    sub = wm[sk // 2::sk, sk // 2::sk]
    sub[: wash.shape[0], : wash.shape[1]] = wash[: sub.shape[0], : sub.shape[1]]
    wd, (wy_, wx_) = nd.distance_transform_edt(~wm, return_indices=True)
    wd = wd.astype(np.float32) * cell + (noise(h.shape, 250 / cell, rng, 2, 3) - 0.5) * 2.0 * ws["bend_m"]
    wa = acc2.reshape(nyc, nxc)[np.clip(wy_ // sk, 0, nyc - 1), np.clip(wx_ // sk, 0, nxc - 1)]
    del wy_, wx_
    wwid = np.clip(ws["width_per_sqrt_km2_m"] * np.sqrt(wa), *ws["width_m"]) * 0.5
    wdep = np.clip(ws["depth_m"] + ws["depth_per_doubling_m"] * np.log2(np.maximum(wa / ws["min_km2"], 1.0)), 0, ws["max_depth_m"])
    cut = wdep * (1.0 - smoothstep(0.0, wwid + cell, wd)) * (1.0 - up) * wm.any()
    new = new - cut
    washed = (cut > 0.8) | (fan_lift > 1.5)

    # --- marks for the ground (steppe_ground.py) and the numbers ---------
    # The burnt grass only in the hearts of the biggest islands.
    heart = (c_ == 1) & (area[lab_] >= ic["heart_km2"] * 1e6) & (din >= np.maximum(ic["heart_m"], ic["heart_share"] * Ri[lab_]))
    heart &= ~bench & region
    marks = {}
    for name_, m_ in (("heart", heart), ("wash", washed)):
        blk = np.zeros(((b1 - b0) * k, (c1 - c0) * k), np.float32)
        hh_, ww_ = min(blk.shape[0], m_.shape[0]), min(blk.shape[1], m_.shape[1])
        blk[:hh_, :ww_] = m_[:hh_, :ww_]
        frac = blk.reshape(b1 - b0, k, c1 - c0, k).mean(axis=(1, 3))
        full = np.zeros(biomes.shape, np.uint8)
        full[b0:b1, c0:c1] = frac >= ws["mark_share"] if name_ == "wash" else frac >= 0.5
        marks[name_] = full[::-1]        # back to the biome picture's rows (north first)
    np.savez_compressed(args.world + "_steppe_marks.npz", **marks)
    sites = peaks
    inner = weight > 0.99
    gy2, gx2 = np.gradient(new, cell)
    sl2 = np.hypot(gx2, gy2)
    del gy2, gx2
    # A gentle rise: ground between 0.2 and 0.7 off the washes, fans and the
    # walls' own rounded edges (a cell either side of a sheer step).
    steep_near = nd.binary_dilation(sl2 > 0.7, iterations=1)
    gentle = inner & (sl2 > 0.2) & (sl2 < 0.7) & ~steep_near & ~washed
    flr = low_all & inner
    flab, fn = nd.label(flr)
    fsz = np.bincount(flab.ravel())[1:] if fn else np.array([0])
    in_region = np.zeros(nis + 1, bool)
    in_region[np.unique(lab_[isl & inner])] = True
    col_ids = [i for i in range(1, nis + 1) if cls[i] == 2 and in_region[i]]
    placed = [{"x": round((x0 + cx_ + 0.5) * cell, 1), "y": round((y0 + cy_ + 0.5) * cell, 1),
               "radius_m": round(float(Ri[i]), 1), "rise_m": round(float(Hm[i] * rise[i]), 1)}
              for i, (cy_, cx_) in zip(col_ids, nd.center_of_mass(isl, lab_, col_ids))] if col_ids else []
    counts = {name_: int(((cls == ci) & in_region).sum()) for ci, name_ in ((1, "mesas"), (2, "columns"), (3, "peaks"), (4, "rubble"))}
    stats = {"islands": counts, "floor_peaks": len(peaks), "fans": len(kept_fans),
             "wash_km": round(float(wm.sum()) * cell * sk / 1000, 1),
             "heart_km2": round(float(heart.sum()) * cell * cell / 1e6, 2),
             "island_share": round(float(isl[inner].mean()), 3),
             "walls_share": round(float((sl2[inner] > 0.7).mean()), 3),
             "gentle_share": round(float(gentle.sum() / max(inner.sum(), 1)), 3),
             "floor_connected": round(float(fsz.max() / max(fsz.sum(), 1)), 3)}
    stats["ok"] = bool(stats["gentle_share"] <= r["checks"]["gentle_max"] and stats["floor_connected"] >= r["checks"]["floor_connected_min"])
    print("steppe relief checks: " + json.dumps(stats))
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

    Path(args.world + "_steppe_relief.json").write_text(json.dumps({"buttes": placed, "pinnacles": sites, "checks": stats}, indent=1))
    moved = np.abs(out - h)[weight > 0.5]
    gy, gx = np.gradient(out, cell)
    sl = np.hypot(gx, gy)[weight > 0.99]
    print(f"steppe relief: box {y1 - y0}x{x1 - x0} cells, span median {np.median(span[weight > 0.5]):.0f} m, "
          f"moved median {np.median(moved):.0f} m (95% {np.percentile(moved, 95):.0f} m), "
          f"walls (slope > 0.7) {np.mean(sl > 0.7) * 100:.0f}%, flat (< 0.1) {np.mean(sl < 0.1) * 100:.0f}%, "
          f"columns {len(placed)}, floor peaks {len(sites)}, channels {int(chan.sum())} cells, marsh base synced {synced}")


if __name__ == "__main__":
    main()
