"""world_terrain_parts - the large parts of the hybrid terrain that are
meshes, not megaliths (doc/rework/MESH_TERRAIN_PLAN.md): the canyon's spires
with the bridges between them and across the canyons, ledges overhanging the
rims, the clefts of the black lands, sea stacks, floating isles, the sentinel
peaks, and the terrain-sized rock forms placed by zone (hogbacks, tilted
masses, sleeping trolls, the valley spire). The cliff walls are built
separately (world_cliffs.py).

Placed by world_features.py from the "terrain" section of
content/config/terrain_core/props.json. Each function takes the world, that
section, the model catalogue, a random generator and `add(kind, name, x, y,
yaw, scale, **extra)`.
"""
import math

import numpy as np
from scipy import ndimage

from world_features import cells_of, poisson


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


def canyon_spires(world, rules, catalogue, rng, add):
    """The canyon steppe's spires (doc/BIOMES_RU.md, 03): part of the canyon's
    relief, not megaliths - columns left standing on the floors with their
    tops at the level of the steppe islands and the plateau rim round them,
    and natural bridges between spires whose tops are alike."""
    zones = world.zones
    # --- spires: on the canyon floors, tops at the high ground's level ---
    sp = rules["spires"]
    canyon = np.isin(zones, sp["zones"])
    high = np.where(np.isin(zones, sp["top_zones"]), world.hz, -1e4)
    reach = max(3, int(round(sp["reach_km"] * 1000 / world.cell)))
    # The high ground round a point: its upper rim, not a lone peak.
    top = ndimage.maximum_filter(ndimage.uniform_filter(high, 3), size=reach)
    floor = ndimage.minimum_filter(np.where(canyon, world.hz, 1e4), size=max(3, reach // 3))
    rise = top - world.hz
    low = world.hz < floor + sp["floor_share"] * np.maximum(top - floor, 0)
    ok = canyon & low & (rise > sp["min_rise_m"])
    # Clear of the walls: a spire on a canyon's slope or at its rim stands with
    # its foot in the air on the downhill side (and the wall's mesh skin is there).
    gy, gx = np.gradient(world.hz, world.cell)
    steep = np.hypot(gx, gy) > sp["wall_slope"]
    ok &= ndimage.distance_transform_edt(~steep) * world.cell > sp["wall_gap_m"]
    pts, (ys, xs) = cells_of(world, ok, rng)
    area_km2 = canyon.sum() * (world.cell / 1000) ** 2
    count = int(area_km2 / 100 * sp["per_100km2"])
    # Clusters: a slow noise makes fields of spires and empty stretches.
    ck = max(2, int(sp["cluster_km"] * 1000 / world.cell))
    clump = ndimage.gaussian_filter(rng.random(zones.shape), ck / 2)
    clump = (clump - clump.min()) / max(np.ptp(clump), 1e-6)
    weights = clump[ys, xs] ** 3 + 0.02
    spires = poisson(pts, weights, count, sp["min_gap_km"] * 1000, rng)
    names = _names(catalogue, sp["kind"])
    spire_list = []
    # Some of them are spiral rises: a ramp winding round the column to its top.
    rises = _names(catalogue, sp["rise_kind"]) if sp.get("rise_kind") else []
    for x, y in spires:
        i, j = int(y // world.cell), int(x // world.cell)
        kind = "spire"
        name = names[rng.integers(len(names))]
        if rises and rng.random() < sp.get("rise_share", 0.0):
            kind, name = sp["rise_kind"], rises[rng.integers(len(rises))]
        m = catalogue[name]
        tall = m["hi"][2]
        ground = world.lowest(x, y, 0.5 * (m["hi"][0] - m["lo"][0]))
        t = top[i, j] * rng.uniform(*sp["top_share"])
        if t - ground < sp["min_rise_m"]:
            continue
        sz = (t - ground) / tall
        sxy = sz ** 0.85 * rng.uniform(*sp["width"])   # nearly as built: no squat boulders
        add(kind, name, x, y, rng.uniform(0, math.tau), (sxy, sxy, sz), top=round(float(t), 1),
            ground=round(ground, 1))
        spire_list.append((x, y, t, ground, sxy * 0.5 * (m["hi"][0] - m["lo"][0])))

    # --- bridges between spires whose tops are alike ---
    br = rules["bridges"]
    bnames = _names(catalogue, br["kind"])
    used = set()
    pairs = []
    for a in range(len(spire_list)):
        for b in range(a + 1, len(spire_list)):
            xa, ya, ta, ga, ra = spire_list[a]
            xb, yb, tb, gb, rb = spire_list[b]
            d = math.hypot(xb - xa, yb - ya)
            if not (br["gap_km"][0] * 1000 <= d <= br["gap_km"][1] * 1000):
                continue
            rise = min(ta - ga, tb - gb)
            if abs(ta - tb) > br["top_diff"] * rise:
                continue
            pairs.append((d, a, b))
    rng.shuffle(pairs)
    want = int(len(spire_list) * br["share"])
    for d, a, b in pairs:
        if len(used) // 2 >= want:
            break
        if a in used or b in used:
            continue
        used |= {a, b}
        xa, ya, ta, ga, ra = spire_list[a]
        xb, yb, tb, gb, rb = spire_list[b]
        rise = min(ta - ga, tb - gb)
        deck = min(ta, tb) - rng.uniform(0.02, 0.18) * rise
        th = rng.uniform(*br["thickness"])
        kind, name = br["kind"], bnames[rng.integers(len(bnames))]
        if br.get("odd_kind") and rng.random() < br.get("odd_share", 0.0):
            odd = _names(catalogue, br["odd_kind"])
            kind, name = br["odd_kind"], odd[rng.integers(len(odd))]
        add(kind, name, (xa + xb) / 2, (ya + yb) / 2, math.atan2(yb - ya, xb - xa),
            (d / 1000.0, th, th * math.sqrt(d / 1000.0)), z=round(float(deck), 1))
    return len(spire_list)


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
        kind, name = r["kind"], names[rng.integers(len(names))]
        if r.get("odd_kind") and rng.random() < r.get("odd_share", 0.0):
            odd = _names(catalogue, r["odd_kind"])
            kind, name = r["odd_kind"], odd[rng.integers(len(odd))]
        add(kind, name, mx, my, math.atan2(y1 - y0, x1 - x0),
            ((gap * 1.4) / 1000.0, th, th * math.sqrt(gap / 1000.0)), z=round(float(min(h0, h1) - rng.uniform(5, 25)), 1))
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
        a, b = si[i, j], sj[i, j]
        out = np.array([-gx[a, b], -gy[a, b]])
        if np.linalg.norm(out) < 1e-6:
            continue
        out /= np.linalg.norm(out)
        # Walk out to the true rim at full resolution: the first point where
        # the ground falls away; the ledge's origin is the rim.
        h0 = world.height(x, y)
        rim = None
        for step in range(1, 60):
            px, py = x + out[0] * step * 16.0, y + out[1] * step * 16.0
            if world.height(px, py) < h0 - r["fall_m"]:
                rim = (x + out[0] * (step - 2) * 16.0, y + out[1] * (step - 2) * 16.0)
                break
        if rim is None:
            continue
        x, y = rim
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        made.append((x, y))
        name = names[rng.integers(len(names))]
        m = catalogue[name]
        s = rng.uniform(*r["size_m"]) / _size(m)
        add(r["kind"], name, x, y, math.atan2(out[1], out[0]), (s, s, s), z=round(world.height(x, y) + 2.0, 1))
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


def switchbacks(world, rules, catalogue, rng, add):
    """Switchback climbs cut along the canyon walls (doc/BIOMES_RU.md, 03:
    heavy winding paths that drop and climb sharply): on a sheer stretch, the
    model's wall plane laid along it, its ledges standing out downhill, from
    the foot to the rim."""
    r = rules["switchbacks"]
    zone = np.isin(world.zones, r["zones"])
    gy, gx = np.gradient(world.hz, world.cell)
    slope = np.hypot(gx, gy)
    lo = ndimage.minimum_filter(world.hz, 5)
    hi = ndimage.maximum_filter(world.hz, 5)
    ok = zone & (slope > r["slope"]) & (hi - lo > r["min_drop_m"])
    ys, xs = np.nonzero(ok)
    if not len(xs):
        return 0
    names = _names(catalogue, r["kind"])
    made = []
    for k in rng.permutation(len(xs)):
        i, j = ys[k], xs[k]
        x, y = (j + 0.5) * world.cell, (i + 0.5) * world.cell
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < r["spacing_m"] ** 2 for p in made):
            continue
        down = np.array([-gx[i, j], -gy[i, j]]) / max(slope[i, j], 1e-6)
        # Walk downhill to the foot: the model stands there, its wall plane
        # (y = 0) against the face, its ledges (-y) facing out over the floor.
        foot = (x, y)
        for step in range(1, 40):
            px, py = x + down[0] * step * 16.0, y + down[1] * step * 16.0
            if world.height(px, py) > world.height(*foot) - 2.0:
                break
            foot = (px, py)
        fx, fy = foot[0] - down[0] * r["into_m"], foot[1] - down[1] * r["into_m"]
        drop = float(hi[i, j] - lo[i, j])
        name = names[rng.integers(len(names))]
        m = catalogue[name]
        sz = drop * rng.uniform(0.85, 0.98) / m["hi"][2]
        sxy = rng.uniform(*r["width"])
        made.append((x, y))
        add(r["kind"], name, fx, fy, math.atan2(down[1], down[0]) + math.pi / 2, (sxy, 1.0, sz),
            z=round(world.height(*foot) - 5.0, 1))
        if len(made) >= r["count"]:
            break
    return len(made)


def place(world, rules, catalogue, rng, add):
    """Every terrain part the section names; returns how many of each."""
    import world_features as wf
    counts = {}
    for name, fn in (("spires", canyon_spires), ("canyon_bridges", canyon_bridges), ("rim_ledges", rim_ledges),
                     ("clefts", clefts), ("sea_stacks", sea_stacks), ("floating_isles", floating_isles),
                     ("sentinels", sentinels), ("switchbacks", switchbacks)):
        if name in rules:
            counts[name] = fn(world, rules, catalogue, rng, add)
    counts["groups"] = wf.place_groups(world, rules, catalogue, rng, add)
    return counts
