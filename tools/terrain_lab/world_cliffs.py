"""world_cliffs - the cliff walls of the hybrid terrain (doc/rework/MESH_TERRAIN_PLAN.md).

Called by world_features.py. A heightfield can only slope: at 32 m a texel the
knife edge of the plateau over the steppe, the canyon walls and the cut coast
come out as ramps. Here each steep band of the world gets a skin of mesh laid
over it that pushes the slope out sideways, downhill, in proportion to how
high up the band a point is - so the ramp stands up into a wall - and a
little more near the top, so the rim overhangs. Past the top edge the push
dies away over a lip: the rim's flat top reaches out over the wall. Below
the foot and round the edges the skin lies on the ground, sunk a few metres,
so the ground takes over where the wall ends.

The skin is cut into tiles of 1024 m, each a model in three levels (grid of
`grid_m`, twice, four times that), written to <world>_cliffs/ and listed in
the features as instances at their tile corner.
"""
import math
import struct
from pathlib import Path

import numpy as np
from scipy import ndimage

TILE_M = 1024.0


def _write_mesh(path, pos, nrm, uv, tris):
    lo = pos.min(0)
    hi = pos.max(0)
    v = np.concatenate([pos, nrm, uv], 1).astype("<f4")
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<III", 1, len(v), tris.size))
        f.write(struct.pack("<6f", *lo, *hi))
        f.write(v.tobytes())
        f.write(tris.astype("<u4").tobytes())


def _vnoise(x, y, cell, seed):
    """Smooth value noise of world metres, 0..1: the same at a point whatever
    tile asks, so neighbouring tiles agree on their shared border."""
    gx, gy = x / cell, y / cell
    ix, iy = np.floor(gx), np.floor(gy)
    fx, fy = gx - ix, gy - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)

    def h(a, b):
        v = (a.astype(np.int64) * 73856093) ^ (b.astype(np.int64) * 19349663) ^ (seed * 83492791)
        v = (v ^ (v >> 13)) * 1274126177
        return ((v ^ (v >> 16)) & 0xffff) / 65535.0

    return (h(ix, iy) * (1 - fx) + h(ix + 1, iy) * fx) * (1 - fy) + (h(ix, iy + 1) * (1 - fx) + h(ix + 1, iy + 1) * fx) * fy


def _skirt(P, N, UV, tris, depth):
    """Hangs a skirt from the tile's open edges on its border, `depth` down
    along the surface's own inward side: where a neighbour tile is drawn at
    another level of detail, the crack between them shows the skirt, not sky."""
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    key = np.sort(e, 1)
    _, inv, counts = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    open_ = e[counts[inv.ravel()] == 1]
    if not len(open_):
        return P, N, UV, tris
    on = lambda q: (np.abs(q[:, 0]) < 0.5) | (np.abs(q[:, 0] - TILE_M) < 0.5) |                    (np.abs(q[:, 1]) < 0.5) | (np.abs(q[:, 1] - TILE_M) < 0.5)
    border = open_[on(P[open_[:, 0]]) & on(P[open_[:, 1]])]
    if not len(border):
        return P, N, UV, tris
    ends = np.unique(border)
    low = np.full(len(P), -1, np.int64)
    low[ends] = len(P) + np.arange(len(ends))
    drop = P[ends] - N[ends] * depth
    drop[:, 2] = np.minimum(drop[:, 2], P[ends, 2] - depth)
    P2 = np.concatenate([P, drop])
    N2 = np.concatenate([N, N[ends]])
    UV2 = np.concatenate([UV, UV[ends] * np.array([0.7, 1.0])])
    a, b = border[:, 0], border[:, 1]
    # Wound as the edge runs in its own triangle, so the skirt faces out with it.
    skirt = np.concatenate([np.stack([b, a, low[a]], 1), np.stack([b, low[a], low[b]], 1)])
    return P2, N2, UV2, np.concatenate([tris, skirt])


def fields(h, zones, c, rules):
    """The push field over a block of the map (32 m texels): where the skin
    is, which way and how far each point is pushed, how deep in it lies.
    `zones`: zone names per texel of the block."""
    cr = rules["cliffs"]
    hs = ndimage.gaussian_filter(h, 1.0)
    gy, gx = np.gradient(hs, c)
    slope = np.hypot(gx, gy)
    band = np.zeros(h.shape, bool)
    sea_m = None
    for zone, rule in cr["zones"].items():
        # A slope, or {"slope", "coast_m"}: only within coast_m of the sea
        # (the isles: their sea cliffs, not every crag inland).
        thr = rule["slope"] if isinstance(rule, dict) else rule
        here = (zones == zone) & (slope > thr)
        if isinstance(rule, dict) and "coast_m" in rule and here.any():
            if sea_m is None:
                sea_m = ndimage.distance_transform_edt(h > 0.5) * c
            here &= sea_m < rule["coast_m"]
        band |= here
    reach = int(cr["band_m"] / c) | 1
    lo = ndimage.minimum_filter(h, reach)
    hi = ndimage.maximum_filter(h, reach)
    band &= (hi - lo) > cr["min_drop_m"]
    band = ndimage.binary_closing(band, iterations=1)
    lab, count = ndimage.label(band)
    if count:
        sizes = ndimage.sum(band, lab, range(1, count + 1))
        band &= np.isin(lab, 1 + np.nonzero(sizes >= cr["min_cells"])[0])
    drop = np.maximum(hi - lo, 1.0)
    t = np.clip((h - lo) / drop, 0, 1)
    run = (h - lo) / np.maximum(slope, 0.35)
    over = cr["overhang"]
    # Where the heightfield already stands near vertical (the generator keeps
    # some walls sheer) the run is nothing: the skin still stands proud of it
    # by `min_push_m`, give or take, and the rim reaches out over the foot by
    # a share of the drop.
    wander = ndimage.gaussian_filter(np.random.default_rng(int(h.size) % 9973).random(h.shape), 3.0)
    wander = (wander - wander.mean()) / max(wander.std(), 1e-6)
    base = cr["min_push_m"] * np.clip(1.0 + 0.45 * wander, 0.3, 2.0) + cr["overhang_drop"] * drop * t * t
    push = np.where(band, np.minimum(np.maximum(run * (1 + over * t * t), base), cr["max_push_m"]), 0.0)
    # Downhill, horizontally.
    dx = -gx / np.maximum(slope, 1e-3)
    dy = -gy / np.maximum(slope, 1e-3)
    region = band
    lift = np.zeros(h.shape)
    if band.any():
        # The lip: past the top of a band the push dies away, slower than it
        # moves the ground (so the skin never folds), only on the high side.
        dist, (ni, nj) = ndimage.distance_transform_edt(~band, return_indices=True)
        dist *= c
        top_push = push[ni, nj]
        lip_len = np.maximum(top_push * 1.6, 3 * c)
        high_side = h > hi[ni, nj] - 0.2 * drop[ni, nj]
        lip = ~band & high_side & (dist < lip_len)
        push = np.where(lip, top_push * (1 - dist / lip_len), push)
        dx = np.where(lip, dx[ni, nj], dx)
        dy = np.where(lip, dy[ni, nj], dy)
        # The apron: below the foot, scree banked against the wall - the skin
        # rises out of the floor to meet it, so the ground never shows in the
        # seam where a sheer wall stands on it.
        apron_len = cr["apron_m"]
        apron = ~band & ~high_side & (dist < apron_len)
        fall = np.clip(1 - dist / apron_len, 0, 1)
        lift = np.where(apron, np.clip(cr["scree_share"] * drop[ni, nj], 4.0, cr["scree_max_m"]) * fall ** 1.5, 0.0)
        push = np.where(apron, push[ni, nj] * fall, push)
        dx = np.where(apron, dx[ni, nj], dx)
        dy = np.where(apron, dy[ni, nj], dy)
        region = band | lip | apron
    # Smooth the vector field so neighbours never tear apart.
    px = ndimage.gaussian_filter(np.where(region, push * dx, 0).astype(np.float32), 1.2)
    py = ndimage.gaussian_filter(np.where(region, push * dy, 0).astype(np.float32), 1.2)
    weight = ndimage.gaussian_filter(region.astype(np.float32), 1.2)
    fade = np.clip(weight * 2, 0, 1)
    px = np.where(weight > 0.02, px / np.maximum(weight, 0.02), 0) * fade
    py = np.where(weight > 0.02, py / np.maximum(weight, 0.02), 0) * fade
    skin = ndimage.binary_dilation(region, iterations=cr["margin_cells"])
    inner = ndimage.gaussian_filter(region.astype(np.float32), 1.0)
    return {"lift": ndimage.gaussian_filter(lift, 1.0).astype(np.float32), "h": h, "px": px.astype(np.float32), "py": py.astype(np.float32), "skin": skin,
            "inner": inner, "t": t.astype(np.float32), "zones": zones, "band": band}


def build(world, rules, out_dir, rng):
    """Writes the tiles, returns their instances. Works through the map in
    blocks (with a halo wider than any filter here) to keep memory small."""
    cr = rules["cliffs"]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("cliff_*.mesh"):
        old.unlink()
    c = world.size / world.n
    per_tile = int(TILE_M / c)
    zk = world.n // world.zones.shape[0]
    block = 32 * per_tile
    halo = 64
    instances, triangles, vertices, band_cells = [], 0, 0, 0
    for bi in range(0, world.n, block):
        for bj in range(0, world.n, block):
            r0, r1 = max(bi - halo, 0), min(bi + block + halo, world.n)
            c0, c1 = max(bj - halo, 0), min(bj + block + halo, world.n)
            zsub = world.zones[r0 // zk:(r1 + zk - 1) // zk, c0 // zk:(c1 + zk - 1) // zk]
            if not np.isin(zsub, list(cr["zones"])).any():
                continue
            zones = np.repeat(np.repeat(zsub, zk, 0), zk, 1)[r0 % zk:r0 % zk + r1 - r0, c0 % zk:c0 % zk + c1 - c0]
            f = fields(world.h[r0:r1, c0:c1], zones, c, rules)
            core = (slice(bi - r0, bi - r0 + block), slice(bj - c0, bj - c0 + block))
            band_cells += int(f["band"][core].sum())
            tiles = block // per_tile
            skin_t = f["skin"][core].reshape(tiles, per_tile, tiles, per_tile).any(axis=(1, 3))
            band_t = f["band"][core].reshape(tiles, per_tile, tiles, per_tile).any(axis=(1, 3))
            for li, lj in zip(*np.nonzero(skin_t & ndimage.binary_dilation(band_t))):
                ti, tj = bi // per_tile + li, bj // per_tile + lj
                made = tile(f, ti, tj, (r0, c0), c, cr, out_dir)
                if made is None:
                    continue
                inst, tr, vt = made
                instances.append(inst)
                triangles += tr
                vertices += vt
    print(f"cliffs: {len(instances)} tiles, {triangles / 1e6:.2f} M triangles, {vertices / 1e6:.2f} M vertices at "
          f"{cr['grid_m']} m (band {band_cells * c * c / 1e6:.0f} km2)")
    return instances


def tile(f, ti, tj, origin, c, cr, out_dir):
    """One tile's three levels, or None when no quad of it is skin."""
    r0, c0 = origin
    per_tile = int(TILE_M / c)
    sink = cr["sink_m"]
    triangles = vertices = 0
    x0, y0 = tj * TILE_M, ti * TILE_M
    name = f"cliff_{ti}_{tj}"
    ok = True
    for level in range(3):
        step = cr["grid_m"] * (1 << level)
        m = int(round(TILE_M / step))
        g = np.arange(m + 1) * (TILE_M / m)
        X, Y = np.meshgrid(x0 + g, y0 + g)
        # Map coordinates in texels (texel centres at +0.5).
        u = X / c - 0.5 - c0
        v = Y / c - 0.5 - r0
        coords = [v.ravel(), u.ravel()]
        z = ndimage.map_coordinates(f["h"], coords, order=1, mode="nearest").reshape(X.shape)
        px = ndimage.map_coordinates(f["px"], coords, order=1, mode="nearest").reshape(X.shape)
        py = ndimage.map_coordinates(f["py"], coords, order=1, mode="nearest").reshape(X.shape)
        inner = ndimage.map_coordinates(f["inner"], coords, order=1, mode="nearest").reshape(X.shape)
        lift = ndimage.map_coordinates(f["lift"], coords, order=1, mode="nearest").reshape(X.shape)
        tt = ndimage.map_coordinates(f["t"], coords, order=1, mode="nearest").reshape(X.shape)
        skin = ndimage.map_coordinates(f["skin"].astype(np.float32), coords, order=0, mode="nearest").reshape(X.shape) > 0.5
        # Ledges and runnels: the wall is pushed out in bands of rock
        # and drawn in along gullies, so it reads as strata.
        # Added, not multiplied (their product was a grid of pits): ledges by
        # height, wandering; runnels by place along the wall, from noise.
        strata = cr["strata"] * (0.65 * np.sin(z / cr["strata_m"] + 2.5 * _vnoise(X, Y, 260.0, 1))
                                 + 0.35 * np.sin(z / (cr["strata_m"] * 2.7) + 1.0))
        gully = cr["gullies"] * (2.0 * _vnoise(X, Y, 45.0, 2) - 1.0 + 0.6 * (2.0 * _vnoise(X, Y, 17.0, 3) - 1.0))
        mod = 1.0 + strata + gully
        PX = X + px * mod
        PY = Y + py * mod
        PZ = z + lift - sink * (1 - np.clip(inner * 1.5, 0, 1))
        # Quads whose four corners are all in the skin.
        q = skin[:-1, :-1] & skin[1:, :-1] & skin[:-1, 1:] & skin[1:, 1:]
        if not q.any():
            if level == 0:
                ok = False
                break
            # Too coarse to hold any of it: the finer level stands in.
            import shutil
            shutil.copyfile(out_dir / f"{name}_lod{level - 1}.mesh", out_dir / f"{name}_lod{level}.mesh")
            continue
        idx = np.arange((m + 1) * (m + 1)).reshape(m + 1, m + 1)
        a, b, cc, d = idx[:-1, :-1][q], idx[:-1, 1:][q], idx[1:, 1:][q], idx[1:, :-1][q]
        tris = np.stack([np.stack([a, b, cc], 1), np.stack([a, cc, d], 1)], 1).reshape(-1, 3)
        used = np.unique(tris)
        remap = np.full(idx.size, -1, np.int64)
        remap[used] = np.arange(len(used))
        tris = remap[tris]
        P = np.stack([PX - x0, PY - y0, PZ], -1)
        # Normals from the displaced grid.
        di = np.gradient(P, axis=1)
        dj = np.gradient(P, axis=0)
        N = np.cross(di, dj)
        N /= np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-6)
        P = P.reshape(-1, 3)[used]
        N = N.reshape(-1, 3)[used]
        under = np.clip(-N[:, 2], 0, 1)
        ao = np.clip(1 - 0.5 * under, 0.35, 1)
        UV = np.stack([ao, np.clip(tt.reshape(-1)[used], 0.1, 1)], 1)
        P, N, UV, tris = _skirt(P, N, UV, tris, cr["skirt_m"])
        _write_mesh(out_dir / f"{name}_lod{level}.mesh", P, N, UV, tris.ravel())
        if level == 0:
            triangles += len(tris)
            vertices += len(P)
    if not ok:
        return None
    # The tile's rock: the material of the zone most of its wall is in.
    sl = (slice(ti * per_tile - r0, (ti + 1) * per_tile - r0), slice(tj * per_tile - c0, (tj + 1) * per_tile - c0))
    zs = f["zones"][sl][f["band"][sl]]
    if not zs.size:
        # Only the lip or the scree of a wall next door: the rock of the walls round it.
        zs = f["zones"][sl][f["skin"][sl] & np.isin(f["zones"][sl], list(cr["zones"]))]
    if zs.size:
        names, counts = np.unique(zs, return_counts=True)
        zone = names[np.argmax(counts)]
    else:
        zone = ""
    material = cr["materials"].get(str(zone), cr["materials"].get("default", "rock_grey"))
    return ({"m": name, "dir": out_dir.name, "x": round(float(x0), 1), "y": round(float(y0), 1),
             "z": 0.0, "yaw": 0.0, "s": [1.0, 1.0, 1.0], "material": material, "detail": True}, triangles, vertices)
