# LR-167: the relief's cliff pieces at 48 px (same names and meaning as LR-156's 16 px set):
#   n   the face of the higher ground to the north (seen from the south: the tall one)
#   s   the lip of the higher ground to the south;  e / w  the side faces
#   c_ne c_nw c_se c_sw  corners;  ramp_n/s/e/w  steps where a road climbs
# Straight pieces wrap along their length so a long cliff has no seams.
import math
import random

from paint import Cv, Noise, sh, rgb, mix

N = 48
STYLES = {
    "stone":     dict(lip="#5f9a3c", hi="#b8b8b0", mid="#8a8a84", dark="#5a5a58", crack="#3a3a38", spot=None, strata=False),
    "mossy":     dict(lip="#4a7a2c", hi="#9a9a8a", mid="#6e7068", dark="#4a4c46", crack="#2a2c28", spot=("#4f8a34", "#3a6a28"), strata=False),
    "earth":     dict(lip="#4f6a3a", hi="#a8845a", mid="#8a6a44", dark="#5a4430", crack="#3a2a1c", spot=("#6a4a2a", "#4a6a2a"), strata=True),
    "sandstone": dict(lip="#c8a85a", hi="#e0b87a", mid="#c08a4a", dark="#8a5a30", crack="#5a3a20", spot=None, strata=True),
    "snow":      dict(lip="#eef2f6", hi="#b8bcc4", mid="#8a8e98", dark="#5a5e68", crack="#3a3e48", spot=("#f0f4f8", "#d8e0e8"), strata=False),
    "limestone": dict(lip="#8a9a48", hi="#ece4cc", mid="#cfc4a4", dark="#9a8e72", crack="#6a6050", spot=("#7a8a40", "#5a6a30"), strata=True),
    "basalt":    dict(lip="#7a8a5a", hi="#6a6a72", mid="#46464e", dark="#2a2a32", crack="#18181e", spot=("#8a9a6a", "#c8c8c0"), strata=False),
}


def wander(seed, base, lo, hi, n=N, step=0.35):
    """a ragged line that wraps (its end meets its start)"""
    rnd = random.Random(seed)
    nz = Noise(seed, n, 8)
    return [max(lo, min(hi, int(round(base + (nz(x, 0) - 0.5) * (hi - lo) * 1.6)))) for x in range(n)]


def lumps(seed, n=N):
    """the face's boulders across the width (centre, half width), wrapping"""
    rnd = random.Random(seed)
    out, x = [], 0
    while x < n:
        w = rnd.choice((4, 5, 6, 7, 8))
        out.append((x + w, w))
        x += 2 * w - rnd.randint(0, 2)
    return out


def face_px(st, seed, top, foot, width=N):
    """the rock face between top[x] and foot[x]: lumps lit from the top left, strata, cracks"""
    cv = Cv(width, N, wrap=False)
    L = lumps(seed, width)
    nz = Noise(seed + 3, width, 4)
    for x in range(width):
        # the nearest lump, wrapping
        best = min(L, key=lambda l: min(abs(l[0] - x), width - abs(l[0] - x)))
        c, w = best
        d = (x - c) / max(1, w)
        if d > 1.5:
            d -= width / max(1, w)
        for y in range(top[x], foot[x] + 1):
            depth = (y - top[x]) / max(1, foot[x] - top[x])
            if st["strata"]:
                band = ((y + (x // 9) % 3) // 3) % 3
                col = st["hi"] if band == 0 and depth < 0.75 else st["mid"]
                if d > 0.6:
                    col = st["dark"]
                if band == 2 and (x + y) % 5 == 0:
                    col = sh(rgb(st["mid"]), 0.86)
            else:
                if d < -0.4 and depth < 0.8:
                    col = st["hi"]
                elif d > 0.5 or depth > 0.86:
                    col = st["dark"]
                else:
                    col = st["mid"]
                if nz(x, y) > 0.78:
                    col = sh(rgb(col), 1.1)
            cv.set(x, y, col)
        if abs(d) > 0.92:   # the seam between two lumps
            for y in range(top[x] + 2, foot[x] - 1):
                cv.set(x, y, st["crack"])
    rnd = random.Random(seed + 9)
    for _ in range(5):   # cracks running down
        x, y = rnd.randrange(width), rnd.randint(10, 20)
        for _k in range(rnd.randint(4, 9)):
            if top[x % width] < y < foot[x % width]:
                cv.set(x % width, y, st["crack"])
            y += 1
            x += rnd.choice((-1, 0, 0, 1))
    if st["spot"]:   # moss, roots, snow hanging from the lip
        for _ in range(9):
            x = rnd.randrange(width)
            c = rnd.choice(st["spot"])
            for k in range(rnd.randint(2, 9)):
                y = top[x % width] + k
                if y < foot[x % width]:
                    cv.set(x % width, y, c)
                    cv.set((x + 1) % width, y, c)
                x += rnd.choice((-1, 0, 0, 1))
    return cv


def piece_n(st, seed):
    top = wander(seed, 8, 5, 11)
    foot = wander(seed + 1, 36, 33, 39)
    cv = face_px(st, seed, top, foot)
    lip = rgb(st["lip"])
    for x in range(N):
        for y in range(0, top[x] - 1):
            cv.set(x, y, sh(lip, 1.15) if y < 2 else (lip if y < top[x] - 3 else sh(lip, 0.8)))
        cv.set(x, top[x] - 1, sh(lip, 0.4))
        cv.set(x, foot[x] + 1, sh(rgb(st["dark"]), 0.45))
        for s in range(2, 7):   # the cliff's shadow on the ground below
            if foot[x] + s < N:
                cv.set(x, foot[x] + s, "SHADOW")
    rnd = random.Random(seed + 4)
    for _ in range(4):   # scree at the foot
        x = rnd.randrange(2, N - 3)
        y = min(N - 2, foot[x] + 2)
        cv.set(x, y, st["mid"]); cv.set(x + 1, y, st["dark"]); cv.set(x, y - 1, st["hi"])
    return cv


def piece_s(st, seed):
    cv = Cv(N)
    lip = rgb(st["lip"])
    edge = wander(seed, 40, 37, 43)
    for x in range(N):
        cv.set(x, edge[x] - 1, "SHADOW")
        cv.set(x, edge[x], sh(lip, 0.35))
        cv.set(x, edge[x] + 1, sh(lip, 0.7))
        for y in range(edge[x] + 2, N):
            cv.set(x, y, sh(lip, 1.12) if y == edge[x] + 2 else lip)
    return cv


def piece_e(st, seed):
    cv = Cv(N)
    edge = wander(seed, 36, 33, 39)
    lip = rgb(st["lip"])
    nz = Noise(seed, N, 4)
    for y in range(N):
        e = edge[y]
        cv.set(e - 1, y, "SHADOW")
        cv.set(e - 2, y, "SHADOW" if y % 2 else None)
        cv.set(e, y, sh(rgb(st["dark"]), 0.45))
        for x in range(e + 1, N - 3):
            k = (x - e) / max(1, N - 3 - e)
            col = st["dark"] if k < 0.3 else st["mid"]
            if nz(x, y) > 0.72:
                col = st["hi"]
            if st["spot"] and nz(x * 2, y) < 0.12:
                col = st["spot"][0]
            cv.set(x, y, col)
        for x in range(N - 3, N):
            cv.set(x, y, sh(lip, 0.75) if x == N - 3 else lip)
    return cv


def flip(cv):
    return cv.flipx()


def corner(full, keep):
    cv = Cv(N)
    for y in range(N):
        for x in range(N):
            if keep(x):
                cv.set(x, y, full.get(x, y))
    return cv


def ramp_n(st, seed):
    cv = Cv(N)
    tread, riser = rgb(st["hi"]), rgb(st["mid"])
    for y in range(0, 42):
        for x in range(N):
            if x <= 5 or x >= 42:   # the rock beside the steps
                cv.set(x, y, st["dark"] if x in (5, 42) else st["mid"])
                if y < 6:
                    cv.set(x, y, st["lip"])
                continue
            k = (y - 2) % 8
            cv.set(x, y, sh(tread, 0.4) if k == 0 else (sh(tread, 1.1) if k == 1 else (tread if k < 4 else sh(riser, 0.95 - 0.04 * (k - 4)))))
    for x in range(N):
        for s in range(42, 46):
            cv.set(x, s, "SHADOW" if (x + s) % 2 == 0 else None)
    return cv


def ramp_s(st, seed):
    cv = Cv(N)
    for y in (36, 42):
        for x in range(6, 42):
            cv.set(x, y, sh(rgb(st["dark"]), 0.5))
            cv.set(x, y + 1, st["hi"])
            cv.set(x, y + 2, st["mid"])
    return cv


def ramp_e(st, seed):
    cv = Cv(N)
    for x in (30, 38):
        for y in range(N):
            cv.set(x, y, sh(rgb(st["dark"]), 0.5))
            cv.set(x + 1, y, st["hi"])
            cv.set(x + 2, y, st["mid"])
            cv.set(x + 3, y, st["mid"])
    return cv


def bank(d, seed):
    """a soft step on gentle ground: a dithered shade under the rise, no rock"""
    cv = Cv(N)
    rnd = random.Random(seed)
    dens = [0.85, 0.65, 0.45, 0.3, 0.18, 0.08]
    for k, p in enumerate(dens):
        for t in range(N):
            if rnd.random() < p:
                x, y = {"n": (t, k), "s": (t, N - 1 - k), "e": (N - 1 - k, t), "w": (k, t)}[d]
                cv.set(x, y, "SHADOW")
    return cv


def all_cliffs(variants=2):
    S = {}
    for v in range(variants):
        for d in "nsew":
            S["cf_bank_%s_%d" % (d, v)] = bank(d, 100 + v * 7 + ord(d))
    for cn, (a, b) in {"c_ne": ("n", "e"), "c_nw": ("n", "w"), "c_se": ("s", "e"), "c_sw": ("s", "w")}.items():
        cv = Cv(N)
        rnd = random.Random(cn)
        for y in range(10):
            for x in range(10):
                if rnd.random() < 0.7 - 0.07 * (x + y):
                    cv.set(N - 1 - x if b == "e" else x, y if a == "n" else N - 1 - y, "SHADOW")
        S["cf_bank_" + cn] = cv
    for d in "nsew":
        S["cf_bank_ramp_" + d] = S["cf_bank_%s_0" % d]
    for si, (name, st) in enumerate(STYLES.items()):
        base = 1000 * (si + 1)
        for v in range(variants):
            n = piece_n(st, base + v * 17)
            S["cf_%s_n_%d" % (name, v)] = n
            S["cf_%s_s_%d" % (name, v)] = piece_s(st, base + v * 17 + 5)
            e = piece_e(st, base + v * 17 + 9)
            S["cf_%s_e_%d" % (name, v)] = e
            S["cf_%s_w_%d" % (name, v)] = flip(e)
            if v == 0:
                S["cf_%s_c_ne" % name] = corner(n, lambda x: x >= 36)
                S["cf_%s_c_nw" % name] = corner(n, lambda x: x <= 11)
                s = piece_s(st, base + 77)
                S["cf_%s_c_se" % name] = corner(s, lambda x: x >= 36)
                S["cf_%s_c_sw" % name] = corner(s, lambda x: x <= 11)
        S["cf_%s_ramp_n" % name] = ramp_n(st, base)
        S["cf_%s_ramp_s" % name] = ramp_s(st, base)
        re = ramp_e(st, base)
        S["cf_%s_ramp_e" % name] = re
        S["cf_%s_ramp_w" % name] = flip(re)
    return S
