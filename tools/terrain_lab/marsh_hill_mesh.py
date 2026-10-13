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


def write(path, tris, ground_z, weights=None):
    """FOLM v2, flat-shaded (each triangle its own vertices). uv.x: darker at
    the foot of a wall; uv.y: height share up the hill; blend: where moss
    gathers (a step's foot and brink), read by props.hlsl."""
    tris = np.asarray(tris, np.float64)
    pos = tris.reshape(-1, 3)
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    nrm = np.repeat(n, 3, axis=0)
    zmin, zmax = ground_z, max(pos[:, 2].max(), ground_z + 1)
    share = np.clip((pos[:, 2] - zmin) / (zmax - zmin), 0, 1)
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
