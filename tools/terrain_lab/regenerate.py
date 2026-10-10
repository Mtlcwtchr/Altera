"""Another world by the same rules, another seed - started from the lab with G.

Builds it into terrain_lab_out/seed_<S>/ (the worlds stay side by side to compare),
starts a new lab on it and closes the one that asked (--pid).

    python tools/terrain_lab/regenerate.py [--seed S] [--pid PID]
"""
import argparse
import random
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_world as bw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--rules", default="content/config/terrain_core/world_rules.json")
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else random.randint(1, 99999)
    out = bw.OUT / f"seed_{seed}"
    print(f"=== world, seed {seed} -> {out}", flush=True)
    km = bw.build(seed, out, args.rules)
    lab = [str(bw.LAB), "--heightmap", out / "world_final.r16", "--world-km", km, "--height-range", 6000, "--sea", 0.13,
           "--biomes", out / "world_biomes.png", "--water-rules", out / "world_water.png",
           "--main-river-area", 250, "--main-lake-area", 4]
    if args.pid:
        subprocess.run(["taskkill", "/F", "/PID", str(args.pid)], check=False)
    subprocess.Popen([str(a) for a in lab], cwd=bw.ROOT)
    print("lab started, seed", seed)


if __name__ == "__main__":
    main()
