"""The canyon's meshes in the world (user 15.10): the sandbox's analysis and
placing (tools/canyon_lab/analyze.py, place.py) and its ways up and across
(paths.py: spiral ledges round columns, stone bridges) run on the world's canyon as
canyon_relief.py cut it - pinnacles on the columns' and peaks' sites, heaps
of slab rubble at the tall ones' feet and along the walls' feet - and turned
into world_features.json's instances.

    python tools/terrain_lab/canyon_features.py [--world terrain_lab_out/world]

Run after canyon_relief.py; world_features.py takes <world>_canyon_features.json
in. Each instance stands at the height the canyon's own ground gives it
(`z`: the client's height under it is the macro map's, not the fine relief's).
"""

import argparse
import json
import math
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
LAB = ROOT / "tools" / "canyon_lab"
RULES = ROOT / "content" / "config" / "terrain_core" / "props.json"


def write_decor(path, rocks, ox, oy):
    """A decor list (src/terrain_core/props.hpp): "DECR", u32 version 1, u32
    model count, each name (u16 length, bytes); u32 rock count, each f32 x, y
    (world metres), z, turn (radians), size (metres), u32 model."""
    names = sorted({r["m"] for r in rocks})
    index = {n: i for i, n in enumerate(names)}
    with open(path, "wb") as fh:
        fh.write(b"DECR")
        fh.write(struct.pack("<II", 1, len(names)))
        for n in names:
            b = n.encode("utf-8")
            fh.write(struct.pack("<H", len(b)) + b)
        fh.write(struct.pack("<I", len(rocks)))
        rec = np.zeros(len(rocks), dtype=[("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("yaw", "<f4"), ("s", "<f4"), ("m", "<u4")])
        rec["x"] = [ox + r["x"] for r in rocks]
        rec["y"] = [oy + r["y"] for r in rocks]
        rec["z"] = [r["z"] for r in rocks]
        rec["yaw"] = [r["m_yaw"] for r in rocks]
        rec["s"] = [r["size"] for r in rocks]
        rec["m"] = [index[r["m"]] for r in rocks]
        fh.write(rec.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", default="terrain_lab_out/world")
    ap.add_argument("--seed", type=int, default=5)
    args = ap.parse_args()
    canyon = json.loads(Path(args.world + "_canyon.json").read_text(encoding="utf-8"))
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    mat, moss, cover = rules.get("materials", {}), rules.get("moss", {}), rules.get("cover", {})
    items, checks, decors = [], {}, []
    for k, run in enumerate(canyon["runs"]):
        for tool in ("analyze.py", "place.py", "paths.py", "decor.py"):
            subprocess.run([sys.executable, str(LAB / tool), "--dir", run, "--seed", str(args.seed + k)]
                           if tool == "place.py" else [sys.executable, str(LAB / tool), "--dir", run],
                           check=True, stdout=subprocess.DEVNULL)
        st = json.loads((Path(run) / "stats.json").read_text(encoding="utf-8"))
        ox, oy = st["origin_m"]
        placed = json.loads((Path(run) / "instances.json").read_text(encoding="utf-8"))["instances"]
        for i in placed:
            o = {"m": i["name"], "x": round(ox + i["x"], 2), "y": round(oy + i["y"], 2), "z": round(i["z"], 2),
                 "yaw": round(math.radians(i["yaw"]), 4),
                 "s": [round(i["s"] * i["k"], 4), round(i["s"] * i["k"], 4), round(i["s"], 4)],
                 "material": mat.get(i["kind"], "rock_orange"), "cat": "terrain", "canyon": i["role"]}
            for key, table in (("moss", moss), ("cover", cover)):
                if table.get(i["kind"], 0) > 0:
                    o[key] = table[i["kind"]]
            items.append(o)
        # The bridges and spiral ledges (paths.py): a mesh each, its own heights.
        ways = json.loads((Path(run) / "paths.json").read_text(encoding="utf-8"))
        mesh_dir = os.path.relpath(Path(run) / "paths", Path(args.world).parent).replace("\\", "/")
        for wy in ways["ways"]:
            items.append({"m": wy["name"], "dir": mesh_dir, "x": round(ox + wy["origin"][0], 2),
                          "y": round(oy + wy["origin"][1], 2), "z": 0.0, "yaw": 0.0, "s": [1.0, 1.0, 1.0],
                          "material": mat.get("canyon_pinnacle", "rock_orange"), "cat": "terrain", "canyon": wy["kind"]})
        # The small rocks (decor.py): too many for the features' JSON - a
        # binary list beside it, read by the client's props (props.hpp).
        dec = json.loads((Path(run) / "decor.json").read_text(encoding="utf-8"))
        decor_file = Path(f"{args.world}_canyon_decor_{k + 1}.bin")
        write_decor(decor_file, dec["rocks"], ox, oy)
        decors.append({"file": decor_file.name, "dir": os.path.relpath(Path(run) / "decor", Path(args.world).parent).replace("\\", "/"),
                       "material": mat.get("canyon_pinnacle", "rock_orange"), "rocks": len(dec["rocks"])})
        a = json.loads((Path(run) / "analysis.json").read_text(encoding="utf-8"))
        p = json.loads((Path(run) / "placement.json").read_text(encoding="utf-8"))
        checks[Path(run).name] = {"relief": {key: v["value"] for key, v in a["checks"].items()},
                                  "climb": a["climb"], "placing": {key: v["value"] for key, v in p["checks"].items()},
                                  "by_role": p["by_role"], "dropped": p["dropped"],
                                  "ways": {k: v for k, v in ways["result"].items() if k != "spirals_dropped"},
                                  "decor": {k: v for k, v in dec["result"].items() if k != "dropped"}}
    out = Path(args.world + "_canyon_features.json")
    out.write_text(json.dumps({"comment": "tools/terrain_lab/canyon_features.py: the canyon's pinnacles and slab rubble, "
                                          "taken in by world_features.py", "instances": items, "decor": decors,
                                          "checks": checks},
                              separators=(",", ":")), encoding="utf-8")
    roles = {}
    for o in items:
        roles[o["canyon"]] = roles.get(o["canyon"], 0) + 1
    print(f"canyon features: {len(items)} instances -> {out.name}: " + json.dumps(roles))
    print("canyon checks: " + json.dumps(checks))


if __name__ == "__main__":
    main()
