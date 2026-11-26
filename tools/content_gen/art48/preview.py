# LR-167: a PNG sheet of sprite entries for a look (not shipped)
import sys
from PIL import Image

from export import to_rgba


def sheet(entries, path, scale=2, cols=8, bg=(60, 52, 44), tile=None):
    names = list(entries)
    sz = max(max(len(e["rows"]), len(e["rows"][0])) for e in entries.values())
    cell = sz + 4
    rows = (len(names) + cols - 1) // cols
    img = Image.new("RGBA", (cols * cell, rows * cell), bg + (255,))
    for i, n in enumerate(names):
        px = to_rgba(entries[n])
        h, w = len(px), len(px[0])
        im = Image.new("RGBA", (w, h))
        im.putdata([c for r in px for c in r])
        ox, oy = (i % cols) * cell + 2, (i // cols) * cell + 2
        if tile is not None:
            for ty in range(0, sz, 48):
                for tx in range(0, sz, 48):
                    img.alpha_composite(tile, (ox + tx, oy + ty))
        img.alpha_composite(im, (ox + (sz - w) // 2, oy + (sz - h)))
    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(path)
