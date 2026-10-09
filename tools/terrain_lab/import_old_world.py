#!/usr/bin/env python3
"""import_old_world - the old assembled world (doc/OldWorld/assembled) as a
terrain-core macro map.

    python tools/terrain_lab/import_old_world.py [--out doc/OldWorld/terrain_core_full.png]

The assembled world is 1048 x 2228 km at 256 m a sample, the main continent
in the north and the archipelagos in the south. The terrain core takes square
worlds, so this lays the whole of it into the square of 2^21 m (2097 km,
8192 samples): centred east-west with open sea either side, and cut north-south
only where there is nothing but sea. Heights are kept as they are (u16 over
min_m..max_m); the script prints the terrain_lab arguments that put the sea
where the assembled world had it.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "doc" / "OldWorld" / "assembled"
SIDE = 8192   # 2^21 m at 256 m a sample


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "doc" / "OldWorld" / "terrain_core_full.png"))
    args = ap.parse_args()

    world = json.loads((WORLD / "world.json").read_text(encoding="utf-8"))
    spec = world["rasters"]["height"]
    lo, hi = float(spec["min_m"]), float(spec["max_m"])
    sample = world["world"]["sample_m"]
    heights = np.array(Image.open(WORLD / spec["file"]))
    rows, cols = heights.shape
    sea = int(round(-lo / (hi - lo) * 65535))
    floor = int(round((float(spec.get("default_m", lo)) - lo) / (hi - lo) * 65535))

    # North-south: keep every row with land, centred in the square.
    land_rows = np.where((heights > sea).any(axis=1))[0]
    first, last = int(land_rows[0]), int(land_rows[-1])
    if last - first + 1 > SIDE:
        raise SystemExit(f"land spans {last - first + 1} rows, more than {SIDE}")
    y0 = int(np.clip((first + last) // 2 - SIDE // 2, 0, max(0, rows - SIDE)))
    band = heights[y0:y0 + SIDE]

    square = np.full((SIDE, SIDE), floor, dtype=np.uint16)
    x0 = (SIDE - cols) // 2
    square[: band.shape[0], x0:x0 + cols] = band
    Image.fromarray(square).save(args.out)

    print(f"rows {y0}..{y0 + band.shape[0]} of {rows} (land {first}..{last}), columns centred at {x0}")
    print(f"{SIDE}^2 samples, {SIDE * sample / 1000:.3f} km a side; heights {lo:.0f}..{hi:.0f} m, sea at {sea / 65535:.4f}")
    print(f"wrote {args.out}")
    print("terrain_lab arguments:")
    print(f"  --heightmap {Path(args.out).relative_to(ROOT).as_posix()} --world-km {SIDE * sample / 1000:.3f} "
          f"--height-range {hi - lo:.0f} --sea {sea / 65535:.4f}")


if __name__ == "__main__":
    main()
