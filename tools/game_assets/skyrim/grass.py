"""AL-68: Skyrim grass as a dev stand-in for the terrain core's grass sprites.

    python grass.py [--data <Skyrim SE Data>] [--repo D:/AlteraGame/altera]

The grass cover (src/terrain_core/grass.cpp) draws 18 sprites, 14 grass then 4
flowering, tinted by the ground's colour in the shader, so they are wanted
light and colourless (see tools/terrain_lab/build_grass.py). This takes tufts
from Skyrim's grass textures (whole or a half of one), keeps their light and
shade as grey, stands each on the bottom edge of a 256 square, and writes
assets/terrain_core/grass/skyrim/NN.png, which grass.cpp prefers when present.
Git-ignored, replace_before_prod.
"""
import argparse, io, json, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsa import Library, texture_key

G = 'landscape/grass/'
# (texture, part: all | left | right)
GRASS = [(G + 'fieldgrassobj01', 'all'), (G + 'fieldgrassobj01', 'left'), (G + 'fieldgrassobj01', 'right'),
         (G + 'tundragrassobj01', 'all'), (G + 'frozenmarshgrassobj01', 'left'), (G + 'tundragrassobj03', 'all'),
         (G + 'tundragrassobj04', 'all'), (G + 'fallforestgrassobj01', 'all'), (G + 'coastbeachgrassobj01', 'left'),
         (G + 'volcanictundragrassobj01', 'all'), (G + 'volcanictundragrassobj02', 'all'),
         (G + 'frozenmarshgrassobj01', 'all'), (G + 'coastbeachgrassobj01', 'all'), (G + 'tundragrassobj01', 'left')]
FLOWERS = [(G + 'fieldgrassobj02', 'all'), (G + 'fieldgrassobj02', 'left'), (G + 'fieldgrassobj02', 'right'),
           ('plants/mountainflower01blue', 'all')]


def sprite(lib, tex, part, S=256):
    d = lib.read(texture_key(tex + '.dds'))
    if d is None:
        raise RuntimeError('missing ' + tex)
    im = Image.open(io.BytesIO(d))
    im.load()
    im = im.convert('RGBA')
    w, h = im.size
    if part == 'left':
        im = im.crop((0, 0, w // 2, h))
    elif part == 'right':
        im = im.crop((w // 2, 0, w, h))
    a = np.asarray(im, np.float32) / 255.0
    solid = a[:, :, 3] > 0.4
    ys, xs = np.nonzero(solid)
    im = im.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    side = max(im.size)
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.size[0]) // 2, side - im.size[1]))
    # coverage-keeping shrink (thin blades would drop under the alpha test)
    a = np.asarray(sq, np.float32) / 255.0
    solid = (a[:, :, 3] > 0.4).astype(np.float32)
    grey = (a[:, :, 0] * 0.3 + a[:, :, 1] * 0.59 + a[:, :, 2] * 0.11) * solid
    small = lambda x: np.asarray(Image.fromarray(x).resize((S, S), Image.BOX), np.float32)
    cov = small(solid)
    g = small(grey) / np.maximum(cov, 1e-4)
    # light and colourless, as the shader tints it: lift the tuft's mean to ~0.85
    m = g[cov > 0.2].mean() if (cov > 0.2).any() else 0.5
    g = np.clip(g * (0.85 / max(m, 1e-3)), 0, 1)
    alpha = np.clip(cov * 2.5, 0, 1)
    out = np.dstack([g, g, g, alpha])
    return Image.fromarray((out * 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    a = ap.parse_args()
    out = os.path.join(a.repo, 'assets', 'terrain_core', 'grass', 'skyrim')
    os.makedirs(out, exist_ok=True)
    lib = Library(a.data)
    rows = []
    for i, (tex, part) in enumerate(GRASS + FLOWERS):
        sprite(lib, tex, part).save(os.path.join(out, '%02d.png' % i))
        rows.append(dict(sprite='%02d' % i, texture=tex, part=part, replace_before_prod=True))
        print('%02d <- %s (%s)' % (i, tex, part))
    json.dump({'_doc': 'AL-68: Skyrim SE grass stand-ins, dev only, replace before prod.', 'replace_before_prod': True,
               'sprites': rows}, open(os.path.join(out, 'registry_grass.json'), 'w', encoding='utf-8'), indent=1)


if __name__ == '__main__':
    main()
