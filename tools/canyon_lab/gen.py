"""The canyon's relief on a small patch (user 15.10: a biome worked out in a
light scene first). Order, as the land itself went:

1. old rivers: a priority flood on a coarse tilted rumpled surface gives
   the dry river network; valleys are as wide as sqrt(draining area);
2. drought: cracks along two warped Voronoi nets break the banks into
   islands (the fine net only near the valleys, where crumbling began);
3. what is too narrow to walk on is cut off the islands and becomes stone
   columns, or worn down to peaks;
4. the islands rise in sheer steps (terraces), not slopes: rings of distance
   from the edge, each ring one wall; levels then snapped to broken slabs;
5. the floor: low stone shelves, peaks round the feet of the islands, talus
   of fallen blocks under the walls, dry stream beds, fans at their mouths;
6. burnt grass only in the hearts of the biggest islands.

Arrays are [row, col], row 0 at the top of the pictures; heights in metres
over the patch's lowest floor.
"""

import heapq

import numpy as np
import scipy.ndimage as nd

from noise import fbm, unit, warp_coords, voronoi, pair_border, stamp_cones


def edt(mask, cell, **kw):
    out = nd.distance_transform_edt(mask, **kw)
    if isinstance(out, tuple):
        return (out[0] * cell,) + out[1:]
    return out * cell


def rivers(P, rng, shape, cell, W=None):
    R = P["rivers"]
    cc = R["coarse_cell_m"]
    ny, nx = shape
    my, mx = max(2, int(round(ny * cell / cc))), max(2, int(round(nx * cell / cc)))
    sky, skx = ny / my, nx / mx
    if W is None:
        yy = np.linspace(0.0, 1.0, my)[:, None]
        surf = R["tilt_m"] * (1.0 - yy) + R["rumple_m"] * fbm(rng, (my, mx), cc, R["rumple_scale_m"], 4)
    else:
        # In the world: the land as it lay, a little rumpled.
        surf = nd.zoom(W["surf"], (my / ny, mx / nx), order=1)[:my, :mx]
        surf = surf + R["rumple_m"] * fbm(rng, (my, mx), cc, R["rumple_scale_m"], 4)
    surf = surf.astype(np.float64)
    tie = fbm(rng, (my, mx), cc, R["rumple_scale_m"] * 0.5, 2).ravel()
    # Priority flood from the bottom edge (in the world: from the canyon's
    # rim, wherever it lies lowest): every cell learns where it drains.
    filled = surf.copy()
    recv = np.full(my * mx, -1, np.int64)
    if W is None:
        seen = np.zeros((my, mx), bool)
        seen[-1, :] = True
        starts = (my - 1) * mx + np.arange(mx)
    else:
        inside = nd.zoom(W["mask"].astype(np.float32), (my / ny, mx / nx), order=1)[:my, :mx] > 0.5
        seen = ~inside
        starts = np.flatnonzero(seen & nd.binary_dilation(inside, structure=np.ones((3, 3))))
    heap = [(float(filled.flat[i]), float(tie[i]), int(i)) for i in starts]
    heapq.heapify(heap)
    order = []
    nbr = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    while heap:
        z, _, i = heapq.heappop(heap)
        y0, x0 = divmod(i, mx)
        for dy, dx in nbr:
            y, x = y0 + dy, x0 + dx
            if 0 <= y < my and 0 <= x < mx and not seen[y, x]:
                seen[y, x] = True
                j = y * mx + x
                filled.flat[j] = max(filled.flat[j], z + 1e-3)
                recv[j] = i
                order.append(j)
                heapq.heappush(heap, (float(filled.flat[j]), float(tie[j]), j))
    acc = np.full(my * mx, cc * cc / 1e6)
    for j in reversed(order):
        acc[recv[j]] += acc[j]
    chan = acc >= R["min_km2"]

    # Centre lines drawn on the fine grid, each pixel tagged with its area.
    cl = np.zeros((ny, nx), np.float32)
    ctr = lambda j: ((j // mx + 0.5) * sky, (j % mx + 0.5) * skx)
    fans = []
    for j in np.flatnonzero(chan):
        r = recv[j]
        a = ctr(j)
        b = ctr(r) if r >= 0 else (a[0] + sky, a[1])
        t = np.linspace(0, 1, int(max(sky, skx) * 1.5) + 2)
        py = np.clip((a[0] + (b[0] - a[0]) * t).astype(int), 0, ny - 1)
        px = np.clip((a[1] + (b[1] - a[1]) * t).astype(int), 0, nx - 1)
        cl[py, px] = np.maximum(cl[py, px], acc[j])
        # A small stream reaching a much bigger one leaves a fan where its
        # narrow valley opens onto the big one's floor.
        if r >= 0 and acc[j] <= R["fans"]["max_km2"] and acc[r] >= R["fans"]["ratio"] * acc[j]:
            fans.append((a, b, acc[j], acc[r]))
    dist, (iy, ix) = edt(cl == 0, cell, return_indices=True)
    at = warp_coords(rng, shape, cell, R["warp_scale_m"], R["warp_m"])
    dist = nd.map_coordinates(dist.astype(np.float32), at, order=1)
    ia, ja = np.rint(at[0]).astype(int), np.rint(at[1]).astype(int)
    area = cl[iy[ia, ja], ix[ia, ja]]
    del iy, ix
    half = np.clip(0.5 * R["width_per_sqrtkm2_m"] * np.sqrt(area), *(np.array(R["width_m"]) * 0.5))
    half = half * (1.0 + R["wander"] * fbm(rng, shape, cell, R["wander_scale_m"], 3))
    bed_w = np.clip(R["bed_w_per_sqrtkm2_m"] * np.sqrt(area), *R["bed_w_m"])
    # The floor: the flooded surface, smooth, dropping gently downstream.
    if W is None:
        f = filled.reshape(my, mx)
        f = (f - f.min()) / max(1e-6, np.ptp(f))
        floor = nd.zoom(f, (ny / my, nx / mx), order=1)[:ny, :nx] * R["floor_drop_m"]
        floor = nd.gaussian_filter(floor, 60.0 / cell).astype(np.float32)
    else:
        floor = W["floor"].astype(np.float32)
    return dict(dist=dist, half=half, area=area, bed_w=bed_w, floor=floor, fans=fans,
                acc=acc.reshape(my, mx), chan=chan.reshape(my, mx))


def cracks(P, rng, n, cell, valley):
    to_valley = edt(~valley, cell)
    crack = np.zeros(valley.shape, bool)
    for lv in P["cracks"]:
        at = warp_coords(rng, n, cell, lv["spacing_m"] * 0.6, lv["warp_m"])
        lab, _, _ = voronoi(rng, n, cell, lv["spacing_m"], at)
        border, hsh = pair_border(lab)
        line = border & (hsh < lv["open"])
        if lv.get("near_m"):
            u = unit(fbm(rng, n, cell, lv["spacing_m"], 2))
            line &= to_valley < lv["near_m"] * (0.5 + u)
        d = edt(~line, cell)
        u = unit(fbm(rng, n, cell, lv["spacing_m"] * 0.4, 2))
        half = 0.5 * (lv["width_m"][0] + (lv["width_m"][1] - lv["width_m"][0]) * u)
        crack |= d < half
    return crack & ~valley


def generate(P, seed, world=None):
    """`world` (tools/terrain_lab/canyon_relief.py: the canyon cut into the
    world): mask (rows, cols) inside the canyon, surf (the land the rivers ran
    off, metres), floor (the canyon floor's level, metres), edge_m (islands
    keep that far off the mask's edge). Without it a square of `size_m`."""
    rng = np.random.default_rng(seed)
    cell = P["cell_m"]
    if world is None:
        n = int(round(P["size_m"] / cell))
        shape = (n, n)
    else:
        shape = world["mask"].shape
        n = shape
    ny, nx = shape
    out = dict(n=ny, nx=nx, cell=cell)

    # --- 1. old rivers -----------------------------------------------------
    rv = rivers(P, rng, shape, cell, world)
    valley = rv["dist"] < rv["half"]
    if world is not None:
        # (a strip of floor all round inside the rim: the canyon meets its
        # neighbours on its floor, no island cut off by the edge)
        valley |= edt(world["mask"], cell) < world["edge_m"]
    floor = rv["floor"]

    # --- 2. drought cracks -------------------------------------------------
    crack = cracks(P, rng, n, cell, valley)
    high0 = ~valley & ~crack
    high0 = nd.binary_opening(high0, iterations=1)
    # The scarps retreated: every bank wore back by a wandering distance,
    # so the plateau falls apart into islands standing on the canyon floor.
    Rt = P["retreat"]
    e_h = edt(high0, cell)
    ret = Rt["m"] * (Rt["min_k"] + (2 - 2 * Rt["min_k"]) * unit(fbm(rng, n, cell, Rt["scale_m"], 3)))
    core = e_h > ret

    # --- 3. too narrow to walk on: cut off as remnants ----------------------
    I = P["islands"]
    rw = I["walk_r_m"] / cell
    inner = nd.distance_transform_edt(core) > rw
    opened = (nd.distance_transform_edt(~inner) <= rw) & core
    lab, ni = nd.label(opened, structure=np.ones((3, 3)))
    idx = np.arange(1, ni + 1)
    area = nd.sum(np.ones_like(lab), lab, idx) * cell * cell
    e_o = edt(opened, cell)
    rmax = nd.maximum(e_o, lab, idx)
    small = area < I["min_area_m2"]
    # Small islands go with the remnants.
    keep_isl = np.concatenate([[False], ~small])
    opened = keep_isl[lab]
    lab = np.where(opened, lab, 0)
    rem = core & ~opened
    # Only real pieces count (not the slivers the opening shaves off corners).
    rl, nrl = nd.label(rem, structure=np.ones((3, 3)))
    ra = nd.sum(np.ones_like(rl), rl, np.arange(1, nrl + 1)) * cell * cell
    rem &= np.concatenate([[False], ra >= P["remnants"]["min_area_m2"]])[rl]
    # Ridges that wore away entirely may leave a stump.
    vanished = high0 & ~core & (e_h == nd.maximum_filter(e_h, size=9)) & (e_h >= P["remnants"]["col_min_r_m"])

    # --- 4. the islands' heights: terraces of sheer steps -------------------
    # (in the world the tops are the land as it lay: `top` over the floor)
    top_base = I["top_m"] if world is None or "top" not in world else world["top"]
    top_field = top_base + I["top_var_m"] * fbm(rng, n, cell, I["top_scale_m"], 2)
    cy = nd.center_of_mass(np.ones_like(lab), lab, idx)
    K = I["max_rings"]
    S = np.zeros((ni + 1, K), np.float32)       # step heights per ring
    W = np.full((ni + 1, K), 1e9, np.float32)   # ring start distance
    H = np.zeros(ni + 1, np.float32)
    islands = []
    for k in range(ni):
        if small[k]:
            continue
        i = k + 1
        yk, xk = int(cy[k][0]), int(cy[k][1])
        rk = float(rmax[k])
        drop = rng.choice(len(I["drop_weights"]), p=I["drop_weights"])
        if rk < I["low_r_m"]:
            drop = min(drop + 1, len(I["drop_weights"]) - 1)
        h_top = max(I["min_top_m"], float(top_field[yk, xk]) - drop * I["tier_m"])
        nr_max = int(np.clip(rk / I["ring_min_m"] + 1, 1, K))
        nr = 1 if (rng.random() < I["sheer_only_p"] or nr_max < 2) else int(rng.integers(2, nr_max + 1))
        # Lowest wall the tallest, the upper steps shorter.
        wts = np.sort(rng.dirichlet(np.ones(nr) * 2.5))[::-1]
        while nr > 1 and (wts * h_top).min() < I["min_step_m"]:
            nr -= 1
            wts = np.sort(rng.dirichlet(np.ones(nr) * 2.5))[::-1]
        steps = wts * h_top
        ws = rng.uniform(*I["ring_w_m"], nr - 1)
        ws = ws * min(1.0, 0.7 * rk / max(1.0, ws.sum()))
        S[i, :nr] = steps
        W[i, 0] = 0.0
        W[i, 1:nr] = np.cumsum(ws)
        H[i] = h_top
        islands.append(dict(id=i, area_m2=float(area[k]), r_max_m=rk, top_m=h_top, rings=nr,
                            y=yk, x=xk))
    mod = 1.0 + I["ring_noise"] * fbm(rng, n, cell, I["ring_scale_m"], 3)
    d = e_o + I["edge_jit_m"] * fbm(rng, n, cell, I["edge_jit_scale_m"], 3)
    rel_isl = np.zeros(shape, np.float32)
    for r in range(K):
        rel_isl += S[lab, r] * (d > W[lab, r] * mod)
    rel_isl *= opened
    rel_isl += opened * I["top_rough_m"] * fbm(rng, n, cell, 50.0, 3)

    # --- the remnants: columns, or worn to peaks ----------------------------
    Rm = P["remnants"]
    e_r = nd.distance_transform_edt(rem) * cell
    near_isl = nd.distance_transform_edt(lab == 0, return_distances=False, return_indices=True)
    parent = lab[near_isl[0], near_isl[1]]
    del near_isl
    mx = (e_r == nd.maximum_filter(e_r, size=5)) & rem & (e_r >= Rm["min_r_m"])
    ay, ax = np.nonzero(mx)
    by, bx = np.nonzero(vanished)
    stump = rng.random(by.size) < Rm["stump_p"]
    by, bx = by[stump], bx[stump]
    rb = np.minimum(e_h[by, bx], Rm["stump_r_max_m"]) * rng.uniform(0.45, 0.85, by.size)
    cy_ = np.concatenate([ay, by])
    cx_ = np.concatenate([ax, bx])
    cr_ = np.concatenate([e_r[ay, ax], rb])
    order_ = np.argsort(-cr_)
    taken = np.zeros(shape, bool)
    columns, peaks = [], []
    cones_col, cones_peak = [], []
    for o in order_:
        y, x = cy_[o], cx_[o]
        if taken[y, x]:
            continue
        r = float(cr_[o])
        sp = max(Rm["spacing_k"] * r, Rm["spacing_min_m"]) / cell
        y0, y1 = int(max(0, y - sp)), int(min(ny, y + sp + 1))
        x0, x1 = int(max(0, x - sp)), int(min(nx, x + sp + 1))
        yy, xx = np.mgrid[y0:y1, x0:x1]
        taken[y0:y1, x0:x1] |= (yy - y) ** 2 + (xx - x) ** 2 <= sp * sp
        hp = H[parent[y, x]] if parent[y, x] > 0 else float(np.mean(top_base))
        if r >= Rm["col_min_r_m"] and rng.random() > Rm["col_to_peak"]:
            hc = max(I["min_top_m"], hp - rng.choice([0, 0, 1]) * I["tier_m"] + rng.uniform(-6, 4))
            rc = min(r * Rm["col_r_k"], I["walk_r_m"] * 0.9)
            cones_col.append((y, x, rc, hc, Rm["col_power"]))
            columns.append(dict(y=int(y), x=int(x), r_m=rc, h_m=float(hc)))
        else:
            hc = hp * rng.uniform(*Rm["peak_h_frac"])
            rc = max(r * Rm["peak_r_k"], 6.0)
            cones_peak.append((y, x, rc, hc, rng.uniform(*Rm["peak_power"])))
            peaks.append(dict(y=int(y), x=int(x), r_m=rc, h_m=float(hc), kind="remnant"))

    # --- 5. the floor -------------------------------------------------------
    F = P["floor"]
    high_any = opened | rem
    d_out = edt(~high_any, cell)
    # Low stone shelves, away from the beds.
    q = unit(fbm(rng, n, cell, F["shelf_scale_m"], 3))
    lev = np.floor(np.clip((q - (1 - F["shelf_share"])) / F["shelf_share"], 0, 0.999) * F["shelf_levels"] + (q > 1 - F["shelf_share"]))
    # (the gap wanders, so no shelf edge runs alongside a bed like a canal)
    clear = rv["dist"] > rv["bed_w"] + F["shelf_gap_m"] * (0.4 + 3.0 * unit(fbm(rng, n, cell, 150.0, 3)))
    rel_shelf = (lev * F["shelf_step_m"] * clear).astype(np.float32)
    # Broken stone benches at the islands' feet: what the retreating walls
    # left standing a step or two over the floor, not on every side.
    Sk = F["skirt"]
    sw = Sk["w_m"] * (0.2 + 1.6 * unit(fbm(rng, n, cell, Sk["scale_m"], 3)))
    gate = unit(fbm(rng, n, cell, Sk["scale_m"] * 1.5, 2)) > 1 - Sk["share"]
    skirt = np.where(d_out < 0.5 * sw, Sk["h_m"][1], np.where(d_out < sw, Sk["h_m"][0], 0.0))
    skirt = skirt * gate * ~high_any * (rv["dist"] > rv["bed_w"] + F["shelf_gap_m"])
    rel_shelf = np.maximum(rel_shelf, skirt.astype(np.float32))

    # Slabs: the blocky levels snapped to broken slabs (walls follow the
    # slabs' edges, never a smooth contour).
    Sb = P["slabs"]
    at = warp_coords(rng, n, cell, Sb["size_m"] * 3, Sb["warp_m"])
    slab, sy, sx = voronoi(rng, n, cell, Sb["size_m"], at)
    blocky = np.maximum(rel_isl, rel_shelf)
    seed_val = np.concatenate([[0.0], blocky[sy, sx]]).astype(np.float32)
    # A slab takes the level at its own seed; slabs of an island's edge
    # whose seed fell off it break away entirely.
    blocky_q = seed_val[slab]
    # Hold the cracks and valleys open: a slab may not bridge them.
    blocky_q = np.where(opened | (rel_shelf > 0) | (blocky_q < F["shelf_step_m"] * 0.5), blocky_q,
                        np.minimum(blocky_q, rel_shelf))
    # No one-cell pits or needles (the warped lookup strays single cells).
    blocky_q = nd.grey_opening(nd.grey_closing(blocky_q, size=3), size=3)
    # Pockets of floor shut in by walls (unreachable on foot) silted up to a
    # little under their lowest rim; the big ones are left as basins.
    low = blocky_q < 1
    pl, npl = nd.label(low, structure=np.ones((3, 3)))
    if npl > 1:
        pid = np.arange(1, npl + 1)
        pa = nd.sum(np.ones_like(pl), pl, pid) * cell * cell
        main_ = pa.argmax() + 1
        ring = nd.grey_dilation(pl, size=3) * ~low
        rim = nd.minimum(blocky_q, ring, pid)
        fill = (pa < F["pocket_fill_m2"]) & (pid != main_)
        lvl = np.concatenate([[0.0], np.where(fill, np.maximum(0, rim - np.where(pa < 3000, 0.0, 3.0)), 0.0)]).astype(np.float32)
        blocky_q = np.where(low, lvl[pl], blocky_q)
    # The floor in one piece: a basin of floor cut off by benches gets a
    # gap broken through the benches towards the nearest open floor (never
    # through an island).
    # (connected only through passages a man passes: the floor shrunk by
    # a few metres first, so a diagonal crack between two benches is no way)
    low = nd.binary_erosion(blocky_q < 1, iterations=max(1, int(3.0 / cell)))
    pl, npl = nd.label(low, structure=np.ones((3, 3)))
    if npl > 1:
        pa = np.bincount(pl.ravel())[1:]
        main_ = pa.argmax() + 1
        dmain, (my, mx_) = nd.distance_transform_edt(pl != main_, return_indices=True)
        gap = P["floor"]["gap_m"] / cell
        for c in np.flatnonzero(pa * cell * cell >= F["pocket_fill_m2"]) + 1:
            if c == main_:
                continue
            m = pl == c
            k = np.argmin(np.where(m, dmain, np.inf))
            y0, x0 = divmod(k, nx)
            y1, x1 = my[y0, x0], mx_[y0, x0]
            t = np.linspace(0, 1, int(dmain[y0, x0]) + 2)
            ly, lx = (y0 + (y1 - y0) * t).astype(int), (x0 + (x1 - x0) * t).astype(int)
            if blocky_q[ly, lx].max() >= 20:
                continue
            # (in a window round the line: the same cut, not a whole-map pass)
            g2 = int(np.ceil(gap / 2)) + 2
            wy0, wy1 = max(0, ly.min() - g2), min(ny, ly.max() + g2 + 1)
            wx0, wx1 = max(0, lx.min() - g2), min(nx, lx.max() + g2 + 1)
            cut = np.zeros((wy1 - wy0, wx1 - wx0), bool)
            cut[ly - wy0, lx - wx0] = True
            cut = nd.distance_transform_edt(~cut) < gap / 2
            win_ = blocky_q[wy0:wy1, wx0:wx1]
            blocky_q[wy0:wy1, wx0:wx1] = np.where(cut & (win_ < 20), 0.0, win_)
    # Some slabs dropped by a fraction of a step (broken tops).
    dropk = np.concatenate([[0.0], rng.uniform(0, 1, sy.size)]).astype(np.float32)[slab]
    blocky_q -= np.where(dropk < Sb["drop_p"], Sb["drop_m"] * (0.4 + dropk / Sb["drop_p"]), 0) * (blocky_q > 20) * (e_o < Sb["drop_band_m"])

    # Peaks of the floor, crowding round the islands' feet, off the beds.
    Fp = F["peaks"]
    area_km2 = ny * nx * (cell / 1000.0) ** 2
    if world is not None:
        area_km2 *= float(world["mask"].mean())
    m_ = int(Fp["per_km2"] * area_km2 * 4)
    py = rng.uniform(0, ny, m_).astype(int)
    px = rng.uniform(0, nx, m_).astype(int)
    ok = ~high_any[py, px] & (rv["dist"][py, px] > rv["bed_w"][py, px] + Fp["bed_gap_m"])
    if world is not None:
        ok &= world["mask"][py, px]
    w = Fp["base"] + Fp["near_bonus"] * np.exp(-d_out[py, px] / Fp["near_m"])
    w = w * (d_out[py, px] > Fp["min_off_m"])
    acc_ = ok & (rng.random(m_) < w / (Fp["base"] + Fp["near_bonus"]))
    py, px = py[acc_], px[acc_]
    keep = min(py.size, int(Fp["per_km2"] * area_km2))
    for y, x in zip(py[:keep], px[:keep]):
        hp = rng.uniform(*Fp["h_m"])
        rp = rng.uniform(*Fp["r_m"])
        cones_peak.append((y, x, rp, hp, rng.uniform(*Fp["power"])))
        peaks.append(dict(y=int(y), x=int(x), r_m=rp, h_m=hp, kind="floor"))
        for _ in range(rng.integers(Fp["satellites"][0], Fp["satellites"][1] + 1)):
            a = rng.uniform(0, 6.283)
            dd = rp * rng.uniform(1.4, 3.0) / cell
            ys, xs = int(y + dd * np.sin(a)), int(x + dd * np.cos(a))
            if 0 <= ys < ny and 0 <= xs < nx and not high_any[ys, xs]:
                hs, rs = hp * rng.uniform(0.3, 0.7), rp * rng.uniform(0.5, 0.8)
                cones_peak.append((ys, xs, rs, hs, rng.uniform(*Fp["power"])))
                peaks.append(dict(y=ys, x=xs, r_m=rs, h_m=hs, kind="satellite"))
    rel_col = stamp_cones(np.zeros(shape, np.float32), cones_col, cell, rng)
    rel_peak = stamp_cones(np.zeros(shape, np.float32), cones_peak, cell, rng)

    # Talus of fallen blocks under the walls (a fraction of the wall's height).
    T = F["talus"]
    # (piled on whatever lies at the wall's foot - floor or a bench - as
    # deep as a share of the wall standing over it, never up to its top)
    tall = (blocky_q > T["wall_from_m"]) | (rel_col > 6)
    d_t, (ty, tx) = edt(~tall, cell, return_indices=True)
    drop = np.maximum(blocky_q, rel_col)[ty, tx] - blocky_q
    a = drop * T["frac"] * (2 * unit(fbm(rng, n, cell, T["scale_m"], 3)))
    a = np.minimum(np.minimum(a, T["max_m"]), drop - T["clear_m"])
    # (smoothed: where the nearest wall switches from one to another the
    # pile would step by metres - a seam no one could walk over)
    a = nd.gaussian_filter(np.maximum(a, 0), T["smooth_m"] / cell)
    talus = np.clip(a - d_t * np.tan(np.radians(T["slope_deg"])), 0, None)
    bl, bsy, _ = voronoi(rng, n, cell, T["block_m"])
    bump = np.concatenate([[0.0], rng.uniform(0, 1, bsy.size)]).astype(np.float32)[bl]
    talus = np.where(talus > 0.5, talus + T["block_h_m"] * bump * np.minimum(1, talus / 4), talus)
    talus = talus * ~tall

    # Fans where the small dry streams open out.
    Fa = P["rivers"]["fans"]
    rel_fan = np.zeros(shape, np.float32)
    fans = []
    for a_, b_, A, Ab in rv["fans"]:
        u = np.array([b_[0] - a_[0], b_[1] - a_[1]])
        u /= np.linalg.norm(u) + 1e-9
        hw_trunk = 0.5 * P["rivers"]["width_per_sqrtkm2_m"] * np.sqrt(Ab)
        hw_trunk = min(hw_trunk, P["rivers"]["width_m"][1] * 0.5)
        apex = np.array(b_) - u * hw_trunk * 0.85 / cell
        R_ = np.interp(A, [P["rivers"]["min_km2"], Fa["max_km2"]], Fa["radius_m"]) / cell
        hf = rng.uniform(*Fa["height_m"])
        y0, y1 = int(max(0, apex[0] - R_)), int(min(ny, apex[0] + R_ + 1))
        x0, x1 = int(max(0, apex[1] - R_)), int(min(nx, apex[1] + R_ + 1))
        if y0 >= y1 or x0 >= x1:
            continue
        yy, xx = np.mgrid[y0:y1, x0:x1]
        dy, dx = yy - apex[0], xx - apex[1]
        dd = np.hypot(dy, dx)
        cosang = (dy * u[0] + dx * u[1]) / (dd + 1e-6)
        sector = np.clip((cosang - 0.2) / 0.5, 0, 1)
        fh = hf * np.clip(1 - dd / R_, 0, 1) ** 1.4 * sector
        rel_fan[y0:y1, x0:x1] = np.maximum(rel_fan[y0:y1, x0:x1], fh)
        fans.append(dict(y=float(apex[0]), x=float(apex[1]), r_m=float(R_ * cell), h_m=float(hf)))
    rel_fan *= ~high_any

    # Dry beds cut into the floor.
    # (a shallow bed: never deeper than a quarter of its width, so it stays
    # a dry wash to walk across, not a trench)
    inc = np.minimum(F["bed_depth_m"], 0.25 * rv["bed_w"]) * np.clip(1 - (rv["dist"] / rv["bed_w"]) ** 2, 0, 1)
    wash = (rv["dist"] < rv["bed_w"] * 1.25) & ~high_any

    # Columns and peaks are meshes (stage 3): the ground keeps only their
    # talus, unless `stamp` asks for the stand-in cones.
    if Rm.get("stamp", True):
        rel = np.maximum.reduce([blocky_q + talus, rel_col, rel_peak, rel_fan])
    else:
        rel = np.maximum(blocky_q + talus, rel_fan)
    floor_n = F["floor_rough_m"] * (fbm(rng, n, cell, 80.0, 3) + 0.6 * np.abs(fbm(rng, n, cell, 25.0, 2)))
    # (the floor's roll under everything, the beds fading out under talus and
    # fans: no seam where a pile of scree starts)
    h = floor + floor_n * np.clip(1 - rel / 8.0, 0, 1) - inc * np.clip(1 - rel / 2.0, 0, 1) + rel
    if world is None:
        h -= h.min()

    # --- 6. burnt grass in the hearts of the biggest islands ----------------
    G = P["grass"]
    # (in the world as many as on so much of the sandbox's patch)
    n_big = G["islands"] if world is None else max(1, int(round(G["islands"] * area_km2 / 16.78)))
    big = sorted(islands, key=lambda s: -s["area_m2"])[:n_big]
    grass = np.zeros(shape, bool)
    gn = unit(fbm(rng, n, cell, G["scale_m"], 3))
    for s in big:
        i = s["id"]
        core = (lab == i) & (e_o > G["core"] * s["r_max_m"]) & (blocky_q >= H[i] - 6)
        grass |= core & (gn > 1 - G["cover"])
        s["grass"] = True

    out.update(h=h.astype(np.float32), floor=floor, rel=rel, valley=valley, crack=crack,
               lab=lab, opened=opened, rem=rem, rel_isl=blocky_q, rel_col=rel_col, rel_peak=rel_peak,
               talus=talus, rel_fan=rel_fan, rel_shelf=rel_shelf, wash=wash, grass=grass,
               chan_dist=rv["dist"], islands=islands, columns=columns, peaks=peaks, fans=fans)
    return out
