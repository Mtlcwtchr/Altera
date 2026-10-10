"""Builds the generated world, start to finish, into terrain_lab_out/world_*.

    python tools/terrain_lab/build_world.py [--seed N] [--out DIR] [--rules FILE] [--no-erode]

1. world_gen.py: layout, zones, relief from the rules     -> world_height.png, world_biomes.png
2. terrain_lab --erode: thermal erosion on the GPU (8192^2) -> world_eroded.r16
3. world_gen.py --finish: the cliffs put back              -> world_final.r16
4. world_features.py: spires, landmarks, cliff meshes     -> world_features.json, world_cliffs/

Then view it with the run configuration "Terrain lab (generated world)", or
    terrain_lab.exe --heightmap terrain_lab_out/world_final.r16 --world-km 262.144 --height-range 6000
                    --sea 0.13 --biomes terrain_lab_out/world_biomes.png --water-rules terrain_lab_out/world_water.png
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "terrain_lab_out"
LAB = ROOT / "cmake-build-windows" / "terrain_lab.exe"


def run(*args):
    print(">", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def build(seed, out, rules_file, no_erode=False):
    """Builds one world into `out`/world_*; returns the world's size in km."""
    out.mkdir(parents=True, exist_ok=True)
    km = json.load(open(ROOT / rules_file, encoding="utf-8"))["world_km"]
    gen = [sys.executable, "tools/terrain_lab/world_gen.py", "--rules", rules_file, "--out", out / "world"]
    if seed is not None:
        gen += ["--seed", seed]
    run(*gen, "--heights", 2048)
    if no_erode:
        return km
    run(LAB, "--headless", "64x64", "--heightmap", out / "world_height.png", "--world-km", km,
        "--height-range", 6000, "--sea", 0.13, "--erode", out / "world_eroded.r16", "--erode-scale", 4)
    run(*gen, "--finish", out / "world_eroded.r16")
    # The mesh part of the hybrid: spires, landmarks and cliff walls placed on
    # this very heightmap (doc/rework/MESH_TERRAIN_PLAN.md).
    feat = [sys.executable, "tools/terrain_lab/world_features.py", "--world", out / "world"]
    if seed is not None:
        feat += ["--seed", seed]
    run(*feat)
    print(f"done: {out / 'world_final.r16'}  (preview: {out / 'world_final_preview.png'})")
    return km


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--rules", default="content/config/terrain_core/world_rules.json")
    ap.add_argument("--no-erode", action="store_true", help="stop after the raw relief (no GPU needed)")
    args = ap.parse_args()
    build(args.seed, Path(args.out), args.rules, args.no_erode)


if __name__ == "__main__":
    main()
