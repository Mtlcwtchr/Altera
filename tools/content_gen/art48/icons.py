# LR-167: item and action icons, 24x24, in the same three-tone style with the coloured
# outline; a light from the top left. Names keep the old ones (it_*, ic_*), so craft2d.json,
# resources.json and map2d.json point at them unchanged.
import math
import random

from paint import Cv, cap, ell, poly, rect, sh, rgb, OR, AND, SUB, blob

S = 24
WOOD, WOOD_D, IRON, STEEL, BRONZE, GOLD, CLOTH, LEATHER = "#8a5a34", "#5a3a22", "#7a7e86", "#b8bcc4", "#b8873a", "#e0b030", "#c8b890", "#8a5a2b"


def C():
    return Cv(S)


def handle(cv, x0, y0, x1, y1, col=WOOD, r=1.3):
    cv.paint(cap(x0, y0, x1, y1, r), col, rim=1)


def blade(cv, pts, col=STEEL):
    cv.paint(poly(pts), col, rim=1)


def round_food(col, r=7, cx=12, cy=13, stem=None, leaf=None, spot=None):
    cv = C()
    cv.paint(ell(cx, cy, r, r * 0.92), col)
    if spot:
        cv.dot(cx - r * 0.45, cy - r * 0.45, sh(rgb(col), 1.5), 2, 1)
    if stem:
        cv.paint(cap(cx, cy - r + 1, cx + 1, cy - r - 3, 0.8), stem, rim=1)
    if leaf:
        cv.paint(ell(cx + 3, cy - r - 2, 2.6, 1.3), leaf, rim=1)
    return cv.finish()


def pot(col, neck=4, body=7, lip=None, handle_r=False, cy=14):
    cv = C()
    cv.paint(OR(ell(12, cy, body, body * 0.85), rect(12 - neck / 2, 4, 12 + neck / 2, cy - body + 2)), col)
    cv.paint(ell(12, 4.5, neck / 2 + 1.5, 1.4), lip or sh(rgb(col), 0.9))
    if handle_r:
        cv.paint(cap(12 + neck / 2, 6, 12 + body + 1, cy - 2, 1), col, rim=1)
    return cv.finish()


def garment(col, kind="tunic", trim=None):
    cv = C()
    if kind == "tunic":
        cv.paint(poly([(7, 4), (17, 4), (22, 9), (19, 11), (18, 9), (18, 21), (6, 21), (6, 9), (5, 11), (2, 9)]), col)
        cv.paint(ell(12, 4.5, 3, 1.6), sh(rgb(col), 0.5), flat=True)
    elif kind == "trousers":
        cv.paint(poly([(6, 3), (18, 3), (19, 21), (14, 21), (12, 10), (10, 21), (5, 21)]), col)
        cv.paint(rect(6, 3, 18, 5), sh(rgb(col), 0.75), flat=True)
    elif kind == "cloak":
        cv.paint(poly([(9, 3), (15, 3), (21, 21), (3, 21)]), col)
        cv.paint(ell(12, 4, 2, 2), GOLD)
    elif kind == "cap":
        cv.paint(SUB(ell(12, 15, 9, 9), rect(0, 15, 24, 24)), col)
        cv.paint(rect(3, 14, 21, 17), trim or sh(rgb(col), 0.75))
    elif kind == "boots":
        for x in (4, 13):
            cv.paint(OR(rect(x, 5, x + 6, 17), rect(x, 14, x + 9, 20)), col)
            cv.paint(rect(x, 5, x + 6, 7), sh(rgb(col), 1.2), flat=True)
    if trim and kind in ("tunic", "cloak"):
        cv.paint(rect(6, 19, 18, 21), trim, flat=True)
    return cv.finish()


def tool(kind):
    cv = C()
    if kind == "axe":
        handle(cv, 6, 21, 15, 4)
        blade(cv, [(13, 3), (20, 2), (21, 10), (15, 8)])
    elif kind == "pick":
        handle(cv, 6, 21, 13, 6)
        cv.paint(OR(cap(4, 7, 13, 4, 1.3, 0.8), cap(13, 4, 21, 9, 0.8, 1.3)), IRON, rim=1)
    elif kind == "hammer":
        handle(cv, 7, 21, 13, 8)
        cv.paint(rect(8, 3, 20, 9), IRON)
    elif kind == "hoe":
        handle(cv, 5, 21, 17, 4)
        blade(cv, [(15, 4), (19, 4), (20, 11), (16, 9)], IRON)
    elif kind == "spade":
        handle(cv, 12, 3, 12, 14)
        blade(cv, [(7, 13), (17, 13), (16, 20), (12, 22), (8, 20)], IRON)
        cv.paint(rect(9, 2, 15, 4), WOOD, rim=1)
    elif kind == "saw":
        cv.paint(poly([(3, 10), (20, 7), (21, 12), (4, 15)]), STEEL, fn=lambda x, y, c: sh(c, 0.6) if y > 12.5 and int(x) % 2 == 0 else None)
        cv.paint(rect(18, 5, 23, 14), WOOD)
    elif kind == "sickle":
        cv.paint(SUB(ell(13, 10, 8, 8), ell(10, 12, 7, 7)), STEEL)
        handle(cv, 6, 21, 9, 15)
    elif kind == "knife":
        handle(cv, 4, 20, 9, 15, LEATHER, 1.6)
        blade(cv, [(8, 14), (20, 3), (11, 16)])
    elif kind == "sword":
        blade(cv, [(10, 15), (19, 3), (21, 5), (12, 17)])
        cv.paint(cap(6, 13, 13, 20, 1.1), BRONZE, rim=1)
        handle(cv, 5, 21, 9, 17, LEATHER, 1.3)
        cv.paint(ell(4, 22, 1.6, 1.6), GOLD)
    elif kind == "spear":
        handle(cv, 4, 21, 17, 6, WOOD, 1.0)
        blade(cv, [(15, 8), (21, 2), (18, 9)], BRONZE)
    elif kind == "bow":
        cv.paint(SUB(ell(8, 12, 8, 11), ell(5, 12, 7, 10)), WOOD)
        cv.paint(cap(8, 1, 8, 23, 0.4), "#e8e0d0", flat=True)
    elif kind == "broom":
        handle(cv, 18, 3, 10, 15)
        cv.paint(poly([(9, 13), (13, 16), (8, 22), (3, 19)]), "#c8a050", fn=lambda x, y, c: sh(c, 0.8) if int(x + y) % 2 else None)
    elif kind == "fork":
        handle(cv, 6, 21, 14, 8)
        for dx in (-2, 0, 2):
            cv.paint(cap(14 + dx, 8, 16 + dx, 2, 0.6), IRON, rim=1)
    elif kind == "needle":
        cv.paint(cap(5, 19, 19, 5, 0.7), STEEL, rim=1)
        cv.paint(cap(19, 5, 21, 8, 0.6), "#c83a2a", rim=1, flat=True)
    elif kind == "spindle":
        cv.paint(cap(12, 2, 12, 22, 0.7), WOOD, rim=1)
        cv.paint(ell(12, 12, 6, 4), "#e8dcc0", fn=lambda x, y, c: sh(c, 0.85) if int(y) % 2 else None)
        cv.paint(ell(12, 19, 3, 1.4), WOOD_D)
    elif kind == "torch":
        handle(cv, 9, 22, 13, 9)
        cv.paint(poly([(10, 10), (12, 1), (13, 5), (16, 3), (15, 10)]), "#f08020", flat=True, fn=lambda x, y, c: "#f8d048" if 11 < x < 14 and y > 5 else None)
    elif kind == "snare":
        cv.paint(SUB(ell(12, 12, 8, 8), ell(12, 12, 6.8, 6.8)), "#c8b080", flat=True)
        handle(cv, 12, 20, 12, 23, WOOD, 1)
    return cv.finish()


def coin():
    cv = C()
    for i, (x, y) in enumerate(((9, 15), (15, 14), (12, 10))):
        cv.paint(ell(x, y, 5, 4), GOLD, fn=lambda px, py, c, x=x, y=y: sh(c, 0.8) if abs(math.hypot(px - x, (py - y) * 1.2) - 2.5) < 0.6 else None)
    return cv.finish()


def build():
    I = {}
    # food
    I["it_apple"] = round_food("#c83a2a", stem="#5a3a22", leaf="#5a9a34", spot=True)
    I["it_berries"] = berries()
    I["it_bread"] = bread()
    I["it_cheese"] = cheese()
    I["it_egg"] = round_food("#eee4d0", r=6)
    I["it_fish"] = fish()
    I["it_meat"] = meat()
    I["it_sausage"] = sausage()
    I["it_pie"] = pie()
    I["it_mushroom"] = mushroom()
    I["it_nut"] = round_food("#a8784a", r=5, stem="#6a4a2a")
    I["it_root"] = root()
    I["it_herb"] = herb()
    I["it_honey"] = pot("#e0a830", neck=6, body=7, lip="#8a5a2a")
    I["it_flour"] = sack_icon("#e8e0d0")
    I["it_wheat"] = wheat()
    I["it_seeds"] = seeds()
    I["it_bowl_food"] = bowl(True)
    I["it_bowl"] = bowl(False)
    # drink and containers
    I["it_ale"] = mug()
    I["it_cup"] = cup()
    I["it_jug"] = pot("#b8683a", neck=5, body=7, handle_r=True)
    I["it_jar"] = pot("#c8a070", neck=7, body=7)
    I["it_pot"] = cookpot()
    I["it_bottle"] = pot("#4a8a5a", neck=3, body=6, lip="#8a5a2a")
    I["it_flask"] = pot("#7ab8c8", neck=3, body=7)
    I["it_potion"] = potion()
    I["it_bucket"] = bucket()
    I["it_basket"] = basket()
    I["it_bag"] = sack_icon("#a8845a")
    # tools and weapons
    for k in ("axe", "pick", "hammer", "hoe", "spade", "saw", "sickle", "knife", "sword", "spear", "bow", "broom", "fork", "needle", "spindle", "torch", "snare"):
        I["it_" + k] = tool(k)
    I["it_stick"] = stick()
    I["it_rope"] = rope()
    I["it_thread"] = rope("#e8dcc0", thin=True)
    I["it_cloth"] = cloth()
    I["it_pelt"] = pelt()
    I["it_bandage"] = rope("#f0ece0")
    # clothing and armour
    I["it_tunic"] = garment("#a8322a", "tunic", trim=GOLD)
    I["it_trousers"] = garment("#5a4a3a", "trousers")
    I["it_cloak"] = garment("#3a5a8a", "cloak")
    I["it_cap"] = garment("#8a3a2a", "cap", trim="#e8dcc0")
    I["it_boots"] = garment("#6a4428", "boots")
    I["it_belt"] = belt()
    I["it_helmet"] = helmet()
    I["it_mail"] = mail()
    I["it_shield"] = shield_icon() if False else shield_icon()
    # materials
    I["it_ore"] = ore()
    I["it_ingot"] = ingot()
    I["it_gem"] = gem()
    I["it_coin"] = coin()
    I["it_bone"] = bone()
    I["it_fang"] = fang()
    I["it_horn"] = horn()
    I["it_feather"] = feather()
    I["it_scale"] = scale_icon()
    I["it_hay"] = hay()
    I["it_carcass"] = carcass()
    I["it_organ"] = round_food("#9a2a3a", r=6)
    # culture
    I["it_book"] = book()
    I["it_scroll"] = scroll()
    I["it_parchment"] = parchment()
    I["it_candle"] = candle()
    I["it_charm"] = charm()
    I["it_idol"] = idol()
    I["it_artifact"] = artifact()
    I["it_crown"] = crown()
    I["it_comb"] = comb()
    I["it_dice"] = dice()
    I["it_doll"] = doll()
    I["it_toy"] = toy()
    I["it_drum"] = drum()
    I["it_flute"] = flute()
    I["it_pipe"] = pipe()
    I["it_spoon"] = spoon()
    return I


def berries():
    cv = C()
    cv.paint(cap(6, 4, 14, 10, 0.6), "#3a6a2a", rim=1)
    for x, y in ((9, 13), (14, 12), (11, 17), (16, 17), (7, 18), (13, 21)):
        cv.paint(ell(x, y, 3, 3), "#8a2a5a")
        cv.dot(x - 1, y - 1, "#d870a0")
    return cv.finish()


def bread():
    cv = C()
    cv.paint(ell(12, 14, 10, 6.5), "#c88a40")
    for x in (7, 11, 15):
        cv.paint(cap(x, 11, x + 3, 15, 0.6), "#e8c070", flat=True)
    return cv.finish()


def cheese():
    cv = C()
    cv.paint(poly([(3, 17), (20, 8), (21, 19), (3, 21)]), "#e8c050")
    for x, y in ((9, 17), (15, 15), (17, 19)):
        cv.paint(ell(x, y, 1.4, 1.2), "#b88a20", flat=True)
    return cv.finish()


def fish():
    cv = C()
    cv.paint(OR(ell(11, 12, 8, 4.5), poly([(17, 12), (23, 7), (22, 17)])), "#7a9ab0", fn=lambda x, y, c: "#d8e0e8" if y > 13.5 and x < 17 else None)
    cv.dot(6, 11, "#1a1a1a")
    return cv.finish()


def meat():
    cv = C()
    cv.paint(ell(11, 12, 8, 6.5), "#b8443a", fn=lambda x, y, c: "#e8c8b0" if math.hypot(x - 11, y - 12) > 6 else None)
    cv.paint(OR(cap(16, 16, 21, 21, 1.4), ell(21, 21, 1.8, 1.8), ell(22, 19.5, 1.6, 1.6)), "#eee4d0", rim=1)
    return cv.finish()


def sausage():
    cv = C()
    cv.paint(SUB(ell(12, 14, 9, 8), ell(12, 16, 6, 5)), "#9a4a3a")
    cv.paint(cap(3, 13, 2, 10, 0.6), "#c8b080", rim=1)
    return cv.finish()


def pie():
    cv = C()
    cv.paint(ell(12, 15, 10, 5), "#a86a2a")
    cv.paint(ell(12, 13, 9, 4), "#d8a050", under=False, fn=lambda x, y, c: "#8a2a3a" if (int(x) + int(y)) % 5 == 0 else None)
    return cv.finish()


def mushroom():
    cv = C()
    cv.paint(rect(10, 12, 15, 21), "#e8dcc0")
    cv.paint(SUB(ell(12, 12, 9, 7), rect(0, 13, 24, 24)), "#b8382a")
    for x, y in ((8, 9), (13, 7), (16, 10)):
        cv.dot(x, y, "#f0ece0", 2, 1)
    return cv.finish()


def root():
    cv = C()
    cv.paint(poly([(8, 7), (16, 7), (13, 21), (11, 21)]), "#d88a3a", fn=lambda x, y, c: sh(c, 0.8) if int(y) % 4 == 0 else None)
    for dx in (-3, 0, 3):
        cv.paint(cap(12, 7, 12 + dx, 2, 0.8), "#5a9a34", rim=1)
    return cv.finish()


def herb():
    cv = C()
    cv.paint(cap(12, 22, 12, 8, 0.6), "#4a7a2a", rim=1)
    for i, (x, y, a) in enumerate(((8, 9, -1), (16, 8, 1), (9, 15, -1), (15, 14, 1), (12, 4, 0))):
        cv.paint(ell(x, y, 3.2, 1.8), "#5aa03a")
    cv.paint(rect(10, 18, 14, 20), "#c8402a", flat=True)
    return cv.finish()


def sack_icon(col):
    cv = C()
    cv.paint(OR(ell(12, 15, 8, 7), poly([(9, 9), (15, 9), (16, 4), (8, 4)])), col)
    cv.paint(cap(8, 8, 16, 8, 1), "#6a4a2a", rim=1)
    return cv.finish()


def wheat():
    cv = C()
    for dx, top in ((-4, 4), (0, 2), (4, 5)):
        cv.paint(cap(12 + dx, top + 8, 12, 22, 0.5), "#c8a040", rim=1)
        cv.paint(ell(12 + dx, top + 4, 2, 5), "#e0b850", fn=lambda x, y, c: sh(c, 0.8) if int(y) % 2 else None)
    cv.paint(rect(9, 16, 15, 18), "#8a5a2a", flat=True)
    return cv.finish()


def seeds():
    cv = C()
    rnd = random.Random(3)
    for _ in range(9):
        x, y = rnd.uniform(5, 19), rnd.uniform(8, 19)
        cv.paint(ell(x, y, 1.8, 1.2), "#a8844a")
    return cv.finish()


def bowl(full):
    cv = C()
    cv.paint(SUB(ell(12, 12, 10, 9), rect(0, 0, 24, 12)), "#a8784a")
    cv.paint(ell(12, 12, 10, 3), "#5a3a22" if not full else "#c89040", under=False,
             fn=(lambda x, y, c: "#8a5a2a" if (int(x) + int(y)) % 4 == 0 else None) if full else None)
    return cv.finish()


def mug():
    cv = C()
    cv.paint(rect(6, 6, 16, 21), "#8a5a34", fn=lambda x, y, c: "#5a5a5a" if int(y) in (9, 17) else None)
    cv.paint(SUB(ell(17, 13, 4, 4), ell(17, 13, 2, 2)), "#8a5a34")
    cv.paint(ell(11, 6, 5, 2), "#f0e8c0", under=False)
    return cv.finish()


def cup():
    cv = C()
    cv.paint(OR(SUB(ell(12, 8, 9, 7), rect(0, 0, 24, 8)), rect(11, 14, 13, 19), ell(12, 20, 5, 1.5)), "#b8683a",
             fn=lambda x, y, c: "#2a2018" if 9 < y < 12 else None)   # a kylix with a black band
    cv.paint(ell(12, 8, 9, 2), "#2a1a10", under=False)
    return cv.finish()


def cookpot():
    cv = C()
    cv.paint(SUB(ell(12, 12, 9, 8), rect(0, 0, 24, 9)), "#3a3a3e")
    cv.paint(ell(12, 9, 9, 2.5), "#6a4a2a", under=False)
    cv.paint(SUB(ell(12, 8, 8, 7), ell(12, 8, 7, 6)), "#5a5a5e", flat=True)
    return cv.finish()


def potion():
    cv = C()
    cv.paint(OR(ell(12, 15, 7, 7), rect(10, 3, 14, 9)), "#c8e0e8")
    cv.paint(AND(ell(12, 15, 6, 6), lambda x, y: y > 13), "#a83aa8", flat=True)
    cv.paint(rect(10, 2, 14, 4), "#8a5a2a")
    cv.dot(9, 12, "#ffffff")
    return cv.finish()


def bucket():
    cv = C()
    cv.paint(poly([(5, 8), (19, 8), (17, 21), (7, 21)]), "#8a5a34", fn=lambda x, y, c: "#5a5a5a" if int(y) in (10, 18) else None)
    cv.paint(ell(12, 8, 7, 2), "#3a6a9a", under=False)
    cv.paint(SUB(ell(12, 8, 8, 7), OR(ell(12, 8, 7, 6), rect(0, 8, 24, 24))), "#5a5a5a", flat=True)
    return cv.finish()


def basket():
    cv = C()
    cv.paint(poly([(4, 10), (20, 10), (18, 21), (6, 21)]), "#c8a060", fn=lambda x, y, c: sh(c, 0.75) if (int(x) + int(y) * 2) % 4 == 0 else None)
    cv.paint(SUB(ell(12, 10, 7, 7), OR(ell(12, 10, 5.6, 5.6), rect(0, 10, 24, 24))), "#a8803a", flat=True)
    return cv.finish()


def stick():
    cv = C()
    handle(cv, 4, 20, 20, 4, "#7a5232", 1.4)
    cv.paint(cap(12, 12, 15, 15, 0.7), "#7a5232", rim=1)
    return cv.finish()


def rope(col="#c8a868", thin=False):
    cv = C()
    r = 1.0 if thin else 1.6
    for k in range(3):
        cv.paint(SUB(ell(12, 12 + k * 0.5, 8 - k * 2.4, 6 - k * 1.8), ell(12, 12 + k * 0.5, 8 - k * 2.4 - r * 1.5, 6 - k * 1.8 - r * 1.5)), col,
                 fn=lambda x, y, c: sh(c, 0.75) if (int(x) + int(y)) % 3 == 0 else None)
    return cv.finish()


def cloth():
    cv = C()
    for i, c in enumerate(("#3a5a8a", "#a8322a", "#e8dcc0")):
        cv.paint(rect(4, 6 + i * 5, 20, 11 + i * 5), c)
    return cv.finish()


def pelt():
    cv = C()
    cv.paint(poly([(6, 3), (18, 3), (21, 8), (19, 14), (21, 21), (14, 18), (10, 18), (3, 21), (5, 14), (3, 8)]), "#9a6a3a",
             fn=lambda x, y, c: sh(c, 0.82) if (int(x) * 3 + int(y)) % 5 == 0 else None)
    return cv.finish()


def belt():
    cv = C()
    cv.paint(SUB(ell(12, 12, 10, 7), ell(12, 12, 8, 5)), LEATHER)
    cv.paint(rect(16, 15, 21, 20), BRONZE)
    return cv.finish()


def helmet():
    cv = C()
    cv.paint(OR(SUB(ell(12, 13, 9, 10), rect(0, 14, 24, 24)), rect(3, 13, 7, 21), rect(17, 13, 21, 21)), BRONZE)
    cv.paint(rect(11, 12, 13, 20), BRONZE)
    cv.paint(poly([(4, 4), (12, 0), (20, 4), (12, 3)]), "#a8322a", fn=lambda x, y, c: sh(c, 0.7) if int(x) % 2 else None)
    return cv.finish()


def mail():
    cv = C()
    cv.paint(poly([(6, 3), (18, 3), (21, 8), (19, 21), (5, 21), (3, 8)]), "#9aa3ab", fn=lambda x, y, c: "#6d767d" if (int(x) + int(y)) % 2 else None)
    return cv.finish()


def shield_icon():
    cv = C()
    cv.paint(ell(12, 12, 10, 10), "#a8322a")
    cv.paint(SUB(ell(12, 12, 10, 10), ell(12, 12, 8.6, 8.6)), BRONZE, flat=True)
    cv.paint(ell(12, 12, 2.5, 2.5), GOLD)
    return cv.finish()


def ore():
    cv = C()
    cv.paint(blob(12, 13, 8, 4, lobes=4, amp=0.2), "#6a6460", fn=lambda x, y, c: "#c86a3a" if (int(x) * 3 + int(y) * 5) % 9 == 0 else None)
    return cv.finish()


def ingot():
    cv = C()
    cv.paint(poly([(3, 18), (6, 10), (18, 10), (21, 18)]), "#9aa0a8")
    cv.paint(poly([(6, 10), (18, 10), (16, 12), (8, 12)]), "#c8ccd4", flat=True)
    return cv.finish()


def gem():
    cv = C()
    cv.paint(poly([(5, 9), (9, 4), (15, 4), (19, 9), (12, 20)]), "#3aa8c8", fn=lambda x, y, c: sh(c, 1.3) if y < 9 and x < 12 else None)
    cv.paint(cap(5, 9, 19, 9, 0.4), "#a8e8f8", flat=True)
    return cv.finish()


def bone():
    cv = C()
    cv.paint(OR(cap(6, 18, 18, 6, 1.6), ell(5, 17, 2, 2), ell(7, 20, 2, 2), ell(17, 4, 2, 2), ell(20, 7, 2, 2)), "#eee4d0")
    return cv.finish()


def fang():
    cv = C()
    cv.paint(poly([(8, 3), (16, 3), (14, 12), (12, 21), (10, 12)]), "#eee4d0")
    return cv.finish()


def horn():
    cv = C()
    cv.paint(OR(*[cap(4 + i * 1.6, 20 - i * 1.6 - i * i * 0.08, 5.6 + i * 1.6, 18.4 - i * 1.6 - i * i * 0.08, 3.2 - i * 0.28) for i in range(10)]), "#c8b090",
             fn=lambda x, y, c: sh(c, 0.85) if int(x + y) % 3 == 0 else None)
    return cv.finish()


def feather():
    cv = C()
    cv.paint(poly([(18, 2), (21, 5), (8, 19), (5, 18)]), "#e8e0d0", fn=lambda x, y, c: "#3a6aa8" if y < 7 else None)
    cv.paint(cap(4, 21, 19, 4, 0.4), "#8a7a6a", flat=True)
    return cv.finish()


def scale_icon():
    cv = C()
    for x, y in ((8, 9), (16, 9), (12, 15)):
        cv.paint(SUB(ell(x, y, 5, 5), rect(0, y + 3, 24, 24)), "#4a8a6a", fn=lambda px, py, c: sh(c, 1.25) if py < y - 2 else None)
    return cv.finish()


def hay():
    cv = C()
    cv.paint(SUB(ell(12, 16, 10, 12), rect(0, 20, 24, 24)), "#d8b850", fn=lambda x, y, c: sh(c, 0.8) if (int(x) * 2 + int(y)) % 5 == 0 else None)
    return cv.finish()


def carcass():
    cv = C()
    cv.paint(ell(12, 14, 9, 5), "#8a6a4a")
    cv.paint(ell(20, 12, 3, 2.5), "#8a6a4a")
    for x in (7, 11, 15):
        cv.paint(cap(x, 17, x + 1, 22, 0.8), "#6a4a3a", rim=1)
    return cv.finish()


def book():
    cv = C()
    cv.paint(rect(5, 4, 19, 20), "#7a2a2a")
    cv.paint(rect(7, 18, 19, 21), "#e8e0d0", flat=True)
    cv.paint(rect(9, 8, 16, 11), GOLD, flat=True)
    return cv.finish()


def scroll():
    cv = C()
    cv.paint(rect(6, 5, 18, 19), "#e8dcc0", fn=lambda x, y, c: "#8a7a5a" if int(y) % 3 == 0 and 8 < x < 16 else None)
    cv.paint(OR(cap(4, 5, 20, 5, 1.8), cap(4, 19, 20, 19, 1.8)), "#c8b890", rim=1)
    return cv.finish()


def parchment():
    cv = C()
    cv.paint(poly([(5, 3), (19, 4), (18, 21), (4, 20)]), "#e0d0a8", fn=lambda x, y, c: "#8a7a5a" if int(y) % 3 == 0 and 7 < x < 16 else None)
    return cv.finish()


def candle():
    cv = C()
    cv.paint(rect(9, 9, 15, 21), "#eee4c8")
    cv.paint(ell(12, 21, 6, 1.6), "#b8873a")
    cv.paint(poly([(11, 8), (12, 3), (13, 8)]), "#f8c040", flat=True)
    return cv.finish()


def charm():
    cv = C()
    cv.paint(cap(12, 2, 12, 8, 0.4), "#8a6a4a", flat=True)
    cv.paint(poly([(12, 8), (19, 14), (12, 21), (5, 14)]), BRONZE)
    cv.paint(ell(12, 14, 2.2, 2.2), "#3a6ac0", flat=True)
    return cv.finish()


def idol():
    cv = C()
    cv.paint(OR(ell(12, 6, 4, 4), poly([(8, 9), (16, 9), (18, 21), (6, 21)])), "#a8845a")
    cv.dot(10, 5, "#2a1a10")
    cv.dot(13, 5, "#2a1a10")
    return cv.finish()


def artifact():
    cv = C()
    cv.paint(ell(12, 12, 8, 8), "#5a3a8a")
    cv.paint(ell(10, 10, 3, 3), "#c8a8f0", flat=True)
    for a in range(0, 360, 45):
        r = math.radians(a)
        cv.dot(12 + math.cos(r) * 10, 12 + math.sin(r) * 10, GOLD)
    return cv.finish()


def crown():
    cv = C()
    cv.paint(poly([(4, 19), (4, 7), (8, 12), (12, 5), (16, 12), (20, 7), (20, 19)]), GOLD)
    cv.paint(ell(12, 15, 2, 2), "#c83a2a", flat=True)
    return cv.finish()


def comb():
    cv = C()
    cv.paint(rect(4, 6, 20, 11), "#e8dcc0")
    for x in range(5, 20, 2):
        cv.paint(rect(x, 11, x + 1, 19), "#d8ccb0", flat=True)
    return cv.finish()


def dice():
    cv = C()
    cv.paint(rect(5, 9, 15, 19), "#eee4d0")
    cv.paint(rect(5, 6, 15, 9), "#ffffff", flat=True)
    cv.paint(poly([(15, 6), (19, 4), (19, 16), (15, 19)]), "#c8c0b0")
    for x, y in ((8, 12), (12, 16), (10, 14)):
        cv.dot(x, y, "#2a1a10")
    return cv.finish()


def doll():
    cv = C()
    cv.paint(OR(ell(12, 6, 4, 4), poly([(8, 10), (16, 10), (19, 21), (5, 21)])), "#c8a070")
    cv.paint(poly([(8, 11), (16, 11), (19, 21), (5, 21)]), "#a8322a")
    return cv.finish()


def toy():
    cv = C()
    cv.paint(OR(ell(11, 12, 7, 4), cap(16, 10, 20, 5, 2)), "#a87a4a")
    for x in (7, 15):
        cv.paint(ell(x, 18, 2.4, 2.4), "#5a3a22")
    return cv.finish()


def drum():
    cv = C()
    cv.paint(rect(4, 8, 20, 19), "#a8322a", fn=lambda x, y, c: "#e8dcc0" if (int(x) - int(y)) % 6 == 0 else None)
    cv.paint(ell(12, 8, 8, 2.5), "#e8dcc0", under=False)
    return cv.finish()


def flute():
    cv = C()
    cv.paint(cap(3, 20, 21, 4, 1.4), "#c8a868", rim=1)
    for i in range(4):
        cv.dot(8 + i * 3, 15 - i * 3, "#3a2a1a")
    return cv.finish()


def pipe():
    cv = C()
    cv.paint(cap(3, 9, 15, 13, 0.9), "#6a4a2a", rim=1)
    cv.paint(OR(rect(14, 9, 20, 18), ell(17, 18, 3, 1.5)), "#8a5a34")
    cv.paint(ell(17, 9, 3, 1), "#2a1a10", under=False)
    return cv.finish()


def spoon():
    cv = C()
    handle(cv, 4, 20, 14, 10, WOOD, 1.0)
    cv.paint(ell(16, 7, 4, 5), WOOD)
    cv.paint(ell(16, 7, 2.4, 3.2), sh(rgb(WOOD), 0.7), flat=True)
    return cv.finish()


def all_icons():
    return build()
