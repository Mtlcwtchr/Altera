# LR-167: furniture, work stations and the belts' holy and memorial things, 3/4 view, feet
# at the bottom middle. 48 px canvases are one cell, 96 px two cells wide.
import math
import random

from paint import Cv, Scaled, Noise, cap, ell, poly, rect, sh, rgb, mix, OR, AND, SUB, blob
from solid import box, cyl, legs

WOOD, WOOD_D, STONE, MARBLE, BRONZE, GOLD, IRON = "#8a5a34", "#5a3a22", "#8a8680", "#e8e2d4", "#b8873a", "#e0b030", "#6a6e74"


def grain(x, y, c):
    return sh(c, 0.86) if (int(y) * 5 + int(x) // 6) % 7 == 0 else None


def stones_fn(cell=5):
    def f(x, y, c):
        u, v = int(x), int(y)
        if (u + (v // cell) * 3) % cell == 0 or v % cell == 0:
            return sh(c, 0.72)
        return None
    return f


def C(n=48, k=1):
    return Cv(n) if k == 1 else Scaled(n, k)


def shadow(cv, cx, cy, rx, ry=None):
    cv.shadow(ell(cx, cy, rx, ry or rx * 0.25))


# --- furniture ------------------------------------------------------------------------------
def bed():
    cv = C()
    shadow(cv, 26, 44, 20, 4)
    box(cv, 6, 8, 36, 30, 6, WOOD, WOOD_D, fn_top=grain)
    cv.paint(rect(8, 10, 40, 36), "#d8ccb0", under=False, soft=True)              # the sheet
    cv.paint(rect(8, 18, 40, 36), "#a8322a", under=False, soft=True,
             fn=lambda x, y, c: sh(c, 0.85) if int(x) % 8 == 0 else None)            # a woollen blanket
    cv.paint(ell(24, 13, 9, 3.5), "#eee8d8", soft=True)                            # the pillow
    return cv.finish()


def table():
    cv = C()
    shadow(cv, 25, 42, 19, 4)
    legs(cv, (9, 37), 22, 40, WOOD_D, 3)
    box(cv, 5, 12, 38, 12, 4, WOOD, WOOD_D, fn_top=grain)
    cv.paint(ell(17, 16, 4, 2), "#c8b890")   # a bowl
    cv.paint(ell(31, 17, 3, 2.4), "#8a5a3a")  # a jug
    cv.paint(rect(30, 12, 33, 16), "#8a5a3a")
    return cv.finish()


def chest():
    cv = C()
    shadow(cv, 25, 42, 17, 4)
    box(cv, 9, 16, 30, 10, 16, WOOD, WOOD_D, fn_top=grain, fn_front=grain)
    for x in (9, 21, 36):
        cv.paint(rect(x, 16, x + 3, 42), IRON, rim=1)
    cv.paint(rect(22, 28, 26, 33), GOLD, rim=1)
    return cv.finish()


def barrel():
    cv = C()
    shadow(cv, 26, 43, 12, 3)
    cyl(cv, 24, 14, 11, 4, 26, "#8a5a34", fn=lambda x, y, c: "#5a5a5a" if int(y) in (19, 20, 33, 34) else (sh(c, 0.8) if int(x) % 4 == 0 else None))
    return cv.finish()


def sack():
    cv = C()
    shadow(cv, 25, 43, 13, 3)
    cv.paint(OR(blob(24, 32, 11, 3, lobes=3, amp=0.1), ell(24, 18, 4, 4)), "#c8b080")
    cv.paint(cap(20, 21, 28, 21, 1.4), "#8a6a40", rim=1)
    return cv.finish()


def hearth():
    cv = C()
    shadow(cv, 25, 40, 18, 5)
    rnd = random.Random(1)
    for a in range(0, 360, 36):   # a ring of stones
        r = math.radians(a)
        cv.paint(ell(24 + math.cos(r) * 14, 30 + math.sin(r) * 7, 4, 3), sh(rgb(STONE), rnd.uniform(0.85, 1.1)))
    cv.paint(ell(24, 30, 10, 5), "#2a2018", under=False)
    for i, (x0, x1) in enumerate(((16, 32), (32, 17), (24, 24))):
        cv.paint(cap(x0, 33, x1, 27, 1.8), "#6a4a2a", rim=1)
    fire(cv, 24, 30, 1.0)
    return cv.finish()


def fire(cv, cx, by, k=1.0):
    cv.paint(poly([(cx - 7 * k, by), (cx - 4 * k, by - 9 * k), (cx - 1 * k, by - 6 * k), (cx + 1 * k, by - 16 * k),
                   (cx + 4 * k, by - 7 * k), (cx + 6 * k, by - 10 * k), (cx + 7 * k, by)]), "#e86a1a", flat=True,
             fn=lambda x, y, c: "#f8d048" if abs(x - cx) < 3 * k and y > by - 8 * k else (sh(c, 0.8) if abs(x - cx) > 5 * k else None))
    cv.dot(cx, by - 18 * k, "#f8a030")
    cv.dot(cx - 3 * k, by - 13 * k, "#f8d048")


def anvil():
    cv = C()
    shadow(cv, 25, 43, 15, 3)
    cv.paint(poly([(14, 42), (34, 42), (30, 34), (18, 34)]), WOOD_D)                     # the block
    cv.paint(poly([(19, 34), (29, 34), (28, 26), (20, 26)]), IRON)                        # the waist
    cv.paint(poly([(8, 22), (14, 18), (36, 18), (40, 22), (36, 26), (12, 26)]), IRON)    # the face and horn
    cv.paint(rect(14, 18, 36, 20), sh(rgb(IRON), 1.3), flat=True)
    cv.paint(cap(36, 23, 41, 31, 0.6), "#3a3a3a", flat=True)                              # a hammer leaning
    cv.paint(rect(37, 28, 44, 32), "#4a4a4e")
    return cv.finish()


def well(style="stone"):
    cv = C()
    shadow(cv, 26, 44, 18, 4)
    col = {"stone": STONE, "mud": "#c8a878", "marble": MARBLE, "log": WOOD}[style]
    cyl(cv, 24, 26, 15, 6, 12, col, fn=stones_fn(4) if style != "log" else grain)
    cv.paint(ell(24, 26, 12, 4.5), "#1a2430", under=False)
    cv.paint(ell(24, 27, 9, 2.5), "#2a4a6a", under=False)
    legs(cv, (9, 37), 6, 26, WOOD_D, 3)
    cv.paint(cap(9, 7, 40, 7, 1.5), WOOD, rim=1)
    cv.paint(poly([(5, 8), (24, 0), (43, 8), (40, 10), (24, 3), (8, 10)]), "#a8322a" if style != "mud" else "#c8a060")
    cv.paint(cap(24, 7, 24, 20, 0.5), "#c8b890", flat=True)
    cv.paint(rect(21, 18, 27, 23), WOOD, rim=1)
    return cv.finish()


def stall(cloth="#c83a2a"):
    cv = C()
    shadow(cv, 25, 44, 20, 4)
    legs(cv, (6, 40), 8, 42, WOOD_D, 2)
    box(cv, 4, 26, 40, 8, 10, WOOD, WOOD_D, fn_top=grain)
    for i, c in enumerate(("#d84a2a", "#e0b030", "#6a9a3a", "#c8a070")):
        cv.paint(ell(10 + i * 9, 28, 3.5, 2.5), c)
    cv.paint(poly([(2, 10), (46, 10), (44, 4), (4, 4)]), cloth,
             fn=lambda x, y, c: "#f0e8d0" if (int(x) // 6) % 2 else None)
    for x in range(4, 46, 6):
        cv.paint(ell(x + 3, 11, 3, 2), "#f0e8d0" if (x // 6) % 2 else cloth)
    return cv.finish()


def chopping_block():
    cv = C()
    shadow(cv, 25, 42, 12, 3)
    cyl(cv, 24, 26, 10, 4, 12, "#8a6040", top="#c8a070")
    cv.paint(ell(24, 26, 6, 2.4), "#a8845a", under=False, flat=True)
    cv.paint(cap(24, 26, 34, 14, 0.9), WOOD_D, rim=1)
    cv.paint(poly([(20, 25), (28, 25), (27, 21), (21, 21)]), IRON)
    return cv.finish()


def firewood():
    cv = C()
    shadow(cv, 25, 43, 18, 3)
    for r, n in enumerate((4, 3, 2)):
        for i in range(n):
            x = 24 - (n - 1) * 5 + i * 10
            y = 38 - r * 8
            cv.paint(cap(x - 14, y, x + 14, y, 4), "#7a5232", rim=1)
            cv.paint(ell(x + 14, y, 3, 4), "#c8a070", fn=lambda px, py, c, x=x, y=y: sh(c, 0.8) if int(math.hypot(px - x - 14, py - y)) == 1 else None)
    return cv.finish()


def log():
    cv = C()
    shadow(cv, 25, 40, 20, 3)
    cv.paint(cap(4, 34, 40, 30, 5), "#6a4a30", fn=lambda x, y, c: sh(c, 0.75) if (int(x) + int(y) * 3) % 9 == 0 else None)
    cv.paint(ell(41, 30, 3.5, 5), "#b89060")
    cv.paint(cap(18, 30, 22, 22, 1.2, 0.6), "#6a4a30", rim=1)
    return cv.finish()


def fence():
    cv = C()
    shadow(cv, 24, 40, 22, 2)
    for x in (4, 22, 40):
        cv.paint(rect(x, 16, x + 4, 40), WOOD_D, rim=1)
        cv.paint(poly([(x, 16), (x + 2, 13), (x + 4, 16)]), WOOD_D)
    for y in (20, 30):
        cv.paint(rect(0, y, 48, y + 3), WOOD, rim=1, fn=grain)
    return cv.finish()


def grave(style="mound"):
    cv = C()
    shadow(cv, 25, 42, 16, 3)
    cv.paint(ell(24, 36, 16, 7), "#6a5034")
    cv.paint(ell(24, 34, 14, 5), "#7a6040", under=False)
    if style == "stone":
        cv.paint(poly([(18, 34), (18, 14), (24, 10), (30, 14), (30, 34)]), STONE, fn=stones_fn(30))
    else:
        cv.paint(rect(22, 10, 26, 34), WOOD_D, rim=1)
        cv.paint(rect(16, 15, 32, 19), WOOD_D, rim=1)
    cv.paint(ell(18, 37, 2, 1.5), "#e8d060")
    return cv.finish()


def rubble():
    cv = C()
    shadow(cv, 25, 42, 18, 4)
    rnd = random.Random(4)
    for i in range(9):
        x, y = 12 + rnd.uniform(0, 24), 26 + rnd.uniform(0, 14)
        cv.paint(blob(x, y, rnd.uniform(3, 6), i, lobes=3, amp=0.15), sh(rgb(STONE), rnd.uniform(0.8, 1.1)))
    cv.paint(cap(10, 30, 30, 22, 1.5), WOOD_D, rim=1)
    return cv.finish()


def wreck():
    cv = C()
    shadow(cv, 25, 42, 20, 4)
    cv.paint(poly([(6, 40), (40, 40), (44, 30), (10, 32)]), "#5a4030", fn=grain)
    for x in (12, 20, 28, 36):
        cv.paint(cap(x, 36, x - 3, 20, 1.4, 1.0), "#6a4a30", rim=1)
    return cv.finish()


# --- work stations ------------------------------------------------------------------------------
def forge():
    cv = C()
    shadow(cv, 25, 44, 20, 4)
    box(cv, 6, 14, 36, 12, 16, STONE, fn_top=stones_fn(5), fn_front=stones_fn(5))
    cv.paint(ell(24, 19, 9, 3.5), "#2a1810", under=False)
    cv.paint(ell(24, 19, 6, 2), "#f86a1a", flat=True, fn=lambda x, y, c: "#f8d048" if abs(x - 24) < 2 else None)
    cv.paint(poly([(36, 30), (46, 26), (46, 34), (36, 36)]), "#7a5232")   # the bellows
    cv.paint(rect(14, 30, 34, 38), "#2a1810", under=False)                 # the ash pit
    return cv.finish()


def oven(style="dome"):
    cv = C()
    shadow(cv, 25, 44, 20, 4)
    cv.paint(SUB(ell(24, 34, 19, 18), rect(0, 40, 48, 48)), "#c08a5a", fn=lambda x, y, c: sh(c, 0.88) if (int(x) + int(y) * 2) % 11 == 0 else None)
    cv.paint(ell(24, 37, 7, 6), "#1a1008")
    cv.paint(ell(24, 40, 5, 2), "#f86a1a", flat=True)
    cv.paint(rect(5, 39, 43, 42), "#8a6a4a", rim=1)
    return cv.finish()


def loom():
    cv = C()
    shadow(cv, 25, 44, 18, 3)
    legs(cv, (8, 38), 6, 42, WOOD_D, 3)
    cv.paint(rect(6, 6, 42, 9), WOOD, rim=1)
    for x in range(12, 37, 2):
        cv.paint(rect(x, 9, x + 1, 30), "#e8dcc0", flat=True)
    cv.paint(rect(11, 14, 37, 26), "#3a6aa8", flat=True, fn=lambda x, y, c: "#e0b030" if int(y) % 4 == 0 else ("#a8322a" if (int(x) + int(y)) % 8 == 0 else None))
    for x in range(12, 37, 4):   # loom weights
        cv.paint(ell(x, 33, 1.6, 2), "#8a6a4a")
    return cv.finish()


def potter_wheel():
    cv = C()
    shadow(cv, 25, 42, 14, 3)
    cyl(cv, 24, 30, 13, 4, 6, "#7a5232", top="#a87a4a")
    cv.paint(OR(ell(24, 24, 6, 6), rect(21, 16, 27, 22)), "#c87a4a")
    cv.paint(ell(24, 16, 3, 1), "#6a3a20", under=False)
    return cv.finish()


def tub():
    cv = C()
    shadow(cv, 25, 43, 17, 3)
    cyl(cv, 24, 24, 16, 5, 14, "#8a5a34", fn=lambda x, y, c: "#5a5a5a" if int(y) in (28, 35) else (sh(c, 0.8) if int(x) % 5 == 0 else None))
    cv.paint(ell(24, 24, 13, 3.6), "#3a5a7a", under=False)
    cv.paint(ell(20, 23, 3, 0.8), "#7aa8d0", flat=True)
    return cv.finish()


def cook_pot():
    cv = C()
    shadow(cv, 25, 43, 16, 4)
    cv.paint(cap(8, 42, 24, 8, 1.2), WOOD_D, rim=1)
    cv.paint(cap(40, 42, 24, 8, 1.2), WOOD_D, rim=1)
    cv.paint(cap(24, 8, 24, 20, 0.6), "#3a3a3a", flat=True)
    cv.paint(SUB(ell(24, 28, 11, 9), rect(0, 0, 48, 21)), "#3a3a3e")
    cv.paint(ell(24, 21, 10, 3), "#8a6a3a", under=False)
    fire(cv, 24, 42, 0.6)
    return cv.finish()


def storage_jar(col="#b8683a", band="#2a2018"):
    """an amphora / pithos: the Middle Sea's black-figure band"""
    cv = C()
    shadow(cv, 25, 44, 11, 3)
    body = OR(ell(24, 28, 11, 14), rect(20, 8, 28, 16), ell(24, 8, 6, 2))
    cv.paint(body, col, fn=lambda x, y, c: band if 24 <= y < 30 else (sh(c, 0.9) if 22 <= y < 24 else None))
    cv.paint(OR(cap(19, 12, 15, 20, 1.2), cap(29, 12, 33, 20, 1.2)), col, rim=1)
    cv.paint(ell(24, 8, 4, 1.2), "#2a1810", under=False)
    return cv.finish()


def drying_rack():
    cv = C()
    shadow(cv, 25, 44, 20, 3)
    legs(cv, (6, 40), 10, 42, WOOD_D, 2)
    cv.paint(rect(4, 10, 44, 12), WOOD, rim=1)
    rnd = random.Random(2)
    for i in range(6):
        x = 8 + i * 6
        c = rnd.choice(("#a8322a", "#c89a5a", "#6a8a3a", "#8a5a3a"))
        cv.paint(poly([(x, 12), (x + 5, 12), (x + 4, 26 + rnd.randint(0, 8)), (x + 1, 26 + rnd.randint(0, 8))]), c)
    return cv.finish()


def mortar():
    cv = C()
    shadow(cv, 25, 43, 12, 3)
    cyl(cv, 24, 26, 10, 4, 12, STONE, fn=stones_fn(40))
    cv.paint(ell(24, 26, 7, 2.6), "#4a4038", under=False)
    cv.paint(cap(24, 26, 32, 10, 1.6, 1.2), "#8a6a4a", rim=1)
    return cv.finish()


def quern():
    cv = C()
    shadow(cv, 25, 43, 16, 3)
    cyl(cv, 24, 30, 15, 5, 6, STONE)
    cyl(cv, 24, 24, 12, 4, 5, sh(rgb(STONE), 1.1))
    cv.paint(ell(24, 24, 2.5, 1), "#2a2018", under=False)
    cv.paint(cap(32, 23, 33, 14, 1.2), WOOD, rim=1)
    return cv.finish()


def tanning_pit():
    cv = C()
    shadow(cv, 25, 44, 20, 3)
    cv.paint(ell(24, 32, 19, 9), "#6a5034")
    cv.paint(ell(24, 32, 15, 6.5), "#5a4a2a", under=False, fn=lambda x, y, c: "#8a7a3a" if (int(x) + int(y)) % 7 == 0 else None)
    cv.paint(poly([(8, 14), (30, 10), (34, 22), (12, 24)]), "#a8784a")   # a hide on a frame
    return cv.finish()


def campfire():
    cv = C()
    shadow(cv, 25, 40, 14, 4)
    for x0, x1 in ((12, 36), (36, 12)):
        cv.paint(cap(x0, 38, x1, 30, 2), "#6a4a2a", rim=1)
    fire(cv, 24, 36, 1.2)
    return cv.finish()


def workbench():
    cv = C()
    shadow(cv, 25, 42, 20, 4)
    legs(cv, (6, 40), 22, 40, WOOD_D, 3)
    box(cv, 3, 14, 42, 10, 5, "#a87a4a", WOOD_D, fn_top=grain)
    cv.paint(cap(10, 17, 20, 15, 0.9), IRON, rim=1)          # a saw
    cv.paint(rect(28, 13, 34, 18), "#8a6a4a", rim=1)          # a plane
    for i in range(3):
        cv.dot(38 + i * 2, 18, "#d8c090")
    return cv.finish()


def grindstone():
    cv = C()
    shadow(cv, 25, 43, 16, 3)
    legs(cv, (12, 34), 22, 42, WOOD_D, 3)
    cv.paint(ell(24, 22, 10, 12), "#a8a49a", fn=lambda x, y, c: sh(c, 0.85) if int(math.hypot(x - 24, y - 22)) % 3 == 0 else None)
    cv.paint(cap(24, 22, 36, 18, 1), IRON, rim=1)
    return cv.finish()


def shrine_basic():
    """the old generic shrine (a household altar)"""
    cv = C()
    shadow(cv, 25, 43, 14, 3)
    box(cv, 12, 22, 24, 6, 14, STONE, fn_front=stones_fn(6))
    cv.paint(ell(24, 22, 5, 2), "#e8d060")
    fire(cv, 24, 22, 0.5)
    return cv.finish()


# --- the belts' holy and memorial things (96 px = 2 cells wide unless said) ----------------------
def column(cv, cx, by, h, col=MARBLE, r=5, cap_col=None):
    cv.paint(rect(cx - r - 3, by - 4, cx + r + 3, by), sh(rgb(col), 0.95))                   # the base
    cv.paint(rect(cx - r, by - h, cx + r, by - 4), col,
             fn=lambda x, y, c: sh(c, 0.82) if (int(x) - cx) % 3 == 0 else None)              # flutes
    cv.paint(rect(cx - r - 3, by - h - 4, cx + r + 3, by - h), cap_col or col)               # the capital


def greek_temple(k=2):
    """a portico: four columns under an architrave and a pediment with a painted frieze"""
    cv = C(96, k)
    cv.shadow(ell(50, 90, 44, 6))
    box(cv, 4, 78, 88, 6, 8, "#ddd6c4", "#c8c0aa", fn_front=lambda x, y, c: sh(c, 0.85) if int(y) in (86,) else None)
    for cx in (16, 38, 58, 80):
        column(cv, cx, 80, 44)
    cv.paint(rect(6, 26, 90, 34), MARBLE)
    cv.paint(rect(6, 30, 90, 33), "#3a5aa8", flat=True, fn=lambda x, y, c: "#e0b030" if int(x) % 6 < 2 else None)   # the frieze
    cv.paint(poly([(4, 26), (48, 8), (92, 26)]), MARBLE)
    cv.paint(poly([(14, 24), (48, 12), (82, 24)]), "#c8402a", flat=True)                     # the painted tympanum
    cv.paint(ell(48, 19, 4, 3), GOLD, flat=True)
    return cv.finish()


def greek_altar():
    cv = C()
    shadow(cv, 25, 44, 18, 3)
    box(cv, 8, 22, 32, 8, 14, MARBLE, "#d0c8b4", fn_front=lambda x, y, c: "#a83a24" if int(y) == 33 and int(x) % 4 < 2 else None)
    cv.paint(OR(ell(10, 22, 3, 3), ell(38, 22, 3, 3)), MARBLE)    # the volutes
    fire(cv, 24, 26, 0.8)
    return cv.finish()


def statue(god_col=MARBLE, plinth="#c8c0aa", pose="spear", k=2):
    """a marble god on a plinth (the figure in the pilot's proportions, a long robe), 2 m"""
    cv = C(48, k)
    cv.shadow(ell(26, 46, 14, 2))
    box(cv, 10, 34, 28, 4, 9, plinth)
    cv.paint(poly([(18, 34), (30, 34), (28, 16), (20, 16)]), god_col, fn=lambda x, y, c: sh(c, 0.84) if (int(x) - 20) % 3 == 0 and y > 22 else None)
    cv.paint(ell(24, 11, 3.6, 4.2), god_col)
    cv.paint(cap(20, 17, 16, 27, 1.6), god_col, rim=1)
    if pose == "spear":
        cv.paint(cap(30, 4, 30, 34, 0.6), BRONZE, flat=True, rim=1)
        cv.paint(cap(28, 17, 30, 22, 1.5), god_col, rim=1)
    elif pose == "bowl":
        cv.paint(cap(28, 17, 33, 21, 1.5), god_col, rim=1)
        cv.paint(ell(34, 20, 3, 1.4), GOLD)
    return cv.finish()


def herm():
    cv = C()
    shadow(cv, 25, 44, 9, 2)
    cv.paint(rect(19, 16, 29, 42), MARBLE)
    cv.paint(ell(24, 11, 5, 6), MARBLE)
    cv.paint(cap(22, 15, 26, 15, 2.4), "#d8d0c0", rim=1)   # the beard
    cv.paint(OR(rect(16, 20, 19, 23), rect(29, 20, 32, 23)), MARBLE)
    return cv.finish()


def tripod():
    cv = C()
    shadow(cv, 25, 44, 12, 2)
    for x0, x1 in ((14, 20), (34, 28), (24, 24)):
        cv.paint(cap(x0, 44, x1, 20, 1), BRONZE, rim=1)
    cv.paint(SUB(ell(24, 18, 10, 7), rect(0, 0, 48, 14)), BRONZE)
    cv.paint(ell(24, 14, 10, 2.5), "#3a2a1a", under=False)
    cv.paint(OR(ell(14, 10, 2.5, 3), ell(34, 10, 2.5, 3)), BRONZE, rim=1)
    return cv.finish()


def horns_of_consecration():
    cv = C()
    shadow(cv, 25, 44, 18, 2)
    box(cv, 8, 34, 32, 4, 6, "#d8c8a0")
    cv.paint(OR(poly([(10, 34), (16, 34), (12, 12), (8, 14)]), poly([(32, 34), (38, 34), (40, 14), (36, 12)]), rect(14, 26, 34, 34)), "#e8dcc0")
    cv.paint(rect(14, 28, 34, 30), "#b02c20", flat=True)
    return cv.finish()


def obelisk(k=2):
    cv = C(96, k)
    cv.shadow(ell(54, 92, 20, 3))
    box(cv, 34, 82, 28, 4, 8, "#c8a878")
    cv.paint(poly([(40, 84), (56, 84), (53, 16), (43, 16)]), "#d8b888",
             fn=lambda x, y, c: "#5a4030" if (int(y) % 6 == 0 and 44 < x < 52) or (int(x) % 4 == 0 and int(y) % 6 in (2, 3) and 44 < x < 52) else None)
    cv.paint(poly([(43, 16), (53, 16), (48, 8)]), GOLD)
    return cv.finish()


def sphinx(k=1.5):
    cv = C(96, k)
    cv.shadow(ell(50, 84, 40, 5))
    box(cv, 6, 70, 84, 6, 10, "#c8a878")
    col = "#dcb880"
    cv.paint(OR(ell(40, 60, 30, 11), cap(16, 68, 70, 68, 4)), col)       # the lion body
    cv.paint(OR(cap(70, 60, 86, 68, 4, 3), cap(64, 62, 80, 68, 3.5, 3)), col, rim=1)   # the paws
    cv.paint(OR(poly([(54, 58), (80, 58), (77, 32), (57, 32)]), ell(67, 31, 10, 6)), "#2a5aa8",
             fn=lambda x, y, c: "#e0b030" if int(y) % 4 < 2 else None)    # the striped headcloth
    cv.paint(ell(67, 41, 6, 8), col)                                       # the face inside it
    cv.dot(64, 39, "#2a2018", 2, 1)
    cv.dot(69, 39, "#2a2018", 2, 1)
    cv.dot(66, 45, sh(rgb(col), 0.6), 3, 1)
    cv.paint(cap(12, 66, 8, 54, 1.4), col, rim=1)                          # the tail
    return cv.finish()


def naos(k=1.5):
    """the Nile's little shrine: a pylon gate in painted mudbrick"""
    cv = C(96, k)
    cv.shadow(ell(50, 90, 40, 5))
    for x0 in (6, 58):
        cv.paint(poly([(x0, 88), (x0 + 32, 88), (x0 + 28, 20), (x0 + 4, 20)]), "#d8c090",
                 fn=lambda x, y, c: "#2a6aa8" if 26 <= y < 30 else ("#c8a030" if 30 <= y < 32 else (sh(c, 0.88) if int(y) % 8 == 0 else None)))
    cv.paint(rect(38, 34, 58, 88), "#1a1208")
    cv.paint(rect(34, 28, 62, 36), "#d8c090", fn=lambda x, y, c: "#c8a030" if int(y) == 31 else None)
    cv.paint(ell(48, 32, 6, 2.5), GOLD, flat=True)        # the winged sun
    cv.paint(OR(cap(36, 32, 44, 32, 1.2), cap(52, 32, 60, 32, 1.2)), "#2a6aa8", flat=True)
    return cv.finish()


def lotus_column():
    cv = C()
    shadow(cv, 25, 45, 9, 2)
    cv.paint(rect(18, 12, 30, 44), "#d8c8a0", fn=lambda x, y, c: "#2a6aa8" if int(y) % 10 == 0 else ("#c8402a" if int(y) % 10 == 1 else None))
    cv.paint(poly([(14, 12), (34, 12), (30, 2), (26, 6), (24, 0), (22, 6), (18, 2)]), "#4a9a6a")
    return cv.finish()


def slavic_idol():
    """a carved four-faced wooden god on a hill-top (Zbruch idol style)"""
    cv = C(48)
    cv.shadow(ell(26, 46, 12, 2))
    cv.paint(rect(17, 8, 31, 46), "#8a6440", fn=lambda x, y, c: sh(c, 0.8) if int(x) in (24,) else (sh(c, 0.9) if int(y) % 9 == 0 else None))
    cv.paint(poly([(15, 9), (33, 9), (30, 2), (18, 2)]), "#6a4a30")                    # the round cap
    for fx in (20, 27):
        cv.dot(fx, 13, "#2a1a10", 2, 1)
    cv.paint(cap(20, 18, 28, 18, 0.6), "#2a1a10", flat=True)
    cv.paint(rect(19, 26, 29, 29), "#a8322a", flat=True)                               # a painted horn held
    cv.paint(rect(18, 36, 30, 38), "#2a1a10", flat=True)
    return cv.finish()


def celtic_stone():
    cv = C()
    shadow(cv, 25, 45, 10, 2)
    cv.paint(poly([(16, 45), (32, 45), (31, 12), (26, 4), (18, 10)]), "#8a8a80",
             fn=lambda x, y, c: "#c8c8b8" if (int(x) == 24 and int(y) % 2 == 0 and y > 12) or (int(y) % 5 == 0 and abs(x - 24) < 3) else None)
    cv.paint(blob(22, 12, 4, 3, lobes=4, amp=0.2), "#6a8a3a", flat=True)      # lichen
    return cv.finish()


def dolmen(k=1.5):
    cv = C(96, k)
    cv.shadow(ell(50, 86, 40, 5))
    for x0 in (14, 66):
        cv.paint(poly([(x0, 86), (x0 + 16, 86), (x0 + 14, 44), (x0 + 2, 46)]), "#7a7a72", fn=stones_fn(20))
    cv.paint(AND(blob(48, 40, 40, 7, lobes=3, amp=0.08), ell(48, 40, 40, 10)), "#8a8a80", fn=stones_fn(24))
    cv.paint(rect(30, 46, 66, 86), "#1a1814", under=False)
    return cv.finish()


def well_crane(k=1):
    """the forests' well with a sweep (zhuravl)"""
    cv = C(96, k)
    cv.shadow(ell(40, 90, 30, 5))
    cyl(cv, 30, 70, 14, 5, 14, WOOD, fn=grain)
    cv.paint(ell(30, 70, 11, 3.6), "#1a2430", under=False)
    cv.paint(cap(70, 90, 68, 30, 2.4), WOOD_D, rim=1)
    cv.paint(cap(90, 48, 20, 22, 1.4), WOOD, rim=1)
    cv.paint(cap(22, 22, 26, 64, 0.5), "#3a2a1a", flat=True)
    cv.paint(rect(22, 62, 30, 68), WOOD_D, rim=1)
    cv.paint(rect(84, 44, 94, 54), STONE)
    return cv.finish()


def runestone():
    cv = C(48)
    cv.shadow(ell(26, 46, 12, 2))
    cv.paint(poly([(12, 46), (36, 46), (34, 10), (24, 2), (14, 8)]), "#8a8682")

    def snake(x, y, c):
        # the rune serpent band: a red ribbon looping over the face
        d = abs(math.hypot(x - 24, (y - 24) * 0.8) - 9)
        if d < 1.4:
            return "#b02c20"
        if d < 1.9:
            return "#2a1a10"
        if 16 < y < 34 and abs(x - 24) < 1 and int(y) % 3:
            return "#2a1a10"
        return None
    cv.paint(poly([(15, 44), (33, 44), (31, 12), (24, 5), (17, 10)]), "#9a968e", flat=True, fn=snake)
    return cv.finish()


def cairn():
    cv = C()
    shadow(cv, 25, 44, 16, 3)
    rnd = random.Random(8)
    for row, (n, y, r) in enumerate(((5, 40, 5), (4, 33, 4.6), (3, 26, 4.2), (2, 20, 3.6), (1, 14, 3.2))):
        for i in range(n):
            x = 24 - (n - 1) * r + i * 2 * r + rnd.uniform(-1, 1)
            cv.paint(blob(x, y, r, row * 10 + i, lobes=3, amp=0.12), sh(rgb("#8a8a84"), rnd.uniform(0.85, 1.1)))
    return cv.finish()


def balbal():
    """the steppe's stone warrior holding a cup"""
    cv = C()
    shadow(cv, 25, 45, 9, 2)
    cv.paint(OR(rect(17, 18, 31, 45), ell(24, 12, 7, 8)), "#a89a84")
    cv.dot(20, 11, "#4a4034", 3, 1)
    cv.dot(25, 11, "#4a4034", 3, 1)
    cv.paint(OR(cap(20, 16, 22, 17, 0.6), cap(28, 16, 26, 17, 0.6)), "#4a4034", flat=True)   # the moustache
    cv.paint(cap(18, 26, 28, 30, 1.2), "#9a8c76", rim=1)
    cv.paint(ell(28, 28, 3, 2.4), "#8a7c66")                                                # the cup
    cv.paint(rect(17, 35, 31, 37), "#7a6c56", flat=True)                                    # the belt
    return cv.finish()


def kurgan(k=1.5):
    cv = C(96, k)
    cv.shadow(ell(50, 84, 46, 6))
    cv.paint(ell(48, 78, 44, 18), "#8a9a4a", fn=lambda x, y, c: "#b8a85a" if (int(x) * 3 + int(y) * 7) % 13 == 0 else None)
    cv.paint(ell(48, 66, 30, 8), "#9aa858", under=False, soft=True)
    Cv.blit(cv, balbal(), int(48 * k - 24), int(66 * k - 46))   # the stone man on top, at the world's scale
    return cv.finish()


def horse_totem():
    """a pole with horse skulls and ribbons (the steppe's sky offerings)"""
    cv = C(48)
    cv.shadow(ell(26, 46, 8, 2))
    cv.paint(cap(24, 46, 24, 4, 1.6), WOOD_D, rim=1)
    cv.paint(OR(ell(24, 8, 5, 3), cap(24, 8, 24, 16, 2, 1.2)), "#e8e0c8")
    cv.dot(22, 8, "#2a1a10")
    cv.dot(26, 8, "#2a1a10")
    for i, c in enumerate(("#3a6ac0", "#f0e8d0", "#c83a2a")):
        cv.paint(cap(25, 20 + i * 2, 33 + i * 2, 30 + i * 4, 0.8), c, flat=True, rim=1)
    return cv.finish()


def ovoo():
    cv = cairn()
    cv2 = C()
    cv2.blit(cv, 0, 0)
    for x0, x1 in ((24, 22), (24, 30)):
        cv2.paint(cap(x0, 16, x1, 2, 0.8), WOOD_D, rim=1)
    for i, c in enumerate(("#3a6ac0", "#f0e8d0", "#e0b030", "#3a9a5a")):
        cv2.paint(cap(22 + i, 4 + i * 2, 14 + i * 2, 10 + i * 3, 0.7), c, flat=True, rim=1)
    return cv2.finish()


def dwarf_forge_shrine():
    cv = C(48)
    shadow(cv, 25, 45, 18, 2)
    box(cv, 8, 26, 32, 6, 12, "#6a6a72", fn_front=stones_fn(6))
    cv.paint(poly([(14, 26), (34, 26), (30, 6), (18, 6)]), "#7a7a80")
    cv.paint(poly([(19, 22), (29, 22), (27, 10), (21, 10)]), "#1a1410")
    cv.paint(ell(24, 18, 3, 3), "#f8a030", flat=True)
    cv.paint(rect(16, 12, 32, 14), GOLD, flat=True)
    return cv.finish()


def yurt(k=1.5):
    """the steppe's round felt house (an object, not wall modules): lattice wall, felt dome,
    the crown ring on top, a painted door"""
    cv = C(96, k)
    cv.shadow(ell(50, 88, 44, 6))
    wall = AND(ell(48, 70, 40, 14), lambda x, y: True)
    cv.paint(OR(rect(8, 46, 88, 72), ell(48, 72, 40, 12)), "#e8e0c8",
             fn=lambda x, y, c: ("#a8322a" if 50 <= y < 53 else ("#e0b030" if 53 <= y < 54 else (sh(c, 0.86) if (int(x) // 10) % 2 and y > 58 else None))))
    cv.paint(OR(poly([(8, 48), (48, 18), (88, 48)]), ell(48, 48, 40, 8)), "#eee8d8",
             fn=lambda x, y, c: "#8a6a4a" if abs((x - 48) * 0.3 - (y - 18) * 0) < 0.5 or int(y) in (30, 40) else None)
    cv.paint(ell(48, 20, 7, 3), "#8a5a34")   # the crown ring (tunduk)
    cv.paint(rect(40, 56, 56, 80), "#c8402a", fn=lambda x, y, c: "#e0b030" if (int(x) + int(y)) % 6 == 0 else None)
    cv.paint(cap(48, 40, 48, 10, 0.8), "#a8a49a", flat=True)   # the smoke pipe
    return cv.finish()


def all_decor():
    return {
        # furniture and things (keep the old names map2d.json and the build pieces use)
        "ob_bed": bed(), "ob_table": table(), "ob_chest": chest(), "ob_barrel": barrel(), "ob_sack": sack(),
        "ob_hearth": hearth(), "ob_anvil": anvil(), "ob_well": well("stone"), "ob_stall": stall(),
        "ob_block": chopping_block(), "ob_firewood": firewood(), "ob_log": log(), "ob_pen": fence(),
        "ob_grave": grave(), "ob_rubble": rubble(), "ob_wreck": wreck(), "ob_shrine": shrine_basic(),
        "ob_frame": loom(),
        "ob_well_mud": well("mud"), "ob_well_marble": well("marble"), "ob_well_log": well("log"),
        "ob_grave_stone": grave("stone"), "ob_stall_blue": stall("#2a5aa8"),
        # work stations (st_ names of craft2d.json)
        "st_forge": forge(), "st_oven": oven(), "st_campfire": campfire(), "st_bench": workbench(), "st_loom": loom(),
        "st_wheel": potter_wheel(), "st_tub": tub(), "st_pot": cook_pot(), "st_jar": storage_jar(), "st_rack": drying_rack(),
        "st_mortar": mortar(), "st_quern": quern(), "st_grind": grindstone(), "st_pit": tanning_pit(),
        # the Middle Sea
        "dc_med_temple": greek_temple(), "dc_med_altar": greek_altar(), "dc_med_statue": statue(), "dc_med_statue_bronze": statue("#b8873a", pose="bowl"),
        "dc_med_herm": herm(), "dc_med_tripod": tripod(), "dc_med_amphora": storage_jar("#c87a42"), "dc_med_horns": horns_of_consecration(),
        # the Nile
        "dc_nile_obelisk": obelisk(), "dc_nile_sphinx": sphinx(), "dc_nile_naos": naos(), "dc_nile_column": lotus_column(),
        # the Middle Forests
        "dc_wood_idol": slavic_idol(), "dc_wood_stone": celtic_stone(), "dc_wood_dolmen": dolmen(), "dc_wood_well_crane": well_crane(),
        # the North
        "dc_north_runestone": runestone(), "dc_north_cairn": cairn(), "dc_north_forge_shrine": dwarf_forge_shrine(),
        # the steppe
        "dc_steppe_balbal": balbal(), "dc_steppe_kurgan": kurgan(), "dc_steppe_totem": horse_totem(), "dc_steppe_ovoo": ovoo(), "dc_steppe_yurt": yurt(),
    }
