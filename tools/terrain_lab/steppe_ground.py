"""The canyon steppe's and the stone desert's ground (doc/BIOME_PLAN.md 3.3).

    python tools/terrain_lab/steppe_ground.py [--world terrain_lab_out/world] [--seed N]

Paints three biomes of their own into <world>_biomes.png, on its grid (64 m),
from the heights <world>_final.r16, by the rules in
content/config/terrain_core/steppe_ground.json:

- canyon_steppe (3) and stone_desert (16) re-sorted by the ground: the bare dry
  tops on every flat tier above the floor, stone on walls and floor;
- mesa_pocket (19): the burnt grass, only in the hearts of the biggest islands
  (a layer's `mark`: the masks steppe_relief.py leaves in <world>_steppe_marks.npz);
- canyon_floor (20): cracked dry crust on the flat low ground of the canyons;
- dry_wash (21): the last streams' dry washes and their fans (mark `wash`).

Their materials and plants are in biomes.json and foliage.json under the same
ids; the shader blends them into their neighbours like any biome border.

Re-run safe: the ids it replaced are kept in <world>_steppe_ground.png
(0 = untouched) and put back before painting again (run it after
steppe_relief.py: the tops and floors follow the cut relief). The numbers go to
<world>_steppe_ground.json and, as `steppe_ground`, into the checks
world_features.py writes.
"""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as nd

ROOT = Path(__file__).resolve().parents[2]
RULES = ROOT / "content" / "config" / "terrain_core" / "steppe_ground.json"
WORLD_RULES = ROOT / "content" / "config" / "terrain_core" / "world_rules.json"


def value_noise(shape, cell_px, rng):
    """Smooth value noise over `shape` with cells of `cell_px` pixels, plus an
    octave at a third of that so the patches have ragged edges."""
    out = np.zeros(shape, np.float32)
    for scale, weight in ((cell_px, 0.7), (cell_px / 3.0, 0.3)):
        gh = int(np.ceil(shape[0] / scale)) + 2
        gw = int(np.ceil(shape[1] / scale)) + 2
        grid = rng.random((gh, gw)).astype(np.float32)
        y = np.arange(shape[0]) / scale
        x = np.arange(shape[1]) / scale
        out += weight * nd.map_coordinates(grid, np.meshgrid(y, x, indexing="ij"), order=3, mode="nearest")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", default="terrain_lab_out/world")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    args = ap.parse_args()
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    world_m = json.loads(WORLD_RULES.read_text(encoding="utf-8"))["world_km"] * 1000.0

    biomes_path = Path(args.world + "_biomes.png")
    side_path = Path(args.world + "_steppe_ground.png")
    biomes = np.array(Image.open(biomes_path))
    if side_path.exists():
        before = np.array(Image.open(side_path))
        back = before > 0
        biomes[back] = before[back]
    original = biomes.copy()

    raw = np.fromfile(args.world + "_final.r16", "<u2")
    n = int(np.sqrt(raw.size))
    h = (raw.reshape(n, n).astype(np.float32) / 65535.0 - args.sea) * args.height_range
    b = biomes.shape[0]
    k = n // b
    h = h.reshape(b, k, b, k).mean(axis=(1, 3))
    h = h[::-1]   # the heights' rows run south to north, the biome picture's north to south
    cell = world_m / b

    gy, gx = np.gradient(nd.uniform_filter(h, 3), cell)
    slope = np.hypot(gx, gy)
    win = max(3, int(round(rules["rel_window_m"] / cell)) | 1)
    lo = nd.minimum_filter(h, win)
    hi = nd.maximum_filter(h, win)
    rel = (h - lo) / np.maximum(hi - lo, 1.0)
    sigma = rules["wet_window_m"] / cell / 2.0
    hollow = nd.gaussian_filter(h, sigma) - h          # > 0 in a hollow
    wet = np.clip(hollow / max(np.percentile(np.abs(hollow), 95), 1e-3) * 0.5 + 0.5, 0, 1)

    marks_path = Path(args.world + "_steppe_marks.npz")
    marks = dict(np.load(marks_path)) if marks_path.exists() else {}

    report = {"cell_m": cell, "layers": {}}
    for li, layer in enumerate(rules["layers"]):
        rng = np.random.default_rng(args.seed * 7919 + layer["biome"] * 104729 + li)
        noise = value_noise(biomes.shape, layer["noise_m"] / cell, rng)
        cand = np.isin(biomes, layer["in"]) & (slope <= layer.get("slope_max", 9.0)) & (slope >= layer.get("slope_min", 0.0))
        cand &= (rel >= layer.get("rel_min", 0.0)) & (rel <= layer.get("rel_max", 1.0))
        cand &= wet <= layer.get("wet_max", 1.0)
        if "mark" in layer:
            cand &= marks[layer["mark"]].astype(bool) if layer["mark"] in marks else False
        paint = np.zeros_like(cand)
        stats = {}
        if "share" in layer:
            # Per top: a connected stretch of the candidates gets its own share.
            labels, count = nd.label(cand)
            ww = layer.get("wet_weight", 0.0)
            score = noise * (1.0 - ww) + wet * ww
            shares = []
            sizes = nd.sum(np.ones_like(labels), labels, index=np.arange(1, count + 1))
            for i in range(1, count + 1):
                if sizes[i - 1] < layer.get("min_top_cells", 1):
                    continue
                m = labels == i
                s = rng.uniform(*layer["share"])
                thr = np.quantile(score[m], 1.0 - s)
                pick = m & (score >= thr)
                paint |= pick
                shares.append(float(pick.sum() / m.sum()))
            stats["tops"] = len(shares)
            if shares:
                stats["share_per_top"] = [round(min(shares), 3), round(float(np.mean(shares)), 3), round(max(shares), 3)]
            need = layer["share"]
            stats["ok"] = bool(not shares or (min(shares) >= need[0] * 0.5 and max(shares) <= need[1] * 1.5))
        else:
            if cand.any():
                thr = np.quantile(noise[cand], 1.0 - layer["cover"])
                paint = cand & (noise >= thr)
        biomes[paint] = layer["biome"]
        stats["candidates_km2"] = round(float(cand.sum()) * cell * cell / 1e6, 2)
        stats["painted_km2"] = round(float(paint.sum()) * cell * cell / 1e6, 2)
        for src in layer["in"]:
            area = float((original == src).sum()) * cell * cell / 1e6
            stats[f"share_of_{src}"] = round(float((paint & (original == src)).sum()) * cell * cell / 1e6 / max(area, 1e-6), 3)
        report["layers"][layer["name"]] = stats
        print(f"{layer['name']:13s} -> {layer['biome']}: " + json.dumps(stats))

    side = np.where(biomes != original, original, 0).astype(np.uint8)
    Image.fromarray(side).save(side_path)
    Image.fromarray(biomes.astype(np.uint8)).save(biomes_path)
    report["ok"] = all(v.get("ok", True) for v in report["layers"].values())
    Path(args.world + "_steppe_ground.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
