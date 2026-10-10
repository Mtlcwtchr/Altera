#!/usr/bin/env python3
"""world_features - where the mega structures of a generated world stand (the
mesh part of the hybrid terrain, doc/rework/MESH_TERRAIN_PLAN.md).

    python tools/terrain_lab/world_features.py                       # terrain_lab_out/world_*
    python tools/terrain_lab/world_features.py --world terrain_lab_out/seed_5/world

Reads what the generator already wrote - <world>_final.r16 (heights),
<world>_zones.png (the zones, north up, with a legend), <world>_biomes.png -
and the rules in content/config/terrain_core/props.json; writes
<world>_features.json (every instance: model, place, turn, scale, material)
and <world>_features_preview.png. The lab reads the json with --features.

Heights in the json are absolute metres above the sea. A spire carries its
`top`: the lab stands it on the ground it finds there and stretches it up to
that height, so a spire always reaches the high ground round it, whatever the
erosion did to its foot. A bridge carries its deck height `z`.
"""
import argparse
import ast
import json
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
RULES = ROOT / "content" / "config" / "terrain_core" / "props.json"
WORLD_RULES = ROOT / "content" / "config" / "terrain_core" / "world_rules.json"
CATALOGUE = ROOT / "assets" / "terrain_core" / "props" / "props.json"


def zone_colours():
    """The generator's own legend (world_gen.ZONE_COLOURS), read from its
    source rather than imported: the generator need not be importable here."""
    text = (Path(__file__).with_name("world_gen.py")).read_text(encoding="utf-8")
    m = re.search(r"ZONE_COLOURS\s*=\s*(\{.*?\n\})", text, re.S)
    return ast.literal_eval(m.group(1))


def load_zones(path):
    """Zone names per pixel, y up (row 0 = south)."""
    img = np.asarray(Image.open(path).convert("RGB"))
    side = img.shape[0]
    img = img[:, :side][::-1].astype(np.int32)
    colours = zone_colours()
    names = list(colours)
    pal = np.array([colours[n] for n in names], np.int32)
    d = ((img[:, :, None, :] - pal[None, None, :, :]) ** 2).sum(-1)
    return np.array(names)[d.argmin(-1)]


class World:
    def __init__(self, stem, height_range, sea):
        r16 = Path(str(stem) + "_final.r16")
        raw = np.fromfile(r16, "<u2")
        n = int(math.isqrt(raw.size))
        self.h = (raw.reshape(n, n).astype(np.float32) / 65535.0 - sea) * height_range
        self.n = n
        self.zones = load_zones(Path(str(stem) + "_zones.png"))
        rules = json.loads(WORLD_RULES.read_text(encoding="utf-8"))
        self.size = rules["world_km"] * 1000.0
        # Heights at the zone map's grid, for the searches.
        k = n // self.zones.shape[0]
        self.zk = k
        self.hz = self.h.reshape(self.zones.shape[0], k, -1, k).mean(axis=(1, 3))
        self.cell = self.size / self.zones.shape[0]

    def height(self, x, y):
        """Bilinear height at world metres, as the lab samples the macro map."""
        u = np.clip(x / self.size * self.n - 0.5, 0, self.n - 1.001)
        v = np.clip(y / self.size * self.n - 0.5, 0, self.n - 1.001)
        i, j = int(v), int(u)
        fu, fv = u - j, v - i
        h = self.h
        return float((h[i, j] * (1 - fu) + h[i, j + 1] * fu) * (1 - fv) + (h[i + 1, j] * (1 - fu) + h[i + 1, j + 1] * fu) * fv)

    def lowest(self, x, y, r):
        """Lowest ground within `r` metres: where a foot must reach down to."""
        best = self.height(x, y)
        for k in range(12):
            a = k * math.tau / 12
            for f in (0.5, 1.0):
                best = min(best, self.height(x + math.cos(a) * r * f, y + math.sin(a) * r * f))
        return best


def poisson(cands, weights, count, gap, rng, taken=None):
    """Up to `count` of `cands` (N x 2, metres), drawn by weight, none closer
    than `gap` to one another or to `taken`."""
    if not len(cands) or count <= 0:
        return np.zeros((0, 2))
    p = weights / weights.sum()
    order = rng.choice(len(cands), size=min(len(cands), count * 12), replace=False, p=p)
    chosen = list(taken) if taken is not None else []
    out = []
    grid = {}
    cell = gap
    for q in chosen:
        grid.setdefault((int(q[0] // cell), int(q[1] // cell)), []).append(q)
    for i in order:
        c = cands[i]
        gx, gy = int(c[0] // cell), int(c[1] // cell)
        ok = True
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for q in grid.get((gx + dx, gy + dy), ()):
                    if (q[0] - c[0]) ** 2 + (q[1] - c[1]) ** 2 < gap * gap:
                        ok = False
                        break
        if ok:
            out.append(c)
            grid.setdefault((gx, gy), []).append(c)
            if len(out) >= count:
                break
    return np.array(out).reshape(-1, 2)


def cells_of(world, mask, rng):
    """Every cell of `mask` as a point jittered inside its cell (metres)."""
    ys, xs = np.nonzero(mask)
    jitter = rng.random((len(xs), 2))
    return np.stack([(xs + jitter[:, 0]) * world.cell, (ys + jitter[:, 1]) * world.cell], 1), (ys, xs)


def models_of(catalogue, kind):
    names = sorted(n for n, m in catalogue.items() if m["kind"] == kind)
    if not names:
        raise SystemExit(f"no models of kind {kind}: run tools/blender/terrain_props.py --all")
    return names


def scale_for(g, m, rng):
    """`size_m`: the model's largest dimension (a lying giant is as long as
    it says); `height_m`: its height over the ground."""
    if "size_m" in g:
        dims = max(m["hi"][i] - m["lo"][i] for i in range(3))
        return rng.uniform(*g["size_m"]) / max(dims, 1e-3)
    return rng.uniform(*g["height_m"]) / max(m["hi"][2], 1e-3)


def place(world, rules, catalogue, seed):
    rng = np.random.default_rng(seed)
    zones = world.zones
    out = []
    mat = rules.get("materials", {})

    def add(kind, name, x, y, yaw, scale, **extra):
        out.append({"m": name, "x": round(float(x), 1), "y": round(float(y), 1), "yaw": round(float(yaw), 4),
                    "s": [round(float(s), 4) for s in scale], "material": mat.get(kind, ""), **extra})

    # --- spires: on the canyon floors, tops at the high ground's level ---
    sp = rules["spires"]
    canyon = np.isin(zones, sp["zones"])
    high = np.where(np.isin(zones, sp["top_zones"]), world.hz, -1e4)
    reach = max(3, int(round(sp["reach_km"] * 1000 / world.cell)))
    # The high ground round a point: its upper rim, not a lone peak.
    top = ndimage.maximum_filter(ndimage.uniform_filter(high, 3), size=reach)
    floor = ndimage.minimum_filter(np.where(canyon, world.hz, 1e4), size=max(3, reach // 3))
    rise = top - world.hz
    low = world.hz < floor + sp["floor_share"] * np.maximum(top - floor, 0)
    ok = canyon & low & (rise > sp["min_rise_m"])
    pts, (ys, xs) = cells_of(world, ok, rng)
    area_km2 = canyon.sum() * (world.cell / 1000) ** 2
    count = int(area_km2 / 100 * sp["per_100km2"])
    # Clusters: a slow noise makes fields of spires and empty stretches.
    ck = max(2, int(sp["cluster_km"] * 1000 / world.cell))
    clump = ndimage.gaussian_filter(rng.random(zones.shape), ck / 2)
    clump = (clump - clump.min()) / max(np.ptp(clump), 1e-6)
    weights = clump[ys, xs] ** 3 + 0.02
    spires = poisson(pts, weights, count, sp["min_gap_km"] * 1000, rng)
    names = models_of(catalogue, sp["kind"])
    spire_list = []
    for x, y in spires:
        i, j = int(y // world.cell), int(x // world.cell)
        name = names[rng.integers(len(names))]
        m = catalogue[name]
        tall = m["hi"][2]
        ground = world.lowest(x, y, 0.5 * (m["hi"][0] - m["lo"][0]))
        t = top[i, j] * rng.uniform(*sp["top_share"])
        if t - ground < sp["min_rise_m"]:
            continue
        sz = (t - ground) / tall
        sxy = sz ** 0.85 * rng.uniform(*sp["width"])   # nearly as built: no squat boulders
        add("spire", name, x, y, rng.uniform(0, math.tau), (sxy, sxy, sz), top=round(float(t), 1),
            ground=round(ground, 1))
        spire_list.append((x, y, t, ground, sxy * 0.5 * (m["hi"][0] - m["lo"][0])))

    # --- bridges between spires whose tops are alike ---
    br = rules["bridges"]
    bnames = models_of(catalogue, br["kind"])
    used = set()
    pairs = []
    for a in range(len(spire_list)):
        for b in range(a + 1, len(spire_list)):
            xa, ya, ta, ga, ra = spire_list[a]
            xb, yb, tb, gb, rb = spire_list[b]
            d = math.hypot(xb - xa, yb - ya)
            if not (br["gap_km"][0] * 1000 <= d <= br["gap_km"][1] * 1000):
                continue
            rise = min(ta - ga, tb - gb)
            if abs(ta - tb) > br["top_diff"] * rise:
                continue
            pairs.append((d, a, b))
    rng.shuffle(pairs)
    want = int(len(spire_list) * br["share"])
    for d, a, b in pairs:
        if len(used) // 2 >= want:
            break
        if a in used or b in used:
            continue
        used |= {a, b}
        xa, ya, ta, ga, ra = spire_list[a]
        xb, yb, tb, gb, rb = spire_list[b]
        rise = min(ta - ga, tb - gb)
        deck = min(ta, tb) - rng.uniform(0.02, 0.18) * rise
        th = rng.uniform(*br["thickness"])
        name = bnames[rng.integers(len(bnames))]
        add("bridge", name, (xa + xb) / 2, (ya + yb) / 2, math.atan2(yb - ya, xb - xa),
            (d / 1000.0, th, th * math.sqrt(d / 1000.0)), z=round(float(deck), 1))

    # --- megaliths by zone ---
    gx, gy = np.gradient(world.hz, world.cell)
    flat = np.hypot(gx, gy) < rules["max_slope"]
    taken = [(o["x"], o["y"]) for o in out]
    for g in rules["groups"]:
        inside = np.isin(zones, g["zones"])
        deep = ndimage.distance_transform_edt(inside) * world.cell > rules["edge_m"]
        ok = deep & flat & (world.hz > 1.0)
        area_km2 = inside.sum() * (world.cell / 1000) ** 2
        n = int(round(area_km2 / 1000 * g["per_1000km2"] * rng.uniform(0.8, 1.2)))
        pts, _ = cells_of(world, ok, rng)
        centres = poisson(pts, np.ones(len(pts)), n, 6000, rng, taken)
        names = models_of(catalogue, g["kind"])
        for cx, cy in centres:
            k = int(rng.integers(g["cluster"][0], g["cluster"][1] + 1)) if "cluster" in g else 1
            ring = g.get("ring", False)
            turn = rng.uniform(0, math.tau)
            for q in range(k):
                if k == 1:
                    x, y = cx, cy
                elif ring:
                    a = turn + q * math.tau / k
                    x, y = cx + math.cos(a) * g["cluster_m"], cy + math.sin(a) * g["cluster_m"]
                else:
                    r = g["cluster_m"] * math.sqrt(rng.random())
                    a = rng.uniform(0, math.tau)
                    x, y = cx + math.cos(a) * r, cy + math.sin(a) * r
                name = names[rng.integers(len(names))]
                m = catalogue[name]
                s = scale_for(g, m, rng)
                yaw = (turn + q * math.tau / k + math.pi / 2) if ring else rng.uniform(0, math.tau)
                add(g["kind"], name, x, y, yaw, (s, s, s))
            taken.append((cx, cy))

    # --- singles ---
    for g in rules["singles"]:
        inside = zones == g["zone"]
        if not inside.any():
            continue
        deep = ndimage.distance_transform_edt(inside) * world.cell
        score = deep.copy()
        if g.get("near_low"):
            # By the water: the lowest deep ground, where the rivers meet.
            score = np.where(deep > 1500, -world.hz, -1e9)
        i, j = np.unravel_index(np.argmax(score), score.shape)
        x, y = (j + 0.5) * world.cell, (i + 0.5) * world.cell
        name = models_of(catalogue, g["kind"])[0]
        m = catalogue[name]
        s = scale_for(g, m, rng)
        add(g["kind"], name, x, y, rng.uniform(0, math.tau), (s, s, s))
    return out


def preview(world, stem, items, catalogue, path):
    base = Path(str(stem) + "_final_preview.png")
    img = Image.open(base).convert("RGB") if base.exists() else Image.new("RGB", (1024, 1024))
    side = img.size[0]
    d = ImageDraw.Draw(img)
    colours = {"spire": (255, 60, 30), "bridge": (255, 255, 0), "valley_spire": (255, 0, 255)}
    for o in items:
        if o["m"] not in catalogue:
            continue
        kind = catalogue[o["m"]]["kind"]
        px = o["x"] / world.size * side
        py = side - o["y"] / world.size * side
        c = colours.get(kind, (40, 255, 255))
        r = 2 if kind == "spire" else 3
        d.ellipse([px - r, py - r, px + r, py + r], fill=c)
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", default=str(ROOT / "terrain_lab_out" / "world"))
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--no-cliffs", action="store_true", help="leave out the cliff walls")
    args = ap.parse_args()
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    catalogue = json.loads(CATALOGUE.read_text(encoding="utf-8"))
    world = World(args.world, args.height_range, args.sea)
    items = place(world, rules, catalogue, args.seed)
    if not args.no_cliffs:
        import world_cliffs
        items += world_cliffs.build(world, rules, args.world + "_cliffs", np.random.default_rng(args.seed))
    out = Path(args.world + "_features.json")
    out.write_text(json.dumps({"world_m": world.size, "instances": items}, separators=(",", ":")))
    preview(world, args.world, items, catalogue, args.world + "_features_preview.png")
    kinds = {}
    for o in items:
        k = catalogue[o["m"]]["kind"] if o["m"] in catalogue else "cliff"
        kinds[k] = kinds.get(k, 0) + 1
    print(f"{len(items)} instances -> {out}: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
    tops = [o["top"] - o["ground"] for o in items if "top" in o]
    if tops:
        print(f"spires rise {min(tops):.0f} .. {max(tops):.0f} m (median {np.median(tops):.0f})")


if __name__ == "__main__":
    main()
