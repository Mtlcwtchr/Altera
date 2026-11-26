# LR-159: mounts, draught beasts and vehicles of the 2D client as 32x32 grids of the
# palette letters of sprites.json (no new colours): content/sprites_2d/transport2d.json.
# Side view facing east (the client mirrors them for the west); tr_<kind> stands,
# tr_<kind>_1 is the other step of the walk (legs / wheel spokes). The feet and wheels
# stand on row 29 (the client puts row n-3 on the ground spot).
#
# The JSON is the data the game reads: edit it by hand, or change this script and rerun
#   python tools/content_gen/gen_transport_2d.py [--sheet out.png]
# A new kind: a function here (or grids in the JSON) named tr_<species or vehicle type>.
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(ROOT, 'content', 'sprites_2d', 'transport2d.json')
PAL = json.load(open(os.path.join(ROOT, 'content', 'sprites_2d', 'sprites.json'), encoding='utf-8'))['pal']
N = 32
GROUND = 29


class G:
    def __init__(self):
        self.g = [['.'] * N for _ in range(N)]

    def set(self, x, y, c):
        x, y = int(round(x)), int(round(y))
        if 0 <= x < N and 0 <= y < N and c != ' ':
            self.g[y][x] = c

    def get(self, x, y):
        return self.g[y][x] if 0 <= x < N and 0 <= y < N else '.'

    def ell(self, cx, cy, rx, ry, c, shade=None, shade_from=0.55):
        for y in range(N):
            for x in range(N):
                a, b = (x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry
                d = a * a + b * b
                if d <= 1.0:
                    self.set(x, y, shade if shade and b > shade_from else c)

    def rect(self, x0, y0, x1, y1, c):
        for y in range(int(y0), int(y1) + 1):
            for x in range(int(x0), int(x1) + 1):
                self.set(x, y, c)

    def line(self, x0, y0, x1, y1, c, w=1):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) * 2) + 1
        for i in range(n + 1):
            t = i / n
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            for dx in range(w):
                for dy in range(w):
                    self.set(x + dx - (w - 1) / 2, y + dy - (w - 1) / 2, c)

    def wheel(self, cx, cy, r, rim, hub, frame):
        for y in range(N):
            for x in range(N):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if r - 1.3 <= d <= r:
                    self.set(x, y, rim)
        a0 = 0 if frame == 0 else math.pi / 4
        for k in range(4):
            a = a0 + k * math.pi / 4
            self.line(cx - 0.5 + math.cos(a) * (r - 1.5), cy - 0.5 + math.sin(a) * (r - 1.5),
                      cx - 0.5 - math.cos(a) * (r - 1.5), cy - 0.5 - math.sin(a) * (r - 1.5), hub)
        self.set(cx - 0.5, cy - 0.5, 'k')

    def outline(self):
        o = [row[:] for row in self.g]
        for y in range(N):
            for x in range(N):
                if self.g[y][x] != '.':
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    v = self.get(x + dx, y + dy)
                    if v not in '.k':
                        o[y][x] = 'k'
                        break
        self.g = o

    def rows(self):
        return [''.join(r) for r in self.g]


def legs(g, xs, top, c, hoof, frame, spread=1.0, thick=2, far=None):
    """Four legs: xs = [back far, back near, front far, front near] standing; the walk spreads the pairs.
    The far legs are drawn in `far` (darker), behind the near ones."""
    for i, x in enumerate(xs):
        col = far if (far and i % 2 == 0) else c
        off = 0
        if frame:
            off = (-spread if i % 2 == 0 else spread)
        foot = x + off
        g.line(x, top, foot, GROUND - 1, col, thick)
        g.rect(foot - thick / 2 + 0.5, GROUND - 1, foot + thick / 2 - 0.5, GROUND - 1, hoof)


def horse(frame, body='n', dark='N', mane='P', size=1.0, saddle=True, ears='short', leg_top=21, low=0):
    g = G()
    s = size
    cy = 17 + low
    # tail
    g.line(15 - 8.5 * s, cy - 2, 15 - 10 * s, cy + 5, mane, 2)
    # legs behind the body
    legs(g, [15 - 6.5 * s, 15 - 3.5 * s, 15 + 3.5 * s, 15 + 6.5 * s], leg_top + low - 2, body, 'k', frame, 1.2, far=dark)
    g.ell(15, cy, 8.5 * s, 4.6 * s, body, dark, 0.45)
    # neck and head
    nx, ny = 15 + 6.5 * s, cy - 2
    hx, hy = 15 + 10.5 * s, cy - 7.5 * s
    g.line(nx, ny, hx - 1, hy + 1, body, 4)
    g.ell(hx + 0.5, hy, 2.6 * s, 1.9 * s, body)
    g.ell(hx + 2.4 * s, hy + 1.0, 1.4 * s, 1.2 * s, dark)
    g.line(nx - 1, ny - 2, hx - 2, hy - 1, mane, 1)
    if ears == 'short':
        g.set(hx - 0.5, hy - 2.3 * s, body)
    else:   # long (a donkey, a mule)
        g.line(hx - 1, hy - 2, hx - 2.5, hy - 5 * s, body, 1)
        g.line(hx, hy - 2, hx - 0.5, hy - 5 * s, dark, 1)
    g.set(hx + 0.8, hy - 0.6, 'k')   # eye
    if saddle:
        g.rect(13, cy - 5 * s + 0.5, 17, cy - 4 * s + 1, 'R')
        g.rect(14, cy - 4 * s + 1, 16, cy - 1, 'r')
    g.outline()
    return g


def ox(frame):
    g = G()
    cy = 18
    g.line(5, cy - 3, 4, cy + 4, 'E', 1)
    g.set(4, cy + 5, 'P')
    legs(g, [7, 11, 19, 23], cy + 2, 't', 'k', frame, 0.8, 3, far='T')
    g.ell(14, cy, 9.5, 5.6, 't', 'T', 0.4)
    g.ell(10, cy - 3, 3, 2, 't')   # the hump of the back
    g.ell(25, cy - 1, 3.2, 3.0, 't', 'T', 0.5)
    g.ell(27.3, cy + 0.6, 1.6, 1.4, 'e')
    g.line(24, cy - 3.6, 26.5, cy - 6.5, 'w', 1)   # horn
    g.set(26.5, cy - 7, 'g')
    g.set(26, cy - 2, 'k')
    g.rect(19, cy - 6, 23, cy - 5, 'n')   # the yoke
    g.outline()
    return g


def wolf(frame):
    g = G()
    cy = 19
    g.line(6, cy - 3, 2, cy - 6, 'W', 3)   # bushy tail up
    legs(g, [7.5, 10.5, 18.5, 21.5], cy + 2, 'W', 'k', frame, 1.2, far='x')
    g.ell(14, cy, 8, 4.2, 'W', 'g', 0.5)
    g.line(20, cy - 2, 23, cy - 5, 'W', 4)
    g.ell(24, cy - 6, 2.8, 2.2, 'W')
    g.line(25, cy - 5.5, 29, cy - 4.5, 'g', 2)   # snout
    g.set(29.5, cy - 5, 'k')
    g.line(23, cy - 8, 22.5, cy - 10, 'x', 1)   # ears
    g.line(24.5, cy - 8, 24.5, cy - 10, 'x', 1)
    g.set(25, cy - 6.5, 'y')   # the eye
    g.rect(12, cy - 5, 16, cy - 4, 'R')
    g.rect(13, cy - 4, 15, cy - 2, 'r')
    g.outline()
    return g


def lizard(frame):
    g = G()
    cy = 20
    g.line(9, cy, 1, cy + 4, 'D', 3)   # the tail
    g.line(1, cy + 4, 0, cy + 2, 'D', 2)
    for i, x in enumerate([9, 11, 19, 21]):   # bent legs
        off = (-1.2 if i % 2 == 0 else 1.2) if frame else 0
        g.line(x, cy + 2, x - 1.5 + off, cy + 5, 'D', 2)
        g.line(x - 1.5 + off, cy + 5, x + off, GROUND - 1, 'D', 2)
        g.set(x + off + 1, GROUND - 1, 'l')
    g.ell(15, cy, 8.5, 4, 'G', 'l', 0.45)
    for x in range(9, 22, 3):   # the crest
        g.set(x, cy - 4.5, 'z')
    g.line(22, cy - 1, 25, cy - 4, 'G', 3)
    g.ell(27, cy - 4.5, 3.4, 2, 'G', 'D', 0.6)
    g.set(27, cy - 5.6, 'y')
    g.set(30, cy - 4, 'k')
    g.rect(13, cy - 5, 17, cy - 4, 'R')
    g.rect(14, cy - 4, 16, cy - 2, 'r')
    g.outline()
    return g


def plank_bed(g, x0, x1, y0, y1, c='n', d='N'):
    g.rect(x0, y0, x1, y1, c)
    for x in range(int(x0) + 3, int(x1), 4):
        g.rect(x, y0, x, y1, d)
    g.rect(x0, y1, x1, y1, d)


def handcart(frame):
    g = G()
    plank_bed(g, 9, 21, 20, 24)
    g.line(9, 21, 3, 17, 'N', 1)   # the handles, back toward the pusher
    g.line(9, 23, 3, 19, 'N', 1)
    g.line(20, 25, 19, GROUND - 1, 'N', 1)   # the leg
    g.wheel(15, 26, 3.5, 'P', 'e', frame)
    g.outline()
    return g


def cart(frame):
    g = G()
    plank_bed(g, 5, 24, 16, 21)
    g.rect(5, 14, 24, 15, 'e')   # the side board
    g.line(24, 19, 31, 18, 'N', 1)   # the shafts to the beast ahead
    g.wheel(14, 23, 6, 'P', 'n', frame)
    g.outline()
    return g


def wagon(frame):
    g = G()
    plank_bed(g, 2, 28, 18, 22)
    for y in range(9, 18):   # the canvas over hoops
        w = int(round(math.sqrt(max(0, 1 - ((y - 17.5) / 9) ** 2)) * 13))
        g.rect(15 - w, y, 15 + w - 1, y, 'w')
    for x in (5, 11, 17, 23):
        g.line(x, 10 + abs(x - 14) / 4, x, 17, 'g', 1)
    g.line(28, 20, 31, 19, 'N', 1)
    g.wheel(7, 24, 5, 'P', 'n', frame)
    g.wheel(23, 24, 5, 'P', 'n', frame)
    g.outline()
    return g


def coach(frame):
    g = G()
    g.rect(6, 9, 25, 21, 'v')   # the box
    g.rect(6, 8, 25, 8, 'T')
    g.rect(6, 21, 25, 21, 'T')
    g.rect(9, 11, 13, 15, 'U')   # windows
    g.rect(18, 11, 22, 15, 'U')
    g.rect(15, 11, 16, 20, 'V')  # the door
    g.set(16, 16, 't')
    g.rect(24, 6, 29, 7, 'N')    # the driver's box in front
    g.line(26, 8, 25, 12, 'N', 1)
    g.line(25, 20, 31, 19, 'N', 1)
    g.wheel(9, 24, 5, 'P', 't', frame)
    g.wheel(22, 25, 4, 'P', 't', frame)
    g.outline()
    return g


def sled(frame):
    g = G()
    plank_bed(g, 5, 24, 21, 24, 'e', 'E')
    g.line(3, GROUND - 1, 26, GROUND - 1, 'm', 1)   # the runner
    g.line(26, GROUND - 1, 29, GROUND - 4, 'm', 1)  # curling up in front
    for x in (8, 15, 22):
        g.line(x, 25, x, GROUND - 2, 'N', 1)
    g.line(24, 22, 31, 21, 'N', 1)
    g.outline()
    return g


KINDS = {
    'horse': lambda f: horse(f),
    'pony': lambda f: horse(f, body='o', dark='z', mane='h', size=0.75, leg_top=23, low=3),
    'donkey': lambda f: horse(f, body='W', dark='x', mane='x', size=0.8, ears='long', leg_top=22, low=2),
    'mule': lambda f: horse(f, body='e', dark='E', mane='P', size=0.9, ears='long', leg_top=22, low=1),
    'ox': ox,
    'war_wolf': wolf,
    'riding_lizard': lizard,
    'handcart': handcart,
    'cart': cart,
    'wagon': wagon,
    'coach': coach,
    'sled': sled,
}


def main():
    sprites = {}
    for k, fn in KINDS.items():
        sprites['tr_' + k] = fn(0).rows()
        sprites['tr_' + k + '_1'] = fn(1).rows()
    out = {
        '_doc': 'LR-159: mounts and vehicles of the 2D client, 32x32 grids of the sprites.json palette, side view facing '
                'east (mirrored for the west), feet / wheels on row 29. tr_<kind> stands, tr_<kind>_1 is the other step. '
                'Generated by tools/content_gen/gen_transport_2d.py; editable by hand (the window reloads it on start).',
        'sprites': sprites,
    }
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('{\n "_doc": ' + json.dumps(out['_doc'], ensure_ascii=False) + ',\n "sprites": {\n')
        items = list(sprites.items())
        for i, (k, rows) in enumerate(items):
            f.write('  ' + json.dumps(k) + ': [\n')
            f.write(',\n'.join('   ' + json.dumps(r) for r in rows))
            f.write('\n  ]' + (',' if i + 1 < len(items) else '') + '\n')
        f.write(' }\n}\n')
    if '--sheet' in sys.argv:
        sheet(sprites, sys.argv[sys.argv.index('--sheet') + 1])


def sheet(sprites, path, scale=4):
    from PIL import Image
    names = list(sprites)
    cols = 6
    rows = (len(names) + cols - 1) // cols
    img = Image.new('RGBA', (cols * N * scale, rows * N * scale), (120, 150, 90, 255))
    for i, n in enumerate(names):
        ox_, oy = (i % cols) * N * scale, (i // cols) * N * scale
        for y, row in enumerate(sprites[n]):
            for x, ch in enumerate(row):
                if ch == '.':
                    continue
                h = PAL.get(ch, '#ff00ff').lstrip('#')
                c = tuple(int(h[j:j + 2], 16) for j in (0, 2, 4)) + (255,)
                for dy in range(scale):
                    for dx in range(scale):
                        img.putpixel((ox_ + x * scale + dx, oy + y * scale + dy), c)
    img.save(path)


if __name__ == '__main__':
    main()
