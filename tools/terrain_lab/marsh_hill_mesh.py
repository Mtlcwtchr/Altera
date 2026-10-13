"""marsh_hill_mesh - a terraced hill of the great marshes as a mesh: the
heightfield (16 m a texel) cannot hold risers of a few metres, so each hill's
steps are also stood up as low red walls with flat grassy treads, drawn by the
props renderer as a cliff tile of the marshes (`"biomes": [1, 1]`: the biome's
slope rock on the walls, its ground on what faces up, its moss on the rock).

Used by marsh_islands.py; the outline follows the same profile as the
heightfield's raise (marsh_islands.island_profile / raise_island), so the
ground under a hill and its mesh agree.

Low poly on purpose (the style's rough retro feel): flat-shaded faces, the
outline jittered a few metres, each wall broken once halfway up.
"""
import math
import struct

import numpy as np


def _cliffness(theta, cliffs):
    c = np.zeros_like(theta)
    for at, width in cliffs:
        d = np.abs((theta - at + math.pi) % math.tau - math.pi)
        c = np.maximum(c, np.clip(1.5 - 1.5 * d / (width * 0.5), 0, 1))
    return np.clip(c, 0, 1)


def _sector(theta, at, width):
    d = np.abs((theta - at + math.pi) % math.tau - math.pi)
    return np.clip(1.5 - 1.5 * d / (width * 0.5), 0, 1)


def build(prof, rule, level, rng, segments=56):
    """Triangles (N x 3 x 3, local metres round the hill's centre, absolute z)
    of the hill whose profile is `prof` standing on water at `level`."""
    theta = np.linspace(0, math.tau, segments, endpoint=False)
    shape = 1 + rule["wobble"] * prof["wobble"](theta)
    squeeze = 1 - _cliffness(theta, prof["cliffs"]) * rule["cliff_squeeze"]
    ramp = np.zeros_like(theta)
    if "ramp" in prof:
        ramp = _sector(theta, *prof["ramp"]) * (1 - _cliffness(theta, prof["cliffs"]))
    treads, risers = prof["treads"], prof["risers"]
    radius = prof["r_top"] * shape + treads.sum() * squeeze
    z = level + rule["shelf_m"]
    sink = rule.get("mesh_sink_m", 2.0)
    rough = rule.get("mesh_rough_m", 2.5)
    tris, wts = [], []   # triangles and their vertices' moss-edge weights
    for k in range(len(risers)):
        bottom = z - (sink if k == 0 else 0.6)
        top = z + risers[k]
        inner = radius - treads[k] * squeeze
        # The wall: outline jittered, broken once halfway with a lean in or out;
        # on the ramp its top steps back over most of the tread (a slope).
        jit = rng.uniform(-rough, rough, segments)
        mid_j = rng.uniform(-rough, rough, segments) * 0.7
        r0 = radius + jit
        r2 = r0 - ramp * treads[k] * 0.85 + rng.uniform(-0.4, 0.4, segments) * rough
        r1 = 0.5 * (r0 + r2) + mid_j
        rings = [(r0, np.full(segments, bottom)), (r1, np.full(segments, 0.5 * (bottom + top))), (r2, np.full(segments, top))]
        # Moss gathers at a step's foot and along its brink, frayed by noise.
        weights = [rng.uniform(0.6, 1.0, segments), rng.uniform(0.0, 0.35, segments), rng.uniform(0.4, 1.0, segments)]
        # The tread: flat at `top` from the wall's top in to the next riser.
        if k + 1 < len(risers):
            r3 = inner + rng.uniform(-rough, rough, segments) * 0.5
            r3 = np.minimum(r3, r2 - 1.0)
            rings.append((r3, np.full(segments, top)))
            weights.append(rng.uniform(0.0, 0.4, segments))
        rings_xyz = [np.stack([r * np.cos(theta), r * np.sin(theta), zz], 1) for r, zz in rings]
        for (a, wa), (b, wb) in zip(zip(rings_xyz, weights), zip(rings_xyz[1:], weights[1:])):
            for i in range(segments):
                j = (i + 1) % segments
                tris.append((a[i], a[j], b[j]))
                wts.append((wa[i], wa[j], wb[j]))
                tris.append((a[i], b[j], b[i]))
                wts.append((wa[i], wb[j], wb[i]))
        if k + 1 == len(risers):
            # The top: a fan to the centre, slightly domed.
            c = np.array([0.0, 0.0, top + 0.3])
            last = rings_xyz[-1]
            for i in range(segments):
                tris.append((last[i], last[(i + 1) % segments], c))
                wts.append((weights[-1][i], weights[-1][(i + 1) % segments], 0.0))
        radius = inner
        z = top
    t = np.array(tris, np.float64)
    w = np.array(wts, np.float64)
    # Faces out and up: flip any triangle whose normal looks inward or down.
    n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    centre = t.mean(1)
    outward = np.concatenate([centre[:, :2], np.zeros((len(t), 1))], 1)
    flat = np.abs(n[:, 2]) > 0.7 * np.linalg.norm(n, axis=1)
    want = np.where(flat[:, None], np.array([0, 0, 1.0]), outward)
    flip = (n * want).sum(1) < 0
    t[flip] = t[flip][:, [0, 2, 1]]
    w[flip] = w[flip][:, [0, 2, 1]]
    return t, w


def _coherent(rng, segments, k=12, power=0.9):
    """A noise round the ring, smooth from one segment to the next: a few
    random harmonics, about 0..1."""
    theta = np.linspace(0, math.tau, segments, endpoint=False)
    amp = rng.normal(0, 1, k) / (1 + np.arange(k)) ** power
    ph = rng.uniform(0, math.tau, k)
    v = sum(a * np.cos((i + 1) * theta + p) for i, (a, p) in enumerate(zip(amp, ph)))
    return np.clip(0.5 + v / (3 * np.std(v) + 1e-9), 0, 1)


def _grid_normals(pts):
    """Smooth normals of a wall given as rows (up) x segments (round)."""
    t_round = np.roll(pts, -1, 1) - np.roll(pts, 1, 1)
    t_up = np.empty_like(pts)
    t_up[1:-1] = pts[2:] - pts[:-2]
    t_up[0] = pts[1] - pts[0]
    t_up[-1] = pts[-1] - pts[-2]
    n = np.cross(t_round, t_up)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)


def build_on_ground(theta, rho, levels, ramp, rule, ground, rng):
    """Terraces cut into a hill already in the heightfield: wall k stands on
    the ground's contour `rho[k]` (segments, metres from the centre) at
    `levels[k]` and rises to `levels[k + 1]`; a flat tread runs in from its
    brink to the next wall's foot, the last level is the top. Every tread
    lies over ground lower than itself (the contours are the outermost
    crossings), so the hill shows only through the walls. `ground(x, y)`
    (local metres) is the heightfield's own ground: the outermost wall's
    foot sinks into it.

    A wall is natural rock, not a fence: its face bulges and leans with a
    noise smooth round the hill and up the wall, a bedding ledge part way up
    steps it back, a lip swells out under the brink and rounds over onto the
    tread; its normals are smooth (the treads' face up). Moss weights (the
    mesh's `blend`) gather at the foot, under and over the lip and along the
    tread's inner corner, frayed round the hill.

    Returns triangles, their vertices' normals, cavity (uv.x) and moss
    weights, and the check samples (the outer foot's height over the ground,
    the ground over each tread)."""
    segments = len(theta)
    n = len(rho)
    rough = rule.get("mesh_rough_m", 1.2)
    sink = rule.get("mesh_sink_m", 1.0)
    cos, sin = np.cos(theta), np.sin(theta)

    def co():
        return _coherent(rng, segments)
    tread = [rho[k] - (rho[k + 1] if k + 1 < n else 0) for k in range(n)]
    # Each wall's foot a little out from its contour, its brink leaning in or
    # out of it; on the ramp the brink steps back over most of the tread.
    foot = [rho[k] + 0.2 + co() * rough * 0.6 for k in range(n)]
    brink = []
    for k in range(n):
        top = rho[k] + (co() - 0.5) * rough - ramp * tread[k] * 0.85
        top = np.minimum(top, foot[k] + 0.3)
        if k + 1 < n:
            top = np.maximum(top, foot[k + 1] + 1.5)
        brink.append(np.maximum(top, 1.0))
    tris, nrms, cav, wts = [], [], [], []
    foot_gap, tread_poke = [], []

    def quads(P, N, C, W):
        """Triangles between consecutive rows of P (rows x segments x 3)."""
        for j in range(len(P) - 1):
            for i in range(segments):
                q = (i + 1) % segments
                for a, b, c in (((j, i), (j, q), (j + 1, q)), ((j, i), (j + 1, q), (j + 1, i))):
                    tris.append((P[a], P[b], P[c]))
                    nrms.append((N[a], N[b], N[c]))
                    cav.append((C[a], C[b], C[c]))
                    wts.append((W[a], W[b], W[c]))

    for k in range(n):
        lo, hi = levels[k], levels[k + 1]
        rise = hi - lo
        rf, rb = foot[k], brink[k]
        # Up the wall: (share of the rise, offset from the straight face foot
        # -> brink), the bedding ledge, the swelling lip, the rounded brink.
        ledge = rng.uniform(0.35, 0.65)
        step_in = rng.uniform(0.1, 0.3) * float(np.clip(rise / 0.8, 0.5, 1.2))
        rows = [(0.0, 0.0), (0.22, 0.0), (ledge - 0.03, 0.0), (ledge + 0.03, -step_in), (0.82, -step_in * 0.6),
                (0.94, 0.15), (1.0, -0.3)]
        bulge_a, bulge_b = co(), co()
        moss_foot, moss_lip, moss_drape = co(), co(), co()
        P, Cv, Wv = [], [], []
        if k == 0:
            g = ground(rf * cos, rf * sin)
            bottom = np.minimum(g - sink, lo - 0.3)
            foot_gap.append(bottom - g)
            P.append(np.stack([rf * cos, rf * sin, bottom], 1))
            Cv.append(np.full(segments, 0.65))
            Wv.append(0.6 + 0.4 * moss_foot)
        for f, off in rows:
            bulge = ((1 - f) * bulge_a + f * bulge_b - 0.5) * 0.5 * math.sin(math.pi * f) * float(np.clip(rise, 0.5, 1.2))
            r = rf + (rb - rf) * f + off + bulge
            P.append(np.stack([r * cos, r * sin, np.full(segments, lo + rise * f)], 1))
            Cv.append(np.full(segments, 0.7 + 0.3 * min(f / 0.4, 1.0) - (0.12 if 0.85 < f < 0.99 else 0.0)))
            # Moss thick at the foot, hanging from the lip down the upper
            # face in a ragged fringe, a little on the bedding ledge.
            if f == 0.0:
                w = 0.55 + 0.45 * moss_foot
            elif f < 0.3:
                w = 0.5 * moss_foot
            elif f > 0.9:
                w = 0.4 + 0.6 * moss_lip
            elif f > 0.7:
                w = 0.1 + 0.45 * moss_lip
            elif f > ledge:
                w = 0.3 * moss_drape
            else:
                w = 0.1 * moss_lip
            Wv.append(w)
        P = np.array(P)
        quads(P, _grid_normals(P), np.array(Cv), np.array(Wv))
        # The tread: from the brink in to the next wall's foot (the same ring:
        # no seam), a little uneven; the top a low dome to the centre.
        last = k + 1 == n
        inner = np.zeros(segments) if last else foot[k + 1]
        r_edge = np.hypot(P[-1, :, 0], P[-1, :, 1])
        # Rows in from the brink: a strip by the brink and one by the next
        # wall's foot carry their moss, the middle stays grass.
        span = np.maximum(r_edge - inner, 0.5)
        near_brink = r_edge - np.minimum(0.9, span * 0.3)
        near_foot = inner + np.minimum(0.9, span * 0.3)
        mid = 0.5 * (r_edge + inner)
        dz = (co() - 0.5) * 0.08
        dome = lambda f: (0.3 * f if last else 0.0)
        rad = [r_edge, near_brink, mid, near_foot, inner]
        shares = [0.0, 0.3, 0.5, 0.7, 1.0]
        T = np.array([P[-1]] + [np.stack([r * cos, r * sin, hi + (dz if j == 2 else 0.0) + dome(f) + 0 * r], 1)
                                for j, (r, f) in enumerate(zip(rad[1:], shares[1:]), start=1)])
        up = np.zeros_like(T)
        up[..., 2] = 1
        Tc = np.array([np.ones(segments)] * 4 + [np.full(segments, 1.0 if last else 0.8)])
        foot_w = np.zeros(segments) if last else 0.4 + 0.5 * co()
        Tw = np.array([0.35 + 0.65 * moss_lip, 0.3 * moss_lip, 0.08 * co(), foot_w * 0.35, foot_w])
        quads(T, up, Tc, Tw)
        for t in (0.0, 0.25, 0.5, 0.75, 0.98):
            r = r_edge + (inner - r_edge) * t
            tread_poke.append(ground(r * cos, r * sin) - hi)
    t = np.array(tris, np.float64)
    nv = np.array(nrms, np.float64)
    c = np.array(cav, np.float64)
    w = np.array(wts, np.float64)
    # Faces out and up: flip any triangle whose winding disagrees with its normals.
    fn = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    flip = (fn * nv.mean(1)).sum(1) < 0
    for arr in (t, nv, c, w):
        arr[flip] = arr[flip][:, [0, 2, 1]]
    return t, {"normals": nv, "cavity": c, "moss": w}, {"foot_gap": np.concatenate(foot_gap),
                                                         "tread_poke": np.concatenate(tread_poke)}


def write(path, tris, ground_z, weights=None):
    """FOLM v2, flat-shaded (each triangle its own vertices). uv.x: darker at
    the foot of a wall; uv.y: height share up the hill; blend: where moss
    gathers (a step's foot and brink), read by props.hlsl."""
    tris = np.asarray(tris, np.float64)
    pos = tris.reshape(-1, 3)
    zmin, zmax = ground_z, max(pos[:, 2].max(), ground_z + 1)
    share = np.clip((pos[:, 2] - zmin) / (zmax - zmin), 0, 1)
    if isinstance(weights, dict):
        # build_on_ground's: smooth normals, the cavity, and the moss as
        # 2 + weight (props.hlsl: a terrace wall's own rock and moss).
        nrm = weights["normals"].reshape(-1, 3)
        nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        uv = np.stack([weights["cavity"].reshape(-1), share], 1)
        blend = 2.0 + np.clip(weights["moss"].reshape(-1), 0, 1)
    else:
        n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        nrm = np.repeat(n, 3, axis=0)
        ao = np.where(nrm[:, 2] < 0.5, 0.75 + 0.25 * share, 1.0)
        uv = np.stack([ao, share], 1)
        blend = np.zeros(len(pos)) if weights is None else np.asarray(weights, np.float64).reshape(-1)
    v = np.concatenate([pos, nrm, uv, blend[:, None]], 1).astype("<f4")
    idx = np.arange(len(pos), dtype="<u4")
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<III", 2, len(v), idx.size))
        f.write(struct.pack("<6f", *pos.min(0), *pos.max(0)))
        f.write(v.tobytes())
        f.write(idx.tobytes())
    return len(tris)
