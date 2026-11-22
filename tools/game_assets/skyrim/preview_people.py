"""Posed preview of the people bake (askin + aanim) without the game: skins on the CPU
like src/game_view/skinned.cpp and draws flat-shaded views (painter's order).

    python preview_people.py <out.png> [--clips sky__mt_idle,sky__mt_walkforward] [--looks 0,4]
                             [--t 0.3] [--views front,side]
"""
import argparse, json, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from askin import Askin
from people_native import read_aanim, qmat


def pose(a, si, row):
    nj = len(a.names)
    sk = a.skeletons[si]
    W = [None] * nj
    pal = np.zeros((nj, 4, 4))
    for j in range(nj):
        B = np.eye(4)
        q = row[j, :4]
        B[:3, :3] = qmat(q / max(np.linalg.norm(q), 1e-12))
        B[:3, 3] = row[j, 4:7]
        L = sk[j, 0] @ B
        p = a.parents[j]
        W[j] = W[p] @ L if p >= 0 else L
        pal[j] = W[j] @ sk[j, 1]
    return pal


def skin(a, pal, sel):
    v = a.verts[sel]
    P = np.c_[v['pos'].astype(np.float64), np.ones(len(v))]
    out = np.zeros((len(v), 3))
    w = v['w'].astype(np.float64) / 255.0
    for c in range(4):
        M = pal[v['j'][:, c]]
        out += w[:, c, None] * np.einsum('nij,nj->ni', M[:, :3, :], P)
    return out


def draw(a, look, P, uvcol, view, size=360):
    nm, si, fi, ic, h = a.looks[look]
    tris = a.idx[fi:fi + ic].reshape(-1, 3)
    img = Image.new('RGB', (size, size), (225, 225, 220))
    d = ImageDraw.Draw(img)
    if view == 'front':
        X, Y, Z = P[:, 0], P[:, 2], P[:, 1]        # camera at -y looking +y: x right, depth +y
        X = X
    else:
        X, Y, Z = -P[:, 1], P[:, 2], P[:, 0]       # from +x: forward (-y) to the right
    cx = 0.0
    s = (size - 20) / 2.0
    tri_z = Z[tris].mean(1)
    order = np.argsort(-tri_z)
    for t in order:
        i = tris[t]
        col = uvcol[i[0]]
        if col[3] < 100:
            continue
        e1 = P[i[1]] - P[i[0]]
        e2 = P[i[2]] - P[i[0]]
        n = np.cross(e1, e2)
        ln = np.linalg.norm(n)
        if ln < 1e-12:
            continue
        n /= ln
        L = np.array([0.4, -0.7, 0.6]) if view == 'front' else np.array([0.7, -0.4, 0.6])
        k = 0.45 + 0.55 * abs(float(n @ (L / np.linalg.norm(L))))
        pts = [((X[q] - cx) * s + size / 2, size - 10 - Y[q] * s) for q in i]
        d.polygon(pts, fill=tuple(int(c * k) for c in col[:3]))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    ap.add_argument('--clips', default='')
    ap.add_argument('--looks', default='0,4')
    ap.add_argument('--t', default='0.0')
    ap.add_argument('--views', default='front,side')
    ap.add_argument('--mesh', default='assets/altera_game/people/people_sky.askin')
    ap.add_argument('--anim', default='assets/altera_game/people/people_sky.aanim')
    a = ap.parse_args()
    sk = Askin.read(os.path.join(a.repo, a.mesh))
    clips, frames, nj, fps = read_aanim(os.path.join(a.repo, a.anim)) if a.anim else ([], None, 0, 15)
    byname = {c[0]: c for c in clips}
    ah, aw = sk.atlas.shape[:2]
    uv = sk.verts['uv']
    col = sk.atlas[np.clip((uv[:, 1] * ah).astype(int), 0, ah - 1), np.clip((uv[:, 0] * aw).astype(int), 0, aw - 1)]
    tiles = []
    names = [c for c in a.clips.split(',') if c] or ['rest']
    ts = [float(x) for x in a.t.split(',')]
    for look in [int(x) for x in a.looks.split(',')]:
        nm, si, fi, ic, h = sk.looks[look]
        sel = np.arange(len(sk.verts))
        for cn in names:
            for t in ts:
                if cn == 'rest' or cn not in byname:
                    row = np.zeros((len(sk.names), 7))
                    row[:, 3] = 1
                    anc = np.zeros(2)
                else:
                    c = byname[cn + '@f'] if (nm[0] == 'f' and cn + '@f' in byname) else byname[cn]
                    k = int(t * fps) % c[1]
                    fr = frames[c[3] + k]
                    row = fr[:nj * 7].reshape(nj, 7)
                    anc = fr[nj * 7:nj * 7 + 2]
                pal = pose(sk, si, row)
                P = skin(sk, pal, sel)
                P[:, 0] -= anc[0]
                P[:, 1] -= anc[1]
                for v in a.views.split(','):
                    im = draw(sk, look, P, col, v)
                    ImageDraw.Draw(im).text((4, 4), '%s %s t=%.2f %s' % (nm, cn, t, v), fill=(0, 0, 0))
                    tiles.append(im)
    per = max(1, len(names) * len(ts) * len(a.views.split(',')))
    rows = [tiles[i:i + per] for i in range(0, len(tiles), per)]
    W = max(len(r) for r in rows) * 360
    out = Image.new('RGB', (W, 360 * len(rows)), (255, 255, 255))
    for y, r in enumerate(rows):
        for x, im in enumerate(r):
            out.paste(im, (x * 360, y * 360))
    out.save(a.out)
    print('wrote', a.out)


if __name__ == '__main__':
    main()
