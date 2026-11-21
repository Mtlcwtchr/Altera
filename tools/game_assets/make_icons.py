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
    # the workshop (boards 10-15): processes, stations, shapes, materials, wishes, actions
    'c_flame': 'M10 2c1 3 4 5 4 9a4 4 0 0 1-8 0c0-2 1-3 2-4 0 2 1 2 1 2 0-3 0-5 1-7z',
    'c_hammer': 'M4 8l5-5 4 4-5 5zM10 11l6 6',
    'c_cut': 'M3 4l9 9M3 16l9-9M13 11l4 5M13 9l4-5',
    'c_carve': 'M3 15l8-8 3 3-8 8zM12 4l4 4',
    'c_drop': 'M10 2c3 4 5 6 5 9a5 5 0 0 1-10 0c0-3 2-5 5-9z',
    'c_grind': 'M10 3a7 7 0 1 0 0 14a7 7 0 1 0 0-14M10 8a2 2 0 1 0 0 4a2 2 0 1 0 0-4',
    'c_join': 'M3 10h5M12 10h5M7 6l-4 4 4 4M13 6l4 4-4 4',
    'c_weave': 'M4 4v12M8 4v12M12 4v12M16 4v12M3 7h14M3 13h14',
    'c_smelt': 'M4 6h12l-2 10H6zM7 3c0 1 1 1 1 2M12 3c0 1 1 1 1 2M6 11h8',
    'c_knap': 'M4 15l3-10 6-2 3 5-5 9zM8 6l2 5M13 4l-1 6',
    'c_link': 'M3 10a3 3 0 0 1 3-3h3a3 3 0 0 1 0 6H6a3 3 0 0 1-3-3zM11 7h3a3 3 0 0 1 0 6h-3',
    'c_mill': 'M10 3a7 7 0 1 0 0 14a7 7 0 1 0 0-14M10 3v14M3 10h14',
    'c_soak': 'M3 8c2-2 3 2 5 0s3 2 5 0 3 2 4 0M3 13c2-2 3 2 5 0s3 2 5 0 3 2 4 0',
    'c_dry': 'M10 7a3 3 0 1 0 0 6a3 3 0 1 0 0-6M10 2v2M10 16v2M2 10h2M16 10h2M4.5 4.5l1.5 1.5M14 14l1.5 1.5M4.5 15.5L6 14M14 6l1.5-1.5',
    'c_ferment': 'M6 6h8v10H6zM7 3h6v3H7zM9 10h1M11 13h1',
    'c_twist': 'M5 3c6 2 6 5 0 7s6 5 10 7M15 3c-2 1-3 2-3 3',
    'c_sew': 'M4 16L16 4M14 4h2v2M4 9c3 0 4 2 4 4s1 3 4 3',
    'c_mix': 'M3 10h14a7 7 0 0 1-14 0zM12 10l4-7',
    'c_mould': 'M5 7h10l-1 9H6zM6 7c0-2 8-2 8 0',
    'c_tan': 'M4 5l3-2 3 1 3-1 3 2-1 4 1 6H4l1-6z',
    'c_alloy': 'M3 14a4 4 0 0 1 8 0zM9 14a4 4 0 0 1 8 0zM10 4v5M8 7l2 2 2-2',
    'c_repair': 'M4 16l8-8M12 4a3 3 0 0 0 4 4l-2 2-4-4z',
    'c_anvil': 'M3 6h11l3 2H13v3l2 4H5l2-4V8H3z',
    'c_bench': 'M2 8h16v3H2zM4 11v6M16 11v6M7 8V5h3v3',
    'c_tub': 'M3 7h14l-2 10H5zM3 7c2-1 4 1 7 0s5-1 7 0',
    'c_loom': 'M3 3v14M17 3v14M3 5h14M3 15h14M7 5v10M10 5v10M13 5v10',
    'c_pot': 'M6 6h8l2 4-2 7H6l-2-7zM7 6V4h6v2',
    'c_rack': 'M3 4h14M5 4v13M15 4v13M8 4v5M12 4v7',
    'c_wheel': 'M10 4a6 6 0 1 0 0 12a6 6 0 1 0 0-12M10 8a2 2 0 1 0 0 4a2 2 0 1 0 0-4M4 18h12',
    'c_quern': 'M3 12h14v4H3zM4 9h12v3H4zM10 4v5M10 4h4',
    's_lump': 'M3 14l3-8 5-2 6 6-3 6H6z',
    's_blade': 'M10 1l2.2 10L10 13l-2.2-2zM6 13.5h8M10 14v5',
    's_rod': 'M4 16L16 4M3 15l2 2M15 3l2 2',
    's_bar': 'M2 8h16v4H2zM5 8v4M15 8v4',
    's_cord': 'M3 6c4-3 4 3 8 0s4 3 6 0M3 11c4-3 4 3 8 0s4 3 6 0M3 16c4-3 4 3 8 0s4 3 6 0',
    's_cloth': 'M3 4h14v12H3zM3 4l14 12M17 4L3 16',
    's_vessel': 'M5 5h10l-1 11H6zM15 7h2a2 2 0 0 1 0 4h-2',
    's_head': 'M7 3h6v7H7zM10 10v8',
    's_plate': 'M3 5h14v10H3zM6 8h8M6 12h8',
    's_ring': 'M10 4a6 6 0 1 0 0 12a6 6 0 1 0 0-12M10 7a3 3 0 1 0 0 6a3 3 0 1 0 0-6',
    'm_ore': 'M3 14l3-8 5-2 6 6-3 6H6zM8 9h1M12 11h1',
    'm_coal': 'M5 13l2-6 4-2 4 4-1 5z',
    'm_wood': 'M4 6c4-3 8-3 12 0M4 10c4-3 8-3 12 0M4 14c4-3 8-3 12 0',
    'm_hide': 'M4 5l3-2 3 1 3-1 3 2-1 4 1 6H4l1-6z',
    'm_clay': 'M5 7h10l-1 9H6zM6 7c0-2 8-2 8 0',
    'm_wool': 'M4 10a3 3 0 0 1 3-4 3 3 0 0 1 6 0 3 3 0 0 1 3 4 3 3 0 0 1-3 4H7a3 3 0 0 1-3-4',
    'm_plant': 'M10 18V8M10 12c-4 0-6-3-6-7 4 0 6 3 6 7M10 10c3 0 6-2 6-6-4 0-6 2-6 6',
    'm_bone': 'M5 4a2 2 0 1 0 1 3l7 7a2 2 0 1 0 3 1 2 2 0 1 0-1-3L8 5a2 2 0 1 0-3-1z',
    'm_food': 'M4 12h12a6 6 0 0 1-12 0zM7 10c0-2 1-2 1-4M12 10c0-2 1-2 1-4',
    'w_cut': 'M3 4l9 9M3 16l9-9M13 11l4 5M13 9l4-5',
    'w_strike': 'M7 3h6v7H7zM10 10v8',
    'w_hold': 'M5 5h10l-1 11H6zM15 7h2a2 2 0 0 1 0 4h-2',
    'w_warm': 'M10 2c1 3 4 5 4 9a4 4 0 0 1-8 0c0-2 1-3 2-4 0 2 1 2 1 2 0-3 0-5 1-7z',
    'w_bind': 'M3 10h5M12 10h5M7 6l-4 4 4 4M13 6l4 4-4 4',
    'w_feed': 'M4 12h12a6 6 0 0 1-12 0zM7 10c0-2 1-2 1-4M12 10c0-2 1-2 1-4',
    'w_cover': 'M10 2l7 3v5c0 4-3 7-7 8-4-1-7-4-7-8V5z',
    'w_heal': 'M8 3h4v5h5v4h-5v5H8v-5H3V8h5z',
    'a_repeat': 'M5 8h7a4 4 0 0 1 0 8H7M8 5L5 8l3 3',
    'a_inspect': 'M2 10c3-5 13-5 16 0-3 5-13 5-16 0zM10 8a2 2 0 1 0 0 4a2 2 0 1 0 0-4',
    'a_trash': 'M4 6h12M8 6V4h4v2M6 6l1 11h6l1-11',
    'a_rename': 'M4 16l1-4 8-8 3 3-8 8zM12 5l3 3',
    'a_take': 'M10 3v10M6 9l4 4 4-4M4 17h12',
    'a_ok': 'M10 3a7 7 0 1 0 0 14a7 7 0 1 0 0-14M7 10l2 2 4-4',
    'a_book': 'M4 3h9a3 3 0 0 1 3 3v11H7a3 3 0 0 1-3-3zM7 17V3',
    'a_hint': 'M10 2a5 5 0 0 0-3 9v2h6v-2a5 5 0 0 0-3-9zM8 16h4M9 18h2',
    'a_minus': 'M5 10h10',
    'a_plus': 'M5 10h10M10 5v10',
    'a_clear': 'M4 4l12 12M10 3a7 7 0 1 0 0 14a7 7 0 1 0 0-14',
    'k_len': 'M10 1l1.6 11H8.4zM6 13h8M10 13v6',
    'k_hard': 'M7 3h6v7H7zM10 10v8',
    'k_finish': 'M10 2l2.4 5 5.6.7-4 4 1 5.6L10 14.5 5 17.3l1-5.6-4-4 5.6-.7z',
    # the control bar: keys, mouse, free (Sims-like), the camera on the person or free
    'ctl_keys': 'M8 3h4v4H8zM3 9h4v4H3zM8 9h4v4H8zM13 9h4v4h-4zM5 16h10',
    'ctl_mouse': 'M6 7a4 4 0 0 1 8 0v6a4 4 0 0 1-8 0zM10 3v5M6 8h8',
    'ctl_free': 'M8 4a2 2 0 1 0 4 0a2 2 0 1 0-4 0M10 6v6M6 18l4-6 4 6M5 9h10M16 2l.8 1.6 1.6.8-1.6.8-.8 1.6-.8-1.6-1.6-.8 1.6-.8z',
    'cam_lock': 'M2 8h11v8H2zM13 10l5-2.5v9L13 14zM5 8V5.5a2.5 2.5 0 0 1 5 0V8',
    'cam_free': 'M2 8h11v8H2zM13 10l5-2.5v9L13 14zM4 5h2M8 5h2M12 5h2',
    # AL-56, the walls in the top view: up, cut away (the front ones low), down
    'walls_up': 'M3 18V8l7-5 7 5v10zM3 18h14M8 18v-5h4v5',
    'walls_cut': 'M3 18V9l5-3M17 18V9l-5-3M3 18h14M3 14h14M6 14v4M10 14v4M14 14v4',
    'walls_down': 'M2 18h16M2 15h16M2 15v3M18 15v3M7 15v3M13 15v3M10 3v8M7 8l3 3 3-3',
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
