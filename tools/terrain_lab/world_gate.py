"""world_gate - the desert gate A-3 in the world (doc/megaliths/A3_DESERT_GATE.md),
called by world_megaliths.place. It takes the golden desert's one magic
megalith slot (the mirroring prism hero is gone from props.json for it).

The site follows the asset's rule (placement.site of
content/config/terrain_core/megaliths/a3_desert_gate.asset.json): flat sand
deep in the biome, not on a dune crest; of the candidates the one whose
upper half is seen from most of the biome wins (BIOME_PLAN 3.4: the monument
seen from >= 80 % of it). The arch faces the biome's centre of mass. The
instance carries "look": "a3_desert_gate" (the renderer's baked maps).
Writes its checks into world_megaliths.CHECKS["a3_gate"]: the foot not
hanging, the share of the biome it is seen from.
"""
import json
import math
from pathlib import Path

import numpy as np
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "content" / "config" / "terrain_core" / "megaliths" / "a3_desert_gate.asset.json"
KIND = "a3_desert_gate"


def heights(world, x, y):
    """Bilinear heights at arrays of world metres (World.height, vectorised)."""
    u = np.clip(x / world.size * world.n - 0.5, 0, world.n - 1.001)
    v = np.clip(y / world.size * world.n - 0.5, 0, world.n - 1.001)
    i, j = v.astype(np.int64), u.astype(np.int64)
    fu, fv = u - j, v - i
    h = world.h
    return (h[i, j] * (1 - fu) + h[i, j + 1] * fu) * (1 - fv) + (h[i + 1, j] * (1 - fu) + h[i + 1, j + 1] * fu) * fv


def seen_share(world, eyes, x, y, mid, steps=160):
    """Share of the eye points (1.8 m over the ground) from which the point
    (x, y, mid) stands clear of the relief."""
    ex, ey = eyes[:, 0], eyes[:, 1]
    ez = heights(world, ex, ey) + 1.8
    t = np.linspace(0.02, 0.97, steps)[None, :]
    px = ex[:, None] + (x - ex[:, None]) * t
    py = ey[:, None] + (y - ey[:, None]) * t
    line = ez[:, None] + (mid - ez[:, None]) * t
    return float(np.mean(np.all(heights(world, px, py) <= line, axis=1)))


def place(world, catalogue, rng, add, checks):
    asset = json.loads(ASSET.read_text(encoding="utf-8"))
    rule = asset["placement"]
    site = rule["site"]
    names = sorted(n for n, m in catalogue.items() if m["kind"] == KIND)
    if not names:
        raise SystemExit("no A-3 model: run tools/terrain_lab/import_megalith.py a3_desert_gate")
    inside = world.zones == rule["zone"]
    if not inside.any():
        checks["a3_gate"] = {"ok": False, "why": f"no {rule['zone']} in this world"}
        return 0
    m = catalogue[names[0]]
    s = float(rule["scale"])
    cell = world.cell
    extent = math.sqrt(inside.sum()) * cell
    depth = ndimage.distance_transform_edt(inside) * cell
    r = max(1, int(round(site["flat_radius_m"] / cell)))
    foot = np.ones((2 * r + 1, 2 * r + 1), bool)
    relief = ndimage.maximum_filter(world.hz, footprint=foot) - ndimage.minimum_filter(world.hz, footprint=foot)
    # not on a dune crest: no higher than the sand round it (2 km)
    crest = world.hz - ndimage.uniform_filter(world.hz, size=max(3, int(2000 / cell)))
    ok = (inside & (depth > site["edge_depth_share"] * extent) & (relief < site["max_relief_m"])
          & (crest < 2.0) & (world.hz > 1.0))
    why = None
    if not ok.any():   # loosen to the flattest deep cells
        why = "no cell met the rule: took the flattest deep ones"
        ok = inside & (depth > 0.5 * site["edge_depth_share"] * extent)
        ok &= relief <= np.quantile(relief[ok], 0.1)
    ys, xs = np.nonzero(ok)
    pick = rng.choice(len(xs), size=min(site["candidates"], len(xs)), replace=False)
    iy, ix = np.nonzero(inside)
    e = rng.choice(len(ix), size=min(site["eye_points"], len(ix)), replace=False)
    eyes = np.stack([(ix[e] + rng.random(len(e))) * cell, (iy[e] + rng.random(len(e))) * cell], 1)
    best = None
    for k in pick:
        x, y = (xs[k] + 0.5) * cell, (ys[k] + 0.5) * cell
        near = np.hypot(eyes[:, 0] - x, eyes[:, 1] - y) <= site["range_m"]
        mid = world.height(x, y) + 0.5 * m["hi"][2] * s
        share = seen_share(world, eyes[near], x, y, mid) * near.mean() if near.any() else 0.0
        key = (share, depth[ys[k], xs[k]])
        if best is None or key > best[0]:
            best = (key, x, y)
    (share, _), x, y = best
    # the arch (the model's -y) toward the biome's centre of mass
    cy, cx = (iy.mean() + 0.5) * cell, (ix.mean() + 0.5) * cell
    yaw = math.atan2(cx - x, -(cy - y)) if math.hypot(cx - x, cy - y) > 1.0 else 0.0
    add(KIND, names[0], x, y, yaw, (s, s, s), sink=rule["sink_m"], look=KIND)

    # Stood as the lab stands it (the lowest ground under 0.7 of its reach, less
    # the sink); hanging: its foot (5 m below its origin) under the ground
    # everywhere under its reach.
    reach = 0.5 * max(m["hi"][0] - m["lo"][0], m["hi"][1] - m["lo"][1]) * s
    ring = np.linspace(0, math.tau, 16, endpoint=False)
    stand = [world.height(x, y)] + [world.height(x + math.cos(a) * reach * 0.7, y + math.sin(a) * reach * 0.7)
                                    for a in ring]
    base = min(stand) - rule["sink_m"]
    under = [world.height(x + math.cos(a) * reach * 0.5, y + math.sin(a) * reach * 0.5) for a in ring]
    hanging = max(0.0, base + m["lo"][2] * s - min(under + stand) + 0.3)
    checks["a3_gate"] = {
        "site": [round(x, 1), round(y, 1)], "turn_deg": round(math.degrees(yaw), 1),
        "base_m": round(base, 1), "top_m": round(base + m["hi"][2] * s, 1), "hanging_m": round(hanging, 2),
        "biome_extent_km": round(extent / 1000, 1), "candidates": int(len(pick)),
        "seen_from_biome": round(share, 3), "need": 0.8, "note": why,
        "ok": bool(hanging <= 0.0 and share >= 0.8),
    }
    return 1
