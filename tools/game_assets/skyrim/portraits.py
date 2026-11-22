"""AL-68: HUD portraits of the Skyrim stand-in people (people_sky.askin).

    python portraits.py [--repo D:/AlteraGame/altera]

The HUD's portraits (src/game_view/hud.cpp) were rendered by bake_people.py
from our own people; the Skyrim stand-ins (actors.py) had none, so the HUD
kept showing the old faces. This renders each look of people_sky.askin in its
rest pose with the same framing (orthographic, 0.42 m, from the front-right,
128 px) in plain numpy, plus the class mask (red skin, green hair, blue cloth)
the HUD tints by, to assets/altera_game/people/portrait_<look>_sky.png and
portrait_<look>_sky_id.png (git-ignored, replace_before_prod), which the HUD
prefers when present.
"""
import argparse, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from askin import Askin

OUT = 128
SS = 3              # supersampling
ORTHO = 0.46


def render(a, look):
    nm, skel, fi, ic, _ = look
    R = a.rest_world(skel)
    head = R[a.names.index('Head')][:3, 3]
    target = head + [0, 0, 0.02]   # Skyrim heads sit lower on the rig than ours
    cam = target + [0.22, -1.2, 0.05]
    f = target - cam
    f /= np.linalg.norm(f)
    r = np.cross(f, [0, 0, 1.0])
    r /= np.linalg.norm(r)
    u = np.cross(r, f)
    tri = a.idx[fi:fi + ic].reshape(-1, 3)
    V = a.verts
    P = V['pos'].astype(np.float64)
    near = (np.abs(P[:, 0] - target[0]) < 0.4) & (P[:, 2] > target[2] - 0.45)
    tri = tri[near[tri].all(1)]
    S = OUT * SS
    sx = ((P - target) @ r / ORTHO + 0.5) * S
    sy = (0.5 - (P - target) @ u / ORTHO) * S
    dz = (P - cam) @ f
    depth = np.full((S, S), np.inf)
    col = np.zeros((S, S, 3))
    cls = np.zeros((S, S), np.int32)
    hit = np.zeros((S, S), bool)
    atlas = a.atlas.astype(np.float64) / 255.0
    AH, AW = atlas.shape[:2]
    L = np.array([0.45, -0.75, 0.55])
    L /= np.linalg.norm(L)
    for t in tri:
        x, y, z = sx[t], sy[t], dz[t]
        x0, x1 = max(int(np.floor(x.min())), 0), min(int(np.ceil(x.max())), S - 1)
        y0, y1 = max(int(np.floor(y.min())), 0), min(int(np.ceil(y.max())), S - 1)
        if x0 > x1 or y0 > y1:
            continue
        den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(den) < 1e-12:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den
        w1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = depth[y0:y1 + 1, x0:x1 + 1]
        ok = inside & (zz < sub)
        if not ok.any():
            continue
        uv = V['uv'][t].astype(np.float64)
        uu = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
        vv = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
        tx = np.clip((uu * AW).astype(int), 0, AW - 1)
        ty = np.clip((vv * AH).astype(int), 0, AH - 1)
        tex = atlas[ty, tx]
        ok &= tex[:, :, 3] >= 0.5
        if not ok.any():
            continue
        n = V['nrm'][t].astype(np.float64)
        nn = n[0] * w0[..., None] + n[1] * w1[..., None] + n[2] * w2[..., None]
        nn /= np.maximum(np.linalg.norm(nn, axis=-1, keepdims=True), 1e-9)
        nn = np.where((nn @ f)[..., None] > 0, -nn, nn)       # two-sided cards
        shade = 0.42 + 0.7 * np.clip(nn @ L, 0, 1)
        sub[ok] = zz[ok]
        col[y0:y1 + 1, x0:x1 + 1][ok] = tex[:, :, :3][ok] * shade[ok, None]
        cls[y0:y1 + 1, x0:x1 + 1][ok] = V['k'][t[0]]
        hit[y0:y1 + 1, x0:x1 + 1][ok] = True
    pic = np.dstack([np.clip(col, 0, 1), hit.astype(np.float64)])
    ids = np.zeros((S, S, 4))
    for k, ch in ((1, 0), (2, 1), (3, 2)):
        ids[:, :, ch] = cls == k
    ids[:, :, 3] = hit
    down = lambda img: img.reshape(OUT, SS, OUT, SS, 4).mean((1, 3))
    p = down(pic)
    p[:, :, :3] /= np.maximum(p[:, :, 3:], 1e-6)      # straight alpha
    return (np.clip(p, 0, 1) * 255).astype(np.uint8), (np.clip(down(ids), 0, 1) * 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    d = os.path.join(a.repo, 'assets', 'altera_game', 'people')
    sk = Askin.read(os.path.join(d, 'people_sky.askin'))
    for look in sk.looks:
        pic, ids = render(sk, look)
        Image.fromarray(pic, 'RGBA').save(os.path.join(d, 'portrait_%s_sky.png' % look[0]))
        Image.fromarray(ids, 'RGBA').save(os.path.join(d, 'portrait_%s_sky_id.png' % look[0]))
        print('portrait', look[0], 'covered %.0f%%' % (100 * (pic[:, :, 3] > 128).mean()))


if __name__ == '__main__':
    main()
