"""Ways up and across the canyon (user 15.10): natural stone bridges between
the islands' tops and spiral ledges round buttes from the floor to their
tops, chosen by rules on the analysed ground, each built as its own mesh and
checked.

    python tools/canyon_lab/paths.py --tag T | --dir RUN [--seed 3] [--views]

Run after place.py (the pinnacles are kept clear of). Reads the run's
layers.npz, features.json (islands) and instances.json; writes paths.json
(each way: kind, centre line, origin, checks; the totals, and how many
island tops the floor now reaches) and paths/<name>_lod{0,1,2}.mesh (FOLM v2,
metres from the way's origin, x east, y north, z up). Rules in canyon.json
`paths`. With --views renders the run with them (blender_view.py).

Bridge: between two islands' top treads across a gap of `span_m`, gentle
(`grade_max`, a slight `rise_k` arch), landing `land_m` into each tread,
clear of the ground below by `clear_m` along the gap, of the pinnacles and
of the other bridges (no crossing within `cross_gap_m` of height); at most
`per_island` to an island, `spacing_m` apart, `per_km2`. Spiral: a ledge
`width_m` wide hugging a butte's walls (`r_m`, `climb_m` from the floor),
climbing at `grade` from the floor to the top, `headroom_m` between its
turns; islands of bridged groups with no way up first.
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from scipy.spatial import cKDTree

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "terrain_lab"))
import marsh_hill_mesh  # noqa: E402  (FOLM writer)

BLENDER = Path("C:/Program Files/Blender Foundation/Blender 5.2/blender.exe")


class Ground:
    """Heights by run metres (x along columns, y along rows)."""

    def __init__(self, h, cell):
        self.h, self.cell = h, cell

    def at(self, x, y):
        x = np.asarray(x, np.float64) / self.cell
        y = np.asarray(y, np.float64) / self.cell
        return nd.map_coordinates(self.h, [np.atleast_1d(y), np.atleast_1d(x)], order=1, mode="nearest")


def seg_cross(a, b, c, d):
    """2D segments ab, cd cross: the parameters (t on ab, u on cd) or None."""
    r, s = b - a, d - c
    den = r[0] * s[1] - r[1] * s[0]
    if abs(den) < 1e-9:
        return None
    q = c - a
    t = (q[0] * s[1] - q[1] * s[0]) / den
    u = (q[0] * r[1] - q[1] * r[0]) / den
    return (t, u) if 0 <= t <= 1 and 0 <= u <= 1 else None


# --- meshes ------------------------------------------------------------------

def sweep(centre, side, profile, rng, jitter):
    """Triangles of a closed profile swept along a centre line. `centre`
    (N, 3), `side` (N, 2) the horizontal unit across it, `profile` (N, K, 2)
    or (K, 2) of (across, up) per ring. Ends capped."""
    n = len(centre)
    prof = np.broadcast_to(profile, (n,) + np.shape(profile)[-2:]).astype(np.float64)
    k = prof.shape[1]
    rings = np.zeros((n, k, 3))
    rings[..., :2] = centre[:, None, :2] + side[:, None, :] * prof[..., 0:1]
    rings[..., 2] = centre[:, None, 2] + prof[..., 1]
    if jitter > 0:
        rings += rng.normal(0, jitter, rings.shape) * np.array([1.0, 1.0, 0.6])
    strip = []
    for i in range(n - 1):
        for j in range(k):
            a, b = rings[i, j], rings[i, (j + 1) % k]
            c, d = rings[i + 1, j], rings[i + 1, (j + 1) % k]
            strip += [(a, c, b), (b, c, d)]
    strip = np.array(strip)
    # Outward: each side face away from the ring's middle (its ring's mean).
    nrm = np.cross(strip[:, 1] - strip[:, 0], strip[:, 2] - strip[:, 0])
    mids = np.repeat(rings.mean(axis=1)[:-1], 2 * k, axis=0)
    flip = (nrm * (strip.mean(axis=1) - mids)).sum(1) < 0
    strip[flip] = strip[flip][:, ::-1]
    caps = []
    for ring, ahead in ((rings[0], centre[0] - centre[1]), (rings[-1], centre[-1] - centre[-2])):
        mid = ring.mean(axis=0)
        for j in range(k):
            t = np.array([mid, ring[j], ring[(j + 1) % k]])
            if np.cross(t[1] - t[0], t[2] - t[0]) @ ahead < 0:
                t = t[::-1]
            caps.append(t)
    return np.concatenate([strip, np.array(caps)])


def bridge_mesh(pts, P, rng):
    B = P["bridges"]
    n = len(pts)
    t = np.linspace(0, 1, n)
    d = np.gradient(pts[:, :2], axis=0)
    d /= np.linalg.norm(d, axis=1, keepdims=True) + 1e-9
    side = np.stack([-d[:, 1], d[:, 0]], 1)
    span = float(np.linalg.norm(pts[-1, :2] - pts[0, :2]))
    w = pts[0, 3] if pts.shape[1] > 3 else 5.0
    # Thick at the ends (the arch springs from below the rims), thin mid-span.
    end_th = min(B["end_th_k"] * span, B["end_th_max_m"])
    mid_th = max(B["mid_th_m"], B["mid_th_k"] * span)
    th = mid_th + (end_th - mid_th) * np.abs(2 * t - 1) ** 2.2
    wide = w * (1 + 0.35 * np.abs(2 * t - 1) ** 3)
    prof = np.zeros((n, 8, 2))
    for i in range(n):
        hw, tt = wide[i] / 2, th[i]
        prof[i] = [(-hw, -0.15), (-hw + 0.5, 0.12), (hw - 0.5, 0.12), (hw, -0.15),
                   (hw * 1.12, -tt * 0.45), (hw * 0.55, -tt), (-hw * 0.55, -tt), (-hw * 1.12, -tt * 0.45)]
    c = pts[:, :3].copy()
    c[:, 2] -= 0.12
    return sweep(c, side, prof, rng, B["jitter_m"])


def ledge_mesh(pts, out, P, rng):
    S = P["spirals"]
    w = S["width"]
    below = 1.6 + 0.35 * w
    prof = np.array([(-w / 2 - S["embed_m"], 0.1), (w / 2 - 0.35, 0.1), (w / 2, 0.55), (w / 2 + 0.2, -0.05),
                     (w / 2, -0.7), (0.0, -below), (-w / 2 - S["embed_m"], -below - 0.6)])
    c = pts[:, :3].copy()
    c[:, 2] -= 0.1
    return sweep(c, out, prof, rng, S["jitter_m"])


# --- the ground read for ways ------------------------------------------------

def islands_of(L, feats, cell, P):
    """Per island: mask box, top level, top tread, floor round it."""
    h, lab = L["h"], L["lab"]
    zone = L["mask"] if "mask" in L.files else np.ones(h.shape, bool)
    gy, gx = np.gradient(h, cell)
    sl = np.degrees(np.arctan(np.hypot(gx, gy)))
    boxes = nd.find_objects(lab)
    pad = int(40 / cell)
    out = {}
    for s in feats["islands"]:
        i = s["id"]
        if i > len(boxes) or boxes[i - 1] is None:
            continue
        b = boxes[i - 1]
        y0, y1 = max(b[0].start - pad, 0), min(b[0].stop + pad, h.shape[0])
        x0, x1 = max(b[1].start - pad, 0), min(b[1].stop + pad, h.shape[1])
        m = lab[y0:y1, x0:x1] == i
        if m.sum() < 20 or not zone[y0:y1, x0:x1][m].all():
            continue
        hh = h[y0:y1, x0:x1]
        top = float(np.percentile(hh[m], 97))
        tread = m & (hh >= top - P["tread_drop_m"]) & (sl[y0:y1, x0:x1] < P["tread_slope_deg"])
        tread = nd.binary_erosion(tread, iterations=1)
        if tread.sum() < 10:
            continue
        ring = nd.binary_dilation(m, iterations=int(20 / cell)) & ~nd.binary_dilation(m, iterations=int(4 / cell))
        floor = float(np.percentile(hh[ring], 15)) if ring.any() else float(hh.min())
        # (benches at its foot: a ledge round it would run into them)
        bench = float((hh[ring] > floor + 3.0).mean()) if ring.any() else 1.0
        edge = tread & ~nd.binary_erosion(tread)
        ey, ex = np.nonzero(edge)
        ty, tx = np.nonzero(tread)
        out[i] = dict(id=i, box=(y0, x0), mask=m, top=top, floor=floor, r=s["r_max_m"], bench=bench,
                      edge=np.stack([(ex + x0) * cell, (ey + y0) * cell], 1)[::2],
                      tread=np.stack([(tx + x0) * cell, (ty + y0) * cell], 1),
                      cy=float((ty.mean() + y0) * cell), cx=float((tx.mean() + x0) * cell))
    return out


def pinnacles(inst):
    out = [(i["x"], i["y"], i["foot_r"], i["top"]) for i in inst if not i["role"].startswith("rubble")]
    return np.array(out) if out else np.zeros((0, 4))


def clear_of_pinnacles(a, b, z, pins, half_w):
    if not len(pins):
        return True
    ab = b - a
    L2 = max(float(ab @ ab), 1e-9)
    t = np.clip(((pins[:, :2] - a) @ ab) / L2, 0, 1)
    d = np.linalg.norm(a + t[:, None] * ab - pins[:, :2], axis=1)
    near = d < pins[:, 2] + half_w + 2.0
    return not (near & (pins[:, 3] > z - 2.0)).any()


# --- bridges -----------------------------------------------------------------

def bridge_candidates(isl, G, pins, P, rng):
    B = P["bridges"]
    ids = sorted(isl)
    trees = {i: cKDTree(isl[i]["tread"]) for i in ids}
    cands = []
    for a_ in range(len(ids)):
        for b_ in range(a_ + 1, len(ids)):
            A, Bi = isl[ids[a_]], isl[ids[b_]]
            if abs(A["top"] - Bi["top"]) > B["grade_max"] * B["span_m"][1] + 10:
                continue
            if math.hypot(A["cx"] - Bi["cx"], A["cy"] - Bi["cy"]) > A["r"] + Bi["r"] + B["span_m"][1] + 20:
                continue
            d, j = cKDTree(Bi["edge"]).query(A["edge"], k=1)
            order = np.argsort(d)[:B["tries"]]
            best = None
            for o in order:
                gap = float(d[o])
                if not (B["span_m"][0] <= gap <= B["span_m"][1]):
                    continue
                p, q = A["edge"][o], Bi["edge"][j[o]]
                u = (q - p) / max(gap, 1e-6)
                p2, q2 = p - u * B["land_m"], q + u * B["land_m"]
                # (both ends on their tread)
                if trees[A["id"]].query(p2)[0] > 3 * G.cell or trees[Bi["id"]].query(q2)[0] > 3 * G.cell:
                    continue
                pts_, _, _ = deck(p2, q2, G, gap, P)
                zp, zq = float(pts_[0, 2]), float(pts_[-1, 2])
                c = check_bridge(p2, q2, gap, G, pins, P)
                if c is None:
                    continue
                score = gap + 2.0 * abs(zq - zp)
                if best is None or score < best[0]:
                    best = (score, dict(kind="bridge", a=A["id"], b=Bi["id"], p=p2, q=q2, zp=zp, zq=zq, gap=gap,
                                        width=float(rng.uniform(*B["width_m"])), checks=c))
            if best:
                cands.append(best)
    cands.sort(key=lambda c: c[0])
    return [c[1] for c in cands]


def deck(p, q, G, gap, P, n=None):
    """The walking line: on the treads the ground itself (smoothed), over the
    gap from rim to rim with a slight arch."""
    B = P["bridges"]
    L = float(np.linalg.norm(q - p))
    n = n or max(8, int(L / B["step_m"]) + 1)
    t = np.linspace(0, 1, n)
    xy = p[None] + (q - p)[None] * t[:, None]
    land = B["land_m"] / L
    g = G.at(xy[:, 0], xy[:, 1])
    ia, ib = int(np.searchsorted(t, land)), int(np.searchsorted(t, 1 - land)) - 1
    za, zb = float(g[:ia + 1].mean()), float(g[ib:].mean())
    u = np.clip((t - t[ia]) / max(1e-6, t[ib] - t[ia]), 0, 1)
    z = za + (zb - za) * u + B["rise_k"] * gap * np.sin(np.pi * u)
    on = (t < t[ia]) | (t > t[ib])
    z[on] = nd.uniform_filter1d(g, 3)[on] + 0.05
    return np.concatenate([xy, z[:, None]], 1), t, land


def check_bridge(p, q, gap, G, pins, P):
    B = P["bridges"]
    pts, t, land = deck(p, q, G, gap, P, n=int(np.linalg.norm(q - p)) + 2)
    span = (t > land) & (t < 1 - land)
    grade = float(np.max(np.abs(np.diff(pts[span, 2]))) / (t[1] - t[0]) / np.linalg.norm(q - p)) if span.sum() > 2 else 0.0
    if grade > B["grade_max"] + 4 * B["rise_k"]:
        return None
    d = (q - p) / np.linalg.norm(q - p)
    side = np.array([-d[1], d[0]])
    w = B["width_m"][1]
    worst_clear, worst_land_hi, worst_land_lo = 1e9, -1e9, 1e9
    for off in (-w / 2, 0.0, w / 2):
        g = G.at(pts[:, 0] + side[0] * off, pts[:, 1] + side[1] * off)
        # (clear by `clear_m` out over the gap; off the rims, where the
        # ground falls away over a cell or two, only never under it)
        rim = B["rim_m"] / float(np.linalg.norm(q - p))
        mid = (t > land + rim) & (t < 1 - land - rim)
        near_rim = ~mid & (t > land) & (t < 1 - land)
        ends = ~mid & ((t < land * 0.8) | (t > 1 - land * 0.8))
        if mid.any():
            worst_clear = min(worst_clear, float((pts[mid, 2] - g[mid]).min()))
        if near_rim.any() and float((pts[near_rim, 2] - g[near_rim]).min()) < -0.3:
            return None
        if ends.any():
            worst_land_hi = max(worst_land_hi, float((g[ends] - pts[ends, 2]).max()))
            worst_land_lo = min(worst_land_lo, float((g[ends] - pts[ends, 2]).min()))
    if worst_clear < B["clear_m"] or worst_land_hi > B["bury_max_m"] or worst_land_lo < -B["float_max_m"]:
        return None
    if not clear_of_pinnacles(p, q, float(pts[:, 2].min()), pins, w / 2):
        return None
    return dict(clear_min_m=round(worst_clear, 2), end_buried_max_m=round(worst_land_hi, 2),
                end_floating_max_m=round(-worst_land_lo, 2), grade=round(grade, 3))


def choose_bridges(cands, area_km2, P):
    B = P["bridges"]
    want = int(round(B["per_km2"] * area_km2))
    chosen, per = [], {}
    for c in cands:
        if len(chosen) >= want:
            break
        if per.get(c["a"], 0) >= B["per_island"] or per.get(c["b"], 0) >= B["per_island"]:
            continue
        mid = (c["p"] + c["q"]) / 2
        zm = (c["zp"] + c["zq"]) / 2
        ok = True
        for o in chosen:
            if np.linalg.norm(mid - (o["p"] + o["q"]) / 2) < B["spacing_m"]:
                ok = False
                break
            x = seg_cross(c["p"], c["q"], o["p"], o["q"])
            if x is not None and abs(zm - (o["zp"] + o["zq"]) / 2) < B["cross_gap_m"]:
                ok = False
                break
        if ok:
            chosen.append(c)
            per[c["a"]] = per.get(c["a"], 0) + 1
            per[c["b"]] = per.get(c["b"], 0) + 1
    return chosen


# --- spirals -------------------------------------------------------------------

def pinnacle_shell(inst, P):
    """A placed pinnacle's outermost reach: radius by (angle, height over its
    z), from its model's vertices as placed (place.py's turn and scale)."""
    import place
    v = place.read_mesh(place.PROPS / f"{inst['name']}_lod0.mesh").astype(np.float64)
    a = math.radians(inst["yaw"])
    sx = inst["s"] * inst["k"]
    x = (v[:, 0] * math.cos(a) - v[:, 1] * math.sin(a)) * sx
    y = (v[:, 0] * math.sin(a) + v[:, 1] * math.cos(a)) * sx * P["up"]
    z = v[:, 2] * inst["s"]
    na, dz = 72, 1.0
    top = float(z.max())
    nz = int(top / dz) + 2
    shell = np.zeros((na, nz))
    ia = (np.round(np.arctan2(y, x) / (2 * np.pi) * na).astype(int)) % na
    iz = np.clip((z / dz).astype(int), 0, nz - 1)
    np.maximum.at(shell, (ia, iz), np.hypot(x, y))
    # (a ledge passes a slab's tip: the reach over the neighbouring angles
    # and a few metres up and down)
    shell = nd.maximum_filter(shell, size=(5, 5), mode=("wrap", "nearest"))
    return shell, top, na, dz


def spiral_path(inst, G, pins, others, P, rng):
    """A ledge winding up round a column from the ground at its foot to its
    top, ending on a platform there; or (None, why)."""
    S = P["spirals"]
    w = S["width"]
    shell, top, na, dz = pinnacle_shell(inst, P)
    cx, cy, z0 = inst["x"], inst["y"], inst["z"]
    z_end = z0 + top - S["top_below_m"]

    def reach(theta, z):
        k = int(round(theta / (2 * np.pi) * na)) % na
        j = int(np.clip((z - z0) / dz, 0, shell.shape[1] - 1))
        return float(shell[k, max(0, j - 2):j + 4].max())

    # Start where the ground at the foot is lowest and open.
    angs = np.linspace(0, 2 * np.pi, 36, endpoint=False)
    starts = []
    for th in angs:
        R = reach(th, z0 + 2) + w / 2 + 0.5
        g = float(G.at(cx + math.cos(th) * R, cy + math.sin(th) * R)[0])
        starts.append((g, th))
    g0, theta = min(starts)
    z = g0 + 0.15
    direction = 1.0 if rng.random() < 0.5 else -1.0
    ds = S["step_m"]
    pts, outs = [], []
    turned = 0.0
    while z < z_end:
        need = reach(theta, z) + w / 2 + 0.4
        R = need
        x, y = cx + math.cos(theta) * R, cy + math.sin(theta) * R
        g = float(G.at(x, y)[0])
        if g > z + 0.15:
            if len(pts) * ds > S["ground_run_m"]:
                return None, f"ground over the ledge {g - z:.1f} m at {z - z0:.0f} m up"
            if g - z > S["step_up_m"]:
                return None, "a step up its foot"
            z = g + 0.15                      # (still climbing off the scree at its foot)
        pts.append((x, y, z))
        outs.append((math.cos(theta), math.sin(theta)))
        dth = ds / max(R, 4.0)
        theta += direction * dth
        turned += dth
        # (never less than `turn_rise_m` a turn: room over the turn below
        # where the column narrows)
        z += max(S["grade"] * ds, S["turn_rise_m"] * dth / (2 * np.pi))
    pts, outs = np.array(pts), np.array(outs)
    if len(pts) < 10:
        return None, "too short"
    xy = pts[:, :2]
    head = 1e9
    for i_, j_ in cKDTree(xy).query_pairs(w):
        # (the turn below or above, not the next steps along the ledge)
        if abs(i_ - j_) * ds > 3 * w:
            head = min(head, abs(pts[i_, 2] - pts[j_, 2]))
    if head < S["headroom_m"]:
        return None, f"headroom {head:.1f} m"
    near = [p for p in pins if math.hypot(p[0] - cx, p[1] - cy) > 1.0]
    for k in range(0, len(pts) - 1, 4):
        if not clear_of_pinnacles(xy[k], xy[min(k + 4, len(xy) - 1)], pts[k, 2], np.array(near).reshape(-1, 4), w / 2):
            return None, "another pinnacle in the way"
    for o in others:
        if np.min(np.linalg.norm(xy[:, None, :] - o[None, ::3, :2], axis=2)) < w + 2:
            return None, "another way in the way"
    under = G.at(xy[:, 0], xy[:, 1])
    grades = np.abs(np.diff(pts[:, 2])) / np.maximum(np.linalg.norm(np.diff(xy, axis=0), axis=1), 1e-6)
    gap_rock = max(np.hypot(*(xy[k] - (cx, cy))) - reach(math.atan2(xy[k, 1] - cy, xy[k, 0] - cx), pts[k, 2]) - w / 2
                   for k in range(len(xy)))
    if gap_rock > S["gap_to_rock_max_m"]:
        return None, f"off the rock {gap_rock:.1f} m"
    plat = dict(x=cx, y=cy, z=float(pts[-1, 2]), r=float(shell[:, -8:].max() + w * 0.8))
    checks = dict(climb_m=round(float(pts[-1, 2] - pts[0, 2]), 1), turns=round(turned / (2 * np.pi), 2),
                  length_m=round(float(np.linalg.norm(np.diff(xy, axis=0), axis=1).sum()), 1),
                  grade_max=round(float(np.percentile(grades, 99)), 3),
                  headroom_min_m=round(float(head), 1) if head < 1e9 else None,
                  start_over_ground_m=round(float(pts[0, 2] - under[0]), 2),
                  ground_over_ledge_max_m=round(float(max(0.0, (under - pts[:, 2]).max())), 2),
                  gap_to_rock_max_m=round(float(gap_rock), 2))
    return dict(kind="spiral", column=inst, pts=pts, out=outs, platform=plat, checks=checks), None


def platform_mesh(plat, centre_z_off, rng, P):
    """A rough round slab capping the column, the ledge's end on it."""
    r = plat["r"]
    n = 20
    ang = np.linspace(0, 2 * np.pi, n, endpoint=False)
    rr = r * (1 + 0.12 * rng.normal(size=n))
    top = np.stack([np.cos(ang) * rr, np.sin(ang) * rr, np.zeros(n) + 0.1], 1)
    bot = np.stack([np.cos(ang) * rr * 0.55, np.sin(ang) * rr * 0.55, np.zeros(n) - 2.5 - r * 0.25], 1)
    ct, cb = np.array([0, 0, 0.15]), np.array([0, 0, -2.0 - r * 0.3])
    tris = []
    for j in range(n):
        a, b = top[j], top[(j + 1) % n]
        c, d = bot[j], bot[(j + 1) % n]
        tris += [(ct, a, b), (cb, d, c), (a, c, b), (b, c, d)]
    tris = np.array(tris)
    nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    out = tris.mean(axis=1) - np.array([0, 0, -1.0])
    flip = (nrm * out).sum(1) < 0
    tris[flip] = tris[flip][:, ::-1]
    tris = tris + np.array([plat["x"], plat["y"], plat["z"]])
    return tris


# --- the whole ---------------------------------------------------------------

def reach(isl, bridges, spirals):
    """Island tops the floor reaches: up a spiral to its platform, then across
    bridges."""
    adj = {i: set() for i in isl}
    for b in bridges:
        adj[b["a"]].add(b["b"])
        adj[b["b"]].add(b["a"])
    seen = set(s["pid"] for s in spirals)
    todo = list(seen)
    while todo:
        i = todo.pop()
        for j in adj[i]:
            if j not in seen:
                seen.add(j)
                todo.append(j)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--dir")
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--views", action="store_true")
    args = ap.parse_args()
    out = Path(args.dir) if args.dir else ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    P = json.loads((HERE / "canyon.json").read_text(encoding="utf-8"))["paths"]
    st = json.loads((out / "stats.json").read_text(encoding="utf-8"))
    up = 1.0 if st.get("rows") == "south first" else -1.0
    P["up"] = up
    L = np.load(out / "layers.npz")
    feats = json.loads((out / "features.json").read_text(encoding="utf-8"))
    inst = json.loads((out / "instances.json").read_text(encoding="utf-8"))["instances"]
    cell = st["cell_m"]
    G = Ground(L["h"].astype(np.float32).copy(), cell)
    zone = L["mask"] if "mask" in L.files else np.ones(G.h.shape, bool)
    area_km2 = float(zone.sum()) * cell * cell / 1e6
    rng = np.random.default_rng(args.seed)
    pins = pinnacles(inst)
    isl = islands_of(L, feats, cell, P)
    S, B = P["spirals"], P["bridges"]

    # Spirals round columns first: tall ones standing near an island's top
    # tread at about their own height (their platform bridges over to it).
    edges = [(i, I) for i, I in isl.items()]
    cols = [c for c in inst if c["role"] == "column" and S["climb_m"][0] <= c["top"] - c["z"] <= S["climb_m"][1]
            and c["foot_r"] >= S["foot_r_min_m"]]
    scored = []
    for c in cols:
        best = None
        for i, I in edges:
            d = float(np.min(np.hypot(I["edge"][:, 0] - c["x"], I["edge"][:, 1] - c["y"]))) - c["foot_r"]
            if d < B["span_m"][0] or d > S["bridge_reach_m"]:
                continue
            if abs(I["top"] - c["top"]) > B["grade_max"] * d:
                continue
            best = d if best is None else min(best, d)
        if best is not None:
            scored.append((best, c))
    scored.sort(key=lambda t: t[0])
    want = int(round(S["per_km2"] * area_km2))
    spirals, why, others = [], {}, []
    for _, c in scored:
        if len(spirals) >= want:
            break
        if any(math.hypot(c["x"] - s_["column"]["x"], c["y"] - s_["column"]["y"]) < S["spacing_m"] for s_ in spirals):
            continue
        sp, bad = spiral_path(c, G, pins, others, P, rng)
        if sp is None:
            why[f"{c['x']:.0f},{c['y']:.0f}"] = bad
            continue
        spirals.append(sp)
        others.append(sp["pts"])
    # Their platforms become ground the bridges may land on (a top of their
    # own), and the columns no longer bar a bridge to them.
    hh = G.h
    for k, sp in enumerate(spirals):
        pl = sp["platform"]
        pid = -(k + 1)
        sp["pid"] = pid
        r_px = pl["r"] / cell
        y0, y1 = int(max(0, pl["y"] / cell - r_px - 1)), int(min(hh.shape[0], pl["y"] / cell + r_px + 2))
        x0, x1 = int(max(0, pl["x"] / cell - r_px - 1)), int(min(hh.shape[1], pl["x"] / cell + r_px + 2))
        yy, xx = np.mgrid[y0:y1, x0:x1]
        disc = (yy - pl["y"] / cell) ** 2 + (xx - pl["x"] / cell) ** 2 <= r_px * r_px
        hh[y0:y1, x0:x1] = np.where(disc, pl["z"], hh[y0:y1, x0:x1])
        ty, tx = np.nonzero(disc)
        tread = np.stack([(tx + x0) * cell, (ty + y0) * cell], 1)
        rim = disc & ~nd.binary_erosion(disc)
        ry, rx = np.nonzero(rim)
        isl[pid] = dict(id=pid, top=pl["z"], floor=sp["column"]["z"], r=pl["r"], tread=tread,
                        edge=np.stack([(rx + x0) * cell, (ry + y0) * cell], 1), cx=pl["x"], cy=pl["y"], bench=0)
    spun = np.array([[s_["column"]["x"], s_["column"]["y"]] for s_ in spirals]).reshape(-1, 2)
    keep = np.array([not len(spun) or np.min(np.hypot(*(spun - p[:2]).T)) > 1.0 for p in pins], bool)
    bridges = choose_bridges(bridge_candidates(isl, G, pins[keep] if len(pins) else pins, P, rng), area_km2, P)
    # (a platform with no bridge is a lookout: kept)

    # Meshes, in the run's frame turned to x east, y north.
    mdir = out / "paths"
    mdir.mkdir(exist_ok=True)
    for f in mdir.glob("*.mesh"):
        f.unlink()
    ways = []
    tris_total = 0
    for k, b in enumerate(bridges):
        pts, _, _ = deck(b["p"], b["q"], G, b["gap"], P)
        pts = np.concatenate([pts, np.full((len(pts), 1), b["width"])], 1)
        ways.append(dict(name=f"bridge_{k:03d}", kind="bridge", islands=[b["a"], b["b"]], pts=pts,
                         width=b["width"], checks=b["checks"], gap_m=round(b["gap"], 1)))
    for k, s_ in enumerate(spirals):
        ways.append(dict(name=f"spiral_{k:03d}", kind="spiral", islands=[s_["pid"]], pts=s_["pts"],
                         out=s_["out"], platform=s_["platform"], width=S["width"], checks=s_["checks"]))
    mrng = np.random.default_rng(args.seed + 11)
    for wy in ways:
        p = wy["pts"].copy()
        origin = p[:, :3].mean(axis=0)
        origin[2] = 0.0
        if wy["kind"] == "bridge":
            tris = bridge_mesh(p - np.r_[origin, 0][:p.shape[1]], P, mrng)
        else:
            tris = ledge_mesh(p - origin, wy["out"], P, mrng)
            pl = dict(wy["platform"])
            pl["x"] -= origin[0]
            pl["y"] -= origin[1]
            tris = np.concatenate([tris, platform_mesh(pl, 0, mrng, P)])
        # The run's y is the world's north when rows run south first; else
        # turned over (and the faces with it).
        tris = tris.copy()
        tris[..., 1] *= up
        if up < 0:
            tris = tris[:, ::-1]
        for lod in range(3):
            n_ = marsh_hill_mesh.write(mdir / f"{wy['name']}_lod{lod}.mesh", tris, float(p[:, 2].min()) - 2.0)
        tris_total += n_
        wy["origin"] = [float(origin[0]), float(origin[1])]
        wy["pts"] = np.round(p[:, :3], 2).tolist()
        wy.pop("out", None)
        if "platform" in wy:
            wy["platform"] = {k_: round(float(v), 2) for k_, v in wy["platform"].items()}
    reached = reach(isl, bridges, spirals)
    real = {i for i in isl if i > 0}
    tall = [i for i in real if isl[i]["top"] - isl[i]["floor"] >= S["climb_m"][0]]
    res = dict(
        area_km2=round(area_km2, 2), islands=len(real), bridges=len(bridges), spirals=len(spirals),
        triangles=int(tris_total),
        bridge_gap_m=[round(min((b["gap"] for b in bridges), default=0), 1), round(max((b["gap"] for b in bridges), default=0), 1)],
        bridges_to_platforms=sum(1 for b in bridges if b["a"] < 0 or b["b"] < 0),
        reached_tops=len(reached & real), reached_share_of_tall=round(len(reached & set(tall)) / max(1, len(tall)), 3),
        spirals_dropped=why,
        checks=dict(
            bridge_clear_min_m=min((b["checks"]["clear_min_m"] for b in bridges), default=None),
            bridge_end_buried_max_m=max((b["checks"]["end_buried_max_m"] for b in bridges), default=None),
            bridge_end_floating_max_m=max((b["checks"]["end_floating_max_m"] for b in bridges), default=None),
            bridge_grade_max=max((b["checks"]["grade"] for b in bridges), default=None),
            spiral_grade_max=max((s_["checks"]["grade_max"] for s_ in spirals), default=None),
            spiral_headroom_min_m=min((s_["checks"]["headroom_min_m"] or 99 for s_ in spirals), default=None),
            spiral_ground_over_ledge_max_m=max((s_["checks"]["ground_over_ledge_max_m"] for s_ in spirals), default=None),
            spiral_gap_to_rock_max_m=max((s_["checks"]["gap_to_rock_max_m"] for s_ in spirals), default=None),
        ),
    )
    (out / "paths.json").write_text(json.dumps(dict(cell_m=cell, up=up, frame="run metres; meshes x east, y north",
                                                    result=res, ways=ways), default=float), encoding="utf-8")
    print(json.dumps(res, indent=1, default=float))
    if args.views:
        subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P", str(HERE / "blender_view.py"),
                        "--", str(out), "--paths"], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
