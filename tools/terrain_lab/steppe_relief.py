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
    # The dry bed itself: a shallow wash down the valley's middle.
    floor = floor - r["wash_m"] * (1.0 - smoothstep(0.0, rv["wash_width_m"], dist)) * valley
    # The tops: the smoothed land at least `min_drop_m` over the floor, cut
    # into flat tiers; the levels drift slowly so a step is not a contour.
    top = np.maximum(nd.gaussian_filter(s, r["top_smooth_m"] / cell), floor + r["min_drop_m"])
    drift = (noise(h.shape, r["drift_km"] * 1000 / cell, rng, 2) - 0.5) * r["drift"]
    qt = (top - floor) / r["tier_m"] + drift
    step = np.floor(qt) + smoothstep(1.0 - r["riser"], 1.0, qt - np.floor(qt))
    top = floor + np.maximum((step - drift) * r["tier_m"], r["min_drop_m"])
    roll = (noise(h.shape, r["roll_km"] * 1000 / cell, rng, 3) - 0.5) * r["roll_m"]
    top = top + roll
    # Sheer walls: across `wall_m`, by the signed distance to the canyon's edge;
    # a bank of scree at the foot.
    sd = (nd.distance_transform_edt(~canyon) - nd.distance_transform_edt(canyon)) * cell
    up = smoothstep(-r["wall_m"] * 0.5, r["wall_m"] * 0.5, sd)
    scree = r["scree_m"] * np.exp(-np.maximum(-sd, 0.0) / r["scree_reach_m"]) * (sd < 0)
    new = floor + scree + (top - floor - scree) * up

    # --- remnants: what the rivers left of their banks -------------------
    # Where the walls retreated round a valley - the ends of the spurs between
    # two rivers, the bays at the mouths of side valleys - pieces of the bank
    # stood on, weathered and turned to stone (IMG_9830, IMG_9825): a point of
    # the floor near a wall with high ground on much of its round.
    rm = r["remnants"]
    regc = region[::sk, ::sk][:ny, :nx]
    cv = canyon[::sk, ::sk][:ny, :nx]
    topfrac = nd.uniform_filter((~cv).astype(np.float32), max(3, int(rm["look_m"] / cell_c) | 1))
    dwall = nd.distance_transform_edt(cv) * cell_c
    dchan = nd.distance_transform_edt(~chan) * cell_c
    cand = cv & regc & (dwall >= rm["wall_m"][0]) & (dwall <= rm["wall_m"][1])
    cand &= (topfrac >= rm["top_share"][0]) & (topfrac <= rm["top_share"][1]) & (dchan > rm["off_channel_m"])
    ys_, xs_ = np.nonzero(cand)
    score = topfrac[ys_, xs_] + 0.15 * rng.random(len(ys_))
    keep_n = int(rm["per_km2"] * (cv & regc).sum() * cell_c * cell_c / 1e6)
    g = np.random.default_rng(args.seed * 104729 + 17)
    taken = []
    buttes = 0
    placed, sites = [], []
    gap = rm["min_gap_m"]
    for k_ in np.argsort(-score):
        if len(taken) >= keep_n:
            break
        yc, xc = ys_[k_], xs_[k_]
        px, py = (xc + 0.5) * cell_c, (yc + 0.5) * cell_c
        if any((px - qx) ** 2 + (py - qy) ** 2 < gap * gap for qx, qy in taken):
            continue
        taken.append((px, py))
        y, x = min(int(py / cell), h.shape[0] - 1), min(int(px / cell), h.shape[1] - 1)
        height_here = float(top[y, x] - floor[y, x])
        if dwall[yc, xc] >= rm["butte_wall_m"] and g.random() < rm["butte_share"]:
            # A butte: the bank's piece as a column of the land, cliff-skinned.
            bt = r["buttes"]
            rad = min(g.uniform(*bt["radius_m"]), dwall[yc, xc] * 0.7) / cell
            rise = max(height_here * g.uniform(*bt["rise"]), bt["min_rise_m"])
            w = int(rad * 1.6) + 4
            y0_, y1_, x0_, x1_ = max(y - w, 0), min(y + w, h.shape[0]), max(x - w, 0), min(x + w, h.shape[1])
            yy_, xx_ = np.mgrid[y0_:y1_, x0_:x1_]
            ang = np.arctan2(yy_ - y, xx_ - x)
            lob = 1.0
            for k2 in (2, 3, 5):
                lob = lob + g.uniform(0.04, 0.14) * np.sin(k2 * ang + g.uniform(0, math.tau))
            d = np.hypot(yy_ - y, xx_ - x) / (rad * lob)
            edge = r["wall_m"] / cell / rad
            body = 1.0 - smoothstep(1.0 - edge, 1.0, d)
            prof = body * rise
            if g.random() < bt["stepped"]:
                k3 = g.uniform(0.55, 0.8)
                upper = 1.0 - smoothstep(k3 - edge, k3, d)
                share = g.uniform(0.6, 0.8)
                prof = body * rise * share + upper * rise * (1.0 - share)
            win_ = new[y0_:y1_, x0_:x1_]
            np.maximum(win_, floor[y0_:y1_, x0_:x1_] + prof, out=win_)
            buttes += 1
            placed.append({"x": round((x0 + x + 0.5) * cell, 1), "y": round((y0 + y + 0.5) * cell, 1),
                           "radius_m": round(rad * cell, 1), "rise_m": round(float(rise), 1)})
        else:
            sites.append({"x": round((x0 + x + 0.5) * cell, 1), "y": round((y0 + y + 0.5) * cell, 1),
                          "floor_m": round(float(floor[y, x]), 1),
                          "top_m": round(float(floor[y, x] + height_here * g.uniform(*rm["pinnacle_rise"])), 1),
                          "room_m": round(float(dwall[yc, xc]), 1)})
    # The pinnacle sites checked on the finished ground: room to the nearest
    # steep ground (walls, scree, a butte's foot) at 16 m, too little dropped.
    gyf, gxf = np.gradient(new, cell)
    clear = nd.distance_transform_edt(np.hypot(gxf, gyf) <= rm["site_slope"]) * cell
    del gyf, gxf
    kept = []
    for q in sites:
        y, x = int(q["y"] / cell) - y0, int(q["x"] / cell) - x0
        room = float(clear[y, x])
        if room >= rm["min_room_m"]:
            q["room_m"] = round(room, 1)
            q["floor_m"] = round(float(new[y, x]), 1)
            kept.append(q)
    if args.stats:
        rooms_ = [float(clear[int(q["y"] / cell) - y0, int(q["x"] / cell) - x0]) for q in sites]
        sl_ = np.hypot(*np.gradient(new, cell))
        print("site rooms", np.percentile(rooms_, [10, 50, 90]).round() if rooms_ else None,
              "floor steep share", round(float((sl_[canyon & region] > rm["site_slope"]).mean()), 3),
              "floor-only steep", round(float((sl_[canyon & region & (sd < -150)] > rm["site_slope"]).mean()), 3))
    sites = kept
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

    Path(args.world + "_steppe_relief.json").write_text(json.dumps({"buttes": placed, "pinnacles": sites}, indent=1))
    moved = np.abs(out - h)[weight > 0.5]
    gy, gx = np.gradient(out, cell)
    sl = np.hypot(gx, gy)[weight > 0.99]
    print(f"steppe relief: box {y1 - y0}x{x1 - x0} cells, span median {np.median(span[weight > 0.5]):.0f} m, "
          f"moved median {np.median(moved):.0f} m (95% {np.percentile(moved, 95):.0f} m), "
          f"walls (slope > 0.7) {np.mean(sl > 0.7) * 100:.0f}%, flat (< 0.1) {np.mean(sl < 0.1) * 100:.0f}%, "
          f"buttes {buttes}, pinnacle sites {len(sites)}, channels {int(chan.sum())} cells, marsh base synced {synced}")


if __name__ == "__main__":
    main()
