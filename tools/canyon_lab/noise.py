"""Small fast fields for the canyon sandbox: smooth noise (a random grid
B-spline-upsampled by two matrix products), Voronoi labels, cone stamps."""

import numpy as np
import scipy.ndimage as nd


def _bspline_matrix(n, g, f):
    """n x g weights: row i samples the coarse grid at i * f + 1 (cubic B-spline)."""
    t = np.arange(n, dtype=np.float64) * f + 1.0
    i0 = np.floor(t).astype(int)
    u = t - i0
    w = np.stack([(1 - u) ** 3 / 6,
                  (3 * u ** 3 - 6 * u ** 2 + 4) / 6,
                  (-3 * u ** 3 + 3 * u ** 2 + 3 * u + 1) / 6,
                  u ** 3 / 6], 1)
    m = np.zeros((n, g), np.float32)
    for k in range(4):
        np.add.at(m, (np.arange(n), np.clip(i0 - 1 + k, 0, g - 1)), w[:, k])
    return m


def smooth(rng, n, cell, scale_m):
    """One octave of zero-mean, unit-std smooth noise with features ~scale_m."""
    f = cell / scale_m
    g = int(np.ceil(n * f)) + 4
    a = rng.standard_normal((g, g)).astype(np.float32)
    m = _bspline_matrix(n, g, f)
    out = m @ a @ m.T
    return (out - out.mean()) / (out.std() + 1e-9)


def fbm(rng, n, cell, scale_m, octaves=4, gain=0.5):
    out = np.zeros((n, n), np.float32)
    amp, tot, s = 1.0, 0.0, scale_m
    s = max(s, 2.5 * cell)
    for _ in range(octaves):
        if s < 2 * cell and tot > 0:
            break
        out += amp * smooth(rng, n, cell, s)
        tot += amp * amp
        amp *= gain
        s *= 0.5
    return out / np.sqrt(tot)


def unit(x):
    """Rank-free squash of a ~N(0,1) field into 0..1."""
    return 0.5 + 0.5 * np.tanh(x * 0.8)


def warp_coords(rng, n, cell, scale_m, amp_m):
    gy, gx = np.mgrid[0:n, 0:n].astype(np.float32)
    wy = fbm(rng, n, cell, scale_m, 3) * (amp_m / cell)
    wx = fbm(rng, n, cell, scale_m, 3) * (amp_m / cell)
    return np.clip(gy + wy, 0, n - 1), np.clip(gx + wx, 0, n - 1)


def voronoi(rng, n, cell, spacing_m, at=None):
    """Labels of a jittered-grid Voronoi (looked up at warped coords `at`),
    plus the seeds' pixel positions."""
    sp = spacing_m / cell
    g = int(np.ceil(n / sp)) + 1
    sy = ((np.arange(g)[:, None] + rng.uniform(0.1, 0.9, (g, g))) * sp).ravel()
    sx = ((np.arange(g)[None, :] + rng.uniform(0.1, 0.9, (g, g))) * sp).ravel()
    keep = (sy < n) & (sx < n)
    sy, sx = sy[keep].astype(int), sx[keep].astype(int)
    seed = np.zeros((n, n), np.int32)
    seed[sy, sx] = np.arange(1, sy.size + 1)
    _, (iy, ix) = nd.distance_transform_edt(seed == 0, return_indices=True)
    lab = seed[iy, ix]
    if at is not None:
        lab = lab[np.rint(at[0]).astype(int), np.rint(at[1]).astype(int)]
    return lab, sy, sx


def pair_border(lab):
    """Pixels on a border between two labels, with a 0..1 hash of the pair."""
    n = lab.shape[0]
    b = np.zeros(lab.shape, bool)
    hsh = np.ones(lab.shape, np.float32)
    for sl_a, sl_b in (((slice(None), slice(0, n - 1)), (slice(None), slice(1, n))),
                       ((slice(0, n - 1), slice(None)), (slice(1, n), slice(None)))):
        a, c = lab[sl_a].astype(np.int64), lab[sl_b].astype(np.int64)
        d = a != c
        lo, hi = np.minimum(a, c), np.maximum(a, c)
        hv = (((lo * 73856093) ^ (hi * 19349663)) % 10007) / 10007.0
        b[sl_a] |= d
        hsh[sl_a] = np.where(d, np.minimum(hsh[sl_a], hv), hsh[sl_a])
    return b, hsh


def stamp_cones(h, items, cell, rng=None):
    """Max-stamp cones: items of (y, x, r_m, height_m, power). Returns h."""
    n = h.shape[0]
    for y, x, r, ht, pw in items:
        rp = r / cell
        y0, y1 = int(max(0, y - rp - 1)), int(min(n, y + rp + 2))
        x0, x1 = int(max(0, x - rp - 1)), int(min(n, x + rp + 2))
        if y0 >= y1 or x0 >= x1:
            continue
        yy, xx = np.mgrid[y0:y1, x0:x1]
        ang = np.arctan2(yy - y, xx - x)
        # A ragged outline: the radius wobbles with the angle.
        k = rng.uniform(0, 6.28, 3) if rng is not None else np.zeros(3)
        wob = 1 + 0.18 * np.sin(3 * ang + k[0]) + 0.12 * np.sin(5 * ang + k[1]) + 0.08 * np.sin(7 * ang + k[2])
        d = np.hypot(yy - y, xx - x) / (rp * wob)
        c = ht * np.clip(1 - d, 0, 1) ** pw
        h[y0:y1, x0:x1] = np.maximum(h[y0:y1, x0:x1], c)
    return h
