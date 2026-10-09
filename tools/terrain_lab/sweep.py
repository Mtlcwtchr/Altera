#!/usr/bin/env python3
"""sweep - run terrain_lab's benchmark over a matrix of parameters, write a report.

    python tools/terrain_lab/sweep.py --exe cmake-build-windows/terrain_lab.exe --out doc/reports/terrain_core/step1

Each configuration is one headless run of `terrain_lab --bench all`; its
summary.json lands in OUT/runs/<name>/ and the table of all of them in
OUT/sweep.md (and sweep.json).
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (name, args). The matrix is grouped by the question it answers.
def matrix(heightmap):
    runs = []
    # 1. Step and clipmap/grid at the full world size, ~32 km view.
    for spacing in (0.25, 0.5, 1.0, 2.0):
        for clip, grid in ((128, 16), (256, 32), (512, 64), (256, 16), (512, 32)):
            runs.append((f"w2000_s{spacing}_c{clip}_g{grid}_v32",
                         ["--world-km", "2000", "--spacing", str(spacing), "--clip", str(clip),
                          "--grid", str(grid), "--view-km", "32"]))
    # 2. World size, same core settings.
    for world in (16, 256, 2000):
        runs.append((f"w{world}_s0.5_c256_g32_v32",
                     ["--world-km", str(world), "--spacing", "0.5", "--clip", "256", "--grid", "32",
                      "--view-km", "32"]))
    # 3. View distance.
    for view in (8, 16, 64):
        runs.append((f"w2000_s0.5_c256_g32_v{view}",
                     ["--world-km", "2000", "--spacing", "0.5", "--clip", "256", "--grid", "32",
                      "--view-km", str(view)]))
    # 4. The imported map, stretched over the full world.
    if heightmap:
        runs.append(("import2000_s0.5_c256_g32_v32",
                     ["--world-km", "2000", "--heightmap", str(heightmap), "--height-range", "4000",
                      "--spacing", "0.5", "--clip", "256", "--grid", "32", "--view-km", "32"]))
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "cmake-build-windows" / "terrain_lab.exe"))
    ap.add_argument("--out", default=str(ROOT / "doc" / "reports" / "terrain_core" / "step1"))
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--heightmap", default=str(ROOT / "doc" / "NewRef" / "world_heightmap_v5_4096_gray16.png"))
    ap.add_argument("--only", default="", help="substring filter on run names")
    args = ap.parse_args()

    out = Path(args.out)
    (out / "runs").mkdir(parents=True, exist_ok=True)
    cache = out / "cache"
    results = []
    for name, extra in matrix(args.heightmap if Path(args.heightmap).exists() else None):
        if args.only and args.only not in name:
            continue
        run_dir = out / "runs" / name
        run_dir.mkdir(parents=True, exist_ok=True)
        cmd = [str(Path(args.exe).resolve()), "--headless", args.size, "--bench", "all",
               "--out", str(run_dir)] + extra
        print("==", name, flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        (run_dir / "log.txt").write_text(proc.stdout + proc.stderr)
        summary = run_dir / "summary.json"
        if proc.returncode != 0 or not summary.exists():
            print(proc.stdout[-2000:], proc.stderr[-2000:], file=sys.stderr)
            continue
        data = json.loads(summary.read_text())
        data["name"] = name
        results.append(data)
        for png in run_dir.glob("generated_*.png"):
            png.unlink()   # the generated macro map is a cache, not a result
        sc = {s["name"]: s for s in data["scenarios"]}
        print("   jet p99 %.2f ms, fly p99 %.2f ms, cold %.2f ms, misses %d" % (
            sc["jet"]["p99Ms"], sc["fly"]["p99Ms"], sc["teleport"]["coldMs"],
            sum(s["coverageMisses"] for s in data["scenarios"])), flush=True)

    (out / "sweep.json").write_text(json.dumps(results, indent=1))
    lines = ["| run | levels | view km | clipmap MB | tris max | walk p99 | fly p99 | jet p99 | teleport max | cold | texels/f jet | misses |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        p = r["params"]
        sc = {s["name"]: s for s in r["scenarios"]}
        tris = max(s["trianglesMax"] for s in r["scenarios"])
        misses = sum(s["coverageMisses"] for s in r["scenarios"])
        lines.append("| %s | %d | %.1f | %.0f | %.0fk | %.2f | %.2f | %.2f | %.2f | %.2f | %.0fk | %d |" % (
            r["name"], p["levels"], p["viewKm"], p["clipmapMB"], tris / 1000, sc["walk"]["p99Ms"],
            sc["fly"]["p99Ms"], sc["jet"]["p99Ms"], sc["teleport"]["maxMs"], sc["teleport"]["coldMs"],
            sc["jet"]["texelsPerFrame"] / 1000, misses))
    (out / "sweep.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
