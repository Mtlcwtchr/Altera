"""Pictures of the sandbox's heights: hillshade, slope, a coloured map with
cast shadows, and a perspective view (a column-wise height-field ray caster,
vectorised over screen columns: a second or two per frame)."""

import numpy as np
import scipy.ndimage as nd
from PIL import Image

from noise import fbm, unit

PAL = {
    "floor": (192, 104, 54), "wash": (212, 146, 92), "fan": (204, 136, 80),
    "shelf": (176, 92, 50), "talus": (140, 66, 42), "cliff": (118, 44, 30),
    "top": (186, 96, 52), "column": (126, 50, 34), "peak": (132, 56, 36),
    "grass": (210, 128, 52),
}
SKY_TOP, SKY_LOW, HAZE = np.array([120, 150, 186]), np.array([236, 206, 150]), np.array([226, 182, 132])


def normals(h, cell):
    gy, gx = np.gradient(h, cell)
    nz = 1.0 / np.sqrt(1 + gx * gx + gy * gy)
    return -gx * nz, -gy * nz, nz


def slope_deg(h, cell):
    gy, gx = np.gradient(h, cell)
    return np.degrees(np.arctan(np.hypot(gx, gy)))


def sun_dir(az_deg, el_deg):
    """Direction towards the sun; az 0 = up the picture (north), clockwise."""
    a, e = np.radians(az_deg), np.radians(el_deg)
    return np.array([np.cos(e) * np.sin(a), -np.cos(e) * np.cos(a), np.sin(e)])  # x, y(row), z


def hillshade(h, cell, az=250, el=35):
    nx, ny, nz = normals(h, cell)
    s = sun_dir(az, el)
    return np.clip(nx * s[0] + ny * s[1] + nz * s[2], 0, 1)


def shadows(h, cell, az=250, el=35, k=2):
    """Cast shadows, marched at 1/k resolution with integer shifts."""
    hs = h[::k, ::k]
    c = cell * k
    s = sun_dir(az, el)
    d = np.array([s[1], s[0]])
    d /= np.abs(d).max()
    step = np.hypot(*d) * c
    rise = step * np.tan(np.radians(el))
    m = hs.shape[0]
    best = np.full(hs.shape, -1e9, np.float32)
    nsteps = int(np.ptp(hs) / rise) + 2
    for i in range(1, nsteps):
        oy, ox = int(round(d[0] * i)), int(round(d[1] * i))
        if abs(oy) >= m or abs(ox) >= m:
            break
        src = hs[max(0, oy):m + min(0, oy), max(0, ox):m + min(0, ox)] - i * rise
        dst = best[max(0, -oy):m + min(0, -oy), max(0, -ox):m + min(0, -ox)]
        np.maximum(dst, src, out=dst)
    sh = (best > hs + 0.3).astype(np.float32)
    sh = nd.zoom(sh, k, order=1)[: h.shape[0], : h.shape[1]]
    return nd.gaussian_filter(sh, 0.7)


def classes(g, sl):
    """Per-cell material class names -> index map, in draw priority."""
    n = g["n"]
    names = ["floor", "shelf", "fan", "wash", "talus", "top", "column", "peak", "grass", "cliff"]
    c = np.zeros((n, n), np.uint8)
    c[g["rel_isl"] > 1] = names.index("shelf")
    c[g["rel_fan"] > 0.3] = names.index("fan")
    c[g["wash"] & (g["rel"] < 1)] = names.index("wash")
    c[g["talus"] > 1.0] = names.index("talus")
    c[g["rel_isl"] > 20] = names.index("top")
    c[g["rel_col"] > 4] = names.index("column")
    c[(g["rel_peak"] > 3) & (g["rel_peak"] >= g["rel"] - 0.1)] = names.index("peak")
    c[g["grass"]] = names.index("grass")
    c[sl > 58] = names.index("cliff")
    return c, names


def albedo(g, cell, rng):
    h = g["h"]
    sl = slope_deg(h, cell)
    cls, names = classes(g, sl)
    pal = np.array([PAL[k] for k in names], np.float32)
    col = pal[cls]
    # Grain and strata: rock darker/lighter in bands of height, wobbling.
    grain = fbm(rng, g["n"], cell, 12.0, 3)
    wob = fbm(rng, g["n"], cell, 300.0, 2) * 4
    strata = 1 + 0.10 * np.sin((h + wob) / 6.5) + 0.06 * np.sin((h + wob) / 2.3)
    rock = (cls == names.index("cliff")) | (cls == names.index("column")) | (cls == names.index("peak"))
    col *= np.where(rock, strata, 1.0)[..., None]
    g["_rock"] = (rock | (sl > 45)).astype(np.float32)
    g["_wob"] = wob
    col *= (1 + np.where(rock, 0.025, 0.07) * grain)[..., None]
    gr = cls == names.index("grass")
    col[gr] *= (0.85 + 0.3 * unit(fbm(rng, g["n"], cell, 4.0, 2)))[gr][:, None]
    return np.clip(col, 0, 255), cls, names, sl


def lit(g, cell, rng, az=250, el=32):
    col, cls, names, sl = albedo(g, cell, rng)
    h = g["h"]
    lam = hillshade(h, cell, az, el)
    sh = shadows(h, cell, az, el)
    nx, ny, nz = normals(h, cell)
    amb = 0.38 + 0.18 * nz
    light = amb + 0.85 * lam * (1 - sh)
    out = col * light[..., None]
    # Shadows lean cool, lit faces warm (the painted look of the refs).
    tint = 1 - sh[..., None] * np.array([0.08, 0.02, -0.10])
    out = out * tint
    # Wall faces (seen side-on in the views): the rock lit by the face's own
    # smoothed orientation, so a wall reads as one face, not cell streaks.
    hb = nd.gaussian_filter(h, 2.5)
    gy, gx = np.gradient(hb, cell)
    gl = np.hypot(gx, gy) + 1e-6
    s = sun_dir(az, el)
    face = np.clip((-gx / gl) * s[0] + (-gy / gl) * s[1], 0, 1) * np.cos(np.radians(el))
    lw = 0.40 + 1.0 * face * (1 - sh)
    rockc = np.array(PAL["cliff"], np.float32) * (1 + 0.05 * fbm(rng, g["n"], cell, 40.0, 2))[..., None]
    g["_wall"] = np.clip(rockc * lw[..., None] * tint, 0, 255).astype(np.float32)
    return np.clip(out, 0, 255), cls, names, sl


def perspective(h, color, cell, cam, rock=None, wob=None, wall=None, W=1280, Hh=720, ss=2, fov=62, far=7000.0):
    """cam: dict x, y (metres, x right, y down the picture), z (metres over the
    ground under it), yaw (deg, 0 = up the picture, clockwise), pitch (deg down)."""
    W2, H2 = W * ss, Hh * ss
    n = h.shape[0]
    f = (W2 / 2) / np.tan(np.radians(fov / 2))
    yaw = np.radians(cam["yaw"])
    fwd = np.array([np.sin(yaw), -np.cos(yaw)])
    right = np.array([np.cos(yaw), np.sin(yaw)])
    cx, cy = cam["x"], cam["y"]
    gz = nd.map_coordinates(h, [[cy / cell], [cx / cell]], order=1)[0]
    cz = gz + cam["z"]
    horizon = H2 / 2 - f * np.tan(np.radians(cam["pitch"]))
    off = (np.arange(W2) - W2 / 2 + 0.5) / f
    zs = [2.0]
    while zs[-1] < far:
        zs.append(zs[-1] + max(cell * 0.4, zs[-1] * 0.0025))
    zs = np.array(zs, np.float32)
    px = cx + zs[:, None] * (fwd[0] + off[None, :] * right[0])
    py = cy + zs[:, None] * (fwd[1] + off[None, :] * right[1])
    inside = (px >= 0) & (px < n * cell - cell) & (py >= 0) & (py < n * cell - cell)
    coords = [py.ravel() / cell, px.ravel() / cell]
    hz = nd.map_coordinates(h, coords, order=1, mode="nearest").reshape(px.shape)
    Y = horizon - (hz - cz) * f / zs[:, None]
    Y = np.where(inside, Y, H2)
    Y = np.clip(Y, 0, H2)
    Yb = np.minimum.accumulate(Y, axis=0)
    rgb = np.stack([nd.map_coordinates(color[..., c], coords, order=1, mode="nearest").reshape(px.shape)
                    for c in range(3)], -1)
    fog = (1 - np.exp(-zs / 4200.0))[:, None]
    rk = None
    if rock is not None:
        rk = nd.map_coordinates(rock, coords, order=1, mode="nearest").reshape(px.shape)
        wb = nd.map_coordinates(wob, coords, order=1, mode="nearest").reshape(px.shape)
    if wall is not None:
        wrgb = np.stack([nd.map_coordinates(wall[..., c], coords, order=1, mode="nearest").reshape(px.shape)
                         for c in range(3)], -1)
    img = np.zeros((H2, W2, 3), np.float32)
    rows = np.arange(H2) + 0.5
    t = np.clip((rows - 0) / max(1.0, horizon), 0, 1)[:, None]
    sky = SKY_TOP[None, :] * (1 - t) + SKY_LOW[None, :] * t
    for c in range(W2):
        k = np.searchsorted(-Yb[:, c], -rows, side="left")
        hit = k < zs.size
        kk = np.minimum(k, zs.size - 1)
        px_ = rgb[kk, c]
        hp = cz + (horizon - rows) * zs[kk] / f
        if wall is not None:
            below = (hp < hz[kk, c] - 1.0)[:, None]
            px_ = np.where(below, wrgb[kk, c], px_)
        if rk is not None:
            # Strata by the height of this very pixel (a wall's face is one
            # map cell wide, so the map's own colour would streak).
            hw = hp + wb[kk, c]
            st = 1 + 0.12 * np.sin(hw / 6.5) + 0.07 * np.sin(hw / 2.1) + 0.05 * np.sin(hw / 17.0)
            ws = np.maximum(rk[kk, c], below[:, 0]) if wall is not None else rk[kk, c]
            px_ = px_ * (1 + ws * (st - 1))[:, None]
        px_ = px_ * (1 - fog[kk]) + HAZE[None, :] * fog[kk]
        img[:, c] = np.where(hit[:, None], px_, sky)
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    return im.resize((W, Hh), Image.LANCZOS)


def gray(a, lo=None, hi=None):
    lo = np.min(a) if lo is None else lo
    hi = np.max(a) if hi is None else hi
    return Image.fromarray((np.clip((a - lo) / max(1e-6, hi - lo), 0, 1) * 255).astype(np.uint8))
