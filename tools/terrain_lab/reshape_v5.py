"""Reshape the v5 heightmap so water can do what it should.

v5 is a set of bowls: each continent's edge stands at ~2000 m and drops to
the sea in cliffs, while the plains inside lie at ~1100 m. Water on them
cannot reach the sea (the main continent's lowest way out is at 2200 m), so
every river ends in an inland lake, and the caldera's rim is worn through
to its floor on one side.

What this does, in order:
  1. the caldera: its ring raised whole again (no breach) and its floor
     shaped into a shallow bowl, so its own water makes a lake in the middle;
  2. outlets: for each big closed basin (the caldera excepted), the cheapest
     way from its floor to the sea is cut as a broad valley through the rim,
     its floor falling evenly from the basin to the sea - the big rivers'
     courses;
  3. mouths: around where each valley meets the sea, a wide low plain the
     river can spread over (deltas);
  4. coasts: along some stretches of coast (a slow noise picks them) the rim
     is laid back into a long slope down to the water; elsewhere the cliffs
     stay.

    python tools/terrain_lab/reshape_v5.py [--in v5.png] [--out v6.png] [--preview p.png]

Heights: value 0..65535 = 0..1, metres = (value - sea) * range, as the lab
reads it (--sea 0.13 --height-range 6000, 524.288 km across). North up, as
the source.
"""
import argparse
import heapq

import numpy as np
from PIL import Image
from scipy import ndimage

SEA, RANGE, WORLD_KM = 0.13, 6000.0, 524.288


def load(path):
    a = np.array(Image.open(path)).astype(np.float64) / 65535.0
    return (a - SEA) * RANGE


def save(path, h):
    v = np.clip(h / RANGE + SEA, 0.0, 1.0)
    Image.fromarray((v * 65535.0 + 0.5).astype(np.uint16)).save(path)


def ocean_of(h):
    """The sea: below sea level and joined to the map's edge."""
    wet, n = ndimage.label(h <= 0)
    edge = set(np.unique(np.concatenate([wet[0], wet[-1], wet[:, 0], wet[:, -1]]))) - {0}
    return np.isin(wet, list(edge))


def priority_flood(h, ocean):
    """Filled heights: each hollow raised to where it spills (Barnes)."""
    n = h.shape[0]
    filled = h.copy()
    done = np.zeros(h.shape, bool)
    heap = []
    seeds = ocean.copy()
    seeds[0, :] = seeds[-1, :] = seeds[:, 0] = seeds[:, -1] = True
    for y, x in zip(*np.nonzero(seeds)):
        done[y, x] = True
        heap.append((filled[y, x], y, x))
    heapq.heapify(heap)
    steps = ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1))
    while heap:
        z, y, x = heapq.heappop(heap)
        for dy, dx in steps:
            yy, xx = y + dy, x + dx
            if 0 <= yy < n and 0 <= xx < n and not done[yy, xx]:
                done[yy, xx] = True
                filled[yy, xx] = max(filled[yy, xx], z)
                heapq.heappush(heap, (filled[yy, xx], yy, xx))
    return filled


def cheapest_way(h, start, ocean, level, blocked, rough):
    """Dijkstra from `start` to the sea: cost to cross ground above `level`.
    `rough` (about 0.5..1.5) makes some ground dearer than other, so the way
    winds as a river's would instead of running straight."""
    n = h.shape[0]
    cost = np.full(h.shape, np.inf)
    back = np.full(h.shape + (2,), -1, np.int32)
    cost[start] = 0
    heap = [(0.0, start[0], start[1])]
    steps = ((1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
             (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414))
    while heap:
        c, y, x = heapq.heappop(heap)
        if c > cost[y, x]:
            continue
        if ocean[y, x]:
            path = [(y, x)]
            while (y, x) != start:
                y, x = back[y, x]
                path.append((y, x))
            return path[::-1]
        for dy, dx, d in steps:
            yy, xx = y + dy, x + dx
            if not (0 <= yy < n and 0 <= xx < n) or blocked[yy, xx]:
                continue
            # Climbing is dear, the length a little: the way through the lowest saddle.
            step = d * rough[yy, xx] * (1.0 + max(0.0, h[yy, xx] - level) * 0.05)
            if c + step < cost[yy, xx]:
                cost[yy, xx] = c + step
                back[yy, xx] = (y, x)
                heapq.heappush(heap, (c + step, yy, xx))
    return None


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def value_noise(shape, cell, seed):
    rng = np.random.default_rng(seed)
    coarse = rng.random((shape[0] // cell + 2, shape[1] // cell + 2))
    return ndimage.zoom(coarse, cell, order=3)[: shape[0], : shape[1]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", default="doc/NewRef/world_heightmap_v5_4096_gray16.png")
    ap.add_argument("--out", default="doc/NewRef/world_heightmap_v6_4096_gray16.png")
    ap.add_argument("--preview", default="")
    ap.add_argument("--caldera", default="0.64,0.38", help="caldera centre, image fractions x,y (north up)")
    args = ap.parse_args()

    h = load(args.src)
    n = h.shape[0]
    km = WORLD_KM / n                     # km a texel
    yy, xx = np.mgrid[0:n, 0:n]
    ocean = ocean_of(h)

    # ---- 1. the caldera -------------------------------------------------------
    cx0, cy0 = (float(v) for v in args.caldera.split(","))
    box = int(0.08 * n)
    y0, x0 = int(cy0 * n), int(cx0 * n)
    win = h[y0 - box:y0 + box, x0 - box:x0 + box]
    floor_cells, _ = ndimage.label(win < 350)
    lowest = np.unravel_index(np.argmin(win), win.shape)
    floor_mask = floor_cells == floor_cells[lowest]
    fy, fx = ndimage.center_of_mass(floor_mask)
    cy, cx = y0 - box + fy, x0 - box + fx
    radius = np.sqrt(floor_mask.sum() / np.pi)           # texels
    r = np.hypot(yy - cy, xx - cx)
    angle = np.arctan2(yy - cy, xx - cx)
    # The ring: as high as the best of the old rim, never lower than it was.
    ring_r = radius * 1.18
    old_ring = h[(r > radius * 1.05) & (r < radius * 1.45)]
    crest = max(np.percentile(old_ring, 85), 1500.0)
    wobble = 1.0 + 0.12 * np.sin(3 * angle + 1.3) + 0.08 * np.sin(7 * angle + 0.4) + 0.05 * np.sin(13 * angle + 2.1)
    width = radius * 0.22
    ring = crest * wobble * np.exp(-((r - ring_r) / width) ** 2)
    outer = r > ring_r
    h = np.where(outer & (r < ring_r + 3 * width), np.maximum(h, ring), h)
    # Inside: the wall from the crest down to the floor, then a shallow bowl.
    floor_level = float(np.percentile(win[floor_mask], 40))
    bowl = floor_level + 60.0 * (r / radius) ** 2 - 25.0
    wall = floor_level + 35.0 + (crest * wobble - floor_level - 35.0) * smoothstep(radius * 0.92, ring_r, r)
    inside = r <= ring_r
    shaped = np.where(r < radius * 0.92, bowl, np.maximum(bowl, wall))
    keep = smoothstep(0, radius * 0.25, radius * 0.92 - r)    # the old floor's own bumps, faded towards the wall
    h = np.where(inside, shaped + np.clip(h - floor_level, -20, 20) * 0.3 * keep, h)
    caldera = r < ring_r + 2 * width
    print(f"caldera: centre ({cx / n:.4f}, {cy / n:.4f}) image, floor radius {radius * km:.1f} km, "
          f"floor {floor_level:.0f} m, crest {crest:.0f} m")

    # ---- 2. outlets ------------------------------------------------------------
    f = 4                                   # work at a quarter
    small = h.reshape(n // f, f, n // f, f).mean(axis=(1, 3))
    ocean_s = ocean_of(small)
    cal_s = caldera.reshape(n // f, f, n // f, f).any(axis=(1, 3))
    mouths = []
    rough = 0.4 + 1.2 * value_noise(small.shape, 24, 3) + 0.4 * value_noise(small.shape, 7, 5)
    for round_ in range(40):
        ocean_s = ocean_of(small)       # a cut may have joined a hollow to the sea
        filled = priority_flood(small, ocean_s)
        depth = filled - small
        # Hollows under sea level are left as they are: inland seas, not
        # basins a river drains.
        basins, count = ndimage.label((depth > 30) & ~cal_s & (filled > 0))
        if count == 0:
            break
        sizes = ndimage.sum(np.ones_like(depth), basins, index=np.arange(1, count + 1))
        best = int(np.argmax(sizes)) + 1
        area_km2 = sizes[best - 1] * (km * f) ** 2
        if area_km2 < 60:
            break
        cells = basins == best
        start = np.unravel_index(np.argmin(np.where(cells, small, np.inf)), small.shape)
        level = small[start]
        path = cheapest_way(small, start, ocean_s, level, cal_s, rough)
        if path is None:
            break
        # The way smoothed (the search walks the grid in straight steps) and
        # given gentle bends, then drawn densely.
        raw_y = ndimage.gaussian_filter1d(np.array([p[0] for p in path], float), 4.0, mode="nearest")
        raw_x = ndimage.gaussian_filter1d(np.array([p[1] for p in path], float), 4.0, mode="nearest")
        dy, dx = np.gradient(raw_y), np.gradient(raw_x)
        norm = np.maximum(np.hypot(dx, dy), 1e-6)
        s_ = np.concatenate([[0], np.cumsum(np.hypot(np.diff(raw_y), np.diff(raw_x)))])
        fade = np.minimum(1.0, np.minimum(s_, s_[-1] - s_) / 6.0)
        bend = 2.5 * np.sin(s_ / 9.0 + round_) * fade
        fy, fx = raw_y + dx / norm * bend, raw_x - dy / norm * bend
        dense = np.linspace(0, len(fy) - 1, len(fy) * 4)
        py = np.clip(np.round(np.interp(dense, np.arange(len(fy)), fy)).astype(int), 0, small.shape[0] - 1)
        px = np.clip(np.round(np.interp(dense, np.arange(len(fx)), fx)).astype(int), 0, small.shape[1] - 1)
        # The valley floor: from the basin's floor down to the sea, evenly.
        along = np.concatenate([[0], np.cumsum(np.hypot(np.diff(py), np.diff(px)))])
        floor_z = level * (1.0 - along / max(along[-1], 1.0)) - 2.0
        line = np.zeros(small.shape, bool)
        line[py, px] = True
        z_on_line = np.full(small.shape, np.inf)
        z_on_line[py, px] = floor_z
        dist, (iy, ix) = ndimage.distance_transform_edt(~line, return_indices=True)
        dist_km = dist * km * f
        valley = z_on_line[iy, ix] + np.maximum(0.0, dist_km - 1.5) * 110.0   # 1.5 km floor, walls ~1:9
        small = np.minimum(small, valley)
        mouths.append((py[-1] * f + f // 2, px[-1] * f + f // 2))
        print(f"outlet {round_ + 1}: basin {area_km2:.0f} km2 at {level:.0f} m, valley {along[-1] * km * f:.0f} km "
              f"to the sea at ({px[-1] / small.shape[1]:.3f}, {py[-1] / small.shape[0]:.3f})")
    # The cut at full resolution: only ever down, and smoothly.
    cut = ndimage.zoom(small, f, order=1) - ndimage.zoom(h.reshape(n // f, f, n // f, f).mean(axis=(1, 3)), f, order=1)
    cut = ndimage.gaussian_filter(np.minimum(cut, 0.0), 1.5)
    h = h + np.where(caldera, 0.0, cut)

    # ---- 3 and 4. mouths and coasts ---------------------------------------------
    ocean = ocean_of(h)
    to_sea_km = ndimage.distance_transform_edt(~ocean) * km
    # Coast stretches laid back: a slow noise picks about half the coast.
    pick = value_noise(h.shape, n // 12, 7)
    gentle = smoothstep(0.45, 0.6, pick)
    for (my, mx) in mouths:              # and every river mouth, widely
        gentle = np.maximum(gentle, smoothstep(45.0, 20.0, np.hypot(yy - my, xx - mx) * km))
    # The slope: 25 m a km near the shore, steepening inland, with the
    # ground's own small relief on it.
    slope = 25.0 * to_sea_km + 0.6 * to_sea_km ** 2
    # Only the bumps on it, never dips: a dip near the shore would go under
    # the sea and leave a string of lagoons along the coast.
    # The old cliff counts as a huge bump: capped, and faded out towards the
    # shore so the slope meets the water at the water's level.
    relief = np.clip(h - ndimage.gaussian_filter(h, 6), 0.0, 40.0) * smoothstep(1.0, 8.0, to_sea_km)
    laid = np.minimum(h, np.maximum(slope + 0.5 * relief, 3.0))
    h = np.where(ocean | caldera, h, h + (laid - h) * gentle)
    # Deltas: right at a mouth, a near-flat fan barely above the sea.
    for (my, mx) in mouths:
        d = np.hypot(yy - my, xx - mx) * km
        fan = smoothstep(18.0, 6.0, d)
        # Flat, a few metres up, with a little unevenness - not a cone, which
        # would draw every stream round it straight to the mouth.
        flat = 3.0 + 0.4 * to_sea_km + 2.0 * (value_noise(h.shape, 24, 11) - 0.5)
        h = np.where(ocean, h, np.minimum(h, h + (flat - h) * fan))

    save(args.out, h)
    print(f"wrote {args.out}")
    if args.preview:
        shade = np.clip((h + 100) / 3000, 0, 1)
        rgb = np.stack([shade] * 3, -1)
        rgb[ocean_of(h)] = (0.1, 0.2, 0.45)
        Image.fromarray((rgb * 255).astype(np.uint8)).resize((1024, 1024)).save(args.preview)


if __name__ == "__main__":
    main()
