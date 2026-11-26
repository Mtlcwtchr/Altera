# LR-167: 3/4-view primitives: a box shows its lit top and its south face, a cylinder its
# top ellipse and a body shaded across; every part goes through Cv.paint's three tones.
from paint import Cv, cap, ell, poly, rect, sh, rgb, OR, AND, SUB


def box(cv, x0, y0, w, d, h, top, front=None, fn_top=None, fn_front=None, rim=2):
    """top face from (x0, y0) w x d, then the south face h tall under it"""
    front = front or sh(rgb(top), 0.8)
    cv.paint(rect(x0, y0 + d, x0 + w, y0 + d + h), front, fn=fn_front, rim=1)
    cv.paint(rect(x0, y0, x0 + w, y0 + d), top, fn=fn_top, under=False, soft=True)


def cyl(cv, cx, ytop, rx, ry, h, col, fn=None, top=None):
    """an upright cylinder: body from ytop down h, its top ellipse"""
    body = OR(rect(cx - rx, ytop, cx + rx, ytop + h), ell(cx, ytop + h, rx, ry))

    def shade(x, y, c):
        t = (x - (cx - rx)) / (2 * rx)
        k = 1.18 if t < 0.25 else (1.0 if t < 0.62 else (0.82 if t < 0.85 else 0.66))
        r = fn(x, y, c) if fn else None
        return sh(rgb(r) if r is not None else c, k)
    cv.paint(body, col, fn=shade, rim=1)
    cv.paint(ell(cx, ytop, rx, ry), top or sh(rgb(col), 1.12), under=False, soft=True)


def legs(cv, xs, ytop, ybot, col, w=2):
    for x in xs:
        cv.paint(rect(x, ytop, x + w, ybot), col, rim=1)
