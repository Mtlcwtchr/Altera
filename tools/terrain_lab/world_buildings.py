"""world_buildings - built structures (doc/BIOMES_RU.md): the desert's buried
towers and walls, the fallen white rotunda, the spiral tower with its broken
bridge, and the built pieces placed by zone. Kept apart from the terrain parts
and the megaliths: buildings are their own piece of work, off by default
("buildings": {"enabled": false} in content/config/terrain_core/props.json).
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


def _coast(world, zones):
    """Sea cells next to the land of `zones`, and how far each sea cell is
    from that land (metres)."""
    land = world.hz > 1.0
    mine = np.isin(world.zones, zones) & land
    dist = ndimage.distance_transform_edt(~mine) * world.cell
    return ~land, dist


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


def place(world, rules, catalogue, rng, add):
    import world_features as wf
    counts = {}
    for name, fn in (("ruin_chains", ruin_chains), ("rotunda", rotunda), ("spiral_isle", spiral_isle)):
        if name in rules:
            counts[name] = fn(world, rules, catalogue, rng, add)
    counts["groups"] = wf.place_groups(world, rules, catalogue, rng, add)
    return counts
