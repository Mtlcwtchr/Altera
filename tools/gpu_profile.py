#!/usr/bin/env python3
"""gpu_profile - where the GPU spends a frame, for any Vulkan program, from outside it.

Runs the program with the altera_gpu_layer Vulkan layer switched on through
the loader's environment (nothing in the program changes, nothing is
instrumented) and reports GPU time per shader pair, render pass load/store,
compute, copy and blit, averaged over the frames asked for:

    python tools/gpu_profile.py -- cmake-build-windows/altera_client.exe --explore
    python tools/gpu_profile.py --skip 600 --frames 300 -- <program> <args...>
    python tools/gpu_profile.py --report altera_gpu_profile.json    # read a saved one

--live prints a summary every second while the program runs. The layer is built
with the client (target altera_gpu_layer); --layer-dir points elsewhere.
Windows and Linux (Vulkan); on macOS use Instruments (tools/altera_profiler.py).

Timestamps sit at the bottom of the pipe: each segment runs from the previous
piece of work finishing to this one finishing. Frame totals are exact; the split
between neighbours that overlap on the card is an attribution.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYER = "VK_LAYER_ALTERA_gpu_profile"


def find_layer(explicit):
    if explicit:
        return Path(explicit)
    manifests = sorted(ROOT.glob("cmake-build-*/VK_LAYER_ALTERA_gpu_profile.json"),
                       key=lambda p: p.stat().st_mtime, reverse=True)
    if not manifests:
        sys.exit("no built layer found: build the altera_gpu_layer target, or pass --layer-dir")
    return manifests[0].parent


def report(path, skip, frames, top):
    data = json.loads(Path(path).read_text())
    all_frames = data["frames"]
    chosen = all_frames[skip:skip + frames] if frames else all_frames[skip:]
    if not chosen:
        sys.exit("no frames in %s after skipping %d (it holds %d)" % (path, skip, len(all_frames)))
    totals = [f["total_ms"] for f in chosen]
    sums, peaks = {}, {}
    for f in chosen:
        for label, ms in f["ms"].items():
            sums[label] = sums.get(label, 0.0) + ms
            peaks[label] = max(peaks.get(label, 0.0), ms)
    n = len(chosen)
    totals_sorted = sorted(totals)
    print("GPU frames %d (of %d recorded): mean %.2f ms, median %.2f, p95 %.2f, worst %.2f" % (
        n, len(all_frames), statistics.fmean(totals), totals_sorted[n // 2],
        totals_sorted[min(n - 1, int(n * 0.95))], totals_sorted[-1]))
    mean_total = statistics.fmean(totals) or 1.0
    print("\n  %9s %7s %9s  %s" % ("ms/frame", "share", "worst ms", "work"))
    for label, total in sorted(sums.items(), key=lambda kv: -kv[1])[:top]:
        mean = total / n
        print("  %9.3f %6.1f%% %9.3f  %s" % (mean, 100.0 * mean / mean_total, peaks[label], label))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--layer-dir", help="folder with the layer and its manifest (default: newest cmake-build-*)")
    ap.add_argument("--out", default=str(ROOT / ".cache/profiles/gpu_profile.json"), help="where the report goes")
    ap.add_argument("--skip", type=int, default=0, help="frames to leave out at the start (loading)")
    ap.add_argument("--frames", type=int, default=0, help="frames to report after the skipped ones (0: all)")
    ap.add_argument("--top", type=int, default=25, help="how many lines of work to list")
    ap.add_argument("--live", action="store_true", help="a summary line every second while it runs")
    ap.add_argument("--report", help="only print the report of an existing JSON")
    ap.add_argument("program", nargs=argparse.REMAINDER, help="-- program and its arguments")
    args = ap.parse_args()

    if args.report:
        report(args.report, args.skip, args.frames, args.top)
        return 0
    program = args.program[1:] if args.program[:1] == ["--"] else args.program
    if not program:
        ap.error("give a program to run after --")
    layer = find_layer(args.layer_dir).resolve()
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    env = dict(os.environ)
    env["VK_ADD_LAYER_PATH"] = str(layer) + (os.pathsep + env["VK_ADD_LAYER_PATH"] if env.get("VK_ADD_LAYER_PATH") else "")
    env["VK_INSTANCE_LAYERS"] = LAYER + (os.pathsep + env["VK_INSTANCE_LAYERS"] if env.get("VK_INSTANCE_LAYERS") else "")
    env["ALTERA_GPU_PROFILE_OUT"] = str(out)
    if args.live:
        env["ALTERA_GPU_PROFILE_LIVE"] = "1"
    print("profiling with %s from %s" % (LAYER, layer), flush=True)
    code = subprocess.call(program, env=env, cwd=os.getcwd())
    if not out.exists():
        sys.exit("the program left no report (exit %d): did it create a Vulkan device and close it?" % code)
    print("report: %s (program exit %d)\n" % (out, code))
    report(out, args.skip, args.frames, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
