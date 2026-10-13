#!/usr/bin/env python3
"""stele_glade - the glade the carved stele A-2 stands in
(doc/megaliths/A2_CARVED_STELE.md): a damp clearing in the dark forest where
it meets the white mountains and the green plateau, its ground eased flat,
mud and dark grass with dead trees, a pond at the stele's foot and a few
pools about.

    python tools/terrain_lab/stele_glade.py                          # terrain_lab_out/world_*
    python tools/terrain_lab/stele_glade.py --world terrain_lab_out/seed_5/world

Runs after world_gen.py --finish and before marsh_islands.py and
world_features.py (build_world.py does it). Rules:
content/config/terrain_core/stele_glade.json; the pond's shape is the asset's
(content/config/terrain_core/megaliths/a2_carved_stele.asset.json,
placement.foot.pond). Changes, in a window round the glade only:

- <world>_final.r16 (and <world>_final_base.r16, marsh_islands.py's base, if
  there is one): the ground eased flat, the pond and pools cut;
- <world>_biomes.png: the glade's biome;
- <world>_water_skeleton.png (and its _base): the pond and pools painted as
  lakes, so the lab keeps their water (it drops hollows this small);
- <world>_marsh_islands.json: its record of the files it wrote, when they were
  its own, so its next run still starts from its base.

Writes <world>_stele_glade.json (the site, the stele's turn, the pond, the
pools, the windows it owns) and keeps what was under its windows in
<world>_stele_glade_orig.npz: a re-run puts that back first, so it never
shapes the glade twice; a file that changed under it since (a new world) is
taken as it is.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

import world_features as wf

ROOT = Path(__file__).resolve().parents[2]
RULE = ROOT / "content" / "config" / "terrain_core" / "stele_glade.json"
ASSET = ROOT / "content" / "config" / "terrain_core" / "megaliths" / "a2_carved_stele.asset.json"


def sha(data):
    return hashlib.sha1(np.ascontiguousarray(data).tobytes()).hexdigest()


def file_sha(path):
    return hashlib.sha1(Path(path).read_bytes()).hexdigest()


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


class Files:
    """The world's files this script edits, as arrays (row 0 south), each
    with the window it owns put back to what was there before it."""

    def __init__(self, stem, state, orig):
        self.stem = stem
        self.items = {}
        for key, suffix, kind in (("final", "_final.r16", "r16"), ("final_base", "_final_base.r16", "r16"),
                                  ("biomes", "_biomes.png", "L"), ("skeleton", "_water_skeleton.png", "RGB"),
                                  ("skeleton_base", "_water_skeleton_base.png", "RGB")):
            path = Path(stem + suffix)
            if not path.exists():
                continue
            if kind == "r16":
                raw = np.fromfile(path, "<u2")
                n = int(math.isqrt(raw.size))
                arr = raw.reshape(n, n).copy()
            else:
                arr = np.asarray(Image.open(path).convert(kind))[::-1].copy()
            win = state.get("windows", {}).get(key)
            if win is not None and orig is not None and key in orig:
                r0, r1, c0, c1 = win["rows"] + win["cols"]
                if arr.shape[:2] == tuple(win["shape"]) and sha(arr[r0:r1, c0:c1]) == win["written"]:
                    arr[r0:r1, c0:c1] = orig[key]
            self.items[key] = {"path": path, "kind": kind, "arr": arr}

    def save(self, key, arr, window, windows, originals):
        it = self.items[key]
        r0, r1, c0, c1 = window
        originals[key] = it["arr"][r0:r1, c0:c1].copy()
        windows[key] = {"rows": [r0, r1], "cols": [c0, c1], "shape": list(arr.shape[:2]),
                        "written": sha(arr[r0:r1, c0:c1])}
        if it["kind"] == "r16":
            arr.astype("<u2").tofile(it["path"])
        else:
            Image.fromarray(arr[::-1]).save(it["path"])


def find_site(world, zones, rule, skel_wet):
    """The lowest flat spot of the zone near both named zones, clear of water."""
    cell = world.cell
    inside = zones == rule["zone"]
    ok = inside & (ndimage.distance_transform_edt(inside) * cell > rule["edge_m"])
    for other, metres in rule["near"].items():
        ok &= ndimage.distance_transform_edt(zones != other) * cell < metres
    smooth = ndimage.uniform_filter(world.hz, 5)
    gy, gx = np.gradient(smooth, cell)
    ok &= np.hypot(gx, gy) < rule["max_slope"]
    # (clear of the painted water, on the zone map's grid)
    if skel_wet is not None:
        k = skel_wet.shape[0] // zones.shape[0]
        wet = skel_wet[: zones.shape[0] * k, : zones.shape[0] * k].reshape(zones.shape[0], k, -1, k).any(axis=(1, 3))
        ok &= ndimage.distance_transform_edt(~wet) * cell > rule["radius_m"][1] * 1.6
    if not ok.any():
        return None
    size = max(3, int(round(rule["hollow_km"] * 1000 / cell)))
    hollow = ndimage.uniform_filter(world.hz, size) - world.hz
    score = np.where(ok, hollow, -np.inf)
    i, j = np.unravel_index(np.argmax(score), score.shape)
    return (j + 0.5) * cell, (i + 0.5) * cell


def facing(zones, cell, x, y, toward):
    """The stele's turn so that its front (-y in its own frame) looks at the
    nearest cell of `toward`."""
    _, idx = ndimage.distance_transform_edt(zones != toward, return_indices=True)
    i, j = int(y / cell), int(x / cell)
    ty, tx = (idx[0][i, j] + 0.5) * cell, (idx[1][i, j] + 0.5) * cell
    dx, dy = tx - x, ty - y
    # local -y turned by yaw is (sin yaw, -cos yaw)
    return math.atan2(dx, -dy)


def rim(rng, wobble):
    """A ragged radius factor round the glade, as a function of the angle."""
    k = np.arange(2, 7)
    amp = rng.uniform(0.4, 1.0, len(k)) / k
    ph = rng.uniform(0, math.tau, len(k))
    norm = amp.sum()

    def f(theta):
        return 1.0 + wobble * (np.sin(np.multiply.outer(theta, k) + ph) @ amp) / norm
    return f


def value_noise(rng, shape, cell_px):
    g = rng.random((shape[0] // cell_px + 3, shape[1] // cell_px + 3)).astype(np.float32)
    return ndimage.zoom(g, cell_px, order=3)[: shape[0], : shape[1]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", default=str(ROOT / "terrain_lab_out" / "world"))
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    stem = args.world
    rule = json.loads(RULE.read_text(encoding="utf-8"))
    asset = json.loads(ASSET.read_text(encoding="utf-8"))
    pond = asset["placement"]["foot"]["pond"]
    out_json = Path(stem + "_stele_glade.json")
    orig_file = Path(stem + "_stele_glade_orig.npz")
    state = json.loads(out_json.read_text(encoding="utf-8")) if out_json.exists() else {}
    orig = dict(np.load(orig_file)) if orig_file.exists() else None
    before = {k: file_sha(stem + f) for k, f in (("final_sha1", "_final.r16"), ("skeleton_sha1", "_water_skeleton.png"))
              if Path(stem + f).exists()}
    files = Files(stem, state, orig)
    if "final" not in files.items:
        raise SystemExit(f"stele_glade: no {stem}_final.r16")
    rng = np.random.default_rng(args.seed * 104729 + 18)

    world = wf.World(stem, args.height_range, args.sea)
    hr, sea = args.height_range, args.sea
    raw = files.items["final"]["arr"]
    n = raw.shape[0]
    world.h = (raw.astype(np.float32) / 65535.0 - sea) * hr
    world.hz = world.h.reshape(world.zones.shape[0], n // world.zones.shape[0], -1, n // world.zones.shape[0]).mean(axis=(1, 3))
    zones = world.zones
    zones_sha = file_sha(stem + "_zones.png")
    skel_wet = None
    if "skeleton" in files.items:
        s = files.items["skeleton"]["arr"]
        skel_wet = ((s[..., 2] > 200) & (s[..., 0] < 80)) | ((s[..., 0] > 200) & (s[..., 2] < 80))

    # The site: the rule's own, the last run's (same zones), or searched.
    if rule.get("site_m"):
        x, y = rule["site_m"]
    elif state.get("zones_sha1") == zones_sha and "site" in state:
        x, y = state["site"]
    else:
        site = find_site(world, zones, rule, skel_wet)
        if site is None:
            # (a generated world may have no such place: then no stele, and the build goes on)
            print(f"stele_glade: no site in {rule['zone']} near {list(rule['near'])}; no glade")
            out_json.unlink(missing_ok=True)
            return
        x, y = site
    yaw = facing(zones, world.cell, x, y, rule["face"])
    radius = float(rng.uniform(*rule["radius_m"]))
    rim_f = rim(rng, rule["wobble"])

    # --- heights, on the macro map's grid (row 0 south)
    tex = world.size / n
    reach = radius * (1 + rule["wobble"]) + rule["blend_m"] + 128
    r0, r1 = max(0, int((y - reach) / tex)), min(n, int((y + reach) / tex) + 1)
    c0, c1 = max(0, int((x - reach) / tex)), min(n, int((x + reach) / tex) + 1)
    H = world.h[r0:r1, c0:c1].astype(np.float64)
    yy, xx = np.mgrid[r0:r1, c0:c1]
    X, Y = (xx + 0.5) * tex - x, (yy + 0.5) * tex - y
    r = np.hypot(X, Y)
    R = radius * rim_f(np.arctan2(Y, X))
    q = r / R
    ring = (q > 1.05) & (q < 1.45)
    A = np.stack([np.ones(ring.sum()), X[ring], Y[ring]], 1)
    a, b, c = np.linalg.lstsq(A, H[ring], rcond=None)[0]
    level = float(a)
    plane = a + rule["keep_slope"] * (b * X + c * Y)
    hum = (value_noise(rng, H.shape, 2) - 0.5) * 2 * rule["hummock_m"]
    target = plane + hum
    target = target + (level - target) * (1 - smoothstep(rule["flat_m"], rule["flat_m"] + 80, r))

    # The pond (the asset's, in the stele's frame turned by yaw) and the pools.
    cs, sn = math.cos(yaw), math.sin(yaw)
    pcx, pcy = pond["centre_m"]
    pond_c = (x + pcx * cs - pcy * sn, y + pcx * sn + pcy * cs)
    prx, pry = pond["radii_m"]
    cell64 = world.size / files.items["skeleton"]["arr"].shape[0] if "skeleton" in files.items else 64.0
    pools = []
    want = int(rng.integers(rule["pools"][0], rule["pools"][1] + 1))
    for _ in range(200):
        if len(pools) >= want:
            break
        ang = rng.uniform(0, math.tau)
        d = rng.uniform(rule["pool_gap_m"], 0.8 * radius * rim_f(np.array([ang]))[0])
        px, py = x + math.cos(ang) * d, y + math.sin(ang) * d
        # (its middle on a work cell's centre, so the lab paints it)
        px, py = (math.floor(px / cell64) + 0.5) * cell64, (math.floor(py / cell64) + 0.5) * cell64
        pr = float(rng.uniform(*rule["pool_m"])) / 2
        if math.hypot(px - pond_c[0], py - pond_c[1]) < max(prx, pry) + pr + rule["pool_flat_m"]:
            continue
        if any(math.hypot(px - o["x"], py - o["y"]) < pr + o["r"] + rule["pool_flat_m"] for o in pools):
            continue
        pools.append({"x": round(px, 1), "y": round(py, 1), "r": round(pr, 1),
                      "level": round(float(a + rule["keep_slope"] * (b * (px - x) + c * (py - y))), 2)})
    for o in pools:
        d = np.hypot(X + x - o["x"], Y + y - o["y"])
        w = 1 - smoothstep(o["r"] + rule["pool_flat_m"], o["r"] + rule["pool_flat_m"] + 60, d)
        target = target + (o["level"] - target) * w
    blend = 1 - smoothstep(1.0, 1.0 + rule["blend_m"] / R, q)
    Hn = H + (target - H) * blend
    # pond: an oval sinking `pond_depth_m` under the flat
    u = (X + x - pond_c[0]) * cs + (Y + y - pond_c[1]) * sn
    v = -(X + x - pond_c[0]) * sn + (Y + y - pond_c[1]) * cs
    dp = np.hypot(u / prx, v / pry)
    Hn = np.minimum(Hn, level - rule["pond_depth_m"] * smoothstep(1.0, 0.7, dp))
    for o in pools:
        d = np.hypot(X + x - o["x"], Y + y - o["y"]) / o["r"]
        Hn = np.minimum(Hn, o["level"] - rule["pool_depth_m"] * smoothstep(1.0, 0.6, d))

    windows, originals = {}, {}
    written = np.clip(np.round((Hn / hr + sea) * 65535.0), 0, 65535).astype(np.uint16)
    # (the base gets the same window: marsh_islands.py's islands are far off, so
    # base and final agree here, and a re-run of it keeps the glade)
    for key in ("final", "final_base"):
        if key not in files.items or files.items[key]["arr"].shape[0] != n:
            continue
        arr = files.items[key]["arr"].copy()
        arr[r0:r1, c0:c1] = written
        files.save(key, arr, (r0, r1, c0, c1), windows, originals)

    # --- biome and painted water, on their own grids
    lakes = []
    if "biomes" in files.items:
        arr = files.items["biomes"]["arr"].copy()
        m = arr.shape[0]
        cb = world.size / m
        k0, k1 = max(0, int((y - reach) / cb)), min(m, int((y + reach) / cb) + 1)
        l0, l1 = max(0, int((x - reach) / cb)), min(m, int((x + reach) / cb) + 1)
        gy_, gx_ = np.mgrid[k0:k1, l0:l1]
        GX, GY = (gx_ + 0.5) * cb - x, (gy_ + 0.5) * cb - y
        # (the rim ragged: past `rim_core` of the radius the glade's cells thin
        # out into the forest's, at random, out to `rim_out`)
        qq = np.hypot(GX, GY) / (radius * rim_f(np.arctan2(GY, GX)))
        core, outer = rule.get("rim_core", 0.8), rule.get("rim_out", 1.15)
        inside = (qq < core) | ((qq < outer) & (rng.random(qq.shape) > (qq - core) / (outer - core)))
        sub = arr[k0:k1, l0:l1]
        sub[inside] = rule["biome"]
        files.save("biomes", arr, (k0, k1, l0, l1), windows, originals)
    painted = None
    for key in ("skeleton", "skeleton_base"):
        if key not in files.items:
            continue
        arr = files.items[key]["arr"].copy()
        if painted is not None and arr.shape == files.items["skeleton"]["arr"].shape:
            k0, k1, l0, l1 = painted[0]
            arr[k0:k1, l0:l1] = painted[1]
            files.save(key, arr, (k0, k1, l0, l1), windows, originals)
            continue
        m = arr.shape[0]
        cw = world.size / m
        k0, k1 = max(0, int((y - reach) / cw)), min(m, int((y + reach) / cw) + 1)
        l0, l1 = max(0, int((x - reach) / cw)), min(m, int((x + reach) / cw) + 1)
        gy_, gx_ = np.mgrid[k0:k1, l0:l1]
        WX, WY = (gx_ + 0.5) * cw, (gy_ + 0.5) * cw
        uu = (WX - pond_c[0]) * cs + (WY - pond_c[1]) * sn
        vv = -(WX - pond_c[0]) * sn + (WY - pond_c[1]) * cs
        wet = np.hypot(uu / prx, vv / pry) < 0.6
        if not wet.any():
            wet[np.unravel_index(np.argmin(np.hypot(uu / prx, vv / pry)), wet.shape)] = True
        for o in pools:
            wet |= np.hypot(WX - o["x"], WY - o["y"]) < o["r"] * 0.8
        sub = arr[k0:k1, l0:l1]
        sub[wet] = [0, 0, 255]
        if key == "skeleton":
            lakes = [[round(float(a_), 1), round(float(b_), 1)] for a_, b_ in zip(WX[wet], WY[wet])]
            painted = ((k0, k1, l0, l1), sub.copy())
        files.save(key, arr, (k0, k1, l0, l1), windows, originals)

    np.savez_compressed(orig_file, **originals)
    # marsh_islands.py's record of its own output: kept in step when it was its own.
    marsh = Path(stem + "_marsh_islands.json")
    if marsh.exists():
        rec = json.loads(marsh.read_text(encoding="utf-8"))
        for key, path in (("final_sha1", stem + "_final.r16"), ("skeleton_sha1", stem + "_water_skeleton.png")):
            if rec.get(key) and rec[key] == before.get(key):
                rec[key] = file_sha(path)
        marsh.write_text(json.dumps(rec, indent=1, ensure_ascii=False), encoding="utf-8")
    result = {
        "comment": "Written by tools/terrain_lab/stele_glade.py; world_stele.py stands the stele here.",
        "site": [round(x, 1), round(y, 1)], "yaw": round(yaw, 4), "level": round(level, 2),
        "radius_m": round(radius, 1), "scale": rule["scale"], "sink_m": rule["sink_m"],
        "pond": {"centre": [round(pond_c[0], 1), round(pond_c[1], 1)], "radii_m": [prx, pry],
                 "turn": round(yaw, 4), "level": round(level - 0.5, 2), "cells": lakes},
        "pools": pools, "zones_sha1": zones_sha, "windows": windows,
        "slope_kept": round(float(math.hypot(b, c) * rule["keep_slope"]), 4),
    }
    out_json.write_text(json.dumps(result, indent=1), encoding="utf-8")
    print(f"stele glade at {x:.0f}, {y:.0f} (u {x / world.size:.4f}, v {y / world.size:.4f}), level {level:.1f} m, "
          f"radius {radius:.0f} m, turn {math.degrees(yaw):.0f} deg, pond {len(lakes)} cells, {len(pools)} pools, "
          f"ground fall kept {result['slope_kept'] * 100:.1f}%")


if __name__ == "__main__":
    main()
