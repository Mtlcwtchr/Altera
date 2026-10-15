"""world_canyon_arch - the canyon's chain of portals A-4 in the world
(doc/megaliths/A4_CANYON_ARCH.md), called by world_features.py after the
canyon's own parts are in.

The chain is fitted in Blender to one slot of a canyon run
(tools/blender/megalith_a4_canyon_arch.py), so here it only stands where its
manifest says: the model's origin at the site (the slot's floor there),
unturned, scale 1. The canyon's pinnacles, rubble, bridges and spirals that
stand inside a portal's box (manifest placement.portals) are dropped.

Checks into world_megaliths.CHECKS["a4_arch"], on the run's 4 m heights (the
ground the game draws there): a portal's foot is under the ground all the way
across (hanging), its deck's ends run into the rock, the parts dropped, the
share of the canyon's floor (eye points within 6 km) that sees a portal.
"""
import json
import math
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "content" / "config" / "terrain_core" / "megaliths" / "a4_canyon_arch.asset.json"
KIND = "a4_canyon_arch"


class Run:
    """A canyon run's heights (world metres)."""

    def __init__(self, path):
        st = json.loads((path / "stats.json").read_text())
        self.cell = st["cell_m"]
        self.ox, self.oy = st["origin_m"]
        z = np.load(path / "layers.npz")
        self.h = z["h"].astype(np.float32)
        self.mask = z["mask"] > 0

    def at(self, x, y):
        u = np.clip((np.asarray(x) - self.ox) / self.cell, 0, self.h.shape[1] - 1.001)
        v = np.clip((np.asarray(y) - self.oy) / self.cell, 0, self.h.shape[0] - 1.001)
        i, j = v.astype(np.int64), u.astype(np.int64)
        fu, fv = u - j, v - i
        h = self.h
        return (h[i, j] * (1 - fu) + h[i, j + 1] * fu) * (1 - fv) + (h[i + 1, j] * (1 - fu) + h[i + 1, j + 1] * fu) * fv


def in_box(p, x, y):
    """Is (x, y) (world, relative to the site) inside portal p's footprint?"""
    cx, cy = p["centre_m"]
    ux, uy = p["across"]
    dx, dy = x - cx, y - cy
    along_u = dx * ux + dy * uy
    along_a = -dx * uy + dy * ux
    return abs(along_u) <= p["half_length_m"] and abs(along_a) <= p["half_thickness_m"] + 20.0


def place(stem, items, catalogue, checks):
    if not ASSET.exists():
        return items
    asset = json.loads(ASSET.read_text(encoding="utf-8"))
    names = sorted(n for n, m in catalogue.items() if m["kind"] == KIND)
    if not names:
        raise SystemExit("no A-4 model: run tools/terrain_lab/import_megalith.py a4_canyon_arch")
    rule = asset["placement"]
    site = rule["site"]
    sx, sy = site["world_m"]
    z0 = site["floor_z_m"]
    portals = rule["portals"]
    run = Run(ROOT / site["run"])

    # the canyon's own parts inside a portal go
    kept, dropped = [], {}
    for o in items:
        if o.get("canyon"):
            if any(in_box(p, o["x"] - sx, o["y"] - sy) for p in portals):
                k = o["canyon"]
                dropped[k] = dropped.get(k, 0) + 1
                continue
        kept.append(o)
    kept.append({"m": names[0], "x": sx, "y": sy, "z": z0, "yaw": float(site["yaw"]),
                 "s": [float(site["scale"])] * 3, "material": "", "cat": "megaliths", "look": KIND})

    # hanging: the foot (8 m under each portal's floor) against the ground across the portal;
    # deck ends: the ground half way along the deck's ends' run into the rock is over the deck
    hanging, ends_open, open_ends = 0.0, 0, []
    for p in portals:
        cx, cy = p["centre_m"]
        ux, uy = p["across"]
        for f in np.linspace(-1, 1, 81):
            for a in (-p["half_thickness_m"] + 4, 0.0, p["half_thickness_m"] - 4):
                d = f * (p["half_length_m"] - 40)
                x = sx + cx + ux * d - uy * a
                y = sy + cy + uy * d + ux * a
                hanging = max(hanging, (z0 + p["floor_m"] - 8.0) - float(run.at(x, y)))
        for side in (-1, 1):
            d = side * (p["half_length_m"] - 40 - 13)      # half way into the rock (embed 26 m)
            g = float(run.at(sx + cx + ux * d, sy + cy + uy * d))
            if g < z0 + p["deck_m"] - 2:
                ends_open += 1
                open_ends.append([portals.index(p), side, round(z0 + p["deck_m"] - g, 1)])

    # seen from the canyon's floor: eye points on the floor within 6 km, does any portal's crown
    # stand clear of the relief
    rng = np.random.default_rng(5)
    ys, xs = np.nonzero(run.mask[::8, ::8])
    ex = run.ox + (xs * 8 + rng.random(len(xs)) * 8) * run.cell
    ey = run.oy + (ys * 8 + rng.random(len(ys)) * 8) * run.cell
    near = np.hypot(ex - sx, ey - sy) < 6000
    ex, ey = ex[near], ey[near]
    eg = run.at(ex, ey)
    top = np.array([run.at(ex + dx, ey + dy) for dx, dy in ((48, 0), (-48, 0), (0, 48), (0, -48))]).max(0)
    floor = top - eg > 60          # in the canyon, not on its plateaus
    ex, ey, ez = ex[floor], ey[floor], eg[floor] + 1.8
    seen = np.zeros(len(ex), bool)
    t = np.linspace(0.02, 0.97, 140)[None, :]
    for p in portals:
        px, py = sx + p["centre_m"][0], sy + p["centre_m"][1]
        pz = z0 + p["crown_m"]
        lx = ex[:, None] + (px - ex[:, None]) * t
        ly = ey[:, None] + (py - ey[:, None]) * t
        lz = ez[:, None] + (pz - ez[:, None]) * t
        seen |= np.all(run.at(lx, ly) <= lz, axis=1)
    checks["a4_arch"] = {
        "site": [sx, sy], "floor_z_m": z0, "portals": len(portals),
        "hanging_m": round(max(0.0, hanging), 2), "deck_ends_open": ends_open, "open_ends": open_ends,
        "canyon_parts_dropped": dropped,
        "seen_from_canyon_floor_6km": round(float(seen.mean()) if len(seen) else 0.0, 3),
        "floor_points": int(len(seen)),
        "ok": bool(hanging <= 0.0 and ends_open == 0),
    }
    return kept
