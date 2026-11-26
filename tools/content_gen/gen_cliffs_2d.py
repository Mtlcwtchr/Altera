# LR-156: cliffs of the 2D client's relief as 16x16 tiles: content/sprites_2d/cliffs2d.json.
# Every sprite is a grid of palette letters of sprites.json (no new colours), drawn here
# procedurally with a fixed seed, so the file is text and reproducible (no binary assets).
#
# A cell lower than a neighbour shows that neighbour's cliff (src/client2d/relief2d.cpp):
#   n   the face of the higher ground to the north (seen from above-south: the tall one)
#   s   the lip of the higher ground to the south
#   e/w the side faces of the higher ground to the east / west
#   c_ne c_nw c_se c_sw   the corners when only a diagonal neighbour is higher
#   ramp_n ramp_s ramp_e ramp_w   steps where a road climbs (instead of the face)
# Straight pieces come in two variants (_0, _1), picked by the cell's hash.
# Styles (the rock of the higher ground): stone, mossy, earth, sandstone, snow; the file's
# "rules" say which biome gets which and from which level the tops are snowy.
#
#   python tools/content_gen/gen_cliffs_2d.py [--sheet out.png]
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "content", "sprites_2d", "cliffs2d.json")

# lip: the edge of the top, lip2 its highlight; hi/mid/dark the face lit from the top left;
# crack the joints; spot: patches over the face (moss, roots, snow), strata: horizontal layers
STYLES = {
    "stone":     dict(lip="G", lip2="l", hi="m", mid="W", dark="M", crack="x", spot=None, strata=False),
    "mossy":     dict(lip="D", lip2="G", hi="W", mid="M", dark="x", crack="k", spot=("D", "K"), strata=False),
    "earth":     dict(lip="D", lip2="G", hi="e", mid="E", dark="p", crack="P", spot=("n", "N"), strata=True),
    "sandstone": dict(lip="T", lip2="t", hi="t", mid="o", dark="z", crack="Z", spot=None, strata=True),
    "snow":      dict(lip="m", lip2="w", hi="m", mid="W", dark="M", crack="x", spot=("w", "m"), strata=False),
}

RULES = {
    "_doc": "LR-156: стили обрывов 2D (src/client2d/relief2d.cpp). styles — порядок стилей (0 — по умолчанию); by_biome — стиль по биому верхней клетки; high — с какого яруса верх снежный (ярус = floor(высота / level_m из content/world/relief.json)); ramp_on_roads — где дорога идёт на уступ, вместо обрыва ступени. gentle_style — пологий уступ (мягкая бровка вместо скалы), где земля вокруг на slope_span_m в обе стороны поднимается меньше cliff_slope метра на метр. shade_low — затемнение земли на ярусе 0, shade_per_level — на сколько меньше на каждый ярус выше (до 0): низины темнее. Тайлы cf_<стиль>_<кусок>[_<вариант>], генератор tools/content_gen/gen_cliffs_2d.py.",
    "styles": list(STYLES.keys()) + ["bank"],
    "by_biome": {"meadow": "stone", "steppe": "stone", "mountains": "stone", "mountain": "stone", "hills": "stone",
                 "forest": "mossy", "marsh": "earth", "desert": "sandstone"},
    "high": [{"from_level": 14, "style": "snow"}],
    "ramp_on_roads": True,
    "gentle_style": "bank",
    "cliff_slope": 0.04,
    "slope_span_m": 12.0,
    "shade_low": 0.22,
    "shade_per_level": 0.03,
    "variants": 2,
    "glyph": {"n": "▀", "s": "▄", "e": "▐", "w": "▌", "colour": "x"},
}


def blank():
    return [["."] * 16 for _ in range(16)]


def rows(g):
    return ["".join(r) for r in g]


def bulges(rnd, n=16):
    """Boulders across the width: (centre, half width) - the face is lumpy, not a wall of blocks."""
    out, x = [], -1
    while x < n + 1:
        w = rnd.choice([2, 2, 3, 3, 4])
        out.append((x + w, w))
        x += 2 * w - rnd.randint(0, 1)
    return out


def face(st, rnd, top, foot):
    """The rock face between top[x] and foot[x] (inclusive) a column: lumps lit from the top left."""
    g = blank()
    lumps = bulges(rnd)
    for x in range(16):
        c, w = min(lumps, key=lambda l: abs(l[0] - x))
        d = (x - c) / max(1, w)          # -1 left edge .. +1 right edge of the lump
        for y in range(top[x], foot[x] + 1):
            depth = (y - top[x]) / max(1, foot[x] - top[x])   # 0 under the lip .. 1 at the foot
            if st["strata"]:
                band = (y + (x // 5) % 2) % 3
                col = st["hi"] if band == 0 and depth < 0.7 else st["mid"]
                if d > 0.55:
                    col = st["dark"]
            else:
                if d < -0.35 and depth < 0.75:
                    col = st["hi"]
                elif d > 0.45 or depth > 0.8:
                    col = st["dark"]
                else:
                    col = st["mid"]
            g[y][x] = col
        # the seam between two lumps, not all the way down
        if abs(d) > 0.9 and rnd.random() < 0.7:
            for y in range(top[x] + rnd.randint(1, 3), foot[x] + 1 - rnd.randint(0, 2)):
                g[y][x] = st["crack"]
    # a few short cracks running down and aside
    for _ in range(rnd.randint(1, 3)):
        x, y = rnd.randint(1, 14), rnd.randint(4, 7)
        for _k in range(rnd.randint(2, 4)):
            if 0 <= x < 16 and top[x] < y <= foot[x]:
                g[y][x] = st["crack"]
            y += 1
            x += rnd.choice([-1, 0, 1])
    # moss, roots, snow over the face, hanging from the lip
    if st["spot"]:
        for _ in range(rnd.randint(3, 6)):
            x = rnd.randint(0, 15)
            c = rnd.choice(st["spot"])
            for k in range(rnd.randint(1, 4)):
                y = top[x] + k
                if 0 <= x < 16 and y < foot[x]:
                    g[y][x] = c
                x = max(0, min(15, x + rnd.choice([-1, 0, 0, 1])))
    return g


def ragged(rnd, base, lo, hi):
    """A line that wanders by a pixel: the lip and the foot of a cliff are not ruled."""
    out, v = [], base
    for _ in range(16):
        if rnd.random() < 0.35:
            v = max(lo, min(hi, v + rnd.choice([-1, 1])))
        out.append(v)
    return out


def piece_n(st, rnd):
    top = ragged(rnd, 2, 1, 4)        # the first row of rock under the lip
    foot = ragged(rnd, 11, 10, 12)    # the last row of rock
    g = face(st, rnd, top, foot)
    for x in range(16):
        # the lip of the top, ragged, a dark line where the face drops away
        for y in range(0, top[x] - 1):
            g[y][x] = st["lip2"] if (y == 0 and rnd.random() < 0.7) else st["lip"]
        g[top[x] - 1][x] = "k"
        # the foot: an outline, then the cliff's shadow dithered on the ground below
        g[foot[x] + 1][x] = "k"
        if foot[x] + 2 < 16:
            g[foot[x] + 2][x] = "x" if x % 2 == 0 else "k"
        if foot[x] + 3 < 16 and x % 3 == 1:
            g[foot[x] + 3][x] = "x"
    # scree at the foot now and then
    for _ in range(rnd.randint(0, 2)):
        x = rnd.randint(1, 14)
        y = min(15, foot[x] + 2)
        g[y][x] = st["mid"]
        g[y][x + 1] = st["dark"]
    return g


def piece_s(st, rnd):
    g = blank()
    for x in range(16):
        g[13][x] = "k" if rnd.random() < 0.5 else "."
        g[14][x] = "k"
        g[15][x] = st["lip2"] if rnd.random() < 0.6 else st["lip"]
        if rnd.random() < 0.25:
            g[13][x] = "k"
            g[14][x] = st["lip"]
    return g


def piece_e(st, rnd):
    g = blank()
    edge = ragged(rnd, 12, 11, 13)   # where the side face starts, wandering
    for y in range(16):
        e = edge[y]
        if e - 1 >= 0 and y % 2 == 0:
            g[y][e - 1] = "x"            # the side face's shadow on the ground
        g[y][e] = "k"
        for x in range(e + 1, 15):
            g[y][x] = st["dark"] if x == e + 1 else (st["mid"] if rnd.random() < 0.75 else st["dark"])
            if st["spot"] and rnd.random() < 0.1:
                g[y][x] = st["spot"][0]
        g[y][15] = st["lip"] if rnd.random() < 0.7 else st["lip2"]
    return g


def mirror(g):
    return [list(reversed(r)) for r in g]


def corner(full, keep_x, edge_x, edge_rows):
    g = blank()
    for y in range(16):
        for x in range(16):
            if keep_x(x):
                g[y][x] = full[y][x]
    for y in edge_rows:
        if full[y][edge_x] != ".":
            g[y][edge_x] = "k"
    return g


def ramp_n(st, rnd):
    g = blank()
    tread, riser = st["hi"], st["mid"]
    for y in range(0, 14):
        for x in range(16):
            if x <= 1 or x >= 14:   # the rock beside the steps
                g[y][x] = st["dark"] if x in (1, 14) else st["mid"]
                if y < 2:
                    g[y][x] = st["lip"]
                continue
            k = (y - 1) % 3
            g[y][x] = "k" if k == 0 else (tread if k == 1 else riser)
    for x in range(16):
        g[14][x] = "x" if x % 2 == 0 else "."
    return g


def ramp_s(st, rnd):
    g = blank()
    for y in (12, 14):
        for x in range(2, 14):
            g[y][x] = "k"
            g[y + 1][x] = st["hi"]
    return g


def ramp_e(st, rnd):
    g = blank()
    for x in (10, 13):
        for y in range(16):
            g[y][x] = "k"
            g[y][x + 1] = st["hi"]
            g[y][x + 2] = st["mid"] if x + 2 < 16 else g[y][x + 1]
    return g


def bank(piece, rnd):
    """A soft step on gentle ground: a dithered shade under the rise, no rock (any biome)."""
    g = blank()
    dens = [0.75, 0.5, 0.3, 0.15]
    for k, d in enumerate(dens):
        for t in range(16):
            if rnd.random() < d:
                x, y = {"n": (t, k), "s": (t, 15 - k), "e": (15 - k, t), "w": (k, t)}[piece]
                g[y][x] = "k" if k == 0 else "x"
    return g


def build():
    sprites = {}
    rnd = random.Random("cliff:bank")
    for v in range(RULES["variants"]):
        for d in "nsew":
            sprites[f"cf_bank_{d}_{v}"] = rows(bank(d, rnd))
    for c, (a, b) in {"c_ne": ("n", "e"), "c_nw": ("n", "w"), "c_se": ("s", "e"), "c_sw": ("s", "w")}.items():
        g = blank()
        for y in range(4):
            for x in range(4):
                if rnd.random() < 0.6 - 0.12 * (x + y):
                    xx = 15 - x if b == "e" else x
                    yy = y if a == "n" else 15 - y
                    g[yy][xx] = "x"
        sprites[f"cf_bank_{c}"] = rows(g)
    for d in "nsew":
        sprites[f"cf_bank_ramp_{d}"] = sprites[f"cf_bank_{d}_0"]
    for name, st in STYLES.items():
        rnd = random.Random(f"cliff:{name}")
        for v in range(RULES["variants"]):
            n = piece_n(st, rnd)
            sprites[f"cf_{name}_n_{v}"] = rows(n)
            sprites[f"cf_{name}_s_{v}"] = rows(piece_s(st, rnd))
            e = piece_e(st, rnd)
            sprites[f"cf_{name}_e_{v}"] = rows(e)
            sprites[f"cf_{name}_w_{v}"] = rows(mirror(e))
            if v == 0:
                sprites[f"cf_{name}_c_ne"] = rows(corner(n, lambda x: x >= 12, 12, range(3, 12)))
                sprites[f"cf_{name}_c_nw"] = rows(corner(n, lambda x: x <= 3, 3, range(3, 12)))
                s = piece_s(st, rnd)
                sprites[f"cf_{name}_c_se"] = rows(corner(s, lambda x: x >= 12, 12, ()))
                sprites[f"cf_{name}_c_sw"] = rows(corner(s, lambda x: x <= 3, 3, ()))
        sprites[f"cf_{name}_ramp_n"] = rows(ramp_n(st, rnd))
        sprites[f"cf_{name}_ramp_s"] = rows(ramp_s(st, rnd))
        re = ramp_e(st, rnd)
        sprites[f"cf_{name}_ramp_e"] = rows(re)
        sprites[f"cf_{name}_ramp_w"] = rows(mirror(re))
    return sprites


def main():
    out = {
        "_doc": "LR-156: обрывы рельефа 2D-клиента, тайлы 16×16 буквами палитры sprites.json ('.' прозрачно). Сгенерировано tools/content_gen/gen_cliffs_2d.py: правьте там и перезапустите. rules читает src/client2d/relief2d.cpp, тайлы рисуют окно (native/window2d.cpp) и страница (web/game2d.js).",
        "rules": RULES,
        "sprites": build(),
    }
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("wrote", OUT, len(out["sprites"]), "sprites")
    if "--sheet" in sys.argv:
        sheet(out["sprites"], sys.argv[sys.argv.index("--sheet") + 1])


def sheet(sprites, path, z=6):
    # a contact sheet to look at the tiles (not part of the game)
    from PIL import Image
    pal = json.load(open(os.path.join(ROOT, "content", "sprites_2d", "sprites.json"), encoding="utf-8"))["pal"]
    names = list(sprites)
    per = 18
    rows_n = (len(names) + per - 1) // per
    img = Image.new("RGB", (per * 17 * z, rows_n * 17 * z), (40, 90, 40))
    for k, nm in enumerate(names):
        ox, oy = (k % per) * 17 * z, (k // per) * 17 * z
        for y, r in enumerate(sprites[nm]):
            for x, c in enumerate(r):
                if c == ".":
                    continue
                h = pal[c].lstrip("#")
                col = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
                for yy in range(z):
                    for xx in range(z):
                        img.putpixel((ox + x * z + xx, oy + y * z + yy), col)
    img.save(path)
    print("sheet", path)


if __name__ == "__main__":
    main()
