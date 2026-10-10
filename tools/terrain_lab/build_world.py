"""Builds the generated world, start to finish, into terrain_lab_out/world_*.

    python tools/terrain_lab/build_world.py [--seed N] [--rules FILE] [--no-erode]

1. world_gen.py: layout, zones, relief from the rules     -> world_height.png, world_biomes.png
2. terrain_lab --erode: thermal erosion on the GPU (8192^2) -> world_eroded.r16
3. world_gen.py --finish: the cliffs put back              -> world_final.r16

Then view it with the run configuration "Terrain lab (generated world)", or
    terrain_lab.exe --heightmap terrain_lab_out/world_final.r16 --world-km 524.288 --height-range 6000
                    --sea 0.13 --biomes terrain_lab_out/world_biomes.png --water-rules terrain_lab_out/world_water.png
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "terrain_lab_out"
LAB = ROOT / "cmake-build-windows" / "terrain_lab.exe"


def run(*args):
    print(">", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--rules", default="content/config/terrain_core/world_rules.json")
    ap.add_argument("--no-erode", action="store_true", help="stop after the raw relief (no GPU needed)")
    args = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    gen = [sys.executable, "tools/terrain_lab/world_gen.py", "--rules", args.rules, "--out", OUT / "world"]
    if args.seed is not None:
        gen += ["--seed", args.seed]
    run(*gen, "--heights", 2048)
    if args.no_erode:
        return
    run(LAB, "--headless", "64x64", "--heightmap", OUT / "world_height.png", "--world-km", 524.288,
        "--height-range", 6000, "--sea", 0.13, "--erode", OUT / "world_eroded.r16", "--erode-scale", 4)
    run(*gen, "--finish", OUT / "world_eroded.r16")
    print(f"\ndone: {OUT / 'world_final.r16'}  (preview: {OUT / 'world_final_preview.png'})")


if __name__ == "__main__":
    main()
