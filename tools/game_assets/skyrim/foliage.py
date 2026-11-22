"""AL-68: Skyrim trees and bushes as dev stand-ins for the terrain core's foliage.

    python foliage.py [--data <Skyrim SE Data>] [--repo D:/AlteraGame/altera]

For every model content/config/terrain_core/foliage.json scatters, takes a
vanilla Skyrim tree or plant (LAYERS below, cycled over the layer's models),
scales it to the height of the model it stands in for (so the layer's spacing,
scale and range keep their meaning), stands its trunk on the origin, and
writes FOLM v1 + one 256 px picture to assets/terrain_core/foliage/skyrim/,
which src/terrain_core/foliage.cpp prefers when present. A model has one
picture, so the leaves go into the top 3/4 of it and the bark (which tiles in
Skyrim) into the bottom quarter, each bark triangle wrapped into one tile.
Git-ignored, never committed; every model is replace_before_prod
(registry_foliage.json beside the output).
"""
import argparse, io, json, os, struct, sys
import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsa import Library, SEP, texture_key
from nif import Nif

UNIT = 0.0142875
S = 256
LEAF_H = 192          # rows of the leaves; the bark below
T, P = 'landscape/trees/', 'landscape/plants/'
# layer -> (Skyrim meshes, cycled; leaf tint)
LAYERS = {
    'shrubs': ([P + 'swordfern01.nif', P + 'tundrashrub01.nif', P + 'fallforestferncluster01.nif',
                P + 'reachfern01.nif', P + 'clover01.nif'], None),
    'bushes': ([P + 'fallforestshrub01.nif', P + 'vinemaple01.nif', P + 'thicket01.nif', P + 'pineshrub01.nif'], None),
    'autumn_bushes': ([P + 'yellowshrub01.nif', P + 'fallforestshrub02.nif', P + 'deadshrub01.nif',
                       P + 'yellowshrub02.nif'], None),
    'saplings': ([T + 'treeaspen04.nif', T + 'treepineforest03.nif', T + 'treeaspen06.nif',
                  T + 'treepineforest05.nif'], None),
    'conifers': ([T + 'treepineforest03.nif', T + 'treepineforest05.nif', T + 'treepineforest04.nif',
                  T + 'treepineforest02.nif'], None),
    # Skyrim's aspens are autumn-yellow: the broadleaf woods get them greened
    'broadleaf': ([T + 'treeaspen05.nif', T + 'treeaspen06.nif', T + 'treeaspen03.nif', T + 'treeaspen02.nif',
                   T + 'treeaspen01.nif'], (0.8, 1.05, 0.6)),
    'autumn': ([T + 'treeaspen01.nif', T + 'treeaspen02.nif', T + 'treeaspen03.nif', T + 'treeaspen05.nif',
                T + 'treeaspen06.nif'], None),
    'dead_trees': ([T + 'treepineforestdead03.nif', T + 'treepineforestdead05.nif'], None),
}


def shrink_cutout(im, size):
    """An RGBA cut-out texture (leaves, twigs, grass) shrunk keeping its coverage."""
    # Skyrim's needles and twigs are a few texels wide at 2-4K: a plain resize
    # to 256 lets them fall under the alpha test. Keep the coverage instead:
    # the share of solid texels per output texel, boosted, and the colour of
    # the solid ones only (no fringe from the transparent background).
    a = np.asarray(im, np.float32) / 255.0
    solid = (a[:, :, 3] > 0.4).astype(np.float32)
    pre = np.dstack([a[:, :, :3] * solid[:, :, None], solid])
    small = [np.asarray(Image.fromarray(pre[:, :, c]).resize(size, Image.BOX), np.float32) for c in range(4)]
    cov = np.maximum(small[3], 1e-4)
    rgb = np.dstack([small[c] / cov for c in range(3)])
    # the texels the dilation below uncovers take their neighbours' colour, not black
    def blur(x, r=3):
        k = np.ones(2 * r + 1, np.float32) / (2 * r + 1)
        x = np.apply_along_axis(lambda v: np.convolve(v, k, 'same'), 0, x)
        return np.apply_along_axis(lambda v: np.convolve(v, k, 'same'), 1, x)
    bc = np.maximum(blur(small[3]), 1e-4)
    near = np.dstack([blur(small[c]) / bc for c in range(3)])
    rgb = np.where((small[3] > 0.02)[..., None], rgb, near)
    alpha = np.clip(small[3] * 3.0, 0, 1)
    # thicken the clumps a texel, so the mip chain (alpha averaged) keeps them
    # standing at a distance instead of thinning the crowns to bare twigs
    alpha = np.asarray(Image.fromarray((alpha * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3)),
                       np.float32) / 255.0
    out = np.dstack([rgb, alpha])
    return Image.fromarray(np.clip(out * 255, 0, 255).astype(np.uint8))


def picture(lib, path, size, cutout=False):
    d = lib.read(texture_key(path))
    if d is None:
        print('  MISSING TEXTURE', path)
        return Image.new('RGBA', size, (110, 100, 80, 0))
    im = Image.open(io.BytesIO(d))
    im.load()
    im = im.convert('RGBA')
    if not cutout:
        return im.resize(size, Image.LANCZOS)
    return shrink_cutout(im, size)


def old_height(path):
    with open(path, 'rb') as f:
        v = struct.unpack('<4s3I6f', f.read(40))
    return v[9] - v[6]


def build(lib, nif_path, height, tint):
    data = lib.read('meshes' + SEP + nif_path.replace('/', SEP))
    if data is None:
        raise RuntimeError('not in the archives: ' + nif_path)
    shapes = [s for s in Nif(data).shapes() if not s.alpha_blend and len(s.tris)]
    leaves = [s for s in shapes if s.alpha_test]
    bark = [s for s in shapes if not s.alpha_test]
    # the picture: one leaf texture on top (the first; Skyrim trees have one), one bark below
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    if leaves:
        leaf = picture(lib, leaves[0].texture, (S, LEAF_H), cutout=True)
        if tint:
            a = np.asarray(leaf, np.float32)
            a[:, :, :3] *= np.array(tint, np.float32)
            leaf = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
        img.paste(leaf, (0, 0))
    if bark:
        b = picture(lib, bark[0].texture, (S, S - LEAF_H))
        a = np.asarray(b).copy()
        a[:, :, 3] = 255
        img.paste(Image.fromarray(a), (0, LEAF_H))
    P_all = np.concatenate([s.pos for s in shapes]).astype(np.float64) * UNIT
    zlo = P_all[:, 2].min()
    k = height / max(P_all[:, 2].max() - zlo, 1e-3)
    base = P_all[P_all[:, 2] < zlo + 0.6 / max(k, 1e-3)]
    cx, cy = base[:, 0].mean(), base[:, 1].mean()
    leafz = [(s.pos[:, 2].astype(np.float64) * UNIT - zlo) * k for s in leaves]
    crown = float(np.concatenate(leafz).mean()) if leaves else height * 0.6
    verts, idx = [], []
    lo, hi = np.full(3, 1e9), np.full(3, -1e9)
    eps = 1.5 / S
    for s in shapes:
        p = (s.pos.astype(np.float64) * UNIT - [cx, cy, zlo]) * k
        n = s.nrm.astype(np.float64)
        uv = s.uv.astype(np.float64)
        lo, hi = np.minimum(lo, p.min(0)), np.maximum(hi, p.max(0))
        if s.alpha_test:
            # crown normals: out from the crown's centre and a little up, the usual
            # trick for leaf cards (Skyrim's own face every way and drew dark here)
            c = np.array([0.0, 0.0, crown])
            radial = p - c
            radial /= np.maximum(np.linalg.norm(radial, axis=1, keepdims=True), 1e-6)
            n = radial * 0.7 + [0, 0, 0.5]
            n /= np.linalg.norm(n, axis=1, keepdims=True)
            if leaves and s.texture.lower() != leaves[0].texture.lower():
                print('  extra leaf texture ignored:', s.texture)
            u = np.clip(uv[:, 0], eps, 1 - eps)
            v = np.clip(uv[:, 1], eps, 1 - eps) * LEAF_H / S
            first = len(verts)
            for i in range(len(p)):
                verts.append((p[i], n[i], (u[i], v[i])))
            idx.extend((s.tris.reshape(-1) + first).tolist())
        else:
            # bark tiles: each triangle on its own vertices, moved into one tile
            for t in s.tris:
                q = uv[t]
                off = np.floor(q.min(0))
                q = q - off
                span = np.maximum(q.max(0), 1.0)
                q = np.clip(q / span, 0, 1)
                first = len(verts)
                for c in range(3):
                    u = eps + q[c, 0] * (1 - 2 * eps)
                    v = (LEAF_H + 1 + q[c, 1] * (S - LEAF_H - 2)) / S
                    verts.append((p[t[c]], n[t[c]], (u, v)))
                idx.extend([first, first + 1, first + 2])
    return verts, idx, lo, hi, img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    cfg = json.load(open(os.path.join(a.repo, 'content/config/terrain_core/foliage.json'), encoding='utf-8'))
    src = os.path.join(a.repo, 'assets', 'terrain_core', 'foliage')
    out = os.path.join(src, 'skyrim')
    os.makedirs(out, exist_ok=True)
    lib = Library(a.data)
    report = []
    for layer in cfg['layers']:
        if layer['name'] not in LAYERS:
            print('layer', layer['name'], 'keeps its models')
            continue
        nifs, tint = LAYERS[layer['name']]
        for i, name in enumerate(layer['models']):
            nif = nifs[i % len(nifs)]
            h = old_height(os.path.join(src, name + '.mesh'))
            verts, idx, lo, hi, img = build(lib, nif, h, tint)
            with open(os.path.join(out, name + '.mesh'), 'wb') as f:
                f.write(b'FOLM' + struct.pack('<3I6f', 1, len(verts), len(idx), *lo, *hi))
                f.write(b''.join(struct.pack('<8f', *p, *n, *uv) for p, n, uv in verts))
                f.write(np.array(idx, np.uint32).tobytes())
            img.save(os.path.join(out, name + '.png'))
            print('%-10s %-14s <- %-40s tris %5d h %.1f' % (layer['name'], name, nif, len(idx) // 3, hi[2] - lo[2]))
            report.append(dict(model=name, layer=layer['name'], nif=nif, tris=len(idx) // 3,
                               replace_before_prod=True))
    json.dump({'_doc': 'AL-68: Skyrim SE stand-ins for the terrain foliage, dev only, replace before prod.',
               'source': 'Skyrim Special Edition (Bethesda), Data/*.bsa', 'replace_before_prod': True,
               'models': report}, open(os.path.join(out, 'registry_foliage.json'), 'w', encoding='utf-8'), indent=1)


if __name__ == '__main__':
    main()
