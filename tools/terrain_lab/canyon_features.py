"""The canyon's meshes in the world (user 15.10): the sandbox's analysis and
placing (tools/canyon_lab/analyze.py, place.py) run on the world's canyon as
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
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAB = ROOT / "tools" / "canyon_lab"
RULES = ROOT / "content" / "config" / "terrain_core" / "props.json"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", default="terrain_lab_out/world")
    ap.add_argument("--seed", type=int, default=5)
    args = ap.parse_args()
    canyon = json.loads(Path(args.world + "_canyon.json").read_text(encoding="utf-8"))
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    mat, moss, cover = rules.get("materials", {}), rules.get("moss", {}), rules.get("cover", {})
    items, checks = [], {}
    for k, run in enumerate(canyon["runs"]):
        for tool in ("analyze.py", "place.py"):
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
        a = json.loads((Path(run) / "analysis.json").read_text(encoding="utf-8"))
        p = json.loads((Path(run) / "placement.json").read_text(encoding="utf-8"))
        checks[Path(run).name] = {"relief": {key: v["value"] for key, v in a["checks"].items()},
                                  "climb": a["climb"], "placing": {key: v["value"] for key, v in p["checks"].items()},
                                  "by_role": p["by_role"], "dropped": p["dropped"]}
    out = Path(args.world + "_canyon_features.json")
    out.write_text(json.dumps({"comment": "tools/terrain_lab/canyon_features.py: the canyon's pinnacles and slab rubble, "
                                          "taken in by world_features.py", "instances": items, "checks": checks},
                              separators=(",", ":")), encoding="utf-8")
    roles = {}
    for o in items:
        roles[o["canyon"]] = roles.get(o["canyon"], 0) + 1
    print(f"canyon features: {len(items)} instances -> {out.name}: " + json.dumps(roles))
    print("canyon checks: " + json.dumps(checks))


if __name__ == "__main__":
    main()
