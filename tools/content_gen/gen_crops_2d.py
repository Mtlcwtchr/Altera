# AL-112: fields and kitchen gardens of the 2D client as 16x16 tiles:
# content/sprites_2d/crops2d.json. Every sprite is a grid of palette letters of
# sprites.json (no new colours), drawn here procedurally with a fixed seed, so
# the file is text and reproducible (no binary assets, no LFS).
#
# The file also holds the plot rules src/client2d/farm2d reads ("plots"):
# how a crop object's ground is cut into strips / beds and which tile shows
# which stage. Rerun after a change:
#   python tools/content_gen/gen_crops_2d.py [--sheet out.png]
#
# Tiles draw rows along x (furrows, beds); the client turns a plot whose rows
# run along y by 90 degrees.
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sprites_2d_src as src  # noqa: E402

PAL = src.PAL


class G:
    def __init__(self, base='.'):
        self.g = [[base] * 16 for _ in range(16)]

    def set(self, x, y, c):
        if 0 <= x < 16 and 0 <= y < 16 and c != ' ':
            self.g[y][x] = c

    def get(self, x, y):
        return self.g[y][x] if 0 <= x < 16 and 0 <= y < 16 else '.'

    def stamp(self, rows, x0, y0):
        # rows of letters, ' ' leaves the cell as it is; (x0, y0) = the centre
        h, w = len(rows), max(len(r) for r in rows)
        ox, oy = x0 - w // 2, y0 - h // 2
        for yy, r in enumerate(rows):
            for xx, c in enumerate(r):
                self.set(ox + xx, oy + yy, c)

    def copy(self):
        n = G()
        n.g = [r[:] for r in self.g]
        return n

    def rows(self):
        return [''.join(r) for r in self.g]


def specks(g, rnd, pairs, only=None):
    for ch, n in pairs:
        for _ in range(n):
            x, y = rnd.randrange(16), rnd.randrange(16)
            if only is None or g.get(x, y) in only:
                g.set(x, y, ch)


S = {}

# --- field ground ---------------------------------------------------------------
# A strip of plough land: furrow rows every 4 px (y % 4 == 3 the furrow bottom).


def tilled(seed):
    rnd = random.Random(seed)
    g = G('n')
    for y in range(16):
        for x in range(16):
            r = y % 4
            g.set(x, y, 'N' if r == 3 else ('e' if r == 0 and (x + y) % 3 else 'n'))
    specks(g, rnd, [('E', 10), ('N', 6), ('p', 6)], only='n')
    return g


def fallow(seed):
    rnd = random.Random(seed)
    g = G('e')
    for y in range(16):
        for x in range(16):
            if y % 4 == 3 and rnd.random() < 0.5:
                g.set(x, y, 'E')
    specks(g, rnd, [('G', 34), ('l', 16), ('D', 14), ('E', 8), ('T', 6)])
    for _ in range(4):   # tufts
        x, y = rnd.randrange(1, 15), rnd.randrange(1, 15)
        g.stamp(["lG", "GD"], x, y)
    return g


def stubble(seed):
    rnd = random.Random(seed)
    g = G('e')
    for y in range(16):
        for x in range(16):
            r = y % 4
            if r == 3:
                g.set(x, y, 'E')
            elif r in (0, 1) and (x + 2 * (y // 4)) % 2 == 0:
                g.set(x, y, 'T' if r == 1 else 't')
    specks(g, rnd, [('t', 14), ('y', 5), ('G', 5), ('E', 6)])
    return g


S['fd_tilled'] = tilled(101).rows()
S['fd_tilled2'] = tilled(102).rows()
S['fd_fallow'] = fallow(111).rows()
S['fd_fallow2'] = fallow(112).rows()
S['fd_stubble'] = stubble(121).rows()
S['fd_stubble2'] = stubble(122).rows()

g = tilled(131)
rnd = random.Random(131)
for y in range(0, 16, 4):
    for x in range(rnd.randrange(2), 16, 3):
        g.set(x, y + 1, 't')
S['fd_sown'] = g.rows()

# --- grain (field) ------------------------------------------------------------
g = tilled(141)
for y in range(0, 16, 4):
    for x in range((y // 4) % 2, 16, 3):
        g.set(x, y + 1, 'l')
        g.set(x, y + 2, 'G')
S['gr_sprout'] = g.rows()

g = tilled(151)
rnd = random.Random(151)
for y in range(0, 16, 4):
    for x in range(16):
        if (x + y // 4) % 2 == 0:
            g.set(x, y, 'l')
            g.set(x, y + 1, 'G')
            g.set(x, y + 2, 'G' if rnd.random() < 0.7 else 'l')
        elif rnd.random() < 0.4:
            g.set(x, y + 1, 'D')
S['gr_young'] = g.rows()


def tall(seed, top, mid, shade, ear=None, ears=0):
    rnd = random.Random(seed)
    g = G(mid)
    for y in range(16):
        for x in range(16):
            r = y % 4
            if r == 3:
                g.set(x, y, shade if rnd.random() < 0.75 else 'n')
            elif r == 0 and rnd.random() < 0.5:
                g.set(x, y, top)
            elif rnd.random() < 0.15:
                g.set(x, y, shade)
    if ear:
        for _ in range(ears):
            x, y = rnd.randrange(16), rnd.randrange(16)
            if y % 4 != 3:
                g.set(x, y, ear)
                g.set(x, y + 1 if (y + 1) % 4 != 3 else y - 1, ear)
    return g


S['gr_grown'] = tall(161, 'l', 'G', 'D').rows()
S['gr_heading'] = tall(171, 'l', 'G', 'D', 'T', 14).rows()   # ears coming, still green
S['gr_ripe'] = tall(181, 'y', 't', 'T', 'o', 16).rows()
S['gr_ripe2'] = tall(182, 'F', 'y', 'T', 'o', 14).rows()

# lodged / shattering: flattened diagonally, soil showing through
rnd = random.Random(191)
g = G('T')
for y in range(16):
    for x in range(16):
        d = (x + y) % 5
        g.set(x, y, 't' if d in (0, 1) else ('o' if d == 2 and rnd.random() < 0.5 else 'T'))
specks(g, rnd, [('e', 22), ('E', 10), ('n', 6)])
S['gr_over'] = g.rows()

# --- kitchen garden ground ------------------------------------------------------
# A bed is one cell across (y) and runs along x: dark worked soil, a raised
# rim of darker earth top and bottom; the path between bed columns is packed earth.


def bed(seed, clean=True):
    rnd = random.Random(seed)
    g = G('p')
    for x in range(16):
        g.set(x, 0, 'P')
        g.set(x, 15, 'P')
        g.set(x, 1, 'E' if x % 3 else 'P')
        g.set(x, 14, 'E' if (x + 1) % 3 else 'P')
    for y in range(2, 14):
        for x in range(16):
            if clean and y in (5, 10):
                g.set(x, y, 'P' if x % 2 else 'p')
    specks(g, rnd, [('E', 12), ('P', 10), ('e', 4)], only='p')
    return g


rnd = random.Random(201)
g = G('e')
specks(g, rnd, [('t', 18), ('E', 14), ('W', 3), ('g', 3), ('G', 3)])
S['gd_path'] = g.rows()

S['gd_tilled'] = bed(211).rows()
g = bed(221, clean=False)
rnd = random.Random(221)
specks(g, rnd, [('T', 10), ('t', 6), ('G', 4), ('D', 3)], only='pE')
for _ in range(3):   # old stalks
    x, y = rnd.randrange(2, 14), rnd.randrange(3, 12)
    g.stamp(["T", "T", "t"], x, y)
S['gd_bare'] = g.rows()

g = bed(231)
for x in range(1, 16, 2):
    g.set(x, 5, 't')
    g.set(x + 1 if x < 15 else x, 10, 't')
S['gd_sown'] = g.rows()

g = bed(241)
for cx in (4, 11):
    for cy in (5, 10):
        g.stamp(["l l", " G "], cx, cy)
S['gd_sprout'] = g.rows()

g = bed(251, clean=False)
rnd = random.Random(251)
for cx in (4, 11):
    g.stamp([" TT T ", "TtTTtT", " tTot ", "  TT  "], cx, 8)
specks(g, rnd, [('T', 6), ('e', 4)], only='p')
S['gd_over'] = g.rows()

# --- garden crops ---------------------------------------------------------------
# Each crop: young / grown / ripe on a bed; two big plants per tile (x 4 and
# 11) or rows of small ones. Shapes are told apart at a glance: cabbage heads,
# carrot fern, red-stemmed beet, onion spikes, leek fans, peas on a twig fence,
# runner beans on stakes, turnips with white shoulders.
CROPS = {}


def crop(name, young, grown, ripe):
    CROPS[name] = {}
    for stage, fn, seed in (("young", young, 1), ("grown", grown, 2), ("ripe", ripe, 3)):
        g = bed(300 + 10 * len(CROPS) + seed)
        fn(g)
        S['gd_%s_%s' % (name, stage)] = g.rows()
        CROPS[name][stage] = ['gd_%s_%s' % (name, stage)]


def at2(g, rows, cy=8):
    for cx in (4, 11):
        g.stamp(rows, cx, cy)


def at_rows(g, rows, xs=(2, 7, 12), ys=(5, 10)):
    for cy in ys:
        for cx in xs:
            g.stamp(rows, cx, cy)


crop("cabbage",
     lambda g: at2(g, [" lG ", "lGGl", " Gl "]),
     lambda g: at2(g, ["  DD  ", " DGGD ", "DGllGD", "DGllGD", " DGGD ", "  DD  "]),
     lambda g: at2(g, [" D  D ", "DGDDGD", "DGllGD", "GlwwlG", "GlwwlG", "DGllGD", "DGDDGD", " D  D "]))
crop("carrot",
     lambda g: at_rows(g, ["l", "G"]),
     lambda g: at_rows(g, ["l l", " l ", "lGl"]),
     lambda g: at_rows(g, ["l l", "lGl", " f ", " o "]))
crop("beet",
     lambda g: at2(g, ["G G", " r "]),
     lambda g: at2(g, ["D  D", "DrrD", " GrG", "  r "]),
     lambda g: at2(g, ["DD  DD", "DrD DrD", " DrrD ", "  VV  ", "  RV  "]))
crop("onion",
     lambda g: at_rows(g, ["l", "l"], xs=(2, 6, 10, 14)),
     lambda g: at_rows(g, ["l G", "GlG", " G "], xs=(2, 6, 10, 14)),
     lambda g: at_rows(g, ["T t", " ww", " wt"], xs=(2, 6, 10, 14)))
crop("leek",
     lambda g: at_rows(g, ["G", "G", "w"], xs=(2, 6, 10, 14), ys=(8,)),
     lambda g: at_rows(g, ["D G", "GDG", " G ", " w ", " w "], xs=(2, 6, 10, 14), ys=(8,)),
     lambda g: at_rows(g, ["D D", "DGD", "DGD", " m ", " w ", " w "], xs=(2, 6, 10, 14), ys=(8,)))


def twigs(g):
    for x in range(16):
        g.set(x, 8, 'n' if x % 3 else 'N')


crop("peas",
     lambda g: (twigs(g), at_rows(g, ["l", "G"], xs=(1, 5, 9, 13), ys=(7, 10))),
     lambda g: (twigs(g), at_rows(g, ["lGl", "GlG", "lGl"], xs=(2, 6, 10, 14), ys=(8,)), g.set(6, 6, 'w'), g.set(13, 10, 'w')),
     lambda g: (twigs(g), at_rows(g, ["lGl", "GlG", "lGl"], xs=(2, 6, 10, 14), ys=(8,)),
                [g.stamp(["D", "G", "D"], x, 8) for x in (4, 8, 12)], g.set(1, 6, 'w'), g.set(11, 10, 'w')))


def stakes(g):
    for cx in (4, 11):
        for y in range(3, 14):
            g.set(cx, y, 'n')


crop("beans",
     lambda g: (stakes(g), at2(g, ["D D", " G "], cy=11)),
     lambda g: (stakes(g), at2(g, [" DD ", "DGnD", "DnGD", "DGnD", " DnD", " DD "])),
     lambda g: (stakes(g), at2(g, [" Dr ", "DGnD", "rnGD", "DGnr", "GDnD", "G DG", "G  G"])))
crop("turnip",
     lambda g: at2(g, ["l l", " G "]),
     lambda g: at2(g, ["ll ll", "lGlGl", " GGG ", "  G  "]),
     lambda g: at2(g, ["ll  ll", "lGllGl", " GwwG ", " wwww ", " vwwv ", "  vv  "]))

# --- the rules for src/client2d/farm2d ------------------------------------------
PLOTS = {
    "_doc": "AL-112: how a crop object (objects.json `crop`) is laid out in cells and which tile shows each "
            "stage. layout strips: the field's zone (else a rect of half_w by strips*strip_m round the object) cut "
            "across its length into `strips` bands; beds: `strips` beds of bed_len cells, max_rows to a column, a "
            "path cell between columns, round the object. Stage of a band/bed from the field's stock: ripe (still "
            "standing, ripe_age > spoil_after: over), sown (grown / grow_days against `stages`: sown, sprout, young, "
            "grown, heading), tilled, else stubble (after this year's harvest) or fallow. garden `crops`: the bed's "
            "crop (the sim grows one 'vegetables'), picked by garden, bed pair and year; `pick_pairs` beds share one.",
    "field": {
        "layout": "strips", "strip_m": 1.2, "half_w": 8.0,
        "stages": [["sown", 0.06], ["sprout", 0.2], ["young", 0.45], ["grown", 0.75], ["heading", 1.0]],
        "tiles": {
            "fallow": ["fd_fallow", "fd_fallow2"], "stubble": ["fd_stubble", "fd_stubble2"],
            "tilled": ["fd_tilled", "fd_tilled2"], "sown": ["fd_sown"], "sprout": ["gr_sprout"],
            "young": ["gr_young"], "grown": ["gr_grown"], "heading": ["gr_heading"],
            "ripe": ["gr_ripe", "gr_ripe2"], "over": ["gr_over"],
        },
    },
    "garden": {
        "layout": "beds", "bed_len": 3, "max_rows": 6, "path": ["gd_path"], "pick_pairs": 2,
        "stages": [["sown", 0.08], ["sprout", 0.25], ["young", 0.55], ["grown", 1.0]],
        "tiles": {
            "fallow": ["gd_bare"], "stubble": ["gd_bare"], "tilled": ["gd_tilled"], "sown": ["gd_sown"],
            "sprout": ["gd_sprout"], "over": ["gd_over"],
        },
        "crops": CROPS,
    },
}


def main():
    out = {
        "_doc": "AL-112: tiles of fields and kitchen gardens (16x16, palette letters of sprites.json, '.' clear) and "
                "the plot rules for src/client2d/farm2d. Generated by tools/content_gen/gen_crops_2d.py; edit there "
                "and rerun.",
        "plots": PLOTS,
        "sprites": {k: v for k, v in sorted(S.items())},
    }
    path = os.path.join(HERE, "..", "..", "content", "sprites_2d", "crops2d.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("wrote", os.path.normpath(path), len(S), "sprites")
    if "--sheet" in sys.argv:
        sheet(sys.argv[sys.argv.index("--sheet") + 1])


def sheet(path, z=4):
    # a contact sheet to look at the tiles (not part of the game)
    from PIL import Image, ImageDraw
    names = sorted(S)
    cols = 8
    rows = (len(names) + cols - 1) // cols
    cw, chh = 16 * z * 2 + 8, 16 * z * 2 + 22
    im = Image.new("RGB", (cols * cw, rows * chh), (24, 20, 16))
    d = ImageDraw.Draw(im)
    for i, n in enumerate(names):
        ox, oy = (i % cols) * cw + 4, (i // cols) * chh + 4
        for ty in range(2):   # 2x2 of the tile: how it repeats
            for tx in range(2):
                for y, r in enumerate(S[n]):
                    for x, c in enumerate(r):
                        if c == '.':
                            continue
                        col = PAL[c]
                        rgb = tuple(int(col[k:k + 2], 16) for k in (1, 3, 5))
                        x0, y0 = ox + (tx * 16 + x) * z, oy + (ty * 16 + y) * z
                        d.rectangle([x0, y0, x0 + z - 1, y0 + z - 1], fill=rgb)
        d.text((ox, oy + 32 * z + 2), n, fill=(230, 220, 200))
    im.save(path)
    print("sheet", path)


if __name__ == "__main__":
    main()
