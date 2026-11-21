"""AL-10: preview pictures of the build pieces for the build catalog.

    python tools/game_assets/make_build_previews.py [--repo D:/AlteraGame/altera]

Renders every piece of content/ai/build.json from our kit bake
(assets/altera_game/buildings/buildings.astm, see bake_buildings.py) in a three
quarter view, textured and lit, on a clear background, and writes
assets/altera_game/ui/build_previews.png (one row of cells) and
build_previews.json {"cell": px, "pieces": {id: index}} read by the HUD.
"""
import argparse, json, os, struct
import numpy as np
from PIL import Image

CELL, SS = 112, 2


def read_astm(path):
    b = open(path, 'rb').read()
    v, nm, nv, ni, nl, ls = struct.unpack_from('<6I', b, 4)
    p = 28
    meshes = {}
    for _ in range(nm):
        n = b[p:p + 32].split(b'\0')[0].decode()
        fi, ic = struct.unpack_from('<2I', b, p + 32)
        meshes[n] = (fi, ic)
        p += 64
    V = np.frombuffer(b, np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2), ('L', '<u4')]), nv, p)
    p += nv * 36
    I = np.frombuffer(b, '<u4', ni, p)
    p += ni * 4
    layers = np.frombuffer(b, np.uint8, nl * ls * ls * 4, p).reshape(nl, ls, ls, 4)
    return meshes, V, I, layers


def render(V, I, layers, fi, ic, size):
    tri = I[fi:fi + ic].reshape(-1, 3)
    P = V['p'].astype(np.float64)
    used = np.unique(tri)
    # view: yaw 35 deg, pitch 28 deg, the piece's front (-y) toward the viewer
    yaw, pitch = np.radians(-35), np.radians(28)
    Rz = np.array([[np.cos(yaw), -np.sin(yaw), 0], [np.sin(yaw), np.cos(yaw), 0], [0, 0, 1]])
    Rx = np.array([[1, 0, 0], [0, np.cos(pitch), -np.sin(pitch)], [0, np.sin(pitch), np.cos(pitch)]])
    Q = P @ Rz.T
    # camera looks along +y; screen x = x, screen y = z tilted
    Q = Q @ Rx.T
    sx, sy, depth = Q[:, 0], Q[:, 2], Q[:, 1]
    lo = np.array([sx[used].min(), sy[used].min()])
    hi = np.array([sx[used].max(), sy[used].max()])
    S = size * SS
    k = (S * 0.84) / max(hi[0] - lo[0], hi[1] - lo[1], 1e-6)
    cx, cy = (lo + hi) * 0.5
    px = (sx - cx) * k + S / 2
    py = S / 2 - (sy - cy) * k
    img = np.zeros((S, S, 4), np.float32)
    zb = np.full((S, S), -1e9)
    light = np.array([0.45, -0.55, 0.7])
    light /= np.linalg.norm(light)
    N = V['n'].astype(np.float64)
    ls = layers.shape[1]
    for t in tri:
        x, y = px[t], py[t]
        x0, x1 = int(max(np.floor(x.min()), 0)), int(min(np.ceil(x.max()), S - 1))
        y0, y1 = int(max(np.floor(y.min()), 0)), int(min(np.ceil(y.max()), S - 1))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(d) < 1e-9:
            continue
        a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
        b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
        c = 1 - a - b
        m = (a >= -1e-6) & (b >= -1e-6) & (c >= -1e-6)
        if not m.any():
            continue
        z = a * depth[t[0]] + b * depth[t[1]] + c * depth[t[2]]
        z = -z   # nearer = larger
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        m &= z > sub
        if not m.any():
            continue
        uv = a[..., None] * V['uv'][t[0]] + b[..., None] * V['uv'][t[1]] + c[..., None] * V['uv'][t[2]]
        L = V['L'][t[0]]
        tu = (np.mod(uv[..., 0], 1.0) * ls).astype(int) % ls
        tv = (np.mod(uv[..., 1], 1.0) * ls).astype(int) % ls
        tex = layers[L][tv, tu, :3].astype(np.float32) / 255.0
        n = N[t].mean(0)
        n /= max(np.linalg.norm(n), 1e-9)
        shade = 0.55 + 0.55 * max(0.0, float(n @ light))
        col = np.clip(tex * shade, 0, 1)
        sub[m] = z[m]
        reg = img[y0:y1 + 1, x0:x1 + 1]
        reg[m, :3] = col[m]
        reg[m, 3] = 1.0
    # a soft dark rim so the piece reads on paper
    a = img[:, :, 3]
    rim = np.zeros_like(a)
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        rim = np.maximum(rim, np.roll(np.roll(a, dx * SS, 1), dy * SS, 0))
    out = np.zeros_like(img)
    out[:, :, :3] = img[:, :, :3] * a[..., None] + np.array([0.17, 0.15, 0.12]) * (1 - a[..., None])
    out[:, :, 3] = np.maximum(a, rim * 0.8)
    im = Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGBA').resize((size, size), Image.LANCZOS)
    return np.asarray(im)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    meshes, V, I, layers = read_astm(os.path.join(a.repo, 'assets/altera_game/buildings/buildings.astm'))
    build = json.load(open(os.path.join(a.repo, 'content/ai/build.json'), encoding='utf-8'))
    cells, index = [], {}
    for p in build['pieces']:
        mesh = p.get('mesh', p['id'])
        if mesh not in meshes:
            print('no mesh for', p['id'])
            continue
        fi, ic = meshes[mesh]
        index[p['id']] = len(cells)
        cells.append(render(V, I, layers, fi, ic, CELL))
        print('preview', p['id'])
    out = os.path.join(a.repo, 'assets/altera_game/ui')
    Image.fromarray(np.concatenate(cells, 1)).save(os.path.join(out, 'build_previews.png'))
    json.dump({'cell': CELL, 'pieces': index}, open(os.path.join(out, 'build_previews.json'), 'w'), indent=1)
    print('wrote', len(cells), 'previews')


if __name__ == '__main__':
    main()
