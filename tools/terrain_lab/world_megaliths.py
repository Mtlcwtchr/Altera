"""world_megaliths - the megaliths (doc/rework/MESH_TERRAIN_PLAN.md): strange,
surreal standing things - rings and stakes sticking out of the ground,
Stonehenge-like stones, standing panels, needles, the stone giants. Not
terrain: they stand on it. Placed by world_features.py from the "megaliths"
section of content/config/terrain_core/props.json.

`heroes`: the great ones, a few to a biome, sized by the biome itself - a
share of its extent (the root of its area) - so that each can be seen from
anywhere in it, whatever size the map is.
"""
import math

import numpy as np
from scipy import ndimage


def heroes(world, rules, catalogue, rng, add):
    r = rules["heroes"]
    kinds = {k: sorted(n for n, m in catalogue.items() if m["kind"] == k) for k in r["kinds"]}
    gy, gx = np.gradient(world.hz, world.cell)
    flat = np.hypot(gx, gy) < r["max_slope"]
    made = 0
    for zone, count in r["zones"].items():
        inside = world.zones == zone
        area = inside.sum() * world.cell ** 2
        if not area:
            continue
        extent = math.sqrt(area)
        height = float(np.clip(r["share"] * extent, *r["height_m"]))
        # Deep in the biome, on ground that is not sea, spread apart.
        depth = ndimage.distance_transform_edt(inside) * world.cell
        ok = inside & flat & (world.hz > 1.0) & (depth > min(depth.max() * 0.5, r["edge_share"] * extent))
        ys, xs = np.nonzero(ok)
        if not len(xs):
            continue
        placed = []
        for k in rng.permutation(len(xs)):
            x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
            if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < (r["spacing_share"] * extent) ** 2 for p in placed):
                continue
            placed.append((x, y))
            kind = r["kinds"][int(rng.integers(len(r["kinds"])))]
            name = kinds[kind][int(rng.integers(len(kinds[kind])))]
            m = catalogue[name]
            s = height * rng.uniform(0.8, 1.15) / max(m["hi"][2], 1e-3)
            add(kind, name, x, y, rng.uniform(0, math.tau), (s, s, s), sink=round(0.04 * height, 1))
            made += 1
            if len(placed) >= count:
                break
    return made


def place(world, rules, catalogue, rng, add):
    import world_features as wf
    counts = {}
    if "heroes" in rules:
        counts["heroes"] = heroes(world, rules, catalogue, rng, add)
    counts["groups"] = wf.place_groups(world, rules, catalogue, rng, add)
    return counts
