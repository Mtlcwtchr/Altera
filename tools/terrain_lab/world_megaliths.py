"""world_megaliths - the megaliths (doc/rework/MESH_TERRAIN_PLAN.md): strange,
surreal standing things that belong to one biome each and do not repeat
elsewhere (doc/BIOMES_RU.md; the user's list of 2026-10-11):

- the white mountains: the broken ring;
- the great marshes: a row of towering rectangular prisms along the border
  with the dark forest, and the small standing panels;
- the green shore at the desert's edge: a cluster of huge white cubes;
- the plain: the remains of giants - heads, hands, a spine, fallen iron
  defenders;
- the golden desert: one mirroring prism, the rest giant remains - spines,
  ribcages, hands, iron defenders;
- the black coast: grey spikes all along it;
- the river valley: one towering straight grey spire (a tower will stand on it).
The isles and the crescent have none.

Placed by world_features.py from the "megaliths" section of
content/config/terrain_core/props.json. Sizes of the great ones follow their
biome (a share of its extent, the root of its area), so each is seen from
anywhere in it whatever the map's size.
"""
import math

import numpy as np
from scipy import ndimage


def _names(catalogue, kind):
    names = sorted(n for n, m in catalogue.items() if m["kind"] == kind)
    if not names:
        raise SystemExit(f"no models of kind {kind}: run tools/blender/terrain_props.py --all")
    return names


def _extent(world, mask):
    return math.sqrt(mask.sum()) * world.cell


def heroes(world, rules, catalogue, rng, add):
    """One kind per biome, `count` of it, height a `share` of the biome's extent."""
    r = rules["heroes"]
    gy, gx = np.gradient(world.hz, world.cell)
    flat = np.hypot(gx, gy) < r["max_slope"]
    made = 0
    for zone, spec in r["zones"].items():
        inside = world.zones == zone
        if not inside.any():
            continue
        extent = _extent(world, inside)
        height = float(np.clip(spec.get("share", r["share"]) * extent, *spec.get("height_m", r["height_m"])))
        depth = ndimage.distance_transform_edt(inside) * world.cell
        ok = inside & flat & (world.hz > 1.0) & (depth > min(depth.max() * 0.5, r["edge_share"] * extent))
        ys, xs = np.nonzero(ok)
        if not len(xs):
            continue
        names = _names(catalogue, spec["kind"])
        placed = []
        for k in rng.permutation(len(xs)):
            x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
            if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < (r["spacing_share"] * extent) ** 2 for p in placed):
                continue
            placed.append((x, y))
            name = names[int(rng.integers(len(names)))]
            m = catalogue[name]
            s = height * rng.uniform(0.9, 1.1) / max(m["hi"][2], 1e-3)
            add(spec["kind"], name, x, y, rng.uniform(0, math.tau), (s, s, s), sink=round(0.03 * height, 1))
            made += 1
            if len(placed) >= spec.get("count", 1):
                break
    return made


def _border(world, a, b, depth_m):
    """Cells of zone `a` within `depth_m` of zone `b`."""
    near_b = ndimage.distance_transform_edt(world.zones != b) * world.cell < depth_m
    return (world.zones == a) & near_b & (world.hz > 0.5)


def prism_row(world, rules, catalogue, rng, add):
    """A row of towering prisms standing along a border (the marshes against
    the dark forest): the border's longest stretch, prisms spaced along it."""
    r = rules["prism_row"]
    edge = _border(world, r["zone"], r["along"], r["depth_m"])
    lab, n = ndimage.label(edge)
    if not n:
        return 0
    sizes = ndimage.sum(edge, lab, range(1, n + 1))
    piece = lab == 1 + int(np.argmax(sizes))
    ys, xs = np.nonzero(piece)
    pts = np.stack([(xs + 0.5) * world.cell, (ys + 0.5) * world.cell], 1)
    # Along the border: its main direction, a run of `count` spaced `gap_m`.
    c = pts.mean(0)
    _, _, vt = np.linalg.svd(pts - c, full_matrices=False)
    t = (pts - c) @ vt[0]
    mid = float(np.median(t))
    names = _names(catalogue, r["kind"])
    height = float(np.clip(r["share"] * _extent(world, world.zones == r["zone"]), *r["height_m"]))
    made = 0
    for k in range(r["count"]):
        want = mid + (k - (r["count"] - 1) / 2) * r["gap_m"]
        i = int(np.argmin(np.abs(t - want)))
        x, y = pts[i]
        name = names[int(rng.integers(len(names)))]
        m = catalogue[name]
        s = height * rng.uniform(0.85, 1.15) / m["hi"][2]
        add(r["kind"], name, x, y, math.atan2(vt[0][1], vt[0][0]) + rng.uniform(-0.1, 0.1), (s, s, s),
            sink=round(0.04 * height, 1))
        made += 1
    return made


def cube_cluster(world, rules, catalogue, rng, add):
    """A tight cluster of huge white cubes on the green shore where it meets
    the desert."""
    r = rules["cube_cluster"]
    edge = _border(world, r["zone"], r["near"], r["depth_m"])
    ys, xs = np.nonzero(edge)
    if not len(xs):
        ys, xs = np.nonzero((world.zones == r["zone"]) & (world.hz > 1.0))
        if not len(xs):
            return 0
    k = int(rng.integers(len(xs)))
    cx, cy = (xs[k] + 0.5) * world.cell, (ys[k] + 0.5) * world.cell
    names = _names(catalogue, r["kind"])
    n = int(rng.integers(r["count"][0], r["count"][1] + 1))
    made = 0
    for q in range(n):
        a = rng.uniform(0, math.tau)
        d = r["cluster_m"] * math.sqrt(rng.random())
        name = names[int(rng.integers(len(names)))]
        m = catalogue[name]
        s = rng.uniform(*r["size_m"]) / max(m["hi"][i] - m["lo"][i] for i in range(3))
        add(r["kind"], name, cx + math.cos(a) * d, cy + math.sin(a) * d, rng.uniform(0, math.tau), (s, s, s))
        made += 1
    return made


def coast_spikes(world, rules, catalogue, rng, add):
    """Grey spikes stuck all along a coast (the black coast): every few
    hundred metres to a couple of kilometres of its shore, a spike or two."""
    r = rules["coast_spikes"]
    land = world.hz > 1.0
    # The open sea: the water that reaches the map's edge (not a lake or a
    # river bed inland).
    lab, _ = ndimage.label(~land)
    edge = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
    sea = np.isin(lab, edge[edge > 0])
    zone = world.zones == r["zone"]
    shore = zone & land & (ndimage.distance_transform_edt(~sea) * world.cell < r["shore_m"])
    ys, xs = np.nonzero(shore)
    if not len(xs):
        return 0
    names = _names(catalogue, r["kind"])
    made = []
    for k in rng.permutation(len(xs)):
        x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
        gap = rng.uniform(*r["gap_m"])
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < gap * gap for p in made):
            continue
        made.append((x, y))
        if len(made) > r.get("max_sites", 400):
            break
        for q in range(int(rng.integers(1, r["group"] + 1))):
            name = names[int(rng.integers(len(names)))]
            m = catalogue[name]
            s = rng.uniform(*r["height_m"]) / m["hi"][2]
            add(r["kind"], name, x + rng.uniform(-60, 60), y + rng.uniform(-60, 60), rng.uniform(0, math.tau),
                (s, s, s))
    return len(made)


def place(world, rules, catalogue, rng, add):
    import world_features as wf
    counts = {}
    for name, fn in (("heroes", heroes), ("prism_row", prism_row), ("cube_cluster", cube_cluster),
                     ("coast_spikes", coast_spikes)):
        if name in rules:
            counts[name] = fn(world, rules, catalogue, rng, add)
    counts["groups"] = wf.place_groups(world, rules, catalogue, rng, add)
    return counts
