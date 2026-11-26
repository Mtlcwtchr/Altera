# LR-167: the painter of the detailed 2D pixel art (48 px = 1 m cell), a port of the
# approved pilot sheet's method: shapes are predicates over art pixels, each painted in
# three tones lit from the top left (light rim, base, cool shadow, dark underside), and
# finish() rings the figure with a coloured outline (the neighbour's colour darkened).
# Deterministic: every random thing is seeded. Output: RGB tuples, exported to palette
# grids by export.py (text in content/, no binary assets).
import math
import random

Rgb = tuple


def hexc(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def rgb(c):
    return hexc(c) if isinstance(c, str) else tuple(c)


def clamp(v):
    return max(0, min(255, int(round(v))))


def sh(c, f):
    """darker (f < 1) or towards white (f > 1)"""
    c = rgb(c)
    if f < 1:
        return tuple(clamp(v * f) for v in c)
    return tuple(clamp(v + (255 - v) * (f - 1)) for v in c)


def warm(c):
    return (clamp(c[0] * 1.04), clamp(c[1]), clamp(c[2] * 0.94))


def cool(c):
    return (clamp(c[0] * 0.95), clamp(c[1] * 0.97), clamp(c[2] * 1.07))


def mix(a, b, t):
    a, b = rgb(a), rgb(b)
    return tuple(clamp(a[i] + (b[i] - a[i]) * t) for i in range(3))


def tones(base):
    b = rgb(base)
    return warm(sh(b, 1.32)), b, cool(sh(b, 0.76)), cool(sh(b, 0.5))


# --- shapes (predicates over (x, y), pixel centres at +0.5) -------------------------------
def bb(f, box):
    f.bb = box
    return f


def _bb(*fs):
    bs = [getattr(f, "bb", None) for f in fs]
    if any(b is None for b in bs):
        return None
    return (min(b[0] for b in bs), min(b[1] for b in bs), max(b[2] for b in bs), max(b[3] for b in bs))


def ell(cx, cy, rx, ry):
    return bb(lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1, (cx - rx, cy - ry, cx + rx, cy + ry))


def rect(x0, y0, x1, y1):
    return bb(lambda x, y: x0 <= x < x1 and y0 <= y < y1, (x0, y0, x1, y1))


def cap(x0, y0, x1, y1, r0, r1=None):
    r1 = r0 if r1 is None else r1
    dx, dy = x1 - x0, y1 - y0
    dd = dx * dx + dy * dy or 1

    def f(x, y):
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / dd))
        px, py, r = x0 + dx * t, y0 + dy * t, r0 + (r1 - r0) * t
        return (x - px) ** 2 + (y - py) ** 2 <= r * r
    m = max(r0, r1)
    return bb(f, (min(x0, x1) - m, min(y0, y1) - m, max(x0, x1) + m, max(y0, y1) + m))


def poly(pts):
    def f(x, y):
        c = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                c = not c
            j = i
        return c
    return bb(f, (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts)))


def OR(*fs):
    return bb(lambda x, y: any(g(x, y) for g in fs), _bb(*fs))


def AND(*fs):
    bs = [getattr(f, "bb", None) for f in fs if getattr(f, "bb", None)]
    box = (max(b[0] for b in bs), max(b[1] for b in bs), min(b[2] for b in bs), min(b[3] for b in bs)) if bs else None
    return bb(lambda x, y: all(g(x, y) for g in fs), box)


def SUB(a, b):
    return bb(lambda x, y: a(x, y) and not b(x, y), getattr(a, "bb", None))


def mirror(f, cx):
    return lambda x, y: f(2 * cx - x, y)


def blob(cx, cy, r, seed, lobes=7, amp=0.22):
    """an irregular round thing (a bush, a stone, a crown of leaves)"""
    rnd = random.Random(seed)
    ph = [rnd.uniform(0, 6.28) for _ in range(3)]
    k = [rnd.randint(2, lobes) for _ in range(3)]
    a = [amp * rnd.uniform(0.4, 1.0) for _ in range(3)]

    def f(x, y):
        dx, dy = x - cx, (y - cy)
        d = math.hypot(dx, dy)
        if d < 1e-6:
            return True
        t = math.atan2(dy, dx)
        rr = r * (1 + sum(a[i] * math.sin(k[i] * t + ph[i]) for i in range(3)) / 1.6)
        return d <= rr
    return bb(f, (cx - r * 1.6, cy - r * 1.6, cx + r * 1.6, cy + r * 1.6))


# --- periodic value noise (tiles must wrap) ------------------------------------------------
class Noise:
    def __init__(self, seed, period=48, cell=8, edge=None):
        """edge: a fixed value on the lattice's border lines, so tiles of different seeds
        (variants of one ground) and rotated tiles meet without a seam"""
        self.p, self.c = period, cell
        n = max(1, period // cell)
        self.n = n
        rnd = random.Random(seed)
        self.g = [[rnd.random() for _ in range(n)] for _ in range(n)]
        if edge is not None:
            for i in range(n):
                self.g[0][i] = self.g[i][0] = edge

    def __call__(self, x, y):
        n, c = self.n, self.c
        fx, fy = (x % self.p) / c, (y % self.p) / c
        x0, y0 = int(fx) % n, int(fy) % n
        x1, y1 = (x0 + 1) % n, (y0 + 1) % n
        tx, ty = fx - int(fx), fy - int(fy)
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        g = self.g
        a = g[y0][x0] + (g[y0][x1] - g[y0][x0]) * tx
        b = g[y1][x0] + (g[y1][x1] - g[y1][x0]) * tx
        return a + (b - a) * ty


def fbm(seed, period=48, cells=(16, 8, 4), edge=None):
    ns = [Noise(seed + i * 101, period, c, edge) for i, c in enumerate(cells)]
    w = [0.55, 0.3, 0.15]

    def f(x, y):
        return sum(w[i] * ns[i](x, y) for i in range(len(ns)))
    return f


# --- the canvas -------------------------------------------------------------------------------
class Cv:
    def __init__(self, w, h=None, wrap=False):
        self.w, self.h = w, (h or w)
        self.px = [None] * (self.w * self.h)
        self.wrap = wrap   # a tile: neighbours wrap around, no outline at the borders

    def get(self, x, y):
        if self.wrap:
            return self.px[(y % self.h) * self.w + (x % self.w)]
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return None
        return self.px[y * self.w + x]

    def set(self, x, y, c):
        if self.wrap:
            x, y = x % self.w, y % self.h
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = None if c is None else (c if c == "SHADOW" else rgb(c))

    def fill(self, c):
        c = rgb(c)
        self.px = [c] * (self.w * self.h)

    def span(self, pred):
        b = getattr(pred, "bb", None)
        if b is None or self.wrap:
            return 0, 0, self.w, self.h
        return (max(0, int(b[0]) - 1), max(0, int(b[1]) - 1), min(self.w, int(b[2]) + 2), min(self.h, int(b[3]) + 2))

    def mask(self, pred):
        m = bytearray(self.w * self.h)
        x0, y0, x1, y1 = self.span(pred)
        for y in range(y0, y1):
            for x in range(x0, x1):
                if pred(x + 0.5, y + 0.5):
                    m[y * self.w + x] = 1
        return m

    def paint(self, pred, base, rim=2, flat=False, soft=False, fn=None, light=1.32, shade=0.76, dark=0.5, under=True):
        """the pilot's three-tone fill: light where the shape has no neighbour up-left, cool
        shadow two pixels in from its down-right edge, dark on the very edge underneath"""
        w, h = self.w, self.h
        m = self.mask(pred)

        def at(x, y):
            if self.wrap:
                return m[(y % h) * w + (x % w)]
            return 0 if (x < 0 or y < 0 or x >= w or y >= h) else m[y * w + x]
        B = rgb(base)
        Lc, Sc, Dc = warm(sh(B, light)), cool(sh(B, shade)), cool(sh(B, dark))
        X0, Y0, X1, Y1 = self.span(pred)
        for y in range(Y0, Y1):
            for x in range(X0, X1):
                if not m[y * w + x]:
                    continue
                c = B
                if not at(x - 1, y - 1) or (rim > 1 and not at(x - 2, y - 2) and at(x + 2, y + 2)):
                    c = Lc
                if not flat:
                    if not at(x + 2, y + 2):
                        c = Sc
                    if not at(x + 1, y + 1) or (under and not at(x, y + 1) and not soft):
                        c = Dc
                if fn:
                    t = fn(x + 0.5, y + 0.5, c)
                    if t is not None:
                        c = rgb(t)
                self.px[y * w + x] = c
        return m

    def plain(self, pred, col):
        """a flat colour, no shading (details, eyes, seams)"""
        c = rgb(col) if not callable(col) else None
        X0, Y0, X1, Y1 = self.span(pred)
        for y in range(Y0, Y1):
            for x in range(X0, X1):
                if pred(x + 0.5, y + 0.5):
                    self.px[y * self.w + x] = c if c else rgb(col(x + 0.5, y + 0.5))

    def shade_by(self, pred, f):
        """multiply what is there by f(x, y) (a soft shadow, a gradient)"""
        for y in range(self.h):
            for x in range(self.w):
                c = self.px[y * self.w + x]
                if c and pred(x + 0.5, y + 0.5):
                    self.px[y * self.w + x] = sh(c, f(x + 0.5, y + 0.5))

    def dot(self, x, y, c, w=1, h=1):
        for j in range(h):
            for i in range(w):
                self.set(int(x) + i, int(y) + j, c)

    def finish(self, outline=0.3):
        """the coloured outline: an empty pixel next to the figure takes its neighbour darkened"""
        if self.wrap or not outline:
            return self
        out = list(self.px)
        for y in range(self.h):
            for x in range(self.w):
                if self.px[y * self.w + x] not in (None, "SHADOW"):
                    continue
                for nx, ny in ((x, y - 1), (x - 1, y), (x + 1, y), (x, y + 1)):
                    n = self.get(nx, ny)
                    if n and n != "SHADOW":
                        out[y * self.w + x] = sh(n, outline)
                        break
        self.px = out
        return self

    def shadow(self, pred, alpha=0.35):
        """a drop shadow on the ground under a standing thing (a translucent dark oval)"""
        X0, Y0, X1, Y1 = self.span(pred)
        for y in range(Y0, Y1):
            for x in range(X0, X1):
                if self.px[y * self.w + x] is None and pred(x + 0.5, y + 0.5):
                    self.px[y * self.w + x] = "SHADOW"

    def blit(self, other, ox, oy):
        for y in range(other.h):
            for x in range(other.w):
                c = other.px[y * other.w + x]
                if c is not None and not (c == "SHADOW" and self.get(ox + x, oy + y) not in (None, "SHADOW")):
                    self.set(ox + x, oy + y, c)

    def flipx(self):
        o = Cv(self.w, self.h, self.wrap)
        for y in range(self.h):
            for x in range(self.w):
                o.px[y * self.w + x] = self.px[y * self.w + (self.w - 1 - x)]
        return o


class Scaled(Cv):
    """a canvas k times bigger that takes shapes in the small canvas's coordinates: a
    monument drawn once can stand 1.5 or 2 times taller, its outlines still one pixel"""

    def __init__(self, base, k):
        super().__init__(int(round(base * k)))
        self.k = k

    def _p(self, pred):
        k = self.k
        f = lambda x, y: pred(x / k, y / k)
        b = getattr(pred, "bb", None)
        if b:
            f.bb = (b[0] * k, b[1] * k, b[2] * k, b[3] * k)
        return f

    def paint(self, pred, base, fn=None, **kw):
        k = self.k
        g = (lambda x, y, c: fn(x / k, y / k, c)) if fn else None
        return super().paint(self._p(pred), base, fn=g, **kw)

    def plain(self, pred, col):
        return super().plain(self._p(pred), col)

    def shadow(self, pred, alpha=0.35):
        return super().shadow(self._p(pred), alpha)

    def dot(self, x, y, c, w=1, h=1):
        k = self.k
        super().dot(x * k, y * k, c, max(1, int(round(w * k))), max(1, int(round(h * k))))

    def blit(self, other, ox, oy):
        super().blit(other, int(ox * self.k), int(oy * self.k))
