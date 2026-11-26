# LR-167: a canvas to a sprite entry of content/sprites_2d/*.json with its own palette:
#   {"pal": {"a": "#rrggbb", "z": "#rrggbbaa"}, "rows": ["..ab..", ...]}
# '.' is clear; up to 88 colours per sprite (more are merged to the nearest).
LETTERS = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
           "!#$%&()*+,-/:;<=>?@[]^_{|}~'`")
SHADOW = "#1a120c58"


def _hex(c):
    return "#%02x%02x%02x" % c


def _near(c, pal):
    best, bd = None, 1e18
    for p in pal:
        d = (c[0] - p[0]) ** 2 * 3 + (c[1] - p[1]) ** 2 * 4 + (c[2] - p[2]) ** 2 * 2
        if d < bd:
            best, bd = p, d
    return best


def entry(cv, max_colours=len(LETTERS) - 1):
    count = {}
    for c in cv.px:
        if c is not None and c != "SHADOW":
            count[c] = count.get(c, 0) + 1
    cols = sorted(count, key=lambda c: -count[c])
    keep = cols[:max_colours]
    remap = {c: (c if c in keep else _near(c, keep)) for c in cols}
    pal, letter = {}, {}
    for i, c in enumerate(keep):
        letter[c] = LETTERS[i]
        pal[LETTERS[i]] = _hex(c)
    if "SHADOW" in cv.px:
        pal[LETTERS[len(keep)]] = SHADOW
        letter["SHADOW"] = LETTERS[len(keep)]
    rows = []
    for y in range(cv.h):
        r = []
        for x in range(cv.w):
            c = cv.px[y * cv.w + x]
            r.append("." if c is None else letter["SHADOW" if c == "SHADOW" else remap[c]])
        rows.append("".join(r))
    return {"pal": pal, "rows": rows}


def to_rgba(e):
    """an entry back to rows of (r, g, b, a) for previews"""
    pal = {}
    for k, v in e["pal"].items():
        v = v.lstrip("#")
        pal[k] = (int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16), int(v[6:8], 16) if len(v) == 8 else 255)
    return [[(0, 0, 0, 0) if ch == "." else pal[ch] for ch in row] for row in e["rows"]]
