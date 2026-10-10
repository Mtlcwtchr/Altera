"""world_landmarks - the one-of-a-kind and composed mega structures of
doc/BIOMES_RU.md, placed by world_features.py: natural bridges across the
canyons rim to rim, chains of buried desert towers joined by walls, the fallen
white rotunda and its stair, the two sentinel peaks at the way into the black
lands, the spiral tower of the cliff coast with its broken bridge floating.

Each function takes the world, its rules (content/config/terrain_core/props.json),
the model catalogue, a random generator and `add(kind, name, x, y, yaw, scale,
**extra)`, and adds instances through it.
"""
import math

import numpy as np
from scipy import ndimage


def _names(catalogue, kind):
    names = sorted(n for n, m in catalogue.items() if m["kind"] == kind)
    if not names:
        raise SystemExit(f"no models of kind {kind}: run tools/blender/terrain_props.py --all")
    return names


def _size(m):
    return max(m["hi"][i] - m["lo"][i] for i in range(3))


def canyon_bridges(world, rules, catalogue, rng, add):
    """Narrow natural bridges across the canyons, rim to rim: from a point on
    a canyon floor, look both ways along a line for the first high ground; if
    both rims stand about as high and not too far apart, a bridge joins them
    just under the lower rim."""
    r = rules["canyon_bridges"]
    canyon = np.isin(world.zones, r["zones"])
    h = world.h
    c = world.size / world.n
    names = _names(catalogue, r["kind"])
    ys, xs = np.nonzero(canyon)
    if not len(xs):
        return 0
    made, tries = [], 0
    while len(made) < r["count"] and tries < r["count"] * 400:
        tries += 1
        k = rng.integers(len(xs))
        x = (xs[k] + rng.random()) * world.cell
        y = (ys[k] + rng.random()) * world.cell
        floor = world.height(x, y)
        a = rng.uniform(0, math.pi)
        d = np.array([math.cos(a), math.sin(a)])
        rims = []
        for sgn in (1, -1):
            hit = None
            for step in range(1, int(r["max_gap_m"] / c)):
                px, py = x + sgn * d[0] * step * c, y + sgn * d[1] * step * c
                hh = world.height(px, py)
                if hh - floor > r["min_depth_m"]:
                    hit = (px, py, hh, step * c)
                    break
            rims.append(hit)
        if None in rims:
            continue
        (x0, y0, h0, d0), (x1, y1, h1, d1) = rims
        gap = d0 + d1
        if gap < r["min_gap_m"] or abs(h0 - h1) > r["rim_diff"] * (min(h0, h1) - floor):
            continue
        # The rims a little further in are what the bridge rests on: still high?
        if min(world.height(x0 + d[0] * 60, y0 + d[1] * 60), world.height(x1 - d[0] * 60, y1 - d[1] * 60)) < min(h0, h1) - 30:
            continue
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        if any((mx - p[0]) ** 2 + (my - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        made.append((mx, my))
        th = rng.uniform(*r["thickness"])
        # Its ends reach well into the rims: span plus a fifth either side.
        add(r["kind"], names[rng.integers(len(names))], mx, my, math.atan2(y1 - y0, x1 - x0),
            ((gap * 1.4) / 1000.0, th, th * math.sqrt(gap / 1000.0)), z=round(float(min(h0, h1) - rng.uniform(5, 25)), 1))
    return len(made)


def ruin_chains(world, rules, catalogue, rng, add):
    """Buried towers in the dunes, far from one another but some in chains,
    joined by walls on arches (doc/BIOMES_RU.md, 04)."""
    r = rules["ruin_chains"]
    zone = world.zones == r["zone"]
    deep = ndimage.distance_transform_edt(zone) * world.cell > r["edge_m"]
    ys, xs = np.nonzero(deep)
    if not len(xs):
        return 0
    towers, walls = _names(catalogue, r["tower"]), _names(catalogue, r["wall"])
    count = 0
    for _ in range(r["chains"]):
        k = rng.integers(len(xs))
        x, y = (xs[k] + 0.5) * world.cell, (ys[k] + 0.5) * world.cell
        heading = rng.uniform(0, math.tau)
        prev = None
        for t in range(int(rng.integers(r["length"][0], r["length"][1] + 1))):
            i, j = int(y // world.cell), int(x // world.cell)
            if not (0 <= i < zone.shape[0] and 0 <= j < zone.shape[1]) or not zone[i, j]:
                break
            name = towers[rng.integers(len(towers))]
            m = catalogue[name]
            tall = m["hi"][2]
            s = rng.uniform(*r["tower_m"]) / tall
            sink = rng.uniform(*r["buried"]) * tall * s
            add(r["tower"], name, x, y, rng.uniform(0, math.tau), (s, s, s), sink=round(sink, 1))
            count += 1
            if prev is not None and rng.random() < r["walls"]:
                px, py = prev
                gap = math.hypot(x - px, y - py)
                ws = rng.uniform(0.9, 1.3)
                add(r["wall"], walls[rng.integers(len(walls))], (x + px) / 2, (y + py) / 2, math.atan2(y - py, x - px),
                    (gap / 1000.0, ws, ws), sink=round(rng.uniform(*r["wall_buried"]) * 70 * ws, 1))
            prev = (x, y)
            heading += rng.uniform(-0.5, 0.5)
            step = rng.uniform(*r["step_m"])
            x, y = x + math.cos(heading) * step, y + math.sin(heading) * step
    return count


def rotunda(world, rules, catalogue, rng, add):
    """The white round building lying on its side in a hollow of the plateau,
    half hidden by the hills round it, a long stair leading up to it."""
    r = rules["rotunda"]
    zone = world.zones == r["zone"]
    deep = ndimage.distance_transform_edt(zone) * world.cell > r["edge_m"]
    if not deep.any():
        return 0
    # A hollow: lowest against the hills round it.
    around = ndimage.uniform_filter(world.hz, 15)
    score = np.where(deep, around - world.hz, -1e9)
    i, j = np.unravel_index(np.argmax(score), score.shape)
    x, y = (j + 0.5) * world.cell, (i + 0.5) * world.cell
    name = _names(catalogue, r["kind"])[0]
    m = catalogue[name]
    s = rng.uniform(*r["size_m"]) / _size(m)
    yaw = rng.uniform(0, math.tau)
    add(r["kind"], name, x, y, yaw, (s, s, s), sink=round(0.15 * m["hi"][2] * s, 1))
    # The stair: in front of its open face (+y of the model), climbing towards it.
    stair = _names(catalogue, r["stair"])[0]
    sm = catalogue[stair]
    ss = rng.uniform(*r["stair_m"]) / _size(sm)
    dx, dy = -math.sin(yaw), math.cos(yaw)
    d = 0.5 * m["hi"][1] * s + 0.5 * _size(sm) * ss + 40
    add(r["stair"], stair, x + dx * d, y + dy * d, yaw + math.pi, (ss, ss, ss))
    return 2


def sentinels(world, rules, catalogue, rng, add):
    """Two natural peaks guard the way into the black lands: where the black
    lands meet the valley nearest its middle, a pair either side of the way."""
    r = rules["sentinels"]
    black = world.zones == r["zone"]
    valley = world.zones == r["from"]
    if not black.any() or not valley.any():
        return 0
    edge = black & ndimage.binary_dilation(valley, iterations=2)
    ys, xs = np.nonzero(edge)
    vy, vx = np.nonzero(valley)
    cy, cx = vy.mean(), vx.mean()
    k = np.argmin((ys - cy) ** 2 + (xs - cx) ** 2)
    x, y = (xs[k] + 0.5) * world.cell, (ys[k] + 0.5) * world.cell
    # Into the black lands, away from the valley.
    inward = np.array([x - (cx + 0.5) * world.cell, y - (cy + 0.5) * world.cell])
    inward /= np.linalg.norm(inward) + 1e-9
    side = np.array([-inward[1], inward[0]])
    names = _names(catalogue, r["kind"])
    for q, sgn in enumerate((-1, 1)):
        px = x + inward[0] * r["inside_m"] + side[0] * sgn * r["gap_m"] / 2
        py = y + inward[1] * r["inside_m"] + side[1] * sgn * r["gap_m"] / 2
        m = catalogue[names[q % len(names)]]
        s = rng.uniform(*r["height_m"]) / m["hi"][2]
        add(r["kind"], names[q % len(names)], px, py, math.atan2(-side[1] * sgn, -side[0] * sgn), (s, s, s))
    return 2


def spiral_isle(world, rules, catalogue, rng, add):
    """The spiral tower at the cliff coast's farthest point, and the pieces of
    its broken bridge floating out over the sea in a line."""
    r = rules["spiral_isle"]
    zone = world.zones == r["zone"]
    if not zone.any():
        return 0
    land = world.hz > 1.0
    sea_dist = ndimage.distance_transform_edt(land) * world.cell
    score = np.where(zone, -sea_dist, -1e9)
    i, j = np.unravel_index(np.argmax(score), score.shape)
    x, y = (j + 0.5) * world.cell, (i + 0.5) * world.cell
    # Towards the open sea: away from the land's mass round it.
    win = 40
    sub = land[max(i - win, 0):i + win, max(j - win, 0):j + win]
    ly, lx = np.nonzero(sub)
    out = np.array([j - (lx.mean() + max(j - win, 0)), i - (ly.mean() + max(i - win, 0))])
    out /= np.linalg.norm(out) + 1e-9
    name = _names(catalogue, r["tower"])[0]
    m = catalogue[name]
    s = rng.uniform(*r["height_m"]) / m["hi"][2]
    ground = world.height(x, y)
    add(r["tower"], name, x, y, rng.uniform(0, math.tau), (s, s, s))
    pieces = _names(catalogue, r["piece"])
    top = ground + m["hi"][2] * s
    for k in range(r["pieces"]):
        t = (k + 1) / (r["pieces"] + 1)
        d = r["reach_m"] * t
        px = x + out[0] * d + rng.uniform(-60, 60)
        py = y + out[1] * d + rng.uniform(-60, 60)
        z = top * rng.uniform(0.35, 0.55) + rng.uniform(-40, 40)
        ps = rng.uniform(0.8, 1.3)
        add(r["piece"], pieces[rng.integers(len(pieces))], px, py, math.atan2(out[1], out[0]) + rng.uniform(-0.4, 0.4),
            (ps, ps, ps), z=round(float(z), 1))
    return 1 + r["pieces"]


def _coast(world, zones):
    """Sea cells next to the land of `zones`, and how far each sea cell is
    from that land (metres)."""
    land = world.hz > 1.0
    mine = np.isin(world.zones, zones) & land
    dist = ndimage.distance_transform_edt(~mine) * world.cell
    return ~land, dist


def floating_isles(world, rules, catalogue, rng, add):
    """Floating rock islands out over the sea off the forest cliff
    (doc/BIOMES_RU.md, 05), their grassy tops high above the water."""
    r = rules["floating_isles"]
    sea, dist = _coast(world, r["zones"])
    ok = sea & (dist > r["off_m"][0]) & (dist < r["off_m"][1])
    ys, xs = np.nonzero(ok)
    if not len(xs):
        return 0
    names = _names(catalogue, r["kind"])
    pts = np.stack([(xs + rng.random(len(xs))) * world.cell, (ys + rng.random(len(xs))) * world.cell], 1)
    order = rng.permutation(len(pts))
    made = []
    for k in order:
        x, y = pts[k]
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        made.append((x, y))
        name = names[rng.integers(len(names))]
        m = catalogue[name]
        s = rng.uniform(*r["size_m"]) / _size(m)
        add(r["kind"], name, x, y, rng.uniform(0, math.tau), (s, s, s), z=round(float(rng.uniform(*r["top_m"])), 1))
        if len(made) >= r["count"]:
            break
    return len(made)


def sea_stacks(world, rules, catalogue, rng, add):
    """Sea stacks off the cliff coasts: columns the sea has cut from the land,
    standing in the shallows a little way out, often in small groups."""
    r = rules["sea_stacks"]
    sea, dist = _coast(world, r["zones"])
    ok = sea & (dist > r["off_m"][0]) & (dist < r["off_m"][1]) & (world.hz > -r["max_depth_m"])
    ys, xs = np.nonzero(ok)
    if not len(xs):
        return 0
    names = _names(catalogue, r["kind"])
    made = []
    order = rng.permutation(len(xs))
    for k in order:
        cx, cy = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
        if any((cx - p[0]) ** 2 + (cy - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        made.append((cx, cy))
        for q in range(int(rng.integers(r["group"][0], r["group"][1] + 1))):
            x = cx + rng.uniform(-1, 1) * r["group_m"]
            y = cy + rng.uniform(-1, 1) * r["group_m"]
            name = names[rng.integers(len(names))]
            m = catalogue[name]
            # Out of the sea, not the sea floor: the lab stretches it from
            # the floor up to `top` above the water.
            top = rng.uniform(*r["height_m"])
            depth = max(0.0, -world.height(x, y))
            s = (top + depth) / m["hi"][2]
            add(r["kind"], name, x, y, rng.uniform(0, math.tau), (s, s, s), top=round(top, 1))
        if len(made) >= r["count"]:
            break
    return len(made)


def rim_ledges(world, rules, catalogue, rng, add):
    """Ledges of hard rock overhanging the canyon rims (doc/BIOMES_RU.md, 03):
    on high ground a step from a steep drop, reaching out over it."""
    r = rules["rim_ledges"]
    zone = np.isin(world.zones, r["zones"])
    gy, gx = np.gradient(world.hz, world.cell)
    slope = np.hypot(gx, gy)
    steep = slope > r["drop_slope"]
    near = ndimage.distance_transform_edt(~steep) * world.cell
    top = ndimage.maximum_filter(world.hz, 5)
    ok = zone & ~steep & (near < r["rim_m"]) & (world.hz > top - r["below_top_m"])
    ys, xs = np.nonzero(ok)
    if not len(xs):
        return 0
    # Which way is out: towards the nearest steep cell's downhill.
    _, (si, sj) = ndimage.distance_transform_edt(~steep, return_indices=True)
    names = _names(catalogue, r["kind"])
    made = []
    for k in rng.permutation(len(xs)):
        i, j = ys[k], xs[k]
        x, y = (j + 0.5) * world.cell, (i + 0.5) * world.cell
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        a, b = si[i, j], sj[i, j]
        out = np.array([-gx[a, b], -gy[a, b]])
        if np.linalg.norm(out) < 1e-6:
            continue
        made.append((x, y))
        name = names[rng.integers(len(names))]
        m = catalogue[name]
        s = rng.uniform(*r["size_m"]) / _size(m)
        add(r["kind"], name, x, y, math.atan2(out[1], out[0]), (s, s, s), z=round(float(world.hz[i, j]) + 2.0, 1))
        if len(made) >= r["count"]:
            break
    return len(made)


def clefts(world, rules, catalogue, rng, add):
    """Deep clefts of the black lands (doc/BIOMES_RU.md, 10): two or three
    jagged blades side by side, the dark gap between them the cleft."""
    r = rules["clefts"]
    zone = np.isin(world.zones, r["zones"])
    deep = ndimage.distance_transform_edt(zone) * world.cell > r["edge_m"]
    ys, xs = np.nonzero(deep & (world.hz > 1.0))
    if not len(xs):
        return 0
    area = zone.sum() * (world.cell / 1000) ** 2
    count = int(area / 1000 * r["per_1000km2"])
    names = _names(catalogue, r["kind"])
    made = []
    for k in rng.permutation(len(xs)):
        cx, cy = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
        if any((cx - p[0]) ** 2 + (cy - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        made.append((cx, cy))
        yaw = rng.uniform(0, math.pi)
        side = np.array([-math.sin(yaw), math.cos(yaw)])
        n = int(rng.integers(2, 4))
        gap = rng.uniform(*r["gap_m"])
        for q in range(n):
            off = (q - (n - 1) / 2) * gap
            name = names[rng.integers(len(names))]
            m = catalogue[name]
            s = rng.uniform(*r["size_m"]) / _size(m)
            add(r["kind"], name, cx + side[0] * off + rng.uniform(-30, 30), cy + side[1] * off + rng.uniform(-30, 30),
                yaw + rng.uniform(-0.12, 0.12), (s, s * rng.uniform(0.8, 1.2), s * rng.uniform(0.85, 1.25)))
        if len(made) >= count:
            break
    return len(made)


def place(world, rules, catalogue, rng, add):
    counts = {}
    for name, fn in (("canyon_bridges", canyon_bridges), ("ruin_chains", ruin_chains), ("rotunda", rotunda),
                     ("sentinels", sentinels), ("spiral_isle", spiral_isle), ("floating_isles", floating_isles),
                     ("sea_stacks", sea_stacks), ("rim_ledges", rim_ledges), ("clefts", clefts)):
        if name in rules:
            counts[name] = fn(world, rules, catalogue, rng, add)
    return counts
