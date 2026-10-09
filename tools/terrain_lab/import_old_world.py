#!/usr/bin/env python3
"""import_old_world - the old assembled world (doc/OldWorld/assembled) as a
terrain-core macro map.

    python tools/terrain_lab/import_old_world.py [--out doc/OldWorld/terrain_core_main.png]

The assembled world is 1048 x 2228 km at 256 m a sample; the terrain core
takes square worlds. This cuts the square 4096 samples (1048.576 km = 2^20 m)
a side round the main continent - the northern cluster; the southern
archipelagos fall outside - and keeps the heights as they are (u16 over
min_m..max_m). It prints the terrain_lab arguments that put the sea where the
assembled world had it.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "doc" / "OldWorld" / "assembled"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "doc" / "OldWorld" / "terrain_core_main.png"))
    args = ap.parse_args()

    world = json.loads((WORLD / "world.json").read_text(encoding="utf-8"))
    spec = world["rasters"]["height"]
    lo, hi = float(spec["min_m"]), float(spec["max_m"])
    heights = np.array(Image.open(WORLD / spec["file"]))
    rows, cols = heights.shape
    side = min(rows, cols)
    sea = int(round(-lo / (hi - lo) * 65535))

    # The northern cluster: land rows down to the first run of open sea
    # more than 50 km wide, centred in the square.
    land_rows = np.where((heights > sea).any(axis=1))[0]
    gaps = np.diff(land_rows)
    wide = np.where(gaps * world["world"]["sample_m"] > 50000)[0]
    split = int(wide[0]) if len(wide) else len(land_rows) - 1
    first, last = land_rows[0], land_rows[split]
    centre = (first + last) // 2
    y0 = int(np.clip(centre - side // 2, 0, rows - side))
    crop = heights[y0:y0 + side, :side]
    Image.fromarray(crop.astype(np.uint16)).save(args.out)

    sample = world["world"]["sample_m"]
    print(f"cut rows {y0}..{y0 + side} of {rows}: {side}^2 samples, {side * sample / 1000:.3f} km a side")
    print(f"heights {lo:.0f}..{hi:.0f} m, sea at {sea / 65535:.4f} of the range")
    print(f"wrote {args.out}")
    print("terrain_lab arguments:")
    print(f"  --heightmap {Path(args.out).relative_to(ROOT).as_posix()} --world-km {side * sample / 1000:.3f} "
          f"--height-range {hi - lo:.0f} --sea {sea / 65535:.4f}")


if __name__ == "__main__":
    main()
