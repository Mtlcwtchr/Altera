"""Rasterise the HUD's line icons (the stroke paths of the design canvas "Altera UI и HUD",
viewBox 0 0 20 20) into one atlas for the game: white strokes on transparent, tinted at run time.

python make_icons.py <repo>
Writes assets/altera_game/ui/icons.png (CELL px cells in a row) and icons.json {name: index}."""
import json, math, os, re, sys
from PIL import Image, ImageDraw

REPO = sys.argv[1] if len(sys.argv) > 1 else 'D:/AlteraGame/altera'
OUT = os.path.join(REPO, 'assets/altera_game/ui')
CELL, SS = 48, 4   # cell px, supersampling

ICONS = {
    # top menu
    'sheet': 'M6 7a4 4 0 1 0 8 0a4 4 0 1 0-8 0M3 18c0-4 3-6 7-6s7 2 7 6',
    'dynasty': 'M10 18V9M10 9L5 4M10 9l5-5M10 13l-5-3M10 13l5-3',
    'colony': 'M2 17V9l4-3 4 3v8M10 17v-6l4-2 4 2v6M1 17h18',
    'build': 'M4 17l7-7M9 4l7 7-2 2-7-7z',
    'annals': 'M10 5c-2-1.5-5-1.5-7 0v11c2-1.5 5-1.5 7 0 2-1.5 5-1.5 7 0V5c-2-1.5-5-1.5-7 0zM10 5v11',
    'map': 'M2 5l5-2 6 2 5-2v12l-5 2-6-2-5 2zM7 3v12M13 5v12',
    # need tiers and the other slots
    'body': 'M10 17l-6-6a4 4 0 0 1 6-5a4 4 0 0 1 6 5z',
    'safety': 'M10 2l7 3v5c0 4-3 7-7 8-4-1-7-4-7-8V5z',
    'belonging': 'M3 10l7-6 7 6M5 9v8h10V9',
    'esteem': 'M3 15h14l1-8-4 3-4-6-4 6-4-3z',
    'calling': 'M4 17l7-7M9 4l7 7-2 2-7-7z',
    'wants': 'M10 2l2.4 5 5.6.8-4 3.9 1 5.5-5-2.7-5 2.7 1-5.5-4-3.9 5.6-.8z',
    'people': 'M4 7a3 3 0 1 0 6 0a3 3 0 1 0-6 0M12 8a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0M2 17c0-3 2.5-5 5-5s5 2 5 5M13 12.5c3 0 5 1.8 5 4.5',
    'values': 'M10 3v14M6 17h8M3 6h14M3 6l-2 5h4zM17 6l-2 5h4z',
    'things': 'M4 7h12l-1 10H5zM7 7V5a3 3 0 0 1 6 0v2',
    # wheel spheres
    'w_social': 'M4 4h12v8H9l-4 4v-4H4z',
    'w_family': 'M10 17l-6-6a4 4 0 0 1 6-5a4 4 0 0 1 6 5z',
    'w_economy': 'M3 10h14a7 7 0 0 1-14 0zM8 6c0-2 2-2 2-4M12 6c0-2 2-2 2-4',
    'w_politics': 'M3 15h14l1-8-4 3-4-6-4 6-4-3z',
    'w_knowledge': 'M10 5c-2-1.5-5-1.5-7 0v11c2-1.5 5-1.5 7 0 2-1.5 5-1.5 7 0V5c-2-1.5-5-1.5-7 0zM10 5v11',
    'w_faith': 'M10 2a8 8 0 1 0 0 16a8 8 0 1 0 0-16M10 6v8M6 10h8',
    'w_violence': 'M4 16l9-9 3-4 1 1-4 3-9 9zM4 12l4 4',
    'w_medicine': 'M8 3h4v5h5v4h-5v5H8v-5H3V8h5z',
    # misc
    'close': 'M5 5l10 10M15 5L5 15',
    'pause': 'M7 4v12M13 4v12',
    'play': 'M6 4l10 6-10 6z',
    'follow': 'M10 2v4M10 14v4M2 10h4M14 10h4M10 7a3 3 0 1 0 0 6a3 3 0 1 0 0-6',
    'hand': 'M7 10V4a1.5 1.5 0 0 1 3 0v5M10 9V3a1.5 1.5 0 0 1 3 0v6M13 9V5a1.5 1.5 0 0 1 3 0v7c0 3-2 6-6 6-3 0-4-2-6-5l-1.5-2.5a1.5 1.5 0 0 1 2.5-1.5L7 11',
}
FILLED = {
    'i_knife': ('M3 17l2-4 9-10 3 3-10 9z', '#d8dce8'),
    'i_bread': ('M3 12c0-5 14-5 14 0v4H3z', '#e0c07a'),
    'i_torch': ('M9 18V9h2v9zM7 9c0-3 3-4 3-7 2 2 3 4 3 7z', '#f09a4a'),
    'i_wood': ('M2 8h16v4H2zM4 13h12v4H4z', '#a87a4a'),
    'i_food': ('M4 9h12c0 5-3 8-6 8s-6-3-6-8zM6 9c0-3 2-5 4-5s4 2 4 5', '#d8a060'),
    'i_herb': ('M10 18V8M10 12c-4 0-6-3-6-7 4 0 6 3 6 7M10 10c3 0 6-2 6-6-4 0-6 2-6 6', '#7ab060'),
    'i_stone': ('M4 15l2-7 6-3 5 4-1 6z', '#a0a0a0'),
    'i_cloth': ('M4 4h12v12H4zM4 8h12M4 12h12', '#c8b088'),
    'i_tool': ('M4 17l7-7M9 4l7 7-2 2-7-7z', '#b8b0a0'),
}

TOK = re.compile(r'[A-Za-z]|-?\d*\.?\d+(?:e-?\d+)?')


def arc_points(x0, y0, rx, ry, phi, large, sweep, x1, y1):
    # SVG endpoint arc to centre form
    if rx == 0 or ry == 0: return [(x1, y1)]
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (x0 - x1) / 2, (y0 - y1) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    lam = (x1p ** 2) / rx ** 2 + (y1p ** 2) / ry ** 2
    if lam > 1: rx *= math.sqrt(lam); ry *= math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0, num / den)) if den else 0
    if large == sweep: co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = c * cxp - s * cyp + (x0 + x1) / 2, s * cxp + c * cyp + (y0 + y1) / 2
    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0: dt -= 2 * math.pi
    elif sweep and dt < 0: dt += 2 * math.pi
    n = max(4, int(abs(dt) / (math.pi / 16)))
    pts = []
    for k in range(1, n + 1):
        t = t1 + dt * k / n
        pts.append((cx + rx * math.cos(t) * c - ry * math.sin(t) * s, cy + rx * math.cos(t) * s + ry * math.sin(t) * c))
    return pts


def parse(d):
    """SVG path -> list of (points, closed) polylines."""
    toks = TOK.findall(d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    polys, cur = [], []
    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]): cmd = toks[i]; i += 1
        rel = cmd.islower(); C = cmd.upper()
        if C == 'M':
            nx, ny = num(), num()
            if rel: nx += x; ny += y
            if cur: polys.append((cur, False))
            x, y = nx, ny; sx, sy = x, y; cur = [(x, y)]
            cmd = 'l' if rel else 'L'
        elif C == 'L':
            nx, ny = num(), num()
            if rel: nx += x; ny += y
            x, y = nx, ny; cur.append((x, y))
        elif C == 'H':
            nx = num(); x = nx + x if rel else nx; cur.append((x, y))
        elif C == 'V':
            ny = num(); y = ny + y if rel else ny; cur.append((x, y))
        elif C == 'C':
            p = [num() for _ in range(6)]
            if rel: p = [p[k] + (x if k % 2 == 0 else y) for k in range(6)]
            for k in range(1, 13):
                t = k / 12; u = 1 - t
                cur.append((u ** 3 * x + 3 * u * u * t * p[0] + 3 * u * t * t * p[2] + t ** 3 * p[4],
                            u ** 3 * y + 3 * u * u * t * p[1] + 3 * u * t * t * p[3] + t ** 3 * p[5]))
            x, y = p[4], p[5]
        elif C == 'S':
            p = [num() for _ in range(4)]
            if rel: p = [p[k] + (x if k % 2 == 0 else y) for k in range(4)]
            cur.append((p[2], p[3])); x, y = p[2], p[3]
        elif C == 'A':
            rx, ry, phi, la, sw, nx, ny = num(), num(), num(), num(), num(), num(), num()
            if rel: nx += x; ny += y
            cur += arc_points(x, y, rx, ry, math.radians(phi), int(la), int(sw), nx, ny)
            x, y = nx, ny
        elif C == 'Z':
            cur.append((sx, sy)); polys.append((cur, True)); cur = [(sx, sy)]; x, y = sx, sy
        else:
            i += 1
    if len(cur) > 1: polys.append((cur, False))
    return [p for p in polys if len(p[0]) > 1]


def render(d, fill=None, stroke=(255, 255, 255, 255), width=1.6):
    S = CELL * SS
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    dr = ImageDraw.Draw(im)
    k = S / 20.0
    polys = parse(d)
    if fill:
        for pts, closed in polys:
            if closed: dr.polygon([(px * k, py * k) for px, py in pts], fill=fill)
    w = int(round(width * k))
    for pts, closed in polys:
        q = [(px * k, py * k) for px, py in pts]
        dr.line(q, fill=stroke, width=w, joint='curve')
        for p in (q[0], q[-1]):
            dr.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=stroke)
    return im.resize((CELL, CELL), Image.LANCZOS)


def hexrgb(h):
    h = h.lstrip('#'); return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


names = list(ICONS) + list(FILLED)
atlas = Image.new('RGBA', (CELL * len(names), CELL), (0, 0, 0, 0))
for n, name in enumerate(names):
    if name in ICONS: im = render(ICONS[name])
    else:
        d, col = FILLED[name]
        im = render(d, fill=hexrgb(col), stroke=(16, 22, 30, 255), width=1.0)
    atlas.paste(im, (n * CELL, 0))
os.makedirs(OUT, exist_ok=True)
atlas.save(os.path.join(OUT, 'icons.png'))
json.dump({'cell': CELL, 'icons': {nm: i for i, nm in enumerate(names)}}, open(os.path.join(OUT, 'icons.json'), 'w'), indent=1)
print('icons', len(names))
