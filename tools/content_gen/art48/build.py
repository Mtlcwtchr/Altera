# LR-167: modular building pieces by build style (the culture's build_style, LR-166):
# walls on the cell lines seen from above (a 12 px strip across a 48 px piece, lit on the
# top-left edge, a cast shadow to the south / east), doors, windows, a corner post, plus
# the floor and roof tiles of the style (tiles.py).
#   wl_<style>_h / _v            a wall 1 m long along x / along y
#   wl_<style>_door_h / _v       a doorway with its leaf
#   wl_<style>_window_h / _v     a window
#   wl_<style>_post              a corner post / column seen from above
import math
import random

from paint import Cv, Noise, sh, rgb, mix, ell, cap

N = 48
TH = 12            # wall thickness in art pixels (25 cm)
A = N // 2 - TH // 2   # the strip's first row


def _logs(u, v, seed):
    """horizontal logs: rounded across, bark grain along"""
    base = rgb("#7a5232")
    k = v / TH
    c = sh(base, 1.25 - 0.55 * k)   # a round log seen from above: lit edge to shaded edge
    if (u * 3 + seed) % 11 == 0:
        c = sh(c, 0.82)
    return c


STYLES = {
    # material colours: a base for the wall body, a joint colour, a trim, the door wood,
    # a pattern of blocks (bw x bh along x across thickness), a band (paint) or none
    "hellenic":  dict(base="#e8e0cc", joint="#b8ac90", bw=16, trim="#a83a24", door="#6a4428", stud="#c8963a", win="#2a2a3a", post="column", post_col="#ece6d6"),
    "labyrinth": dict(base="#d8c8a0", joint="#9a8a68", bw=24, trim="#b02c20", door="#4a2a18", stud="#e0b030", win="#1a1a28", post="minoan", post_col="#b02c20"),
    "cyclopean": dict(base="#8a8478", joint="#4a463e", bw=0, poly=True, trim=None, door="#5a3c24", stud="#8a8a8a", win="#1a1a1a", post="boulder", post_col="#8a8478"),
    "grove":     dict(base="#6a8a3a", joint="#4a6a2a", bw=0, hedge=True, trim="#e090b0", door="#7a5a3a", stud=None, win="#2a3a2a", post="tree", post_col="#6a5038"),
    "nilotic":   dict(base="#d8c090", joint="#a88a58", bw=12, trim="#2a6aa8", trim2="#c8a030", door="#7a5430", stud=None, win="#2a2018", post="palm_col", post_col="#d8c8a0"),
    "sylvan":    dict(base="#8a7a5a", joint="#5a4a34", bw=0, living=True, trim="#c8d8e0", door="#6a5a3a", stud="#d8e0e8", win="#2a3a4a", post="tree", post_col="#7a6a50"),
    "celtic":    dict(base="#c8b48a", joint="#8a7450", bw=0, daub=True, trim="#7a5030", door="#6a4428", stud=None, win="#2a2018", post="post", post_col="#6a4a30"),
    "slavic":    dict(base="#7a5232", joint="#4a3018", bw=0, logs=True, trim="#c83a2a", door="#6a4428", stud=None, win="#2a2a30", post="log_end", post_col="#8a6040"),
    "norse":     dict(base="#6a4a30", joint="#3a2818", bw=0, stave=True, trim="#3a6a8a", door="#5a3a22", stud="#8a8a8a", win="#1a1a20", post="carved", post_col="#5a3a22"),
    "dwarven":   dict(base="#7a7a80", joint="#3a3a40", bw=12, trim="#d8a830", door="#4a3a2a", stud="#d8a830", win="#e08030", post="pillar", post_col="#6a6a72"),
    "cave":      dict(base="#6a645a", joint="#3a362e", bw=0, rough=True, trim=None, door="#5a4030", stud=None, win="#100c08", post="boulder", post_col="#6a645a"),
    "steppe":    dict(base="#e8e0c8", joint="#a89878", bw=0, felt=True, trim="#a8322a", door="#c8402a", stud="#e0b030", win="#2a2018", post="lattice", post_col="#a87a4a"),
    "lair":      dict(base="#5a524a", joint="#2a2620", bw=0, rough=True, bones=True, trim=None, door="#3a2a1a", stud=None, win="#100c08", post="boulder", post_col="#5a524a"),
}


def wall_colour(st, u, v, seed):
    """the wall's top at (u along the wall, v across its thickness 0..TH-1)"""
    S = STYLES[st]
    b = rgb(S["base"])
    j = rgb(S["joint"])
    k = v / (TH - 1)
    lit = 1.18 - 0.36 * k   # the top-left edge lit, the far edge darker
    rnd_h = ((u // 7) * 2654435761 + seed) & 0xffff
    if S.get("logs"):
        return _logs(u, v, seed)
    if S.get("stave"):   # vertical staves seen from above: a row of plank tops
        c = sh(b, lit)
        if u % 6 == 0:
            c = sh(j, 1.0)
        elif u % 6 == 1:
            c = sh(c, 1.15)
        return c
    if S.get("felt"):   # felt over the wooden lattice: a soft band, ropes across
        c = sh(b, 1.06 - 0.18 * k)
        if u % 16 in (0, 1):
            c = rgb("#8a6a4a") if u % 16 == 0 else rgb("#6a4a2a")
        if v in (2, TH - 3) and u % 4 < 2:
            c = rgb(S["trim"])
        return c
    if S.get("daub"):   # wattle and daub: rough plaster with wattle showing through
        n = (u * 7 + v * 13 + seed) % 17
        c = sh(b, lit * (0.94 + 0.06 * (n % 3)))
        if n == 0:
            c = rgb("#8a6a40")
        return c
    if S.get("hedge") or S.get("living"):   # woven living wood: curved withies
        w = math.sin(u * 0.5 + v * 0.9 + (u // 8) * 1.7)
        c = sh(b, lit * (1.0 + 0.12 * w))
        if S.get("hedge") and (u * 5 + v * 3) % 13 == 0:
            c = rgb("#8aba4a")
        if S.get("hedge") and (u * 11 + v * 7) % 41 == 0:
            c = rgb(S["trim"])   # a flower
        if S.get("living") and (u * 7 + v) % 23 == 0:
            c = rgb("#d8e0c8")   # silver bark marks
        return c
    if S.get("rough") or S.get("poly"):   # boulders / rough rock: irregular cells
        cell = 9 if S.get("poly") else 6
        cx = (u + (v // cell) * 4) // cell
        tint = sh(b, 0.9 + 0.2 * (((cx * 37 + (v // cell) * 11 + seed) % 7) / 7))
        lu = (u + (v // cell) * 4) % cell
        if lu == 0:
            return j
        if lu == 1:
            return sh(tint, 1.15 * lit)
        if S.get("bones") and (u * 13 + v * 5) % 47 == 0:
            return rgb("#e8e0c8")
        return sh(tint, lit)
    # blocks: ashlar, mudbrick, dwarven granite
    bw = S["bw"]
    row = v // 6
    off = (row % 2) * (bw // 2)
    lu, lv = (u + off) % bw, v % 6
    tint = sh(b, 0.94 + 0.1 * ((((u + off) // bw) * 7 + row * 3 + seed) % 5) / 5)
    if lu == 0 or lv == 0 and v > 0:
        return j
    c = sh(tint, lit)
    if lu == 1:
        c = sh(c, 1.08)
    if st == "dwarven" and lu == bw // 2 and lv == 3:
        c = rgb(S["trim"])   # a gold rune inlay
    return c


FH = 12   # the height of a wall's south face shown under its top (3/4 view, walls along x)


def face_colour(st, u, w, seed):
    """the wall's south face at (u along, w down from the top 0..FH-1): the material side-on"""
    S = STYLES[st]
    b = sh(rgb(S["base"]), 0.86)
    j = sh(rgb(S["joint"]), 0.8)
    if S.get("logs"):   # stacked logs: round, lit on top, dark seam under each
        k = w % 6
        c = sh(rgb(S["base"]), (1.2, 1.05, 0.95, 0.85, 0.72, 0.5)[k])
        return c
    if S.get("stave"):
        c = sh(b, 1.0 if u % 6 not in (0, 5) else 0.7)
        if u % 6 == 1:
            c = sh(b, 1.15)
        if w in (FH - 3,) and u % 24 < 18:
            c = rgb(S["trim"])
        return c
    if S.get("felt"):   # white felt, the lattice (khana) peeking as a red-brown criss-cross at the hem
        c = sh(rgb(S["base"]), 0.95 - 0.02 * w)
        if w in (1, 2):
            c = rgb(S["trim"]) if (u // 3) % 2 else rgb("#e0b030")
        if w >= FH - 4 and ((u + w) % 6 == 0 or (u - w) % 6 == 0):
            c = rgb("#8a5a3a")
        return c
    if S.get("daub"):   # timber frame with daub panels
        if u % 24 in (0, 1) or w in (0, FH - 1) or (u % 24 - w * 2) in (2, 3):
            return rgb("#6a4428") if u % 24 != 1 else rgb("#8a6040")
        return sh(b, 1.0 - 0.01 * ((u * 7 + w * 3) % 5))
    if S.get("hedge") or S.get("living"):
        v = math.sin(u * 0.7 + w * 1.3) + math.sin(u * 0.3 - w * 0.9)
        c = sh(b, 0.85 + 0.12 * v)
        if S.get("hedge") and (u * 7 + w * 5) % 29 == 0:
            c = rgb(S["trim"])
        return c
    if S.get("rough") or S.get("poly"):
        cell = 8 if S.get("poly") else 5
        cx = (u + (w // cell) * 3) // cell
        tint = sh(b, 0.88 + 0.2 * (((cx * 37 + (w // cell) * 11 + seed) % 7) / 7))
        lu, lw = (u + (w // cell) * 3) % cell, w % cell
        if lu == 0 or lw == cell - 1:
            return j
        if lw == 0:
            return sh(tint, 1.14)
        if S.get("bones") and (u * 13 + w * 5) % 37 == 0:
            return rgb("#e8e0c8")
        return tint
    bw = S["bw"]
    bh = 6 if st != "labyrinth" else 12
    row = w // bh
    off = (row % 2) * (bw // 2)
    lu, lw = (u + off) % bw, w % bh
    tint = sh(b, 0.92 + 0.12 * ((((u + off) // bw) * 7 + row * 3 + seed) % 5) / 5)
    if lu == 0 or lw == bh - 1:
        return j
    if lw == 0:
        return sh(tint, 1.12)
    if st == "nilotic" and row == 0:
        return rgb(S["trim"]) if lw < 3 else rgb(S["trim2"])
    if st == "hellenic" and row == 0 and lw in (1, 2):   # a painted frieze of the meander
        return rgb(S["trim"]) if (u % 8) in (0, 1, 2, 3, 4) else tint
    if st == "labyrinth" and row == 0 and 3 <= lw <= 6:
        return rgb(S["trim"])
    if st == "dwarven" and lu == bw // 2 and lw == 2:
        return rgb(S["trim"])
    return tint


def wall(st, axis="h", kind="wall", seed=0):
    S = STYLES[st]
    cv = Cv(N)
    for u in range(N):
        for v in range(TH):
            c = wall_colour(st, u, v, seed)
            x, y = (u, A + v) if axis == "h" else (A + v, u)
            cv.set(x, y, c)
    # the lit top edge and a dark outer edge
    for u in range(N):
        if axis == "h":
            cv.set(u, A, sh(cv.get(u, A), 1.12))
            cv.set(u, A + TH - 1, sh(cv.get(u, A + TH - 1), 0.62))
        else:
            cv.set(A, u, sh(cv.get(A, u), 1.12))
            cv.set(A + TH - 1, u, sh(cv.get(A + TH - 1, u), 0.62))
    # a painted band along the wall (Minoan red, Nile blue and gold, the Greek meander)
    if S.get("trim") and st in ("hellenic", "labyrinth", "nilotic", "slavic", "norse"):
        for u in range(N):
            v = TH // 2
            on = (st != "hellenic") or (u % 8 < 5)
            if st == "slavic":
                on = (u % 12) in (5, 6)   # carved red dots on the log tops
            if st == "norse":
                on = (u % 24) in (11, 12)
            if on:
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                cv.set(x, y, S["trim"])
                if st == "nilotic" and S.get("trim2"):
                    x2, y2 = (u, A + v + 1) if axis == "h" else (A + v + 1, u)
                    cv.set(x2, y2, S["trim2"])
    if kind == "door":
        gap0, gap1 = 10, 38
        for u in range(gap0, gap1):
            for v in range(TH):
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                cv.set(x, y, None)
        # jambs: darker stone/wood ends
        for u in (gap0 - 2, gap0 - 1, gap1, gap1 + 1):
            for v in range(TH):
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                cv.set(x, y, sh(rgb(S["base"]), 0.7 if u in (gap0 - 1, gap1) else 0.9))
        # the leaf, half open: a plank door swung in, seen from above (a thick line)
        d = rgb(S["door"])
        leaf = (gap1 - gap0 - 2) if axis == "v" else 0   # along x the face shows the door
        for i in range(leaf):
            for t in range(4):
                if axis == "h":
                    x, y = gap0 + 1 + int(i * 0.8), A + TH - 2 - t + int(i * 0.55)
                else:
                    x, y = A + TH - 2 - t + int(i * 0.55), gap0 + 1 + int(i * 0.8)
                cv.set(x, y, sh(d, 1.2 if t == 3 else (0.7 if t == 0 else 1.0)))
        if S.get("stud") and axis == "v":
            for i in range(2, gap1 - gap0 - 4, 6):
                x, y = (gap0 + 1 + int(i * 0.8), A + TH - 4 + int(i * 0.55)) if axis == "h" else (A + TH - 4 + int(i * 0.55), gap0 + 1 + int(i * 0.8))
                cv.set(x, y, S["stud"])
        # the threshold
        for u in range(gap0, gap1):
            for v in (TH // 2 - 1, TH // 2):
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                cv.set(x, y, sh(rgb(S["joint"]), 1.1))
    if kind == "window" and axis == "v":
        w0, w1 = 14, 34
        for u in range(w0, w1):
            for v in range(3, TH - 3):
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                c = rgb(S["win"])
                if v == 3:
                    c = sh(c, 0.6)
                if (u - w0) % 7 == 0:
                    c = rgb(S["door"])   # mullions / shutters
                if st == "dwarven":
                    c = mix(rgb("#e08030"), rgb("#f8d060"), (v - 3) / 6)   # a forge glow behind the grille
                cv.set(x, y, c)
        for u in (w0 - 1, w1):
            for v in range(2, TH - 2):
                x, y = (u, A + v) if axis == "h" else (A + v, u)
                cv.set(x, y, S.get("trim") or S["joint"])
    if axis == "h":   # the south face, 3/4 view
        for u in range(N):
            for w in range(FH):
                c = face_colour(st, u, w, seed)
                if w == FH - 1:
                    c = sh(c, 0.7)
                if kind == "door" and 10 <= u < 38:
                    c = None
                    if w >= 0:
                        # the open doorway: the door leaf on the left, dark inside
                        if u < 16:
                            c = sh(rgb(S["door"]), 1.1 if u in (10,) else 0.95)
                            if S.get("stud") and w % 4 == 2 and u in (12, 14):
                                c = rgb(S["stud"])
                        else:
                            c = rgb("#1a1410") if w < FH - 2 else rgb("#3a2a1e")
                if kind == "window" and 16 <= u < 32 and 2 <= w < FH - 3:
                    c = rgb(S["win"]) if (u - 16) % 8 != 0 else rgb(S["door"])
                    if st == "dwarven":
                        c = mix(rgb("#f8d060"), rgb("#e06020"), (w - 2) / 7)
                    if w == 2:
                        c = sh(c, 0.6)
                if kind == "window" and (u in (15, 32) and 1 <= w < FH - 2 or (16 <= u < 32 and w == FH - 3)):
                    c = sh(rgb(S.get("trim") or S["joint"]), 1.0)
                if c is not None:
                    cv.set(u, A + TH + w, c)
    # the cast shadow (south for a wall along x, east for one along y)
    for u in range(N):
        for s in range(5):
            x, y = (u, A + TH + (FH if axis == "h" else 0) + s) if axis == "h" else (A + TH + s, u)
            if cv.get(x, y) is None and (kind != "door" or True):
                if axis == "h":
                    src = cv.get(u, A + TH + FH - 1) or cv.get(u, A + TH - 1)
                else:
                    src = cv.get(A + TH - 1, u)
                if src is not None:
                    cv.set(x, y, "SHADOW")
    return cv


def post(st):
    S = STYLES[st]
    cv = Cv(N)
    c = N / 2
    p = S["post"]
    col = S["post_col"]
    cv.shadow(ell(c + 4, c + 4, 11, 11))
    if p == "column":   # a fluted column seen from above: a disc with flutes, the abacus square
        cv.paint(lambda x, y: abs(x - c) < 10 and abs(y - c) < 10, sh(rgb(col), 0.95))
        cv.paint(ell(c, c, 8.5, 8.5), col, fn=lambda x, y, k: sh(k, 0.86) if int(math.degrees(math.atan2(y - c, x - c)) / 20) % 2 else None)
    elif p == "minoan":   # a red tapered column, black capital
        cv.paint(ell(c, c, 9.5, 9.5), "#2a2020")
        cv.paint(ell(c, c, 7, 7), col)
    elif p == "palm_col":   # a papyrus-bundle column: a ring of stems
        cv.paint(ell(c, c, 10, 10), col, fn=lambda x, y, k: sh(k, 0.8) if int(math.degrees(math.atan2(y - c, x - c)) / 30) % 2 else None)
        cv.paint(ell(c, c, 4, 4), "#2a6aa8")
    elif p in ("tree",):
        cv.paint(ell(c, c, 9, 9), col, fn=lambda x, y, k: sh(k, 0.85) if (int(math.hypot(x - c, y - c)) % 3 == 0) else None)
        for a in range(0, 360, 60):
            r = math.radians(a)
            cv.paint(cap(c + math.cos(r) * 8, c + math.sin(r) * 8, c + math.cos(r) * 13, c + math.sin(r) * 13, 2, 1), "#5a8a34" if st == "grove" else "#7a9a5a", rim=1)
    elif p == "log_end":   # crossed log ends at a corner: rings of the cut
        cv.paint(lambda x, y: abs(x - c) < 9 and abs(y - c) < 9, col, fn=lambda x, y, k: sh(k, 0.8) if int(math.hypot(x - c, y - c)) % 3 == 0 else None)
    elif p == "carved":   # a carved dragon-head post top
        cv.paint(ell(c, c, 8, 8), col)
        cv.paint(cap(c - 4, c - 2, c + 6, c - 7, 2.2, 1.2), "#3a6a8a", rim=1)
        cv.dot(c + 4, c - 7, "#e0c040")
    elif p == "pillar":
        cv.paint(lambda x, y: abs(x - c) < 10 and abs(y - c) < 10, col)
        cv.paint(lambda x, y: abs(x - c) < 4 and abs(y - c) < 4, "#d8a830", flat=True)
    elif p == "lattice":   # the yurt's door frame post, painted
        cv.paint(lambda x, y: abs(x - c) < 7 and abs(y - c) < 7, col)
        cv.paint(lambda x, y: abs(x - c) < 2 and abs(y - c) < 6, "#a8322a", flat=True)
    elif p == "post":
        cv.paint(ell(c, c, 7, 7), col, fn=lambda x, y, k: sh(k, 0.8) if int(math.hypot(x - c, y - c)) % 2 == 0 else None)
    else:   # a boulder
        cv.paint(ell(c, c, 11, 10), col, fn=lambda x, y, k: sh(k, 0.85) if (int(x) * 3 + int(y)) % 7 == 0 else None)
    return cv.finish()


def all_pieces():
    out = {}
    for st in STYLES:
        for ax in ("h", "v"):
            out["wl_%s_%s" % (st, ax)] = wall(st, ax)
            out["wl_%s_door_%s" % (st, ax)] = wall(st, ax, "door")
            out["wl_%s_window_%s" % (st, ax)] = wall(st, ax, "window")
        out["wl_%s_post" % st] = post(st)
    return out
