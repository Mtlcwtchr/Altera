"""world_cliffs - the cliff walls of the hybrid terrain (doc/rework/MESH_TERRAIN_PLAN.md).

Called by world_features.py, or alone (it then puts its tiles into an
existing features file in place of the old ones):

    python tools/terrain_lab/world_cliffs.py [--world terrain_lab_out/world]

A heightfield can only slope: at 16 m a texel the knife edge of the plateau
over the steppe, the canyon walls and the cut coast come out as ramps. Here
each steep band of the world is stood up into a wall - as a solid, not as a
sheet. At every height z the ground higher than z is grown outwards, each
point of the band by its own reach (how far its ramp runs to the foot, more
near the top, so the rim overhangs): the union of all that is the rock. Its
surface is found on a grid (surface nets) and cut into tiles. Because it is
one solid, walls that meet - round a bay, across a narrow canyon, where two
bands touch - merge into one rock instead of passing through each other, and
nothing can fold. A canyon keeps an opening: no wall reaches further than
`gap_share` of the way to the wall that faces it; where a band stops short
on a steep slope its reach dies away, so no buttress stands out of the bare
ground with a cut end. Below the wall scree is
banked against the foot; everywhere else the surface lies `sink_m` under the
ground, so the ground takes over where the wall ends.

Each part of a wall wears the biome it belongs to (world_biomes.png): its
upper part and its rim the land on top, its foot and the scree the land
below - the plateau's cliffs are the plateau's rock with its grass on the
ledges, the canyon's walls the canyon's. A tile carries the two biomes it
needs (`biomes`), each vertex how far it is the second (the .mesh's fourth
attribute); the renderer looks the biomes' rock and ground up in biomes.json.

The skin is cut into tiles of 1024 m, each a model in three levels (grid of
`grid_m`, twice, four times that), written to <world>_cliffs/ and listed in
the features as instances at their tile corner.
"""
import argparse
import json
import math
import multiprocessing
import os
import struct
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

TILE_M = 1024.0
MARGIN_M = 640.0     # what a tile looks at round it: more than any wall reaches
SHIFT = 8            # points of the finest grid beyond the tile's edge (the coarsest cell and the smoothing)


def _write_mesh(path, pos, nrm, uv, blend, tris):
    lo = pos.min(0)
    hi = pos.max(0)
    v = np.concatenate([pos, nrm, uv, blend[:, None]], 1).astype("<f4")
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<III", 2, len(v), tris.size))
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


def _vnoise3(x, y, z, cell, seed):
    """Smooth value noise in 3D, -1..1, of world metres."""
    gx, gy, gz = x / cell, y / cell, z / cell
    ix, iy, iz = np.floor(gx), np.floor(gy), np.floor(gz)
    fx, fy, fz = gx - ix, gy - iy, gz - iz
    fx, fy, fz = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy), fz * fz * (3 - 2 * fz)

    def h(a, b, c):
        v = (a.astype(np.int64) * 73856093) ^ (b.astype(np.int64) * 19349663) ^ (c.astype(np.int64) * 83492791) ^ (seed * 2654435761)
        v = (v ^ (v >> 13)) * 1274126177
        return ((v ^ (v >> 16)) & 0xffff) / 32767.5 - 1.0

    def plane(c):
        return (h(ix, iy, c) * (1 - fx) + h(ix + 1, iy, c) * fx) * (1 - fy) +                (h(ix, iy + 1, c) * (1 - fx) + h(ix + 1, iy + 1, c) * fx) * fy

    return plane(iz) * (1 - fz) + plane(iz + 1) * fz


def _skirt(P, N, UV, B, tris, depth):
    """Hangs a skirt from the tile's open edges on its border, `depth` down
    along the surface's own inward side: where a neighbour tile is drawn at
    another level of detail, the crack between them shows the skirt, not sky."""
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    key = np.sort(e, 1)
    _, inv, counts = np.unique(key, axis=0, return_inverse=True, return_counts=True)
    open_ = e[counts[inv.ravel()] == 1]
    if not len(open_):
        return P, N, UV, B, tris
    on = lambda q: (np.abs(q[:, 0]) < 0.5) | (np.abs(q[:, 0] - TILE_M) < 0.5) | \
                   (np.abs(q[:, 1]) < 0.5) | (np.abs(q[:, 1] - TILE_M) < 0.5)
    border = open_[on(P[open_[:, 0]]) & on(P[open_[:, 1]])]
    if not len(border):
        return P, N, UV, B, tris
    ends = np.unique(border)
    low = np.full(len(P), -1, np.int64)
    low[ends] = len(P) + np.arange(len(ends))
    drop = P[ends] - N[ends] * depth
    drop[:, 2] = np.minimum(drop[:, 2], P[ends, 2] - depth)
    P2 = np.concatenate([P, drop])
    N2 = np.concatenate([N, N[ends]])
    UV2 = np.concatenate([UV, UV[ends] * np.array([0.7, 1.0])])
    B2 = np.concatenate([B, B[ends]])
    a, b = border[:, 0], border[:, 1]
    # Wound as the edge runs in its own triangle, so the skirt faces out with it.
    skirt = np.concatenate([np.stack([b, a, low[a]], 1), np.stack([b, low[a], low[b]], 1)])
    return P2, N2, UV2, B2, np.concatenate([tris, skirt])


def _march(h, rows, cols, dy, dx, step, count, stop, order=1):
    """Walks from texels (rows, cols) along (dy, dx) in `count` steps of
    `step` texels; returns the steps taken before `stop(h_here, k)` first
    holds (count + 1: never)."""
    hit = np.full(len(rows), count + 1, np.int32)
    live = np.ones(len(rows), bool)
    for k in range(1, count + 1):
        if not live.any():
            break
        r = rows[live] + dy[live] * step * k
        c = cols[live] + dx[live] * step * k
        hv = ndimage.map_coordinates(h, [r, c], order=order, mode="nearest")
        done = stop(hv, live, k)
        idx = np.nonzero(live)[0][done]
        hit[idx] = k
        live[idx] = False
    return hit


def fields(h, zones, biomes, c, rules, origin=(0, 0)):
    """The cliffs' fields over a block of the map (texels of `c` metres):
    where the walls are and how far each point of them reaches, where the
    scree lies, and which biome is on top of each wall and at its foot.
    `zones`: zone names per texel; `biomes`: biome ids per texel; `origin`:
    the block's first texel (row, column) in the map."""
    cr = rules["cliffs"]
    hs = ndimage.gaussian_filter(h, 1.0)
    gy, gx = np.gradient(hs, c)
    slope = np.hypot(gx, gy)
    band = np.zeros(h.shape, bool)
    steep = np.zeros(h.shape, bool)
    sea_m = None
    for zone, rule in cr["zones"].items():
        # A slope, or {"slope", "coast_m"}: only within coast_m of the sea
        # (the isles: their sea cliffs, not every crag inland).
        thr = rule["slope"] if isinstance(rule, dict) else rule
        steep |= (zones == zone) & (slope > 0.7 * thr)
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
    out = {"h": h, "band": band, "push": np.zeros(h.shape, np.float32), "lo": lo.astype(np.float32),
           "drop": drop.astype(np.float32), "t": t.astype(np.float32), "lift": np.zeros(h.shape, np.float32),
           "top": biomes.astype(np.uint8), "foot": biomes.astype(np.uint8), "own": biomes.astype(np.uint8)}
    if not band.any():
        return out
    # How far each point of a band reaches out: the run of its ramp down to
    # the foot (so the ramp stands up), more near the top (the rim
    # overhangs), never less than `min_push_m` give or take (where the
    # heightfield already stands sheer), plus `overhang_drop` of the drop at the rim.
    run = (h - lo) / np.maximum(slope, 0.35)
    over = cr["overhang"]
    # (a noise of the world, not of the block: the blocks agree where they meet)
    Yw, Xw = np.meshgrid((np.arange(h.shape[0]) + origin[0] + 0.5) * c, (np.arange(h.shape[1]) + origin[1] + 0.5) * c,
                         indexing="ij")
    wander = 3.5 * (_vnoise(Xw, Yw, 3.0 * c, 7) - 0.5)
    base = cr["min_push_m"] * np.clip(1.0 + 0.45 * wander, 0.3, 2.0) + cr["overhang_drop"] * drop * t * t
    push = np.where(band, np.minimum(np.maximum(run * (1 + over * t * t), base), cr["max_push_m"]), 0.0)
    # Smoothed along the band only (the band's own neighbours), so the reach
    # does not jump from texel to texel.
    wsum = ndimage.gaussian_filter(band.astype(np.float32), 1.2)
    push = np.where(band, ndimage.gaussian_filter(push.astype(np.float32), 1.2) / np.maximum(wsum, 1e-3), 0.0)
    # Where a band ends along its wall (the wall goes on, steep, but no
    # longer one to stand up) its reach dies away over a few hundred metres,
    # so no buttress stands out of the bare wall with a cut end.
    # (steep by its own zone's measure; outside the cliff zones by a plain one)
    steep |= band | (~np.isin(zones, list(cr["zones"])) & (slope > 0.5))
    share = ndimage.gaussian_filter(band.astype(np.float32), cr.get("end_cells", 8.0)) /         np.maximum(ndimage.gaussian_filter(steep.astype(np.float32), cr.get("end_cells", 8.0)), 1e-3)
    ends = np.clip((share - 0.25) / 0.5, 0, 1) ** 1.5
    push = push * ends

    rows, cols = np.nonzero(band)
    ddx = (-gx / np.maximum(slope, 1e-3))[band]
    ddy = (-gy / np.maximum(slope, 1e-3))[band]
    blo, bdrop, bh = lo[band], drop[band], h[band]
    # The opening in front of each wall: walking downhill, how far until the
    # ground rises again past a third of the wall's drop (the wall across).
    steps = int(math.ceil(2.2 * cr["max_push_m"] / c))
    gap_hit = _march(h, rows.astype(np.float64), cols.astype(np.float64), ddy, ddx, 1.0, steps,
                     lambda hv, live, k: (k * c > 40.0) & (hv > (blo + 0.35 * bdrop)[live]))
    gap = np.where(gap_hit > steps, np.inf, gap_hit * c)
    push[band] = np.minimum(push[band], cr.get("gap_share", 0.8) * 0.5 * gap)

    # Where each wall ends, up and down: walking uphill off the band (its
    # top, the land on top a little further) and downhill off it (its foot).
    up_steps = int(math.ceil(cr.get("wall_max_m", 2500.0) / c))
    bandf = band.astype(np.float32)
    up_hit = _march(bandf, rows.astype(np.float64), cols.astype(np.float64), -ddy, -ddx, 1.0, up_steps,
                    lambda hv, live, k: hv < 0.5, order=0)
    down_hit = _march(bandf, rows.astype(np.float64), cols.astype(np.float64), ddy, ddx, 1.0, up_steps,
                      lambda hv, live, k: hv < 0.5, order=0)
    ztop = ndimage.map_coordinates(h, [rows - ddy * np.minimum(up_hit, up_steps), cols - ddx * np.minimum(up_hit, up_steps)],
                                   order=1, mode="nearest")
    zfoot = ndimage.map_coordinates(h, [rows + ddy * np.minimum(down_hit, up_steps), cols + ddx * np.minimum(down_hit, up_steps)],
                                    order=1, mode="nearest")
    zfoot = np.minimum(zfoot, bh)
    ztop = np.maximum(ztop, bh)
    out["zfoot"] = lo.astype(np.float32)
    out["zspan"] = drop.astype(np.float32)
    out["zfoot"][band] = zfoot
    out["zspan"][band] = np.maximum(ztop - zfoot, 1.0)
    beyond = cr.get("biome_look_m", 160.0) / c
    ur = rows - ddy * (np.minimum(up_hit, up_steps) + beyond)
    uc = cols - ddx * (np.minimum(up_hit, up_steps) + beyond)
    fr = rows + ddy * (np.minimum(down_hit, up_steps) + beyond)
    fc = cols + ddx * (np.minimum(down_hit, up_steps) + beyond)
    top = out["top"].copy()
    foot = out["foot"].copy()
    top[band] = ndimage.map_coordinates(biomes, [ur, uc], order=0, mode="nearest")
    foot[band] = ndimage.map_coordinates(biomes, [fr, fc], order=0, mode="nearest")
    # The sea is no biome for a wall: a sea cliff is the land's rock to its foot.
    foot = np.where(foot == 0, top, foot)
    top = np.where(top == 0, foot, top)
    # Steady along the wall: the commonest of the neighbours, so a border
    # between two biomes runs across the wall as a clean edge, not a speckle.
    for arr in (top, foot):
        vals = np.where(band, arr, 0).astype(np.int32)
        best = np.zeros(h.shape, np.float32)
        pick = arr.copy()
        for b in np.unique(arr[band]):
            share = ndimage.uniform_filter((vals == b).astype(np.float32), 7)
            better = band & (share > best)
            pick[better] = b
            best = np.where(better, share, best)
        arr[band] = pick[band]

    # The scree: below the foot, banked against the wall - the skin rises out
    # of the floor to meet it, so the ground never shows in the seam where a
    # sheer wall stands on it.
    dist, (ni, nj) = ndimage.distance_transform_edt(~band, return_indices=True)
    dist *= c
    high_side = h > hi[ni, nj] - 0.2 * drop[ni, nj]
    apron = ~band & ~high_side & (dist < cr["apron_m"]) & (dist < push[ni, nj] + cr["apron_m"] * 0.5)
    fall = np.clip(1 - dist / cr["apron_m"], 0, 1)
    lift = np.where(apron, np.clip(cr["scree_share"] * drop[ni, nj], 4.0, cr["scree_max_m"]) * fall ** 1.5 * ends[ni, nj], 0.0)
    out.update(push=push.astype(np.float32), top=top, foot=foot,
               lift=ndimage.gaussian_filter(lift, 1.0).astype(np.float32))
    return out


def _sample(arr, X, Y, c, origin, order=1):
    """A block field at world metres (texel centres at +0.5)."""
    r0, c0 = origin
    return ndimage.map_coordinates(arr, [Y / c - 0.5 - r0, X / c - 0.5 - c0], order=order, mode="nearest")


def _surface_nets(F, X, Y, Z, keep_col):
    """The zero surface of F (positive inside), F[k, i, j] at (X[j], Y[i],
    Z[k]): one vertex per cell the surface crosses (the mean of the crossings
    of its edges), a quad round each crossed edge. Quads only where any of
    their four cells has a kept column (`keep_col[i, j]`, per cell).
    -> vertices (world metres, float64), the cell index of each, quads."""
    inside = F > 0
    nz, ny, nx = F.shape
    acc = np.zeros((nz - 1, ny - 1, nx - 1, 3))
    cnt = np.zeros((nz - 1, ny - 1, nx - 1))

    def crossing(a, b):
        fa, fb = F[a], F[b]
        hit = inside[a] != inside[b]
        t = np.where(hit, fa / np.where(hit, fa - fb, 1.0), 0.0)
        return hit, t

    # Edges along x: (k, i, j)-(k, i, j+1).
    hit, t = crossing((slice(None), slice(None), slice(0, -1)), (slice(None), slice(None), slice(1, None)))
    px = X[:-1][None, None, :] + t * (X[1] - X[0])
    pos = np.stack([px, np.broadcast_to(Y[None, :, None], px.shape), np.broadcast_to(Z[:, None, None], px.shape)], -1)
    for dk in (0, 1):
        for di in (0, 1):
            sl = (slice(dk, nz - 1 + dk), slice(di, ny - 1 + di), slice(None))
            acc += pos[sl] * hit[sl][..., None]
            cnt += hit[sl]
    xe = hit
    # Along y.
    hit, t = crossing((slice(None), slice(0, -1), slice(None)), (slice(None), slice(1, None), slice(None)))
    py = Y[:-1][None, :, None] + t * (Y[1] - Y[0])
    pos = np.stack([np.broadcast_to(X[None, None, :], py.shape), py, np.broadcast_to(Z[:, None, None], py.shape)], -1)
    for dk in (0, 1):
        for dj in (0, 1):
            sl = (slice(dk, nz - 1 + dk), slice(None), slice(dj, nx - 1 + dj))
            acc += pos[sl] * hit[sl][..., None]
            cnt += hit[sl]
    ye = hit
    # Along z.
    hit, t = crossing((slice(0, -1), slice(None), slice(None)), (slice(1, None), slice(None), slice(None)))
    pz = Z[:-1][:, None, None] + t * (Z[1] - Z[0])
    pos = np.stack([np.broadcast_to(X[None, None, :], pz.shape), np.broadcast_to(Y[None, :, None], pz.shape), pz], -1)
    for di in (0, 1):
        for dj in (0, 1):
            sl = (slice(None), slice(di, ny - 1 + di), slice(dj, nx - 1 + dj))
            acc += pos[sl] * hit[sl][..., None]
            cnt += hit[sl]
    ze = hit

    has = cnt > 0
    vid = np.full(cnt.shape, -1, np.int64)
    vid[has] = np.arange(int(has.sum()))
    verts = acc[has] / cnt[has][:, None]
    cells = np.argwhere(has)
    keep_cell = np.broadcast_to(keep_col[None], cnt.shape)

    quads = []
    # Each crossed interior edge: the four cells round it, wound one way
    # (fixed later by the normals).
    k, i, j = np.nonzero(xe[1:-1, 1:-1, :])
    k += 1
    i += 1
    quads.append(np.stack([vid[k - 1, i - 1, j], vid[k - 1, i, j], vid[k, i, j], vid[k, i - 1, j]], 1))
    keeps = [keep_cell[k - 1, i - 1, j] | keep_cell[k - 1, i, j] | keep_cell[k, i, j] | keep_cell[k, i - 1, j]]
    k, i, j = np.nonzero(ye[1:-1, :, 1:-1])
    k += 1
    j += 1
    quads.append(np.stack([vid[k - 1, i, j - 1], vid[k - 1, i, j], vid[k, i, j], vid[k, i, j - 1]], 1))
    keeps.append(keep_cell[k - 1, i, j - 1] | keep_cell[k - 1, i, j] | keep_cell[k, i, j] | keep_cell[k, i, j - 1])
    k, i, j = np.nonzero(ze[:, 1:-1, 1:-1])
    i += 1
    j += 1
    quads.append(np.stack([vid[k, i - 1, j - 1], vid[k, i - 1, j], vid[k, i, j], vid[k, i, j - 1]], 1))
    keeps.append(keep_cell[k, i - 1, j - 1] | keep_cell[k, i - 1, j] | keep_cell[k, i, j] | keep_cell[k, i, j - 1])
    q = np.concatenate(quads)
    q = q[np.concatenate(keeps)]
    return verts, cells, q


def _clip(P, attrs, tris, axis, value, keep_above):
    """Cuts the triangles at the plane P[:, axis] = value, keeping one side.
    A cut edge's new point comes from its endpoints taken in a fixed order,
    so the tile on the other side makes the very same point."""
    d = P[:, axis] - value
    if not keep_above:
        d = -d
    ins = d >= 0
    tin = ins[tris]
    n_in = tin.sum(1)
    keep = [tris[n_in == 3]]
    newP, newA = [], []
    base = len(P)
    cache = {}

    def cut(a, b):
        key = (a, b) if a < b else (b, a)
        if key in cache:
            return cache[key]
        pa, pb = P[a], P[b]
        # Lexicographic order of the endpoints, not of their indices: the
        # same in both tiles.
        if tuple(pa) > tuple(pb):
            a, b, pa, pb = b, a, pb, pa
        s = (value - pa[axis]) / (pb[axis] - pa[axis])
        p = pa + (pb - pa) * s
        p[axis] = value
        newP.append(p)
        newA.append([x[a] + (x[b] - x[a]) * s for x in attrs])
        cache[key] = base + len(newP) - 1
        return cache[key]

    for tri, m in zip(tris[(n_in == 1) | (n_in == 2)], tin[(n_in == 1) | (n_in == 2)]):
        # Rotate so the odd vertex is first, keeping the winding.
        if m.sum() == 1:
            r = int(np.argmax(m))
        else:
            r = int(np.argmin(m))
        a, b, cc = tri[r], tri[(r + 1) % 3], tri[(r + 2) % 3]
        ab, ac = cut(a, b), cut(a, cc)
        if m.sum() == 1:
            keep.append(np.array([[a, ab, ac]]))
        else:
            keep.append(np.array([[ab, b, cc], [ab, cc, ac]]))
    if newP:
        P = np.concatenate([P, np.array(newP)])
        attrs = [np.concatenate([x, np.array([na[k] for na in newA])]) for k, x in enumerate(attrs)]
    tris = np.concatenate(keep) if keep else np.zeros((0, 3), np.int64)
    return P, attrs, tris


def tile(f, ti, tj, origin, c, cr):
    """One tile's solid: its levels' meshes (or None when no wall touches
    it) and the two biomes it wears."""
    per = int(round(TILE_M / cr["grid_m"]))
    s = TILE_M / per
    dz = s
    sf = s / 2.0
    margin = math.ceil(MARGIN_M / s) * s
    x0, y0 = tj * TILE_M, ti * TILE_M
    jj = np.arange(-SHIFT, per + SHIFT + 1)
    Xp, Yp = x0 + jj * s, y0 + jj * s
    nf = int(round((TILE_M + 2 * margin) / sf)) + 1
    Xf = x0 - margin + np.arange(nf) * sf
    Yf = y0 - margin + np.arange(nf) * sf
    XF, YF = np.meshgrid(Xf, Yf)
    pushf = _sample(f["push"], XF, YF, c, origin, 1)
    bandf = _sample(f["band"].astype(np.float32), XF, YF, c, origin, 0) > 0.5
    pushf = np.where(bandf, pushf, 0.0)
    if not (pushf > 0).any():
        return None
    hf = _sample(f["h"], XF, YF, c, origin, 1)
    lof = _sample(f["zfoot"], XF, YF, c, origin, 1)
    dropf = _sample(f["zspan"], XF, YF, c, origin, 1)
    topf = _sample(f["top"], XF, YF, c, origin, 0).astype(np.uint8)
    footf = _sample(f["foot"], XF, YF, c, origin, 0).astype(np.uint8)
    # The points: every second fine texel.
    at = (jj * s + margin) / sf
    at = np.round(at).astype(np.int64)
    pidx = np.ix_(at, at)
    XP, YP = np.meshgrid(Xp, Yp)
    hp = hf[pidx]
    pushp = pushf[pidx]
    liftp = _sample(f["lift"], XP, YP, c, origin, 1)
    tp = _sample(f["t"], XP, YP, c, origin, 1)
    ownp = _sample(f["own"], XP, YP, c, origin, 0).astype(np.uint8)
    # The rock's face: ledges by height, wandering; runnels by place along the wall.
    wobble = 2.5 * _vnoise(XP, YP, 260.0, 1)
    gully = cr["gullies"] * (2.0 * _vnoise(XP, YP, 45.0, 2) - 1.0 + 0.6 * (2.0 * _vnoise(XP, YP, 17.0, 3) - 1.0))
    fray = 0.06 * (2.0 * _vnoise(XP, YP, 70.0, 4) - 1.0)

    sink = cr["sink_m"]
    reach_hi = float(np.max(np.where(pushf > 0, hf, -1e9)))
    zlo = float(np.min(hp - sink)) - 2 * dz
    zhi = max(float(np.max(hp + liftp)), reach_hi) + 2 * dz
    k0 = int(math.floor(zlo / dz / 4.0)) * 4
    k1 = k0 + int(math.ceil((zhi / dz - k0) / 4.0)) * 4
    ks = np.arange(k0, k1 + 1)
    Z = ks * dz
    shape = (len(ks),) + hp.shape
    F = np.empty(shape, np.float32)
    T = np.empty(shape, np.float32)
    TOP = np.empty(shape, np.uint8)
    FOOT = np.empty(shape, np.uint8)
    SF = np.zeros(shape, np.float32)
    walled = np.zeros(hp.shape, bool)
    ground_scree = liftp - sink * (1.0 - np.clip(liftp / 4.0, 0, 1))
    band_lo = float(np.min(np.where(pushf > 0, lof, 1e9))) - dz
    for n, z in enumerate(Z):
        g = hp - z
        gt = g + ground_scree
        R = hf > z
        mod = 1.0 + cr["strata"] * (0.65 * np.sin(z / cr["strata_m"] + wobble)
                                    + 0.35 * np.sin(z / (cr["strata_m"] * 2.7) + 1.0)) + gully
        Rp = R[pidx]
        w = np.where(Rp & (pushp > 0), pushp * mod + sf * 0.5, -1e9)
        src_t = tp
        top = ownp
        foot = ownp
        if band_lo <= z <= reach_hi and (R & (pushf > 0)).any() and (~R).any():
            dist, ind = ndimage.distance_transform_edt(~R, return_indices=True)
            d = dist[pidx] * sf
            si, sj = ind[0][pidx], ind[1][pidx]
            r = pushf[si, sj] * mod
            w = np.where(Rp, w, r - (d - sf * 0.5))
            # Buttresses and hollows: the face pushed in and out by a noise in
            # all three dimensions, then the corners of the map's contours
            # rounded (the field smoothed level by level: still one solid).
            rough = cr.get("rock_noise_m", 10.0) * (_vnoise3(XP, YP, z, 150.0, 5) + 0.5 * _vnoise3(XP, YP, z, 55.0, 6))
            w = np.where(pushf[si, sj] > 0, w + rough, w)
            w = ndimage.gaussian_filter(np.maximum(w, -4.0 * s), cr.get("smooth_cells", 1.0), truncate=3.0, mode="nearest")
            wall = (w > gt) & (pushf[si, sj] > 0)
            src_t = np.where(wall, np.clip((z - lof[si, sj]) / np.maximum(dropf[si, sj], 1.0), 0, 1), tp)
            top = np.where(wall, topf[si, sj], ownp)
            foot = np.where(wall, footf[si, sj], ownp)
            # The foot's biome low on the wall, the top's above (the line wanders).
            SF[n] = np.where(wall, 1.0 - np.clip((src_t + fray - cr.get("foot_share", 0.14)) / 0.12 + 0.5, 0, 1), 1.0)
            walled |= wall & (w > -s)
        else:
            SF[n] = 1.0
        F[n] = np.maximum(gt, w)
        T[n] = src_t
        TOP[n] = top
        FOOT[n] = foot
    keep_col = walled | (liftp > 1.0)
    keep_col = ndimage.binary_dilation(keep_col, iterations=2)
    if not keep_col[SHIFT:SHIFT + per, SHIFT:SHIFT + per].any():
        return None
    keep_cell = keep_col[:-1, :-1] | keep_col[1:, :-1] | keep_col[:-1, 1:] | keep_col[1:, 1:]

    # The tile's two biomes: those most of its surface wears.
    near = np.abs(F) < s
    near[:, :SHIFT] = near[:, -SHIFT:] = False
    near[:, :, :SHIFT] = near[:, :, -SHIFT:] = False
    tally = {}
    for ids, wgt in ((TOP, 1.0 - SF), (FOOT, SF)):
        sel = near & (wgt > 0)
        if sel.any():
            vals = ids[sel]
            ws = wgt[sel]
            for b in np.unique(vals):
                if b == 0:
                    continue   # the sea: no stone of its own
                tally[int(b)] = tally.get(int(b), 0.0) + float(ws[vals == b].sum())
    order = sorted(tally, key=lambda b: -tally[b])
    if not order:
        return None
    A = order[0]
    Bb = order[1] if len(order) > 1 and tally[order[1]] > 0.02 * sum(tally.values()) else A
    third = sum(tally[b] for b in order[2:]) / max(sum(tally.values()), 1e-6)
    WB = (1.0 - SF) * (TOP == Bb) + SF * (FOOT == Bb) if Bb != A else np.zeros(shape, np.float32)

    grads = np.gradient(F, dz, s, s)   # along z, y, x
    levels = []
    for level in range(3):
        m = 1 << level
        sl = (slice(None, None, m), slice(SHIFT - m, SHIFT + per + m + 1, m), slice(SHIFT - m, SHIFT + per + m + 1, m))
        Fl = F[sl]
        Xl, Yl, Zl = Xp[sl[2]], Yp[sl[1]], Z[sl[0]]
        kc = keep_col[sl[1], sl[2]]
        kcell = kc[:-1, :-1] | kc[1:, :-1] | kc[:-1, 1:] | kc[1:, 1:]
        verts, cells, quads = _surface_nets(Fl, Xl, Yl, Zl, kcell)
        if not len(quads):
            levels.append(None)
            continue
        # Attributes from the finest grid, trilinear at each vertex.
        fk = (verts[:, 2] - Z[0]) / dz
        fi = (verts[:, 1] - Yp[0]) / s
        fj = (verts[:, 0] - Xp[0]) / s
        coords = [fk, fi, fj]
        gz, gyy, gxx = (ndimage.map_coordinates(g_, coords, order=1, mode="nearest") for g_ in grads)
        N = -np.stack([gxx, gyy, gz], 1)
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
        wb = np.clip(ndimage.map_coordinates(WB.astype(np.float32), coords, order=1, mode="nearest"), 0, 1)
        tv = np.clip(ndimage.map_coordinates(T, coords, order=1, mode="nearest"), 0, 1)
        # Two triangles a quad, along its shorter diagonal.
        a, b, cc, d = quads.T
        d1 = np.linalg.norm(verts[a] - verts[cc], axis=1)
        d2 = np.linalg.norm(verts[b] - verts[d], axis=1)
        alt = d2 < d1
        tris = np.where(alt[:, None, None],
                        np.stack([np.stack([a, b, d], 1), np.stack([b, cc, d], 1)], 1),
                        np.stack([np.stack([a, b, cc], 1), np.stack([a, cc, d], 1)], 1)).reshape(-1, 3)
        # Wound counter-clockwise seen from outside.
        e1 = verts[tris[:, 1]] - verts[tris[:, 0]]
        e2 = verts[tris[:, 2]] - verts[tris[:, 0]]
        fn = np.cross(e1, e2)
        flip = (fn * (N[tris[:, 0]] + N[tris[:, 1]] + N[tris[:, 2]])).sum(1) < 0
        tris[flip] = tris[flip][:, ::-1]
        tris = tris[np.linalg.norm(fn, axis=1) > 1e-6]
        # What lies wholly under the ground is never seen: left out.
        buried = verts[:, 2] < _sample(f["h"], verts[:, 0], verts[:, 1], c, origin, 1) - 0.5 * sink
        tris = tris[~buried[tris].all(1)]
        attrs = [N, wb, tv]
        P = verts
        for axis, value, above in ((0, x0, True), (0, x0 + TILE_M, False), (1, y0, True), (1, y0 + TILE_M, False)):
            P, attrs, tris = _clip(P, attrs, tris, axis, value, above)
        if not len(tris):
            levels.append(None)
            continue
        used = np.unique(tris)
        remap = np.full(len(P), -1, np.int64)
        remap[used] = np.arange(len(used))
        tris = remap[tris]
        N, wb, tv = (x[used] for x in attrs)
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
        Pl = P[used] - np.array([x0, y0, 0.0])
        under = np.clip(-N[:, 2], 0, 1)
        ao = np.clip(1 - 0.5 * under, 0.35, 1)
        UV = np.stack([ao, np.clip(tv, 0.1, 1)], 1)
        levels.append(_skirt(Pl, N, UV, wb, tris, cr["skirt_m"]))
    if levels[0] is None:
        return None
    for level in (1, 2):
        if levels[level] is None:
            # Too coarse to hold any of it: the finer level stands in.
            levels[level] = levels[level - 1]
    return levels, (A, Bb), third


def _block(job):
    """One block of tiles (a worker's job): its fields, then its tiles."""
    h, zones, biomes, origin, tiles, c, rules, out_dir = job
    cr = rules["cliffs"]
    f = fields(h, zones, biomes, c, rules, origin)
    if not f["band"].any():
        return [], 0, 0, 0
    out_dir = Path(out_dir)
    results, triangles, vertices, mixed = [], 0, 0, 0
    for ti, tj in tiles:
        made = tile(f, ti, tj, origin, c, cr)
        if made is None:
            continue
        levels, (A, B), third = made
        name = f"cliff_{ti}_{tj}"
        for level, (P, N, UV, W, tris) in enumerate(levels):
            _write_mesh(out_dir / f"{name}_lod{level}.mesh", P, N, UV, W, tris.ravel())
        triangles += len(levels[0][4])
        vertices += len(levels[0][0])
        mixed += third > 0.1
        results.append({"m": name, "dir": out_dir.name, "x": round(tj * TILE_M, 1), "y": round(ti * TILE_M, 1),
                        "z": 0.0, "yaw": 0.0, "s": [1.0, 1.0, 1.0], "biomes": [int(A), int(B)], "detail": True,
                        "cat": "terrain"})
    return results, triangles, vertices, mixed


def load_biomes(path, n):
    """Biome ids per texel of an n*n map, y up (row 0 = south)."""
    ids = np.asarray(Image.open(path))[::-1]
    k = n // ids.shape[0]
    return ids, k


def build(world, rules, out_dir, rng=None, biomes_path=None, workers=None):
    """Writes the tiles, returns their instances. Works through the map in
    blocks (with a halo wider than any reach here), in parallel."""
    cr = rules["cliffs"]
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("cliff_*.mesh"):
        old.unlink()
    if biomes_path is None:
        stem = str(out_dir)
        biomes_path = (stem[:-len("_cliffs")] if stem.endswith("_cliffs") else stem) + "_biomes.png"
    bmap, bk = load_biomes(biomes_path, world.n)
    c = world.size / world.n
    per_tile = int(TILE_M / c)
    zk = world.n // world.zones.shape[0]
    block = 16 * per_tile
    halo = int(math.ceil((MARGIN_M + 2.4 * cr["max_push_m"] + 600.0) / c))
    jobs = []
    for bi in range(0, world.n, block):
        for bj in range(0, world.n, block):
            r0, r1 = max(bi - halo, 0), min(bi + block + halo, world.n)
            c0, c1 = max(bj - halo, 0), min(bj + block + halo, world.n)
            zsub = world.zones[r0 // zk:(r1 + zk - 1) // zk, c0 // zk:(c1 + zk - 1) // zk]
            if not np.isin(zsub, list(cr["zones"])).any():
                continue
            zones = np.repeat(np.repeat(zsub, zk, 0), zk, 1)[r0 % zk:r0 % zk + r1 - r0, c0 % zk:c0 % zk + c1 - c0]
            bsub = bmap[r0 // bk:(r1 + bk - 1) // bk, c0 // bk:(c1 + bk - 1) // bk]
            biomes = np.repeat(np.repeat(bsub, bk, 0), bk, 1)[r0 % bk:r0 % bk + r1 - r0, c0 % bk:c0 % bk + c1 - c0]
            tiles = [(ti, tj) for ti in range(bi // per_tile, (bi + block) // per_tile)
                     for tj in range(bj // per_tile, (bj + block) // per_tile)]
            jobs.append((world.h[r0:r1, c0:c1].copy(), zones, biomes, (r0, c0), tiles, c, rules, str(out_dir)))
    start = time.time()
    workers = workers or max(1, min(len(jobs), (os.cpu_count() or 4) - 2))
    instances, triangles, vertices, mixed = [], 0, 0, 0
    with multiprocessing.get_context("spawn").Pool(workers) as pool:
        for res, tr, vt, mx in pool.imap_unordered(_block, jobs):
            instances += res
            triangles += tr
            vertices += vt
            mixed += mx
    instances.sort(key=lambda o: (o["y"], o["x"]))
    print(f"cliffs: {len(instances)} tiles, {triangles / 1e6:.2f} M triangles, {vertices / 1e6:.2f} M vertices at "
          f"{cr['grid_m']} m, {mixed} tiles with a third biome; {time.time() - start:.0f} s on {workers} workers")
    return instances


def main():
    import sys
    sys.path.insert(0, str(Path(__file__).parent))
    import world_features
    ap = argparse.ArgumentParser(description="Rebuild only the cliff walls and put them into a features file.")
    ap.add_argument("--world", default=str(world_features.ROOT / "terrain_lab_out" / "world"))
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--features", help="the features file to update (default <world>_features.json)")
    ap.add_argument("--out", help="write the features here (its cliffs beside it) instead of updating in place")
    ap.add_argument("--workers", type=int)
    args = ap.parse_args()
    rules = json.loads(world_features.RULES.read_text(encoding="utf-8"))
    world = world_features.World(args.world, args.height_range, args.sea)
    src = Path(args.features or args.world + "_features.json")
    out = Path(args.out) if args.out else src
    cliffs_dir = out.parent / (Path(args.world).name + "_cliffs")
    items = [o for o in json.loads(src.read_text())["instances"] if not str(o.get("m", "")).startswith("cliff_")]
    cliffs = build(world, rules, cliffs_dir, biomes_path=args.world + "_biomes.png", workers=args.workers)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"world_m": world.size, "instances": items + cliffs}, separators=(",", ":")))
    print(f"{len(items)} other instances kept, {len(cliffs)} cliff tiles -> {out}")


if __name__ == "__main__":
    main()
