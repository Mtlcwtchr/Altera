# LR-167: ground tiles 48x48 (one 1 m cell), seamless (they wrap), lit from the top left.
# Generic ground of the sim's names (tl_grass, tl_forest...) plus the belts' own ground:
#   med_ (the Middle Sea), nile_, wood_ (the Middle Forests), north_, steppe_.
import math
import random

from paint import Cv, Noise, cap, ell, fbm, mix, rgb, sh, blob, poly, rect

N = 48


def posterize(v, levels):
    for i, t in enumerate(levels):
        if v < t:
            return i
    return len(levels)


def ground(base, seed, spread=(0.86, 1.0, 1.12), levels=(0.42, 0.6), period=N, cells=(16, 8, 4), alt=None):
    """a field of the base colour in posterized noise bands (pixel-art, not a gradient)"""
    cv = Cv(N, N, wrap=True)
    f = fbm(seed, period, cells, edge=0.5)
    g = Noise(seed + 7, period, 4, edge=0.5)
    b = rgb(base)
    for y in range(N):
        for x in range(N):
            v = f(x, y) + (g(x, y) - 0.5) * 0.12
            k = posterize(v, levels)
            c = sh(b, spread[k]) if spread[k] <= 1 else sh(b, spread[k])
            if alt and v > alt[0]:
                c = rgb(alt[1])
            cv.px[y * N + x] = c
    return cv


def scatter(seed, n, margin=0, minD=5):
    rnd = random.Random(seed)
    pts = []
    tries = 0
    while len(pts) < n and tries < n * 40:
        tries += 1
        p = (rnd.uniform(margin, N - margin), rnd.uniform(margin, N - margin))
        if all(min(abs(p[0] - q[0]), N - abs(p[0] - q[0])) ** 2 + min(abs(p[1] - q[1]), N - abs(p[1] - q[1])) ** 2 >= minD * minD for q in pts):
            pts.append(p)
    return pts, rnd


def tuft(cv, x, y, col, rnd, h=4, blades=3):
    """a grass tuft: blades leaning, light tips, a dark root"""
    c = rgb(col)
    lt, dk = sh(c, 1.35), sh(c, 0.62)
    for b in range(blades):
        bx = int(x) + b - blades // 2
        lean = rnd.choice((-1, 0, 1)) if b != blades // 2 else 0
        hh = h - abs(b - blades // 2)
        for i in range(hh):
            px = bx + (lean if i >= hh // 2 else 0)
            cv.set(px, int(y) - i, lt if i == hh - 1 else c)
        cv.set(bx, int(y) + 1, dk)


def pebble(cv, x, y, col, r=1.6, seed=0):
    c = rgb(col)
    lt, dk = sh(c, 1.3), sh(c, 0.55)
    xi, yi = int(x), int(y)
    rr = int(math.ceil(r))
    for j in range(-rr, rr + 1):
        for i in range(-rr - 1, rr + 2):
            if (i / (r + 0.6)) ** 2 + (j / r) ** 2 <= 1:
                cc = c
                if j < 0 and i <= 0:
                    cc = lt
                if j >= rr - 1 or i >= rr:
                    cc = sh(c, 0.8)
                cv.set(xi + i, yi + j, cc)
    for i in range(-rr, rr + 1):
        cv.set(xi + i, yi + rr + 1, dk)   # its shadow on the ground


def flower(cv, x, y, col, centre="#f0d050"):
    c = rgb(col)
    for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
        cv.set(int(x) + dx, int(y) + dy, c)
    cv.set(int(x), int(y), centre)
    cv.set(int(x) + 1, int(y) + 2, sh(c, 0.5))


# --- the tiles -----------------------------------------------------------------------------
def grass(seed, base="#5f9a3c", tuftc="#79b24a", n=15, flowers=(), dry=None):
    cv = ground(base, seed)
    pts, rnd = scatter(seed + 1, n, minD=6)
    for (x, y) in pts:
        tuft(cv, x, y, tuftc if not dry or rnd.random() > 0.35 else dry, rnd, h=rnd.choice((3, 4, 4, 5)))
    for i, fc in enumerate(flowers):
        p, r2 = scatter(seed + 20 + i, 2, minD=12)
        for (x, y) in p:
            flower(cv, x, y, fc)
    return cv


def forest_floor(seed):
    cv = ground("#4a6b30", seed, spread=(0.78, 0.95, 1.08), alt=(0.74, "#5c5a2c"))
    pts, rnd = scatter(seed + 3, 10, minD=7)
    for (x, y) in pts:
        k = rnd.random()
        if k < 0.4:   # a fallen leaf
            c = rnd.choice(("#a8702c", "#8a5a24", "#b8902c"))
            cv.set(int(x), int(y), c); cv.set(int(x) + 1, int(y), sh(rgb(c), 1.3)); cv.set(int(x), int(y) + 1, sh(rgb(c), 0.6))
        elif k < 0.75:
            tuft(cv, x, y, "#5e8a36", rnd, h=3)
        else:          # moss cushion
            for j in range(-1, 2):
                for i in range(-2, 3):
                    if abs(i) + abs(j) < 3:
                        cv.set(int(x) + i, int(y) + j, "#6f9a3a" if j < 0 else "#4a7a2a")
    for (x, y) in scatter(seed + 9, 3, minD=14)[0]:   # twigs
        for i in range(5):
            cv.set(int(x) + i, int(y) + i // 2, "#5a3c22")
        cv.set(int(x), int(y) - 1, "#7a5432")
    return cv


def marsh(seed, base="#4f6a3a"):
    cv = ground(base, seed, spread=(0.8, 0.96, 1.08))
    f = fbm(seed + 40, N, (16, 8))
    water = rgb("#3f5f5a")
    for y in range(N):
        for x in range(N):
            v = f(x, y)
            if v > 0.6:
                c = water if v < 0.68 else sh(water, 1.18)
                if f(x - 1, y - 1) <= 0.6:
                    c = sh(water, 0.62)   # the bank's shade on the water
                cv.px[y * N + x] = c
    pts, rnd = scatter(seed + 5, 9, minD=7)
    for (x, y) in pts:
        if f(x, y) <= 0.6:
            tuft(cv, x, y, "#7a8a3a", rnd, h=rnd.choice((4, 5, 6)), blades=2)
    return cv


def water(seed, base="#2f6a9a", deep=False):
    b = sh(rgb(base), 0.8) if deep else rgb(base)
    cv = ground(b, seed, spread=(0.94, 1.0, 1.05), levels=(0.45, 0.62), cells=(24, 12, 6))
    rnd = random.Random(seed + 2)
    for k in range(7):   # wave crests: short light dashes over a darker trough
        x, y = rnd.randrange(N), rnd.randrange(N)
        L = rnd.choice((4, 5, 6))
        for i in range(L):
            cv.set(x + i, y, sh(b, 1.45 if 0 < i < L - 1 else 1.2))
            cv.set(x + i, y + 1, sh(b, 0.78))
    return cv


def sand(seed, base="#d8bf84", ripples=True):
    cv = ground(base, seed, spread=(0.92, 1.0, 1.06))
    if ripples:
        n = Noise(seed + 11, N, 12)
        for y in range(N):
            for x in range(N):
                v = math.sin((y + n(x, y) * 10 + x * 0.25) * 0.55)
                if v > 0.86:
                    cv.px[y * N + x] = sh(rgb(base), 1.12)
                elif v < -0.9:
                    cv.px[y * N + x] = sh(rgb(base), 0.86)
    pts, rnd = scatter(seed + 3, 4, minD=12)
    for (x, y) in pts:
        pebble(cv, x, y, "#a88a5a", r=1.0)
    return cv


def dirt(seed, base="#8a6a44", stones="#9a9080"):
    cv = ground(base, seed, spread=(0.84, 1.0, 1.1))
    pts, rnd = scatter(seed + 4, 7, minD=8)
    for (x, y) in pts:
        pebble(cv, x, y, stones, r=rnd.choice((1.0, 1.4, 1.8)))
    return cv


def road(seed, base="#9a7c54"):
    cv = ground(base, seed, spread=(0.9, 1.0, 1.08))
    for rx in (13, 35):   # wheel ruts
        for y in range(N):
            for x in (rx - 1, rx, rx + 1):
                cv.set(x + int(1.2 * math.sin(y * 0.13 + rx)), y, sh(rgb(base), 0.78 if x != rx - 1 else 0.68))
            cv.set(rx + 2 + int(1.2 * math.sin(y * 0.13 + rx)), y, sh(rgb(base), 1.12))
    pts, rnd = scatter(seed + 6, 6, minD=9)
    for (x, y) in pts:
        pebble(cv, x, y, "#a8a090", r=1.2)
    return cv


def flagstones(seed, base="#c8bca0", joint="#7a6e5a", cols=3, moss=None):
    """paving: irregular slabs, each lit from the top left; seamless"""
    cv = Cv(N, N, wrap=True)
    rnd = random.Random(seed)
    rows = 3
    h = N // rows
    f = fbm(seed + 3, N, (8, 4))
    for r in range(rows):
        off = (r % 2) * (N // (cols * 2))
        y0 = r * h
        widths = [N // cols] * cols
        xs = [off + i * (N // cols) for i in range(cols)]
        for i, x0 in enumerate(xs):
            tint = sh(rgb(base), rnd.uniform(0.9, 1.06))
            w = widths[i]
            for y in range(y0, y0 + h):
                for x in range(x0, x0 + w):
                    lx, ly = x - x0, y - y0
                    c = tint
                    if lx == 0 or ly == 0:
                        c = rgb(joint)
                    elif lx == 1 or ly == 1:
                        c = sh(tint, 1.2)
                    elif lx == w - 1 or ly == h - 1:
                        c = sh(tint, 0.72)
                    elif f(x, y) > 0.66:
                        c = sh(tint, 0.92)
                    if moss and (lx <= 1 or ly <= 1) and f(x * 1.3, y) > 0.55:
                        c = rgb(moss)
                    cv.set(x, y, c)
    return cv


def planks(seed, base="#9a6a3c", n=4, axis="h"):
    cv = Cv(N, N, wrap=True)
    rnd = random.Random(seed)
    w = N // n
    for k in range(n):
        tint = sh(rgb(base), rnd.uniform(0.9, 1.08))
        joint = rnd.randrange(N)
        for a in range(w):
            for b in range(N):
                c = tint
                if a == 0:
                    c = sh(tint, 0.5)
                elif a == 1:
                    c = sh(tint, 1.18)
                elif a == w - 1:
                    c = sh(tint, 0.78)
                elif (b * 7 + a * 3 + k * 11) % 13 == 0:
                    c = sh(tint, 0.88)   # the grain
                if b == joint:
                    c = sh(tint, 0.5)
                if (b - joint) % N in (2,) and a in (w // 2,):
                    c = sh(tint, 0.45)   # a nail
                x, y = (b, k * w + a) if axis == "h" else (k * w + a, b)
                cv.set(x, y, c)
    return cv


def mudbrick(seed, base="#a88458", joint="#6a5034"):
    cv = Cv(N, N, wrap=True)
    rnd = random.Random(seed)
    bh, bw = 8, 16
    for r in range(N // bh):
        off = (r % 2) * (bw // 2)
        for x in range(N):
            for y in range(r * bh, r * bh + bh):
                lx = (x + off) % bw
                ly = y - r * bh
                tint = sh(rgb(base), 0.94 + 0.12 * (((x + off) // bw * 7 + r * 3) % 5) / 5)
                c = tint
                if lx == 0 or ly == 0:
                    c = rgb(joint)
                elif ly == 1 or lx == 1:
                    c = sh(tint, 1.15)
                elif ly == bh - 1:
                    c = sh(tint, 0.78)
                cv.set(x, y, c)
    for (x, y) in scatter(seed, 6, minD=10)[0]:
        cv.set(int(x), int(y), sh(rgb(base), 0.7))   # straw specks
        cv.set(int(x) + 1, int(y), "#c8a860")
    return cv


def mosaic(seed, base="#d8ccae"):
    """a pebble mosaic floor of the Middle Sea: a meander border and a field of tesserae"""
    cv = Cv(N, N, wrap=True)
    col = [rgb(base), sh(rgb(base), 0.9)]
    dark, red = rgb("#3a3028"), rgb("#9a3a24")
    for y in range(N):
        for x in range(N):
            c = col[((x // 2) + (y // 2)) % 2]
            if x % 2 == 0 and y % 2 == 0:
                c = sh(c, 0.82)   # the grout between tesserae
            cv.px[y * N + x] = c
    # a meander (Greek key) band across the middle
    for x in range(N):
        for y in range(18, 30):
            lx, ly = x % 12, y - 18
            on = ly in (0, 11) or (lx == 0 and ly < 9) or (ly == 8 and lx < 9) or (lx == 8 and 3 < ly < 9) or (ly == 3 and 3 < lx < 9)
            if on:
                cv.px[y * N + x] = dark
            elif ly in (1, 10):
                cv.px[y * N + x] = red
    return cv


def felt(seed, base="#9a3a2a"):
    """a felt rug of the steppe (the yurt's floor): a field and a horn ornament"""
    cv = ground(base, seed, spread=(0.92, 1.0, 1.05))
    orn = rgb("#e0c890")
    for y in range(N):
        for x in range(N):
            dx, dy = abs(x - 24 + 0.5), abs(y - 24 + 0.5)
            r = math.hypot(dx, dy)
            # a ram's horn spiral pair, mirrored four ways
            a = math.atan2(dy - 10, dx - 10)
            d = math.hypot(dx - 10, dy - 10)
            if 5.2 < d < 7 and a > -1.2:
                cv.px[y * N + x] = orn
            if 22 <= max(dx, dy) <= 23.5:
                cv.px[y * N + x] = orn
            if max(dx, dy) < 3 and min(dx, dy) < 1.2:
                cv.px[y * N + x] = orn
    return cv


def snow(seed, base="#e6ecf0"):
    cv = ground(base, seed, spread=(0.93, 1.0, 1.03), levels=(0.4, 0.62))
    n = Noise(seed + 2, N, 16, edge=0.5)
    for y in range(N):
        for x in range(N):
            v = n(x, y)
            if 0.6 < v < 0.64:
                cv.px[y * N + x] = rgb("#c8d4e0")   # a drift's soft shade
    for (x, y) in scatter(seed + 5, 6, minD=10)[0]:
        cv.set(int(x), int(y), "#ffffff")
    return cv


def tundra(seed):
    cv = ground("#7a8a5a", seed, spread=(0.84, 1.0, 1.1), alt=(0.72, "#a89a6a"))
    pts, rnd = scatter(seed + 3, 10, minD=7)
    for (x, y) in pts:
        k = rnd.random()
        if k < 0.35:   # lichen
            for i, j in ((0, 0), (1, 0), (0, 1), (-1, 0)):
                cv.set(int(x) + i, int(y) + j, "#c8c890" if j == 0 else "#9a9a6a")
        elif k < 0.55:   # a berry sprig
            cv.set(int(x), int(y), "#a82838"); cv.set(int(x) + 1, int(y), "#3a5a2a")
        else:
            tuft(cv, x, y, "#8a9a5a", rnd, h=3, blades=2)
    for (x, y) in scatter(seed + 8, 3, minD=14)[0]:
        pebble(cv, x, y, "#8a8a88", r=2.0)
    return cv


def steppe(seed, base="#b8a85a"):
    cv = ground(base, seed, spread=(0.86, 1.0, 1.1), alt=(0.76, "#9a9a54"))
    pts, rnd = scatter(seed + 3, 16, minD=6)
    for (x, y) in pts:
        if rnd.random() < 0.6:   # feather grass: tall pale plumes leaning with the wind
            for i in range(6):
                cv.set(int(x) + i // 3, int(y) - i, "#e8dca0" if i > 3 else "#c8b870")
            cv.set(int(x), int(y) + 1, "#7a6a34")
        else:
            tuft(cv, x, y, "#9aa04a", rnd, h=3)
    for (x, y) in scatter(seed + 9, 2, minD=16)[0]:
        flower(cv, x, y, "#8a5ab0", centre="#e0d060")   # steppe sage
    return cv


def silt(seed):
    """the black earth of the river's flood plain"""
    cv = ground("#4a3a2c", seed, spread=(0.84, 1.0, 1.12))
    n = Noise(seed + 4, N, 8)
    for y in range(N):
        for x in range(N):
            if n(x, y) > 0.7 and (x + y) % 3 == 0:
                cv.px[y * N + x] = rgb("#6a5440")   # cracks drying
    pts, rnd = scatter(seed + 3, 6, minD=9)
    for (x, y) in pts:
        tuft(cv, x, y, "#6a8a3a", rnd, h=3, blades=2)
    return cv


def field(seed, base="#7a5232"):
    """ploughed earth in furrows (fields/gardens draw their crops over it)"""
    cv = ground(base, seed, spread=(0.9, 1.0, 1.06))
    for y in range(N):
        for x in range(N):
            k = y % 6
            c = cv.px[y * N + x]
            if k == 0:
                c = sh(c, 1.22)
            elif k in (4, 5):
                c = sh(c, 0.72 if k == 5 else 0.86)
            cv.px[y * N + x] = c
    return cv


def roof_tile(seed, style="thatch"):
    """roof cover seen from above, a 1 m piece repeated over the roof"""
    if style == "thatch":
        cv = ground("#b8963c", seed, spread=(0.86, 1.0, 1.08))
        rnd = random.Random(seed)
        for y in range(0, N, 6):
            for x in range(N):
                cv.set(x, y + 5, sh(rgb("#b8963c"), 0.62))
                cv.set(x, y + 4, sh(rgb("#b8963c"), 0.82))
        for k in range(40):
            x, y = rnd.randrange(N), rnd.randrange(N)
            cv.set(x, y, "#e0c068"); cv.set(x, y + 1, "#d8b850")
        return cv
    if style == "terracotta":   # the Middle Sea: rows of curved tiles
        cv = Cv(N, N, wrap=True)
        base = rgb("#b8583a")
        for y in range(N):
            for x in range(N):
                lx, ly = x % 8, y % 12
                c = base
                if lx in (0, 7):
                    c = sh(base, 0.55)
                elif lx == 1:
                    c = sh(base, 1.25)
                elif lx in (5, 6):
                    c = sh(base, 0.78)
                if ly == 11:
                    c = sh(base, 0.5)
                elif ly == 0:
                    c = sh(c, 1.15)
                cv.px[y * N + x] = c
        return cv
    if style == "shingle":
        cv = Cv(N, N, wrap=True)
        rnd = random.Random(seed)
        base = rgb("#7a5a3a")
        for r in range(N // 8):
            off = (r % 2) * 6
            for y in range(r * 8, r * 8 + 8):
                for x in range(N):
                    lx, ly = (x + off) % 12, y - r * 8
                    tint = sh(base, 0.92 + 0.14 * (((x + off) // 12 * 5 + r * 3) % 4) / 4)
                    c = tint
                    if lx == 0:
                        c = sh(tint, 0.55)
                    elif ly == 7:
                        c = sh(tint, 0.5)
                    elif ly == 6:
                        c = sh(tint, 0.78)
                    elif ly == 0:
                        c = sh(tint, 1.2)
                    cv.set(x, y, c)
        return cv
    if style == "turf":   # the North's longhouse: grass on the roof
        cv = grass(seed + 1, base="#5a8a3a", tuftc="#7aa848", n=12)
        return cv
    if style == "flat":   # the Nile: a flat roof of beaten mud over palm beams
        cv = ground("#c0a070", seed, spread=(0.9, 1.0, 1.06))
        for x in range(0, N, 12):
            for y in range(N):
                cv.set(x, y, sh(rgb("#c0a070"), 0.86))
        return cv
    if style == "reed":
        cv = ground("#a89a5a", seed, spread=(0.88, 1.0, 1.08))
        for x in range(N):
            for y in range(N):
                if (x + (y // 4)) % 3 == 0:
                    cv.set(x, y, sh(rgb("#a89a5a"), 0.8))
        return cv
    if style == "felt":   # the yurt: white felt over the lattice
        cv = ground("#e0d8c4", seed, spread=(0.92, 1.0, 1.04))
        for y in range(0, N, 16):
            for x in range(N):
                cv.set(x, y, "#8a6a4a"); cv.set(x, y + 1, "#6a4a2a")   # the ropes
        return cv
    raise KeyError(style)


def all_tiles():
    """{name: Cv}: the generic ones keep the names the renderer and map2d.json know"""
    T = {}
    T["tl_grass"] = grass(1, flowers=("#e8e0f0",))
    T["tl_grass2"] = grass(2, base="#588f38", tuftc="#6ea444", n=12, flowers=("#e8c040",))
    T["tl_forest"] = forest_floor(3)
    T["tl_marsh"] = marsh(4)
    T["tl_water"] = water(5)
    T["tl_sand"] = sand(6)
    T["tl_dirt"] = dirt(7)
    T["tl_road"] = road(8)
    T["tl_field"] = field(9)
    T["tl_floor"] = planks(10)
    T["tl_roof"] = roof_tile(11, "thatch")
    # the Middle Sea: dry grass with thyme, limestone paving, mosaic, terracotta
    T["tl_med_grass"] = grass(21, base="#8a9a48", tuftc="#a8a858", n=14, flowers=("#c890d8",), dry="#c8b468")
    T["tl_med_scrub"] = dirt(22, base="#a88a58", stones="#d8d0b8")
    T["tl_med_paving"] = flagstones(23, base="#d0c4a4", joint="#8a7c62")
    T["tl_med_mosaic"] = mosaic(24)
    T["tl_med_sea"] = water(25, base="#2a6aa8")
    T["tl_med_roof"] = roof_tile(26, "terracotta")
    # the Nile: silt, desert sand, mudbrick, reed roof
    T["tl_nile_silt"] = silt(31)
    T["tl_nile_sand"] = sand(32, base="#e0c890")
    T["tl_nile_floor"] = mudbrick(33)
    T["tl_nile_river"] = water(34, base="#3a7a7a")
    T["tl_nile_roof"] = roof_tile(35, "flat")
    T["tl_nile_reed_roof"] = roof_tile(36, "reed")
    # the Middle Forests: moss, leaf litter, plank floor, shingle
    T["tl_wood_moss"] = forest_floor(41)
    T["tl_wood_meadow"] = grass(42, base="#4f9038", tuftc="#6aaa44", n=16, flowers=("#e8e8f0", "#d84040"))
    T["tl_wood_floor"] = planks(43, base="#8a6238")
    T["tl_wood_roof"] = roof_tile(44, "shingle")
    # the North: tundra, snow, rock, wide planks, turf roof
    T["tl_north_tundra"] = tundra(51)
    T["tl_north_snow"] = snow(52)
    T["tl_north_rock"] = dirt(53, base="#6a6a66", stones="#8a8a88")
    T["tl_north_floor"] = planks(54, base="#7a5a3a", n=3)
    T["tl_north_sea"] = water(55, base="#2a4a6a", deep=True)
    T["tl_north_roof"] = roof_tile(56, "turf")
    # the steppe: feather grass, dry earth, felt rug, felt roof
    T["tl_steppe_grass"] = steppe(61)
    T["tl_steppe_dry"] = dirt(62, base="#a8905a", stones="#b8b098")
    T["tl_steppe_felt"] = felt(63)
    T["tl_steppe_roof"] = roof_tile(64, "felt")
    # variants of the common grounds (same border, so they mix without seams)
    V = {"tl_grass": lambda k: grass(100 + k, flowers=("#e8e0f0",) if k % 2 else ()),
         "tl_forest": lambda k: forest_floor(110 + k), "tl_sand": lambda k: sand(120 + k), "tl_dirt": lambda k: dirt(130 + k),
         "tl_med_grass": lambda k: grass(140 + k, base="#8a9a48", tuftc="#a8a858", n=14, flowers=("#c890d8",) if k % 2 else (), dry="#c8b468"),
         "tl_nile_sand": lambda k: sand(150 + k, base="#e0c890"), "tl_wood_meadow": lambda k: grass(160 + k, base="#4f9038", tuftc="#6aaa44", n=16, flowers=("#e8e8f0",)),
         "tl_north_tundra": lambda k: tundra(170 + k), "tl_north_snow": lambda k: snow(180 + k), "tl_steppe_grass": lambda k: steppe(190 + k)}
    for name, f in V.items():
        for k in (2, 3):
            T["%s_%d" % (name, k)] = f(k)
    return T
