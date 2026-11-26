# LR-167: trees, bushes, stones, reeds: standing things seen from the front-above (3/4),
# feet at the bottom middle of the canvas, a soft shadow on the ground under them.
import math
import random

from paint import Cv, blob, cap, ell, poly, rect, OR, SUB, AND, sh, rgb, mix, Noise


def leafy(cv, cx, cy, r, base, seed, clumps=7, spread=0.62):
    """a crown of leaf clumps: each clump three-tone, the light ones up-left, holes of sky"""
    rnd = random.Random(seed)
    pts = [(cx, cy, r * 0.75)]
    for i in range(clumps):
        a = rnd.uniform(0, 6.28)
        d = rnd.uniform(0.25, spread) * r
        pts.append((cx + math.cos(a) * d * 1.1, cy + math.sin(a) * d * 0.85, r * rnd.uniform(0.38, 0.55)))
    pts.sort(key=lambda p: p[1] - (p[0] - cx) * 0.3)   # the back and right first, the front-left last
    n = Noise(seed + 5, 256, 3)
    for k, (x, y, rr) in enumerate(pts):
        b = sh(rgb(base), 0.9 + 0.2 * (k / len(pts)))

        def leaves(px, py, c, x=x, y=y, rr=rr):
            # dappled leaves: a few lit specks and dark gaps inside each clump
            v = n(px * 1.0, py * 1.0)
            if v > 0.8:
                return sh(c, 1.18)
            if v < 0.16:
                return sh(c, 0.8)
            return None
        cv.paint(blob(x, y, rr, seed + k * 13, amp=0.28), b, fn=leaves)


def trunk(cv, x, ytop, ybot, w, col="#6a4a30", roots=True):
    cv.paint(poly([(x - w / 2, ytop), (x + w / 2, ytop), (x + w / 2 + 1, ybot), (x - w / 2 - 1, ybot)]), col,
             fn=lambda px, py, c: sh(c, 0.82) if int(py) % 5 == 0 and int(px) % 3 == 1 else None)
    if roots:
        cv.paint(OR(cap(x - w / 2, ybot - 3, x - w / 2 - 4, ybot, 1.4, 0.8), cap(x + w / 2, ybot - 3, x + w / 2 + 4, ybot, 1.4, 0.8)), col, rim=1)


def ground_shadow(cv, cx, cy, rx, ry):
    cv.shadow(ell(cx, cy, rx, ry))


def oak(seed=1, size=96, base="#4a7a2c"):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 6, size - 6, size * 0.36, size * 0.08)
    trunk(cv, c, size * 0.45, size - 6, size * 0.11)
    cv.paint(cap(c, size * 0.55, c - size * 0.2, size * 0.36, 2.5, 1.5), "#6a4a30", rim=1)
    cv.paint(cap(c, size * 0.6, c + size * 0.2, size * 0.4, 2.5, 1.5), "#6a4a30", rim=1)
    leafy(cv, c, size * 0.34, size * 0.34, base, seed, clumps=9)
    return cv.finish()


def birch(seed=2, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 4, size - 5, size * 0.22, size * 0.06)
    w = size * 0.07

    def bark(px, py, col):
        if int(py) % 7 in (0,) and (int(px) + int(py)) % 3 != 0:
            return "#2a2622"
        return None
    cv.paint(poly([(c - w / 2, size * 0.15), (c + w / 2, size * 0.15), (c + w / 2 + 1, size - 5), (c - w / 2 - 1, size - 5)]), "#e8e4dc", fn=bark)
    for k, (dx, dy) in enumerate(((-20, -16), (18, -24), (-12, -36), (14, -40), (0, -50), (-24, -28), (22, -10))):
        cv.paint(cap(c, size * 0.62 + dy * size / 96 + 8, c + dx * size / 96 * 0.8, size * 0.62 + dy * size / 96, 1.0, 0.6), "#3a3430", rim=1)
    for k, (dx, dy) in enumerate(((-20, -16), (18, -24), (-12, -36), (14, -40), (0, -50), (-24, -28), (22, -10))):
        leafy(cv, c + dx * size / 96, size * 0.62 + dy * size / 96, size * 0.16, "#7aa83c", seed + k, clumps=4, spread=0.5)
    return cv.finish()


def conifer(seed=3, size=96, base="#2f5a34", snow=False):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 6, size - 5, size * 0.26, size * 0.07)
    trunk(cv, c, size * 0.8, size - 5, size * 0.08, roots=False)
    tiers = 6
    for i in range(tiers):
        t = i / (tiers - 1)
        top = size * (0.04 + 0.13 * i)
        hw = size * (0.08 + 0.32 * t)
        bot = top + size * 0.22
        rnd = random.Random(seed + i)
        pts = [(c, top)]
        k = 7
        for j in range(k + 1):   # a ragged lower edge of drooping boughs
            x = c - hw + 2 * hw * j / k
            pts.append((x, bot + (3 if j % 2 else -1) + rnd.uniform(-1, 1)))
        pts.append((c, top))

        def needles(px, py, col, top=top):
            if (int(px) * 3 + int(py) * 5) % 11 == 0:
                return sh(col, 1.15)
            if snow and py < top + (px - c) * 0.1 + 5 and px < c + 4:
                return "#f0f4f8"
            return None
        cv.paint(poly(pts[1:-1] + [(c, top)]), sh(rgb(base), 0.92 + 0.12 * t), fn=needles)
    return cv.finish()


def cypress(seed=4, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 3, size - 5, size * 0.12, size * 0.05)
    trunk(cv, c, size * 0.86, size - 5, size * 0.06, roots=False)
    n = Noise(seed, 256, 3)
    cv.paint(lambda x, y: ((x - c) / (size * 0.12 * (1 - abs((y - size * 0.48) / (size * 0.46)) ** 2.2))) ** 2 <= 1 if abs(y - size * 0.48) < size * 0.46 else False,
             "#2f4a2a", fn=lambda px, py, col: sh(col, 1.2) if n(px, py * 0.5) > 0.78 else (sh(col, 0.8) if n(px, py * 0.5) < 0.2 else None))
    return cv.finish()


def olive(seed=5, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 6, size - 5, size * 0.32, size * 0.07)
    # a gnarled twisted trunk forking low
    cv.paint(OR(cap(c - 2, size - 6, c - 6, size * 0.62, 5, 3), cap(c + 2, size - 6, c + 8, size * 0.58, 4.5, 2.6),
                cap(c - 6, size * 0.62, c - 16, size * 0.42, 3, 1.6), cap(c + 8, size * 0.58, c + 16, size * 0.42, 2.6, 1.4)), "#7a6a58", rim=1,
             fn=lambda px, py, col: sh(col, 0.75) if (int(px) + int(py) * 2) % 7 == 0 else None)
    leafy(cv, c, size * 0.38, size * 0.33, "#7a8a5a", seed, clumps=10, spread=0.75)
    rnd = random.Random(seed)
    for _ in range(14):   # olives
        x, y = c + rnd.uniform(-26, 26) * size / 96, size * 0.38 + rnd.uniform(-16, 16) * size / 96
        if cv.get(int(x), int(y)):
            cv.dot(x, y, "#3a3048")
    return cv.finish()


def palm(seed=6, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 10, size - 5, size * 0.28, size * 0.06)
    # a leaning ringed trunk
    for i in range(30):
        t = i / 29
        x = c + 6 * math.sin(t * 1.6) - 3
        y = size - 6 - t * size * 0.66
        cv.paint(ell(x, y, 3.6 - t, 2.2), "#8a6a44" if i % 2 else "#6a4a30", rim=1, soft=True)
    tx, ty = c + 6 * math.sin(1.6) - 3, size - 6 - size * 0.66
    rnd = random.Random(seed)
    for k, a in enumerate((-2.9, -2.4, -1.9, -1.2, -0.6, -0.2, 0.3, 2.6)):
        L = size * rnd.uniform(0.26, 0.34)
        pts = []
        for j in range(12):
            t = j / 11
            pts.append((tx + math.cos(a) * L * t, ty + math.sin(a) * L * t * 0.7 + (t * t) * L * 0.55))
        for j in range(11):
            (x0, y0), (x1, y1) = pts[j], pts[j + 1]
            w = 3.2 * (1 - j / 12) + 0.6
            cv.paint(cap(x0, y0, x1, y1, w, w * 0.85), "#4a8a34" if k % 2 else "#3f7a2c", rim=1, soft=True,
                     fn=lambda px, py, col: sh(col, 0.7) if (int(px) + int(py)) % 3 == 0 else None)
    for i in range(5):   # dates
        cv.paint(ell(tx - 4 + i * 2, ty + 5 + (i % 2), 1.3, 1.6), "#b8642a", rim=1)
    return cv.finish()


def willow(seed=7, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 4, size - 5, size * 0.36, size * 0.08)
    trunk(cv, c, size * 0.42, size - 6, size * 0.12, col="#5a4a38")
    leafy(cv, c, size * 0.3, size * 0.28, "#6a9a48", seed, clumps=6)
    rnd = random.Random(seed)
    for i in range(22):   # hanging strands
        x = c - size * 0.36 + i * size * 0.72 / 21
        y0 = size * 0.32 + rnd.uniform(-4, 6)
        L = rnd.uniform(0.25, 0.4) * size
        cv.paint(cap(x, y0, x + 1, y0 + L, 1.4, 0.8), "#7aaa50" if i % 3 else "#5a8a3c", rim=1, soft=True)
    return cv.finish()


def dead_tree(seed=8, size=96):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 4, size - 5, size * 0.2, size * 0.05)
    col = "#6a5a4a"
    trunk(cv, c, size * 0.3, size - 6, size * 0.1, col=col)
    rnd = random.Random(seed)
    for k in range(6):
        a = -math.pi / 2 + rnd.uniform(-1.1, 1.1)
        y0 = size * rnd.uniform(0.3, 0.55)
        L = size * rnd.uniform(0.18, 0.3)
        x1, y1 = c + math.cos(a) * L, y0 + math.sin(a) * L
        cv.paint(cap(c, y0, x1, y1, 2.2, 0.8), col, rim=1)
        cv.paint(cap(x1, y1, x1 + rnd.uniform(-8, 8), y1 - rnd.uniform(4, 9), 0.9, 0.5), col, rim=1)
    return cv.finish()


def sacred_tree(seed=9, size=96):
    """an old oak with ribbons and votive cloths (the forests' and the steppe's holy tree)"""
    cv = oak(seed, size, base="#3f6f2a")
    rnd = random.Random(seed)
    for i in range(9):
        x = size / 2 + rnd.uniform(-28, 28) * size / 96
        y = size * 0.5 + rnd.uniform(-6, 10) * size / 96
        col = rnd.choice(("#c83a2a", "#f0e8d0", "#3a6ac0", "#e0b030"))
        for j in range(6):
            cv.set(int(x) + (j // 3), int(y) + j, col if j < 5 else sh(rgb(col), 0.6))
    return cv


def bush(seed=10, size=48, base="#4f8a34", berries=None):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 3, size - 4, size * 0.38, size * 0.08)
    leafy(cv, c, size * 0.6, size * 0.32, base, seed, clumps=6, spread=0.7)
    if berries:
        rnd = random.Random(seed)
        for _ in range(9):
            x, y = c + rnd.uniform(-14, 14), size * 0.6 + rnd.uniform(-9, 8)
            if cv.get(int(x), int(y)):
                cv.dot(x, y, berries)
                cv.dot(x - 1, y - 1, sh(rgb(berries), 1.4))
    return cv.finish()


def rock(seed=11, size=48, base="#8a8a84", moss=None, r=0.36):
    cv = Cv(size)
    c = size / 2
    ground_shadow(cv, c + 3, size - 5, size * 0.4, size * 0.08)
    n = Noise(seed, 256, 4)

    def face(px, py, col):
        v = n(px, py)
        if moss and py < size * 0.62 and v > 0.5:
            return moss if v < 0.75 else sh(rgb(moss), 1.2)
        if v < 0.14:
            return sh(col, 0.7)   # cracks
        return None
    body = AND(blob(c, size * 0.62, size * r * 1.15, seed, lobes=3, amp=0.12), ell(c, size * 0.64, size * r * 1.1, size * r * 0.78))
    cv.paint(body, base, fn=face)
    # the lit top facet and a darker front facet: a chunky boulder, not a blob
    cv.paint(AND(body, ell(c - 3, size * 0.5, size * r * 0.8, size * r * 0.42)), sh(rgb(base), 1.14), flat=True, fn=face)
    cv.paint(AND(body, lambda x, y: x > c + size * r * 0.35 and y > size * 0.56), sh(rgb(base), 0.84), flat=True, fn=face)
    return cv.finish()


def reeds(seed=12, size=48, base="#7a8a3a", papyrus=False):
    cv = Cv(size)
    rnd = random.Random(seed)
    cv.shadow(ell(size / 2 + 2, size - 4, size * 0.34, size * 0.07))
    for i in range(15):
        x = size * 0.18 + i * size * 0.64 / 14 + rnd.uniform(-1, 1)
        top = size * rnd.uniform(0.1, 0.35)
        lean = rnd.uniform(-4, 4)
        cv.paint(cap(x, size - 4, x + lean, top, 1.1, 0.6), sh(rgb(base), rnd.uniform(1.0, 1.3)), rim=1, soft=True, flat=True)
        if papyrus:   # the umbel: a fan of fine rays
            for a in range(-3, 4):
                cv.paint(cap(x + lean, top, x + lean + a * 1.6, top - 5 + abs(a) * 0.6, 0.6), "#8aa84a", rim=1, flat=True)
        elif i % 3 == 0:   # a bulrush head
            cv.paint(cap(x + lean * 0.9, top + 3, x + lean * 0.95, top + 8, 1.4), "#6a4a2a", rim=1)
    return cv.finish()


def all_nature():
    return {
        "ob_tree": oak(1, 160),
        "ob_tree_birch": birch(2, 144),
        "ob_pine": conifer(3, 192),
        "ob_pine_snow": conifer(13, 192, snow=True),
        "ob_cypress": cypress(4, 176),
        "ob_olive": olive(5, 128),
        "ob_palm": palm(6, 176),
        "ob_willow": willow(7, 160),
        "ob_dead_tree": dead_tree(8, 128),
        "ob_sacred_tree": sacred_tree(9, 176),
        "ob_bush": bush(10, 64),
        "ob_bush_berry": bush(14, 64, base="#3f7a30", berries="#b02838"),
        "ob_rock": rock(11),
        "ob_rock_moss": rock(15, base="#7a7a74", moss="#5a8a34"),
        "ob_rock_sand": rock(16, base="#c8a46a"),
        "ob_reeds": reeds(12),
        "ob_papyrus": reeds(17, base="#5a8a34", papyrus=True),
    }
