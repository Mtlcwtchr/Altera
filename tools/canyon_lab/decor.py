"""Small decor of the canyon (user 15.10): blocks sticking out of the scree,
fallen slabs at the walls' feet, stones on the floor and the benches -
broken slab-like rocks of the canyon's own stone, scattered by the masks the
analysis reads off the ground, density and spread set per mask.

    python tools/canyon_lab/decor.py --tag T | --dir RUN [--seed 9] [--views]

Run after place.py (and paths.py, whose ways it keeps clear of). Writes
decor/rock_<k>_lod{0,1,2}.mesh (FOLM v2: a few broken blocks, 1 m long,
foot at 0), decor.json (instances in the run's frame: rock, x, y, z, yaw,
tilt and its downhill direction, size, mask; the counts and checks). Rules in canyon.json `decor`:
per mask `per_ha` (rocks a hectare), `size_m` (long side), `bury` (share of
its height under the ground), `gap_k` (spacing in sizes), and where it is
(`talus_m`, `wall_m`...). With --views renders close-ups (blender_view.py).
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from scipy.spatial import ConvexHull, cKDTree

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "terrain_lab"))
import marsh_hill_mesh  # noqa: E402  (FOLM writer)

BLENDER = Path("C:/Program Files/Blender Foundation/Blender 5.2/blender.exe")


def rock_mesh(rng):
    """A broken block of bedded stone: a slab-shaped hull with a corner or
    two knocked off, 1 m long, its foot at z 0."""
    n = 26
    p = rng.uniform(-0.5, 0.5, (n, 3)) * np.array([1.0, rng.uniform(0.5, 0.85), rng.uniform(0.25, 0.55)])
    # (bedding: its top and bottom faces flat-ish, as the strata broke)
    p[:, 2] = np.where(np.abs(p[:, 2]) > 0.6 * np.abs(p[:, 2]).max(), np.sign(p[:, 2]) * np.abs(p[:, 2]).max(), p[:, 2])
    hull = ConvexHull(p)
    v = hull.points
    tris = v[hull.simplices]
    # Outward faces (the hull's simplices are not ordered).
    c = v.mean(axis=0)
    nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    flip = (nrm * (tris.mean(axis=1) - c)).sum(1) < 0
    tris[flip] = tris[flip][:, ::-1]
    tris[..., 2] -= v[:, 2].min()
    tris[..., :2] -= c[:2]
    return tris


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--dir")
    ap.add_argument("--seed", type=int, default=9)
    ap.add_argument("--views", action="store_true")
    args = ap.parse_args()
    out = Path(args.dir) if args.dir else ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    D = json.loads((HERE / "canyon.json").read_text(encoding="utf-8"))["decor"]
    st = json.loads((out / "stats.json").read_text(encoding="utf-8"))
    cell = st["cell_m"]
    L = np.load(out / "layers.npz")
    h = L["h"].astype(np.float32)
    zone = L["mask"] if "mask" in L.files else np.ones(h.shape, bool)
    rng = np.random.default_rng(args.seed)

    # The rocks.
    mdir = out / "decor"
    mdir.mkdir(exist_ok=True)
    for f in mdir.glob("*.mesh"):
        f.unlink()
    shapes = []
    for k in range(D["variants"]):
        t = rock_mesh(rng)
        for lod in range(3):
            marsh_hill_mesh.write(mdir / f"rock_{k}_lod{lod}.mesh", t, 0.0)
        shapes.append(dict(hi=float(t[..., 2].max()), r=float(np.hypot(t[..., 0], t[..., 1]).max())))

    # The masks, off the ground and the generator's layers.
    gy, gx = np.gradient(h, cell)
    sl = np.degrees(np.arctan(np.hypot(gx, gy)))
    rel_isl, talus, wash = L["rel_isl"], L["talus"], L["wash"]
    wall = sl > 60
    d_wall = nd.distance_transform_edt(~wall) * cell
    tall = nd.maximum_filter(np.where(wall, h - nd.minimum_filter(h, size=9), 0), size=9) > D["wall_min_m"]
    d_tall = nd.distance_transform_edt(~(wall & tall)) * cell
    masks = {
        "talus": (talus > D["talus_m"]) & (sl < 45),
        "wall_foot": (d_tall < D["wall_foot_m"]) & ~wall & (sl < 40) & (talus <= D["talus_m"]),
        "floor": (rel_isl < 1) & (talus <= 0.3) & ~wash & (sl < 20),
        "bench": (rel_isl >= 1) & (rel_isl <= 20) & (sl < 25) & (talus <= 0.3),
        "top": (rel_isl > 20) & (sl < 20),
    }
    taken = np.zeros(h.shape, bool)
    for k in masks:
        masks[k] &= zone & ~taken
        taken |= masks[k]

    # Kept clear of: pinnacles, the ways.
    avoid = []
    inst = out / "instances.json"
    if inst.exists():
        avoid += [(i["x"], i["y"], i["foot_r"] + 1.0) for i in json.loads(inst.read_text(encoding="utf-8"))["instances"]]
    ways = out / "paths.json"
    if ways.exists():
        for wy in json.loads(ways.read_text(encoding="utf-8"))["ways"]:
            for p in wy["pts"][::2]:
                avoid.append((p[0], p[1], wy["width"] / 2 + 1.5))
    av = np.array(avoid).reshape(-1, 3)
    av_tree = cKDTree(av[:, :2]) if len(av) else None
    av_r = float(av[:, 2].max()) if len(av) else 0.0

    rocks, counts, dropped = [], {}, {"clash": 0, "steep": 0, "hang": 0, "avoid": 0}
    for name, rule in D["masks"].items():
        m = masks[name]
        area = float(m.sum()) * cell * cell
        want = int(rule["per_ha"] * area / 1e4)
        ys, xs = np.nonzero(m)
        if not len(ys) or want == 0:
            counts[name] = 0
            continue
        # Denser where the mask is deeper (more scree, nearer the wall).
        if name == "talus":
            wgt = np.minimum(talus[ys, xs], 12.0)
        elif name == "wall_foot":
            wgt = 1.0 / (1.0 + d_tall[ys, xs] / 4.0)
        else:
            wgt = np.ones(len(ys))
        pick = rng.choice(len(ys), size=min(want * 3, len(ys)), replace=False, p=wgt / wgt.sum())
        placed = 0
        grid = {}                       # (cell of 8 m) -> [(x, y, size)]
        for i in pick:
            if placed >= want:
                break
            size = float(np.exp(rng.uniform(np.log(rule["size_m"][0]), np.log(rule["size_m"][1]))))
            x, y = (xs[i] + rng.random()) * cell, (ys[i] + rng.random()) * cell
            gk = (int(x // 8), int(y // 8))
            near = [q for a in (-1, 0, 1) for b in (-1, 0, 1) for q in grid.get((gk[0] + a, gk[1] + b), ())]
            if any(math.hypot(q[0] - x, q[1] - y) < rule["gap_k"] * 0.5 * (size + q[2]) for q in near):
                dropped["clash"] += 1
                continue
            if av_tree is not None:
                idx = av_tree.query_ball_point((x, y), size * 0.6 + av_r)
                if idx and (np.hypot(av[idx, 0] - x, av[idx, 1] - y) < av[idx, 2] + size * 0.6).any():
                    dropped["avoid"] += 1
                    continue
            k = int(rng.integers(len(shapes)))
            sh = shapes[k]
            r = sh["r"] * size
            hgt = sh["hi"] * size
            # The ground under its footprint as a plane: it lies on the slope
            # (tilted with it), sunk `bury` of its height at its middle.
            ring = np.linspace(0, 2 * np.pi, 8, endpoint=False)
            ox, oy = np.cos(ring) * r * 0.8, np.sin(ring) * r * 0.8
            g = nd.map_coordinates(h, [(y + oy) / cell, (x + ox) / cell], order=1, mode="nearest")
            g0 = float(nd.map_coordinates(h, [[y / cell], [x / cell]], order=1, mode="nearest")[0])
            A = np.stack([ox, oy, np.ones_like(ox)], 1)
            (sx, sy, c0), *_ = np.linalg.lstsq(A, g, rcond=None)
            tilt = float(np.degrees(np.arctan(math.hypot(sx, sy))))
            if tilt > rule.get("tilt_max_deg", 40):
                dropped["steep"] += 1
                continue
            # (a lump or a step under it the plane misses: it would float there)
            resid = g - A @ np.array([sx, sy, c0])
            if -resid.min() > rule["bury"] * hgt - 0.05 or resid.max() > (1 - rule["bury"]) * hgt * 0.8:
                dropped["hang"] += 1
                continue
            z = float(c0) - rule["bury"] * hgt
            rocks.append(dict(rock=f"rock_{k}", x=round(x, 2), y=round(y, 2), z=round(z, 2),
                              yaw=round(float(rng.uniform(0, 360)), 1), tilt=round(tilt, 1),
                              tilt_dir=round(float(np.degrees(np.arctan2(-sy, -sx))), 1),
                              size=round(size, 2), mask=name, out_m=round(z + hgt - g0, 2),
                              under_m=round(float(rule["bury"] * hgt + resid.min()), 2)))
            placed += 1
            grid.setdefault(gk, []).append((x, y, size))
        counts[name] = placed

    zone_km2 = float(zone.sum()) * cell * cell / 1e6
    res = dict(
        rocks=len(rocks), per_km2=round(len(rocks) / max(zone_km2, 1e-6)),
        by_mask=counts, mask_km2={k: round(float(m.sum()) * cell * cell / 1e6, 3) for k, m in masks.items()},
        dropped=dropped,
        size_m={k: [round(min((r["size"] for r in rocks if r["mask"] == k), default=0), 2),
                    round(max((r["size"] for r in rocks if r["mask"] == k), default=0), 2)] for k in counts},
        checks=dict(
            # every rock stands out of the ground; none floats: its foot under
            # the ground all round (under_m > 0 at every sample of its footprint)
            standing_out_min_m=round(min((r["out_m"] for r in rocks), default=0), 2),
            floating=int(sum(1 for r in rocks if r["under_m"] <= 0)),
        ),
    )
    (out / "decor.json").write_text(json.dumps(dict(cell_m=cell, rocks=rocks, result=res)), encoding="utf-8")
    print(json.dumps(res, indent=1))
    if args.views:
        subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P", str(HERE / "blender_view.py"),
                        "--", str(out), "--paths", "--decor", "--only", "decor0,decor1,decor2"], check=True,
                       stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
