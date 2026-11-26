# LR-167: mounts and vehicles seen from the side facing east (mirrored for the west), at the
# world's scale (48 px = 1 m): a horse is ~2.3 m long. Two frames: _1 the other step / turn
# of the wheels. META gives the row of the feet and how high a rider / driver sits.
import math

from paint import Cv, cap, ell, poly, rect, sh, rgb, OR, AND, SUB, mix
from solid import box, cyl

WOOD, WOOD_D, IRON, GOLDC = "#8a5a34", "#5a3a22", "#5a5e64", "#e0b030"


def quadruped(size, body, legs_col=None, frame=0, L=1.0, H=1.0, neck=True, head="horse", mane=None, tail=None,
              hoof="#2a2018", horns=None, belly=None, saddle=None, fur=None, low=False):
    """a beast side-on: the barrel, four legs (the far ones darker), neck and head"""
    cv = Cv(size)
    c = size / 2
    feet = size - 6
    bl = size * 0.27 * L                                    # half the body length
    by = feet - size * 0.33 * H                             # the body's centre height
    bh = size * 0.145 * H
    cv.shadow(ell(c, feet + 2, bl * 1.3, 3))
    b = rgb(body)
    lc = rgb(legs_col or body)
    swing = (1 if frame else -1) * size * 0.035
    for lx, s in ((c - bl * 0.7, -swing), (c + bl * 0.65, swing)):   # the far legs, darker
        cv.paint(OR(cap(lx + 2, by, lx + 2 + s, by + (feet - by) * 0.55, size * 0.04, size * 0.03),
                    cap(lx + 2 + s, by + (feet - by) * 0.55, lx + 2 + s * 0.6, feet, size * 0.028, size * 0.022)), sh(lc, 0.72), rim=1)
    if tail:
        cv.paint(cap(c - bl, by - bh * 0.6, c - bl - size * 0.07, by + bh * 1.6, size * 0.03, size * 0.04), tail, rim=1, soft=True)
    cv.paint(ell(c, by, bl, bh), body, fn=fur)
    if belly:
        cv.paint(AND(ell(c, by, bl, bh), lambda x, y: y > by + bh * 0.45), belly, flat=True)
    if neck:
        hx, hy = c + bl * 1.05, by - size * (0.2 if not low else 0.04) * H
        cv.paint(cap(c + bl * 0.6, by - bh * 0.2, hx, hy, size * 0.085, size * 0.055), body, fn=fur)
        if head == "horse":
            cv.paint(cap(hx, hy, hx + size * 0.12, hy + size * 0.08, size * 0.05, size * 0.032), body)
            cv.paint(cap(hx - 1, hy - size * 0.03, hx - 1, hy - size * 0.065, size * 0.012, 0.5), body, rim=1)
            cv.dot(hx + 2, hy - 1, "#1a1410", 2, 1)
            cv.dot(hx + size * 0.1, hy + size * 0.06, sh(b, 0.5))
        elif head == "ox":
            cv.paint(ell(hx + size * 0.04, hy + size * 0.04, size * 0.06, size * 0.05), body)
            cv.paint(ell(hx + size * 0.08, hy + size * 0.07, size * 0.03, size * 0.025), sh(b, 0.75))
            cv.dot(hx + 3, hy + 1, "#1a1410", 2, 1)
        elif head == "wolf":
            cv.paint(OR(cap(hx, hy, hx + size * 0.12, hy + size * 0.04, size * 0.04, size * 0.016),
                        poly([(hx - 4, hy - 2), (hx - 1, hy - size * 0.08), (hx + 3, hy - 2)])), body)
            cv.dot(hx + 3, hy - 1, "#e0c040", 2, 1)
            cv.dot(hx + size * 0.12, hy + size * 0.035, "#1a1410")
        elif head == "lizard":
            cv.paint(cap(hx, hy, hx + size * 0.12, hy + size * 0.02, size * 0.04, size * 0.025), body, fn=fur)
            cv.dot(hx + 2, hy - 2, "#e0c040", 2, 1)
        elif head == "camel":
            cv.paint(cap(hx, hy, hx + size * 0.1, hy + size * 0.03, size * 0.035, size * 0.025), body)
            cv.dot(hx + 2, hy - 1, "#1a1410", 2, 1)
        elif head == "deer":
            cv.paint(cap(hx, hy, hx + size * 0.1, hy + size * 0.05, size * 0.032, size * 0.02), body)
            cv.dot(hx + 2, hy - 1, "#1a1410", 2, 1)
        if mane:
            cv.paint(cap(c + bl * 0.55, by - bh * 0.9, hx - 1, hy - size * 0.03, size * 0.025, size * 0.02), mane, rim=1, soft=True)
        if horns == "ox":
            cv.paint(cap(hx, hy - 2, hx - size * 0.05, hy - size * 0.08, size * 0.014, size * 0.006), "#e8dcc0", rim=1)
        if horns == "antlers":
            for k in range(3):
                cv.paint(cap(hx - 2 + k * 2, hy - 3, hx - 8 + k * 6, hy - size * 0.16, 1.2, 0.6), "#c8b090", rim=1)
                cv.paint(cap(hx - 8 + k * 6, hy - size * 0.12, hx - 12 + k * 6, hy - size * 0.16, 0.8, 0.5), "#c8b090", rim=1)
    if head == "camel":   # the hump, the saddle cloth over it
        cv.paint(ell(c - bl * 0.1, by - bh * 0.9, bl * 0.42, bh * 0.85), body)
    if saddle and head == "camel":
        cv.paint(AND(ell(c - bl * 0.1, by - bh * 0.9, bl * 0.44, bh * 0.87), lambda x, y: y > by - bh * 1.2), saddle, rim=1,
                 fn=lambda x, y, k: GOLDC if int(y) == int(by - bh * 0.2) else None)
        saddle = None
    if saddle:
        cv.paint(AND(ell(c + bl * 0.1, by - bh * 0.5, bl * 0.42, bh * 0.75), lambda x, y: y < by + bh * 0.3), saddle, rim=1)
        cv.paint(rect(c + bl * 0.05, by - bh * 0.1, c + bl * 0.12, by + bh * 1.6), "#3a2818", flat=True)
    for lx, s in ((c - bl * 0.75, swing), (c + bl * 0.6, -swing)):   # the near legs
        cv.paint(OR(cap(lx, by, lx + s, by + (feet - by) * 0.55, size * 0.043, size * 0.032),
                    cap(lx + s, by + (feet - by) * 0.55, lx + s * 0.6, feet - 1, size * 0.03, size * 0.024)), lc, rim=1)
        cv.paint(ell(lx + s * 0.6, feet, size * 0.032, size * 0.016), hoof, rim=1)
    return cv.finish()


def wheel(cv, cx, cy, r, frame, col=WOOD, spokes=6):
    cv.paint(ell(cx, cy, r, r), col)
    cv.paint(ell(cx, cy, r - 2.2, r - 2.2), "#2a2018", flat=True, under=False)
    for k in range(spokes):
        a = k * math.pi * 2 / spokes + (math.pi / spokes if frame else 0)
        cv.paint(cap(cx, cy, cx + math.cos(a) * (r - 2), cy + math.sin(a) * (r - 2), 0.9), col, flat=True, rim=1)
    cv.paint(ell(cx, cy, 2.2, 2.2), IRON, flat=True)


def cart(size, frame, kind="cart"):
    cv = Cv(size)
    c = size / 2
    feet = size - 6
    cv.shadow(ell(c, feet + 2, size * 0.42, 3))
    r = {"handcart": size * 0.12, "cart": size * 0.16, "wagon": size * 0.12, "coach": size * 0.13, "chariot": size * 0.15, "kibitka": size * 0.14}[kind]
    if kind == "handcart":
        box(cv, c - size * 0.22, feet - r - size * 0.18, size * 0.4, size * 0.05, size * 0.12, "#a87a4a", WOOD_D)
        cv.paint(cap(c + size * 0.18, feet - r - size * 0.08, c + size * 0.42, feet - r - size * 0.18, 1.4), WOOD, rim=1)
        wheel(cv, c - size * 0.02, feet - r, r, frame)
    elif kind == "chariot":   # the Middle Sea's light two-wheeled car with a bronze-faced front
        cv.paint(poly([(c - size * 0.2, feet - r), (c + size * 0.08, feet - r), (c + size * 0.12, feet - r - size * 0.22), (c - size * 0.02, feet - r - size * 0.2)]), "#b8873a",
                 fn=lambda x, y, k: sh(k, 0.85) if (int(x) + int(y)) % 4 == 0 else None)
        cv.paint(cap(c + size * 0.08, feet - r - 2, c + size * 0.46, feet - r - size * 0.12, 1.4), WOOD, rim=1)
        wheel(cv, c - size * 0.05, feet - r, r, frame, col="#7a5232", spokes=4)
    else:
        L = {"cart": 0.36, "wagon": 0.4, "coach": 0.38, "kibitka": 0.36}[kind]
        top = feet - r - size * 0.12
        if kind == "wagon":   # a canvas tilt over hoops
            cv.paint(SUB(ell(c - size * 0.04, top, size * L * 0.95, size * 0.2), rect(0, top + 1, size, size)), "#e8dcc0",
                     fn=lambda x, y, k: sh(k, 0.85) if int(x) % 10 == 0 else None)
        if kind == "kibitka":   # the steppe's felt-covered wagon house
            cv.paint(SUB(ell(c - size * 0.04, top, size * L * 0.85, size * 0.24), rect(0, top + 1, size, size)), "#e0d8c4",
                     fn=lambda x, y, k: "#a8322a" if int(y) == int(top - size * 0.08) else None)
        if kind == "coach":
            box(cv, c - size * 0.24, top - size * 0.22, size * 0.44, size * 0.04, size * 0.2, "#6a2a2a", "#4a1a1a",
                fn_front=lambda x, y, k: "#2a3040" if (abs(x - c) < size * 0.12 and y < top - 4) else None)
        box(cv, c - size * L, top, size * L * 1.8, size * 0.05, size * 0.1, "#a87a4a", WOOD_D,
            fn_front=lambda x, y, k: sh(k, 0.85) if int(x) % 8 == 0 else None)
        cv.paint(cap(c + size * L * 0.8, feet - r - 2, c + size * 0.48, feet - r - size * 0.1, 1.4), WOOD, rim=1)
        if kind == "cart":
            for i in range(3):
                cv.paint(ell(c - size * 0.18 + i * size * 0.14, top - 2, size * 0.06, size * 0.04), ("#c8a050", "#8a5a34", "#c8a050")[i])
        if kind in ("cart", "kibitka"):
            wheel(cv, c - size * 0.02, feet - r, r, frame)
        else:
            wheel(cv, c - size * 0.24, feet - r, r, frame)
            wheel(cv, c + size * 0.22, feet - r, r, frame)
    return cv.finish()


def sled(size, frame):
    cv = Cv(size)
    c = size / 2
    feet = size - 6
    cv.shadow(ell(c, feet + 1, size * 0.4, 2))
    cv.paint(OR(cap(c - size * 0.38, feet - 2, c + size * 0.3, feet - 2, 1.5), cap(c + size * 0.3, feet - 2, c + size * 0.4, feet - 10, 1.5)), WOOD_D, rim=1)
    for x in (-0.3, -0.1, 0.1, 0.25):
        cv.paint(rect(c + size * x, feet - 12, c + size * x + 2, feet - 2), WOOD_D, rim=1)
    box(cv, c - size * 0.34, feet - 18, size * 0.64, size * 0.04, 4, "#a87a4a", WOOD_D)
    cv.paint(ell(c - size * 0.12, feet - 22, size * 0.14, 5), "#9a7a5a")   # a bundle of pelts
    return cv.finish()


def boat(size, frame, kind="longship"):
    cv = Cv(size)
    c = size / 2
    wl = size - 14   # the water line
    if kind == "longship":
        cv.paint(cap(c, wl - size * 0.06, c, size * 0.36, 1.4), WOOD_D, rim=1)
        cv.paint(rect(c - size * 0.16, size * 0.4, c + size * 0.16, wl - size * 0.14), "#e8dcc0",
                 fn=lambda x, y, k: "#b02c20" if int((x - c + size) // 8) % 2 else None)   # the striped sail
        hull = poly([(c - size * 0.44, wl - size * 0.1), (c - size * 0.36, wl + 4), (c + size * 0.36, wl + 4), (c + size * 0.44, wl - size * 0.1),
                     (c + size * 0.3, wl - size * 0.05), (c - size * 0.3, wl - size * 0.05)])
        cv.paint(hull, "#6a4a30", fn=lambda x, y, k: sh(k, 0.78) if int(y) % 4 == 0 else None)
        cv.paint(cap(c + size * 0.42, wl - size * 0.1, c + size * 0.46, wl - size * 0.2, 2, 1.4), "#6a4a30", rim=1)   # the dragon prow
        cv.paint(ell(c + size * 0.47, wl - size * 0.21, 3, 2), "#6a4a30")
        cv.dot(c + size * 0.475, wl - size * 0.215, "#e0c040")
        for i in range(7):   # the shields along the side
            x = c - size * 0.27 + i * size * 0.09
            cv.paint(ell(x, wl - size * 0.055, 3.6, 3.6), ("#c83a2a", "#e0c040", "#3a6aa8")[i % 3],
                     fn=lambda px, py, k, x=x: "#3a3a3a" if math.hypot(px - x, py - wl + size * 0.055) < 1.2 else None)
        for i in range(6):   # oars
            x = c - size * 0.22 + i * size * 0.09 + (2 if frame else -2)
            cv.paint(cap(x, wl - 2, x - 4, wl + 6, 0.8), WOOD, rim=1, flat=True)
    elif kind == "reedboat":   # the Nile's papyrus boat, bound in bands, curled ends
        cv.paint(rect(c - size * 0.1, wl - size * 0.2, c + size * 0.1, wl - 4), "#e0d8c0", fn=lambda x, y, k: "#2a6aa8" if int(y) == int(wl - size * 0.2) + 1 else None)
        hull = OR(cap(c - size * 0.38, wl - size * 0.12, c - size * 0.25, wl, 3.5, 4.5), cap(c - size * 0.25, wl, c + size * 0.25, wl, 4.5),
                  cap(c + size * 0.25, wl, c + size * 0.38, wl - size * 0.14, 4.5, 3))
        cv.paint(hull, "#c8b060", fn=lambda x, y, k: "#7a5a2a" if int(x) % 7 == 0 else None)
        cv.paint(cap(c + size * 0.38, wl - size * 0.14, c + size * 0.33, wl - size * 0.2, 2.5, 1.5), "#c8b060", rim=1)
        cv.paint(cap(c - size * 0.3, wl - 3, c - size * 0.36, wl + 8, 0.8), WOOD, rim=1)
    elif kind == "galley":   # the Middle Sea's: black hull, the painted eye, a square sail
        cv.paint(cap(c, wl - size * 0.06, c, size * 0.36, 1.4), WOOD_D, rim=1)
        cv.paint(rect(c - size * 0.18, size * 0.3, c + size * 0.18, wl - size * 0.16), "#e8dcc0",
                 fn=lambda x, y, k: "#b02c20" if abs(math.hypot(x - c, y - size * 0.47) - 12) < 2.2 else ("#2a2a30" if math.hypot(x - c, y - size * 0.47) < 5 else None))
        hull = poly([(c - size * 0.42, wl - size * 0.14), (c - size * 0.34, wl + 4), (c + size * 0.4, wl + 4), (c + size * 0.46, wl + 2),
                     (c + size * 0.34, wl - size * 0.06), (c - size * 0.32, wl - size * 0.06)])
        cv.paint(hull, "#2a2a30", fn=lambda x, y, k: "#b02c20" if int(y) == int(wl - size * 0.04) else None)
        cv.paint(ell(c + size * 0.33, wl - 2, 3, 2), "#e8e0d0", flat=True)
        cv.dot(c + size * 0.335, wl - 2, "#2a2a30")
        cv.paint(cap(c - size * 0.42, wl - size * 0.14, c - size * 0.38, wl - size * 0.24, 1.6, 1), "#2a2a30", rim=1)
        for i in range(8):
            x = c - size * 0.25 + i * size * 0.07 + (2 if frame else -2)
            cv.paint(cap(x, wl - 2, x - 4, wl + 6, 0.8), WOOD, rim=1, flat=True)
    for x in range(size):   # the ripple at the hull
        if cv.get(x, wl + 4) not in (None, "SHADOW") or cv.get(x, wl + 3) not in (None, "SHADOW"):
            cv.set(x, wl + 6, "#7aa8d0" if (x + frame * 3) % 6 < 3 else "#4a7aa8")
    return cv.finish()


def all_transport():
    T, META = {}, {}

    def add(name, f, size, seat):
        for fr in (0, 1):
            T[name + ("_1" if fr else "")] = f(fr)
        META[name] = {"feet": size - 6, "seat_dy": seat}

    add("tr_horse", lambda fr: quadruped(112, "#8a5232", frame=fr, mane="#2a1a10", tail="#2a1a10", saddle="#6a2a1a"), 112, 62)
    add("tr_pony", lambda fr: quadruped(88, "#b89060", frame=fr, L=0.95, H=0.95, mane="#e8dcc0", tail="#e8dcc0", saddle="#3a5a8a"), 88, 46)
    add("tr_donkey", lambda fr: quadruped(88, "#8a8278", frame=fr, mane="#3a3430", tail="#3a3430", belly="#c8c0b0"), 88, 44)
    add("tr_mule", lambda fr: quadruped(104, "#6a4a34", frame=fr, mane="#2a1a10", tail="#2a1a10", saddle="#8a6a3a"), 104, 56)
    add("tr_ox", lambda fr: quadruped(112, "#7a5a40", frame=fr, L=1.12, H=0.9, head="ox", horns="ox", tail="#3a2a1a", low=True), 112, 52)
    add("tr_war_wolf", lambda fr: quadruped(104, "#6a6460", frame=fr, L=1.05, H=0.85, head="wolf", tail="#5a5450", low=True,
                                             fur=lambda x, y, c: sh(c, 0.86) if (int(x) + int(y) * 2) % 5 == 0 else None, saddle="#6a2a1a"), 104, 46)
    add("tr_riding_lizard", lambda fr: quadruped(112, "#4a7a4a", frame=fr, L=1.2, H=0.65, head="lizard", tail="#4a7a4a", low=True,
                                                  fur=lambda x, y, c: sh(c, 1.18) if (int(x) // 3 + int(y) // 3) % 2 == 0 else None, saddle="#8a3a2a"), 112, 40)
    add("tr_camel", lambda fr: quadruped(120, "#c8a068", frame=fr, H=1.15, head="camel", tail="#8a6a40", saddle="#a8322a"), 120, 76)
    add("tr_reindeer", lambda fr: quadruped(96, "#8a7a68", frame=fr, head="deer", horns="antlers", belly="#e0d8c8", tail="#e0d8c8"), 96, 48)
    for kind, size in (("handcart", 64), ("cart", 112), ("wagon", 144), ("coach", 144), ("chariot", 96), ("kibitka", 128)):
        add("tr_" + kind, lambda fr, kind=kind, size=size: cart(size, fr, kind), size, 30)
    add("tr_sled", lambda fr: sled(96, fr), 96, 22)
    add("tr_longship", lambda fr: boat(240, fr, "longship"), 240, 40)
    add("tr_galley", lambda fr: boat(240, fr, "galley"), 240, 40)
    add("tr_reedboat", lambda fr: boat(160, fr, "reedboat"), 160, 30)
    return T, META
