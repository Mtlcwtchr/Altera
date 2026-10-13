"""world_stele - the carved stele A-2 and its fallen pieces in the world
(doc/megaliths/A2_CARVED_STELE.md), called by world_megaliths.place.

Stands the stele where tools/terrain_lab/stele_glade.py made its glade
(<world>_stele_glade.json: site, turn, pond), and its fallen pieces round its
foot by the asset's own rule (placement.foot.debris of
content/config/terrain_core/megaliths/a2_carved_stele.asset.json): most of
them close in, in and out of the pond, a few rolled further out. Every
instance carries "look": "a2_carved_stele" (the renderer's baked maps).
Writes its checks into world_megaliths.CHECKS["a2_stele"]: the glade found,
the foot not hanging, the stele seen from the glade's rim and from across
the forest.
"""
import json
import math
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "content" / "config" / "terrain_core" / "megaliths" / "a2_carved_stele.asset.json"
KIND = "a2_carved_stele"


def place(world, catalogue, rng, add, checks):
    glade_file = Path(str(world.stem) + "_stele_glade.json")
    if not glade_file.exists():
        checks["a2_stele"] = {"ok": False, "why": "no glade: run tools/terrain_lab/stele_glade.py"}
        return 0
    glade = json.loads(glade_file.read_text(encoding="utf-8"))
    asset = json.loads(ASSET.read_text(encoding="utf-8"))
    names = sorted(n for n, m in catalogue.items() if m["kind"] == KIND)
    pieces = sorted(n for n, m in catalogue.items() if m["kind"] == KIND + "_debris")
    if not names:
        raise SystemExit("no A-2 model: run tools/terrain_lab/import_megalith.py a2_carved_stele")
    x, y = glade["site"]
    yaw = glade["yaw"]
    s = glade.get("scale", 1.0)
    m = catalogue[names[0]]
    add(KIND, names[0], x, y, yaw, (s, s, s), sink=glade.get("sink_m", 0.5), look=KIND)
    # Stood as the lab stands it (the lowest ground under 0.7 of its reach, less
    # the sink); hanging: its foot (it goes 6 m below its origin) must be under
    # the ground everywhere under it.
    reach = 0.5 * max(m["hi"][0] - m["lo"][0], m["hi"][1] - m["lo"][1]) * s
    ring = np.linspace(0, math.tau, 16, endpoint=False)
    stand = [world.height(x, y)] + [world.height(x + math.cos(a) * reach * 0.7, y + math.sin(a) * reach * 0.7)
                                    for a in ring]
    under = stand + [world.height(x + math.cos(a) * reach, y + math.sin(a) * reach) for a in ring]
    base = min(stand) - glade.get("sink_m", 0.5)
    hanging = max(0.0, base + m["lo"][2] * s - min(under) + 0.3)

    rule = asset["placement"]["foot"]["debris"]
    width = max(m["hi"][0] - m["lo"][0], m["hi"][1] - m["lo"][1]) * s
    count = int(rng.integers(rule["count"][0], rule["count"][1] + 1)) if pieces else 0
    for k in range(count):
        near = k < round(count * rule["near_share"])
        d = width * rng.uniform(*(rule["near_m"] if near else rule["far_m"]))
        a = rng.uniform(0, math.tau)
        px, py = x + math.cos(a) * d, y + math.sin(a) * d
        size = rng.uniform(*rule["size_m"]) if near else rng.uniform(rule["size_m"][0], 0.5 * sum(rule["size_m"]))
        name = pieces[int(rng.integers(len(pieces)))]
        pm = catalogue[name]
        f = size / max(pm["hi"][0] - pm["lo"][0], 1e-3)
        add(KIND + "_debris", name, px, py, rng.uniform(0, math.tau),
            (f, f * rng.uniform(0.8, 1.15), f * rng.uniform(0.7, 1.0)), sink=round(0.35 * size, 2), look=KIND)

    # Seen: the upper half clear of the relief from eye points on the glade's
    # rim and out across the forest (2-12 km).
    top = base + m["hi"][2] * s
    mid = base + 0.5 * (top - base)

    def clear(ex, ey):
        ez = world.height(ex, ey) + 1.8
        dist = math.hypot(x - ex, y - ey)
        for t in np.linspace(0.03, 0.97, max(int(dist / (world.cell * 0.5)), 2)):
            if world.height(ex + (x - ex) * t, ey + (y - ey) * t) > ez + (mid - ez) * t:
                return False
        return True

    rim = [clear(x + math.cos(a) * glade["radius_m"], y + math.sin(a) * glade["radius_m"])
           for a in np.linspace(0, math.tau, 24, endpoint=False)]
    far = []
    for _ in range(80):
        a, d = rng.uniform(0, math.tau), rng.uniform(2000, 12000)
        far.append(clear(x + math.cos(a) * d, y + math.sin(a) * d))
    checks["a2_stele"] = {
        "site": [round(x, 1), round(y, 1)], "turn_deg": round(math.degrees(yaw), 1),
        "base_m": round(base, 1), "top_m": round(top, 1), "hanging_m": round(hanging, 2),
        "pieces": count, "seen_from_rim": round(sum(rim) / len(rim), 2),
        "seen_2_12_km": round(sum(far) / len(far), 2),
        "ok": bool(hanging <= 0.0 and sum(rim) / len(rim) >= 0.9),
    }
    return 1 + count
