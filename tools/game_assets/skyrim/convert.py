"""AL-11: the dev-only Skyrim stand-ins -> assets/altera_game/props/props.astm.

    python convert.py --data "D:/Steam/steamapps/common/Skyrim Special Edition/Data" --repo D:/AlteraGame/altera

Reads registry.json (sim object type / building piece -> a vanilla mesh), pulls
the meshes and their diffuse textures out of the game's archives (bsa.py, nif.py),
scales each to its `fit`, stands it on the ground with the footprint centred,
and writes one ASTM v1 (the format of src/game_view/statics.cpp, see
bake_buildings.py) plus a preview sheet and the registry beside it, every
piece marked replace_before_prod. Nothing from Skyrim is committed except this
baked, git-ignored output: the archives stay on the user's disk.
"""
import argparse, json, os, struct, io, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsa import Library, SEP, texture_key
from nif import Nif
from foliage import shrink_cutout

UNIT = 0.0142875   # metres per Skyrim unit
LS = 256           # texture layer size


def tex_layer(lib, path, cache, layers, cutout=False):
    # alpha: the cut-out of leaves and grass, else opaque (Skyrim keeps gloss there)
    key = (path.lower(), cutout)
    if key in cache:
        return cache[key]
    p = texture_key(key[0])
    data = lib.read(p)
    img = None
    if data:
        try:
            img = Image.open(io.BytesIO(data))
            img.load()
            img = img.convert('RGBA')
            img = shrink_cutout(img, (LS, LS)) if cutout else img.resize((LS, LS), Image.LANCZOS)
        except Exception as e:
            print('  texture', path, 'unreadable:', e)
            img = None
    if img is None:
        print('  MISSING TEXTURE', path)
        img = Image.new('RGBA', (LS, LS), (150, 140, 130, 255))
    cache[key] = len(layers)
    arr = np.asarray(img, np.uint8).copy()
    if not cutout:
        arr[:, :, 3] = 255
    layers.append(arr)
    return cache[key]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    reg = json.load(open(os.path.join(here, 'registry.json'), encoding='utf-8'))
    lib = Library(a.data)
    out_dir = os.path.join(a.repo, 'assets', 'altera_game', 'props')
    os.makedirs(out_dir, exist_ok=True)
    meshes, verts, idx, layers, cache = [], [], [], [], {}
    nv = 0
    report = []
    for e in reg['pieces']:
        name = e['piece']
        first = len(idx)
        mn, mx = [0.0] * 3, [0.0] * 3
        if 'tile' in e:
            # AL-68: a ground tile (roads, furrows): a unit slab 0.1 m deep, the
            # texture on top, laid by the game a tile at a time instead of flat blocks
            L = tex_layer(lib, e['tile'], cache, layers)
            box = [((-.5, -.5, 0), (.5, -.5, 0), (.5, .5, 0), (-.5, .5, 0), (0, 0, 1)),
                   ((-.5, -.5, -.1), (.5, -.5, -.1), (.5, -.5, 0), (-.5, -.5, 0), (0, -1, 0)),
                   ((.5, .5, -.1), (-.5, .5, -.1), (-.5, .5, 0), (.5, .5, 0), (0, 1, 0)),
                   ((.5, -.5, -.1), (.5, .5, -.1), (.5, .5, 0), (.5, -.5, 0), (1, 0, 0)),
                   ((-.5, .5, -.1), (-.5, -.5, -.1), (-.5, -.5, 0), (-.5, .5, 0), (-1, 0, 0))]
            for *q, nrm in box:
                for c, (u, v) in zip(q, ((0, 1), (1, 1), (1, 0), (0, 0))):
                    if nrm[2] == 0:
                        v = 0.95 + 0.05 * v   # the sides: a sliver of the texture
                    verts.append(struct.pack('<8fI', *c, *nrm, u, v, L))
                idx.extend([nv, nv + 1, nv + 2, nv, nv + 2, nv + 3])
                nv += 4
            mn, mx = [-.5, -.5, -.1], [.5, .5, 0.0]
            meshes.append((name, first, len(idx) - first, mn, mx))
            print('%-18s tile %s' % (name, e['tile']))
            report.append(dict(e, ok=True, size_m=[1.0, 1.0, 0.1], replace_before_prod=True))
            continue
        data = lib.read('meshes' + SEP + e['nif'].replace('/', SEP))
        if data is None:
            print('MISSING', name, e['nif'])
            report.append(dict(e, ok=False, why='not in the archives'))
            meshes.append((name, first, 0, mn, mx))
            continue
        shapes = Nif(data).shapes()
        shapes = [s for s in shapes if not s.alpha_blend]
        if not shapes:
            print('EMPTY', name, e['nif'])
            report.append(dict(e, ok=False, why='no static shapes'))
            meshes.append((name, first, 0, mn, mx))
            continue
        P = np.concatenate([s.pos for s in shapes]).astype(np.float64) * UNIT
        lo, hi = P.min(0), P.max(0)
        ext = max(hi[0] - lo[0], hi[1] - lo[1], 1e-3)
        k = e['fit'] / ext if 'fit' in e else e['fit_h'] / max(hi[2] - lo[2], 1e-3) if 'fit_h' in e else e.get('scale', 1.0)
        cx, cy = (lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5
        z0 = 0.0 if e.get('ground') == 'origin' else lo[2]
        if not e.get('hide'):
            for s in shapes:
                L = tex_layer(lib, s.texture, cache, layers, s.alpha_test)
                p = (s.pos.astype(np.float64) * UNIT - [cx, cy, z0]) * k
                for i in range(len(p)):
                    verts.append(struct.pack('<8fI', p[i, 0], p[i, 1], p[i, 2], *s.nrm[i], s.uv[i, 0], s.uv[i, 1], L))
                idx.extend((s.tris.reshape(-1) + nv).tolist())
                nv += len(p)
            Q = (P - [cx, cy, z0]) * k
            mn, mx = Q.min(0).tolist(), Q.max(0).tolist()
        meshes.append((name, first, len(idx) - first, mn, mx))
        size = [round(mx[i] - mn[i], 2) for i in range(3)]
        print('%-18s %-60s tris %6d size %s' % (name, e['nif'], (len(idx) - first) // 3, size))
        report.append(dict(e, ok=True, size_m=size, replace_before_prod=True))
    path = os.path.join(out_dir, 'props.astm')
    with open(path, 'wb') as f:
        f.write(b'ASTM' + struct.pack('<6I', 1, len(meshes), nv, len(idx), len(layers), LS))
        for name, fi, ic, mn, mx in meshes:
            f.write(name.encode()[:31].ljust(32, b'\0') + struct.pack('<2I6f', fi, ic, *mn, *mx))
        f.write(b''.join(verts))
        f.write(np.array(idx, np.uint32).tobytes())
        for L in layers:
            f.write(L.tobytes())
    out = dict(reg)
    out['pieces'] = report
    json.dump(out, open(os.path.join(out_dir, 'registry.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('wrote', path, 'pieces', len(meshes), 'verts', nv, 'tris', len(idx) // 3, 'layers', len(layers),
          'MB', round(os.path.getsize(path) / 1e6, 1))


if __name__ == '__main__':
    main()
