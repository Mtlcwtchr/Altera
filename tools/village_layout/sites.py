"""Synthetic village sites for the planner (AL-84): world configs with a
village_site (heights, water, wood) and a coarse map with forest, for the
headless runner. Usage: python sites.py OUT_DIR"""
import json, math, os, sys

BASE = {"size_m": 12000, "cell_m": 250, "biomes": ["meadow", "forest", "marsh"], "river": False,
        "people_per_family": [4, 6], "traits_per_person": 2, "know_radius_m": 1500, "village_plan": True}
C = 6000.0          # village centre (x, y)
N, CELL = 201, 4.0  # site grid: 800 m square at 4 m


def site(height, wet, wood):
    x0 = y0 = C - (N - 1) * CELL / 2
    h, w, d = [], [], []
    for j in range(N):
        for i in range(N):
            x, y = x0 + i * CELL, y0 + j * CELL
            h.append(int(round(height(x, y) * 10)))
            w.append(1 if wet(x, y) else 0)
            d.append(1 if wood(x, y) else 0)
    return {"x0": x0, "y0": y0, "cell": CELL, "n": N, "h_dm": h, "wet": w, "wood": d}


def coarse(cfg, wet, wood):
    n = int(cfg["size_m"] / cfg["cell_m"])
    bio, wat = [], []
    for j in range(n):
        for i in range(n):
            x, y = (i + 0.5) * cfg["cell_m"], (j + 0.5) * cfg["cell_m"]
            wat.append(1 if wet(x, y) else 0)
            bio.append("forest" if wood(x, y) else "meadow")
    cfg["map_biome"], cfg["map_water"] = bio, wat


def river_x(y):  # a meandering river running north-south east of the centre
    return C + 130 + 45 * math.sin((y - C) / 140.0)


SCENES = {
    # a valley: the river to the east, the ground rising west, woods on the far bank and the western ridge
    "river_valley": dict(families=10,
                         height=lambda x, y: max(0.0, (river_x(y) - x)) * 0.035 + 2 * math.sin(y / 90.0),
                         wet=lambda x, y: abs(x - river_x(y)) < 9,
                         wood=lambda x, y: x > river_x(y) + 40 or x < C - 300),
    # a hillside falling south to a creek, woods up the hill
    "hillside": dict(families=7,
                     height=lambda x, y: (C - y) * 0.05 + 3 * math.sin(x / 70.0),
                     wet=lambda x, y: abs(y - (C + 210 + 20 * math.sin(x / 80.0))) < 6,
                     wood=lambda x, y: y < C - 220),
    # an open plain, a forest to the north, a pond to the south-west
    "plain": dict(families=14,
                  height=lambda x, y: 1.5 * math.sin(x / 120.0) * math.cos(y / 150.0),
                  wet=lambda x, y: math.hypot(x - (C - 220), y - (C + 200)) < 35,
                  wood=lambda x, y: y < C - 260),
}

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, s in SCENES.items():
        cfg = dict(BASE)
        cfg["settlement_name"] = name
        cfg["families"] = s["families"]
        cfg["village_centre"] = [C, C]
        cfg["village_site"] = site(s["height"], s["wet"], s["wood"])
        coarse(cfg, s["wet"], s["wood"])
        json.dump(cfg, open(os.path.join(out, name + ".json"), "w"))
        print(name)
