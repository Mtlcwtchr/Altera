#!/usr/bin/env python3
"""AL-109: 16x16 pixel icons of the workshop's processes and actions.

Writes sprites `pr_<process>` and `ac_<action>` into content/sprites_2d/craft2d.json
("sprites", letters of the sprites.json palette, '.' empty) and the maps
"process_icons" / "action_icons" (id -> sprite). The 2D window and the page draw
the process ring and the action buttons with them, the names go to the tooltip.
Idempotent: rerun after a change here.  python tools/content_gen/gen_craft_icons.py
"""
import json
import math
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PATH = os.path.join(ROOT, "content", "sprites_2d", "craft2d.json")
N = 16


class C:
    def __init__(self):
        self.g = [["." for _ in range(N)] for _ in range(N)]

    def px(self, x, y, c):
        if 0 <= x < N and 0 <= y < N:
            self.g[y][x] = c

    def line(self, x0, y0, x1, y1, c, w=1):
        n = max(int(round(max(abs(x1 - x0), abs(y1 - y0)))), 1)
        for i in range(n + 1):
            x = round(x0 + (x1 - x0) * i / n)
            y = round(y0 + (y1 - y0) * i / n)
            for dx in range(w):
                for dy in range(w):
                    self.px(x + dx, y + dy, c)

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def disc(self, cx, cy, r, c, ry=None):
        ry = r if ry is None else ry
        for y in range(N):
            for x in range(N):
                if ((x + 0.5 - cx) / r) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1.0:
                    self.px(x, y, c)

    def ring(self, cx, cy, r, c, t=1.2):
        for y in range(N):
            for x in range(N):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if r - t <= d <= r:
                    self.px(x, y, c)

    def outline(self):
        out = [row[:] for row in self.g]
        for y in range(N):
            for x in range(N):
                if self.g[y][x] != ".":
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    qx, qy = x + dx, y + dy
                    if 0 <= qx < N and 0 <= qy < N and self.g[qy][qx] not in ".k":
                        out[y][x] = "k"
                        break
        self.g = out
        return self

    def rows(self):
        return ["".join(r) for r in self.g]


def flame(c, cx, cy, s=1.0):
    c.disc(cx, cy, 3.2 * s, "f", 4 * s)
    c.line(cx, cy - 5 * s, cx, cy - 2, "f")
    c.disc(cx, cy + 0.8, 1.6 * s, "F", 2.2 * s)


def hammer(c, hx, hy):
    c.line(hx - 5, hy + 6, hx + 1, hy, "n", 2)
    c.rect(hx - 1, hy - 4, hx + 5, hy - 1, "M")
    c.rect(hx - 1, hy - 4, hx + 5, hy - 4, "m")


ICONS = {}


def icon(name):
    def deco(f):
        ICONS[name] = f
        return f
    return deco


@icon("pr_heat")
def _(c):
    c.rect(3, 12, 12, 13, "n"); c.line(3, 13, 12, 11, "N")
    flame(c, 7.5, 7)


@icon("pr_smelt")
def _(c):
    flame(c, 7.5, 12.5, 0.6)
    c.disc(8, 6.5, 5, "M", 3.6); c.rect(3, 3, 12, 5, "."); c.disc(8, 5, 4.2, "f", 1.2)
    c.rect(4, 5, 11, 5, "F")


@icon("pr_cut")
def _(c):
    c.line(3, 12, 6, 9, "n", 2)
    c.line(7, 8, 13, 2, "m", 2); c.line(8, 9, 13, 4, "M")


@icon("pr_carve")
def _(c):
    c.line(2, 13, 7, 8, "n", 2)
    c.line(8, 7, 11, 4, "m", 2); c.px(12, 3, "m")
    c.px(12, 9, "t"); c.px(13, 11, "t"); c.px(10, 12, "T"); c.px(14, 8, "T")


@icon("pr_knap")
def _(c):
    for y in range(4, 13):
        w = 4 - abs(y - 8) // 1 * 0.8
        c.line(round(7 - w), y, round(7 + w), y, "W")
    c.line(5, 6, 9, 10, "x"); c.px(12, 4, "w"); c.px(13, 6, "g"); c.px(11, 2, "g")


@icon("pr_hammer")
def _(c):
    hammer(c, 8, 7)
    c.px(13, 10, "F"); c.px(14, 12, "f"); c.px(12, 13, "F")


@icon("pr_quench")
def _(c):
    c.disc(8, 10, 4, "u"); c.line(8, 3, 8, 7, "u", 1); c.line(7, 5, 9, 5, "u")
    c.disc(7, 9.5, 1.5, "U"); c.px(3, 2, "w"); c.px(4, 1, "w"); c.px(12, 2, "w"); c.px(13, 1, "w")


@icon("pr_link")
def _(c):
    c.ring(5.5, 8, 3.6, "m", 1.4); c.ring(10.5, 8, 3.6, "M", 1.4)


@icon("pr_grind")
def _(c):
    c.disc(7, 8, 5.5, "W"); c.ring(7, 8, 5.5, "g", 1); c.disc(7, 8, 1.2, "N")
    c.px(13, 5, "F"); c.px(14, 3, "f"); c.px(13, 2, "F"); c.px(15, 6, "F")


@icon("pr_mill")
def _(c):
    c.disc(8, 10, 6, "W", 2.5); c.disc(8, 6.5, 5.5, "g", 2.3); c.line(8, 1, 8, 5, "n", 2)
    c.rect(2, 13, 13, 14, "w")


@icon("pr_soak")
def _(c):
    c.rect(3, 6, 12, 13, "n"); c.rect(4, 6, 11, 8, "u"); c.rect(4, 7, 11, 7, "U")
    c.line(3, 10, 12, 10, "N")


@icon("pr_dry")
def _(c):
    c.disc(8, 8, 3.4, "y")
    for a in range(8):
        t = a * math.pi / 4
        c.px(round(7.5 + 5.6 * math.cos(t)), round(7.5 + 5.6 * math.sin(t)), "o")


@icon("pr_ferment")
def _(c):
    c.disc(8, 10, 5, "e", 4.5); c.rect(6, 3, 9, 6, "e"); c.rect(5, 3, 10, 3, "E")
    c.px(6, 10, "w"); c.px(9, 8, "w"); c.px(10, 11, "w"); c.px(7, 12, "w")


@icon("pr_twist")
def _(c):
    for i in range(28):   # two strands round each other, a cord from corner to corner
        t = i / 27
        x, y = 2 + 11 * t, 13 - 11 * t
        o = 1.6 * math.sin(t * 4 * math.pi)
        c.px(round(x + o), round(y + o), "t")
        c.px(round(x - o), round(y - o), "T")


@icon("pr_weave")
def _(c):
    for y in range(3, 13):
        for x in range(3, 13):
            c.px(x, y, "t" if (x // 2 + y // 2) % 2 else "T")


@icon("pr_sew")
def _(c):
    c.line(3, 13, 12, 3, "m"); c.px(12, 2, "m"); c.px(11, 3, ".")
    for i, (x, y) in enumerate([(12, 4), (13, 6), (12, 8), (10, 9), (8, 11), (9, 13), (11, 14)]):
        c.px(x, y, "r")


@icon("pr_join")
def _(c):
    c.rect(1, 6, 8, 10, "n"); c.rect(8, 4, 14, 8, "o"); c.rect(7, 7, 8, 8, "N")
    c.line(1, 8, 7, 8, "N")


@icon("pr_mix")
def _(c):
    c.disc(8, 10, 6, "e", 3.5); c.rect(2, 6, 14, 8, "."); c.rect(3, 9, 13, 9, "t")
    c.line(10, 9, 13, 2, "n", 1)


@icon("pr_mould")
def _(c):
    c.rect(2, 6, 13, 12, "x"); c.rect(4, 7, 11, 9, "f"); c.rect(4, 8, 11, 8, "F")


@icon("pr_tan")
def _(c):
    c.disc(8, 8, 5.5, "o", 6); c.rect(1, 3, 2, 4, "o"); c.rect(13, 3, 14, 4, "o")
    c.rect(1, 11, 2, 12, "o"); c.rect(13, 11, 14, 12, "o"); c.disc(8, 8, 2.5, "z", 3)


@icon("pr_alloy")
def _(c):
    c.rect(1, 9, 7, 12, "m"); c.rect(9, 9, 14, 12, "o"); c.rect(5, 3, 10, 6, "y")
    c.line(4, 8, 6, 7, "F"); c.line(11, 8, 9, 7, "F")


@icon("pr_repair")
def _(c):
    hammer(c, 7, 8)
    c.rect(11, 10, 14, 11, "G"); c.rect(12, 9, 13, 12, "G")


# AL-117: alchemy and runes
@icon("pr_distill")
def _(c):
    c.disc(6, 9, 4, "u"); c.disc(6, 9, 2, "U")
    c.line(8, 5, 14, 3, "M"); c.line(14, 3, 14, 10, "M")
    c.rect(5, 3, 7, 5, "M")
    flame(c, 6, 14.5, 0.5)


@icon("pr_calcine")
def _(c):
    c.rect(4, 3, 11, 9, "z"); c.rect(5, 3, 10, 4, "W")
    flame(c, 7.5, 12.5, 0.8)


@icon("pr_dissolve")
def _(c):
    c.rect(2, 9, 13, 13, "n"); c.rect(3, 9, 12, 11, "u")
    c.disc(8, 5, 2, "W"); c.px(8, 8, "u"); c.px(6, 7, "u")


@icon("pr_scrape")
def _(c):
    c.rect(2, 4, 11, 13, "e")
    c.line(9, 2, 14, 7, "W", 2); c.line(12, 8, 14, 10, "n")


@icon("pr_inscribe")
def _(c):
    c.rect(2, 2, 13, 13, "W")
    c.line(7, 4, 7, 12, "H"); c.line(7, 4, 11, 7, "H"); c.line(7, 8, 11, 12, "H")


@icon("pr_bind")
def _(c):
    c.ring(8, 8, 7, "u")
    c.line(8, 2, 8, 14, "H"); c.line(2, 8, 14, 8, "H"); c.line(4, 4, 12, 12, "H"); c.line(12, 4, 4, 12, "H")
    c.disc(8, 8, 1.6, "y")


@icon("ac_make")
def _(c):
    c.rect(2, 9, 13, 10, "M"); c.rect(5, 11, 10, 12, "M"); c.rect(3, 13, 12, 14, "x")
    c.rect(2, 9, 13, 9, "m")
    c.line(8, 7, 12, 2, "n", 1); c.rect(10, 1, 14, 3, "W")
    c.px(6, 7, "F"); c.px(4, 6, "f")


@icon("ac_clear")
def _(c):
    c.line(3, 3, 12, 12, "r", 2); c.line(12, 3, 3, 12, "r", 2)


@icon("ac_inspect")
def _(c):
    c.ring(6.5, 6.5, 4.6, "m", 1.5); c.disc(6.5, 6.5, 3.1, "U")
    c.px(5, 5, "w"); c.line(10, 10, 13, 13, "n", 2)


@icon("ac_repair")
def _(c):
    ICONS["pr_repair"](c)


@icon("ac_to_bench")
def _(c):
    c.rect(2, 11, 13, 12, "n"); c.rect(3, 13, 3, 14, "N"); c.rect(12, 13, 12, 14, "N")
    c.rect(7, 1, 8, 6, "y"); c.line(4, 5, 7, 8, "y"); c.line(11, 5, 8, 8, "y")


@icon("ac_memory")
def _(c):
    c.rect(1, 4, 7, 12, "w"); c.rect(8, 4, 14, 12, "w"); c.rect(7, 3, 8, 13, "n")
    for y in (6, 8, 10):
        c.line(2, y, 5, y, "W"); c.line(10, y, 13, y, "W")


@icon("ac_lay")
def _(c):
    c.rect(2, 3, 9, 13, "t"); c.rect(2, 3, 9, 3, "T"); c.rect(2, 13, 9, 13, "T")
    for y in (6, 8, 10):
        c.line(3, y, 7, y, "p")
    c.line(10, 8, 14, 8, "y"); c.line(12, 6, 14, 8, "y"); c.line(12, 10, 14, 8, "y")


@icon("ac_drink")
def _(c):
    c.rect(7, 1, 9, 2, "n"); c.rect(7, 3, 9, 5, "W")
    c.disc(8, 10, 4.5, "W"); c.disc(8, 10.5, 3.4, "r"); c.px(6, 9, "w")


@icon("ac_rune")
def _(c):
    c.disc(8, 8, 6.5, "z"); c.disc(8, 8, 5.5, "x")
    c.line(6, 3, 6, 13, "y"); c.line(6, 3, 10, 6, "y"); c.line(6, 8, 10, 12, "y")


def main():
    with open(PATH, encoding="utf-8") as f:
        data = json.load(f)
    sprites = data.setdefault("sprites", {})
    for name, draw in ICONS.items():
        c = C()
        draw(c)
        sprites[name] = c.outline().rows()
    procs = [p for p in data.get("processes", {})]
    data["process_icons"] = {p: "pr_" + p for p in procs if "pr_" + p in ICONS}
    data["action_icons"] = {n[3:]: n for n in ICONS if n.startswith("ac_")}
    missing = [p for p in procs if "pr_" + p not in ICONS]
    with open(PATH, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"{len(ICONS)} icons -> {os.path.relpath(PATH, ROOT)}" + (f"; no icon: {missing}" if missing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
