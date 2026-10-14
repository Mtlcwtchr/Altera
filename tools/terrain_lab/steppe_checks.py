"""The canyon steppe's own checks (doc/BIOME_PLAN.md 3.3, common ones in 1.5),
run by world_features.py on what it has placed:

- spires do not hang: no point of the base's rim lies lower than the spire's
  foot (`ground`) by more than `hang_m`, and the ground under the base is not
  steeper than `slope_max` (rise over the base's width);
- bridges do not run through spires: the middle of a span (|t| < 0.45 of its
  half length; the rest is where it lands) passes no spire's shaft below that
  spire's top;
- a bridge's ends land: each outer part of the span meets a spire's shaft
  (up to 40 m under its top), a spire's platform at the deck's height, or
  ground no more than `land_m` below the deck (or above it: into the wall);
- the mesa pockets' share per top, from steppe_ground.py's report.
"""

import json
import math
from pathlib import Path

import numpy as np

SPIRES = ("spire",)
BRIDGES = ("bridge", "ribbon_bridge")


def _radius(m, s):
    return max(abs(m["lo"][0]), abs(m["lo"][1]), abs(m["hi"][0]), abs(m["hi"][1])) * s


def run(world, items, catalogue, stem, hang_m=0.3, slope_max=0.35, land_m=25.0):
    spires = []
    for o in items:
        m = catalogue.get(o["m"])
        if not m or m["kind"] not in SPIRES:
            continue
        if "ground" not in o:
            # A column of the canyon islands: from `z` (below the lowest
            # ground round it) up its own height.
            o = dict(o, ground=o["z"], top=o["z"] + m["hi"][2] * o["s"][2])
        else:
            o = dict(o, main=True)
        spires.append((o, _radius(m, o["s"][0])))
    platforms = [(o, _radius(catalogue[o["m"]], o["s"][0])) for o in items
                 if o["m"] in catalogue and catalogue[o["m"]]["kind"] == "spire_platform"]
    hanging, steep, worst_gap, worst_slope = 0, 0, 0.0, 0.0
    for o, r in spires:
        foot = r * 0.55   # the shaft's foot, not the widest boulder of the cap
        rim = [world.height(o["x"] + foot * math.cos(a), o["y"] + foot * math.sin(a))
               for a in np.linspace(0, 2 * math.pi, 16, endpoint=False)]
        gap = o["ground"] - min(rim)
        worst_gap = max(worst_gap, gap)
        hanging += gap > hang_m
        slope = (max(rim) - min(rim)) / (2 * foot)
        # Only the canyon floor's own spires must stand on gentle ground: the
        # islands' rim rock stands on the brink by make, its foot down the wall.
        if o.get("main", False):
            worst_slope = max(worst_slope, slope)
            steep += slope > slope_max

    through, unlanded, bridges = 0, 0, 0
    for o in items:
        m = catalogue.get(o["m"])
        if not m or m["kind"] not in BRIDGES:
            continue
        bridges += 1
        half = max(abs(m["lo"][0]), abs(m["hi"][0])) * o["s"][0]
        ux, uy = math.cos(o["yaw"]), math.sin(o["yaw"])
        z = o["z"]
        # A span's mesh runs on past the spires it stands on, so the outer
        # part of each half is where it lands; only its middle must be clear.
        hit = False
        landed = [False, False]
        for sp, r in spires:
            dx, dy = sp["x"] - o["x"], sp["y"] - o["y"]
            t = (dx * ux + dy * uy) / half
            d = abs(-dx * uy + dy * ux)
            if d > r or not (sp["ground"] - 5 < z < sp["top"] + 40):
                continue
            if abs(t) < 0.45 and d < r * 0.5 and z < sp["top"]:
                hit = True
            if abs(t) >= 0.45:
                landed[t > 0] = True
        for side, sign in ((0, -1), (1, 1)):
            if landed[side]:
                continue
            for f in np.linspace(0.45, 1.0, 12):
                px, py = o["x"] + sign * ux * half * f, o["y"] + sign * uy * half * f
                # Ground up to the deck or over it (the span runs into the
                # wall), or a spire's platform at the deck's height.
                if world.height(px, py) > z - land_m or any(
                        math.hypot(q["x"] - px, q["y"] - py) < rq + 40 and abs(q["z"] - z) < 60 for q, rq in platforms):
                    landed[side] = True
                    break
        through += hit
        unlanded += 2 - sum(landed)

    out = {"spires": len(spires), "hanging": int(hanging), "worst_gap_m": round(worst_gap, 1),
           "steep_base": int(steep), "worst_base_slope": round(worst_slope, 2), "slope_max": slope_max,
           "bridges": bridges, "through_spire": int(through), "ends_not_landed": int(unlanded)}
    report = Path(str(stem) + "_steppe_ground.json")
    if report.exists():
        ground = json.loads(report.read_text(encoding="utf-8"))
        out["mesa_pocket"] = ground["layers"].get("mesa_pocket")
        out["ground_ok"] = ground.get("ok")
    else:
        out["mesa_pocket"] = "no steppe ground: run tools/terrain_lab/steppe_ground.py"
    out["ok"] = bool(hanging == 0 and steep == 0 and through == 0 and out.get("ground_ok", False))
    return out
