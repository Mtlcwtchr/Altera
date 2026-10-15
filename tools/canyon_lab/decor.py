"""Small decor of the canyon (user 15.10; v2 after "не натурально"): the
rock the walls shed, laid where it would fall.

    python tools/canyon_lab/decor.py --tag T | --dir RUN [--seed 9] [--views]

Run after analyze.py and place.py (and paths.py, whose ways it keeps clear
of). Rocks are fragments of bedded stone - boxes of slab proportions broken
by a few planes, sharp-edged, 1 m long (decor/rock_<k>_lod{0,1,2}.mesh,
FOLM v2, foot at 0).

How they lie (canyon.json `decor`):
- Falls: from the walls' rims (mesh_sites.json walls, `per_wall` falls each,
  spread `spread_m` along the wall) blocks drop to the foot and roll on down
  the slope: sizes by a power law (`size_m`, `alpha`: many small, few big),
  the big ones run out further (`runout_m` times size^`runout_pow`), the
  small ones stay up the scree; each fall a cluster, not a sprinkle.
- Lone boulders on the floor (`boulders_per_km2`): old falls rolled far.
- Rims: a few loose blocks at the edges of benches and tops (`rim_per_km`).
Each rock lies on the plane of the ground under it, small ones sunk deeper
(`bury`: share of its height, small..big), never floating; rocks never
overlap (`gap_k`), keep off pinnacles and ways and off sheer ground.
Writes decor.json (rock, x, y, z, yaw, tilt, tilt_dir, size, kind, and for
the client m - rock_<k>_t<bin>_s<turn>, leaning by `tilt_bins_deg` towards +x -
and m_yaw; counts, checks). --views renders close-ups (blender_view.py --decor).
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from scipy.spatial import ConvexHull, HalfspaceIntersection, cKDTree

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "terrain_lab"))
import marsh_hill_mesh  # noqa: E402  (FOLM writer)

BLENDER = Path("C:/Program Files/Blender Foundation/Blender 5.2/blender.exe")


def rock_mesh(rng):
    """A fragment of bedded stone: a box of slab or block proportions with
    a few corners broken off by planes; 1 m long, foot at z 0."""
    l, w = 1.0, rng.uniform(0.45, 0.9)
    t = rng.uniform(0.18, 0.4) if rng.random() < 0.6 else rng.uniform(0.4, 0.7)
    half = np.array([l, w, t]) / 2
    planes = []
    for ax in range(3):
        for sgn in (1, -1):
            n = np.zeros(3)
            n[ax] = sgn
            planes.append(np.r_[n, -half[ax]])
    for _ in range(int(rng.integers(3, 7))):
        n = rng.normal(size=3)
        n[2] *= 0.6                           # (breaks run mostly across the beds)
        n /= np.linalg.norm(n)
        reach = np.abs(n) @ half              # the box's extent along n
        planes.append(np.r_[n, -reach * rng.uniform(0.55, 0.85)])
    hs = HalfspaceIntersection(np.array(planes), np.zeros(3))
    hull = ConvexHull(hs.intersections)
    v = hull.points
    tris = v[hull.simplices]
    c = v.mean(axis=0)
    nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    flip = (nrm * (tris.mean(axis=1) - c)).sum(1) < 0
    tris[flip] = tris[flip][:, ::-1]
    tris = tris - np.array([c[0], c[1], v[:, 2].min()])
    return tris / np.ptp(v[:, 0])


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
    ny, nx = h.shape
    zone = L["mask"] if "mask" in L.files else np.ones(h.shape, bool)
    rng = np.random.default_rng(args.seed)

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
        # For the client (its props turn a model about z only): each rock
        # also leaning by `tilt_bins_deg` towards +x, at four turns of its
        # own - an instance turned to its slope's downhill then lies on it.
        for ti, tb in enumerate(D["tilt_bins_deg"]):
            for si in range(4):
                a_, b_ = math.radians(tb), si * math.pi / 2
                rz = np.array([[math.cos(b_), -math.sin(b_), 0], [math.sin(b_), math.cos(b_), 0], [0, 0, 1]])
                ry = np.array([[math.cos(a_), 0, math.sin(a_)], [0, 1, 0], [-math.sin(a_), 0, math.cos(a_)]])
                tt = t @ (ry @ rz).T
                for lod in range(3):
                    marsh_hill_mesh.write(mdir / f"rock_{k}_t{ti}_s{si}_lod{lod}.mesh", tt, 0.0)

    hs = nd.gaussian_filter(h, 1.5)
    gy, gx = np.gradient(hs, cell)
    sl = np.degrees(np.arctan(np.hypot(gx, gy)))

    def ground(x, y):
        return nd.map_coordinates(h, [np.atleast_1d(y) / cell, np.atleast_1d(x) / cell], order=1, mode="nearest")

    def grad(x, y):
        i, j = int(np.clip(y / cell, 0, ny - 1)), int(np.clip(x / cell, 0, nx - 1))
        return gx[i, j], gy[i, j], sl[i, j]

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

    rocks = []
    grid = {}
    dropped = {"clash": 0, "steep": 0, "uneven": 0, "avoid": 0, "outside": 0}

    def size_draw(lo, hi, alpha):
        u = rng.random()
        return float(min(hi, lo * (1 - u) ** (-1 / alpha)))

    def lay(x, y, size, kind):
        """A rock of `size` at (x, y) on the plane of the ground, or False."""
        if not (0 <= x < nx * cell and 0 <= y < ny * cell) or not zone[int(y / cell), int(x / cell)]:
            dropped["outside"] += 1
            return False
        gk = (int(x // 8), int(y // 8))
        near = [q for a in (-1, 0, 1) for b in (-1, 0, 1) for q in grid.get((gk[0] + a, gk[1] + b), ())]
        if any(math.hypot(q[0] - x, q[1] - y) < D["gap_k"] * 0.5 * (size + q[2]) for q in near):
            dropped["clash"] += 1
            return False
        if av_tree is not None:
            idx = av_tree.query_ball_point((x, y), size * 0.6 + av_r)
            if idx and (np.hypot(av[idx, 0] - x, av[idx, 1] - y) < av[idx, 2] + size * 0.6).any():
                dropped["avoid"] += 1
                return False
        k = int(rng.integers(len(shapes)))
        sh = shapes[k]
        r, hgt = sh["r"] * size, sh["hi"] * size
        ring = np.linspace(0, 2 * np.pi, 8, endpoint=False)
        ox, oy = np.cos(ring) * r * 0.8, np.sin(ring) * r * 0.8
        g = ground(x + ox, y + oy)
        A = np.stack([ox, oy, np.ones_like(ox)], 1)
        (sx, sy, c0), *_ = np.linalg.lstsq(A, g, rcond=None)
        tilt = float(np.degrees(np.arctan(math.hypot(sx, sy))))
        if tilt > D["tilt_max_deg"]:
            dropped["steep"] += 1
            return False
        # Small ones sink deeper into the scree than the big blocks.
        sz = np.clip((size - D["size_m"][0]) / max(1e-6, D["size_m"][1] - D["size_m"][0]), 0, 1)
        bury = D["bury"][0] + (D["bury"][1] - D["bury"][0]) * sz
        resid = g - A @ np.array([sx, sy, c0])
        if -resid.min() > bury * hgt - 0.05 or resid.max() > (1 - bury) * hgt * 0.8:
            dropped["uneven"] += 1
            return False
        z = float(c0) - bury * hgt
        g0 = float(ground(x, y)[0])
        if z + hgt < g0 + 0.1:
            dropped["uneven"] += 1            # (sunk out of sight)
            return False
        ti = int(np.argmin(np.abs(np.array(D["tilt_bins_deg"]) - tilt)))
        yaw = float(rng.uniform(0, 360))
        tilt_dir = float(np.degrees(np.arctan2(-sy, -sx)))
        rocks.append(dict(rock=f"rock_{k}", x=round(x, 2), y=round(y, 2), z=round(z, 2),
                          yaw=round(yaw, 1), tilt=round(tilt, 1), tilt_dir=round(tilt_dir, 1),
                          # (the client's: the leaning model and its turn, radians)
                          m=f"rock_{k}_t{ti}_s{int(rng.integers(4))}",
                          m_yaw=round(math.radians(tilt_dir if ti > 0 else yaw), 4),
                          size=round(size, 2), kind=kind, out_m=round(z + hgt - g0, 2),
                          under_m=round(float(bury * hgt + resid.min()), 2)))
        grid.setdefault(gk, []).append((x, y, size))
        return True

    # --- falls from the walls ------------------------------------------------
    sites = json.loads((out / "mesh_sites.json").read_text(encoding="utf-8"))
    F = D["falls"]
    falls = 0
    for w in sites["walls"]:
        if w["drop_m"] < F["wall_min_m"]:
            continue
        ty, tx = w["y"] * cell, w["x"] * cell
        oy, ox = w["out"]
        # The wall's foot: out from the rim until the ground is down.
        foot = None
        for d in np.arange(1.0, 120.0, 1.0):
            yy, xx = ty + oy * d, tx + ox * d
            if not (0 <= yy < ny * cell and 0 <= xx < nx * cell):
                break
            if h[int(yy / cell), int(xx / cell)] <= w["top_m"] - 0.9 * w["drop_m"]:
                foot = (xx, yy)
                break
        if foot is None:
            continue
        n_falls = rng.poisson(F["per_wall"] * min(2.0, w["drop_m"] / 60.0))
        for _ in range(n_falls):
            falls += 1
            lat = rng.uniform(-F["spread_m"], F["spread_m"])
            fx, fy = foot[0] - oy * lat, foot[1] + ox * lat
            for _ in range(int(rng.poisson(F["rocks"]))):
                size = size_draw(D["size_m"][0], D["size_m"][1], D["alpha"])
                run_ = F["runout_m"] * (size ** F["runout_pow"]) * rng.uniform(0.1, 1.0)
                # Roll down the fall line from the foot, a little astray.
                x, y = fx + rng.normal(0, 2.0), fy + rng.normal(0, 2.0)
                dx, dy = ox, oy
                walked = 0.0
                while walked < run_:
                    gx_, gy_, s_ = grad(x, y)
                    if s_ > 4:
                        n_ = math.hypot(gx_, gy_)
                        dx, dy = 0.6 * dx - 0.4 * gx_ / n_, 0.6 * dy - 0.4 * gy_ / n_
                    m_ = math.hypot(dx, dy) + 1e-9
                    dx, dy = dx / m_ + rng.normal(0, 0.15), dy / m_ + rng.normal(0, 0.15)
                    x, y = x + dx, y + dy
                    walked += 1.0
                    if s_ < 4:
                        walked += 2.0           # (on the flat it stops soon)
                lay(x, y, size, "fall")

    # --- lone boulders on the floor --------------------------------------------
    floor = zone & (L["rel_isl"] < 1) & (L["talus"] <= 0.3) & (sl < 12) & ~L["wash"]
    fy_, fx_ = np.nonzero(floor)
    zone_km2 = float(zone.sum()) * cell * cell / 1e6
    want = int(D["boulders_per_km2"] * zone_km2)
    for i in rng.choice(len(fy_), size=min(len(fy_), want), replace=False) if len(fy_) else []:
        size = size_draw(D["size_m"][1] * 0.25, D["size_m"][1], D["alpha"])
        lay((fx_[i] + rng.random()) * cell, (fy_[i] + rng.random()) * cell, size, "boulder")

    # --- loose blocks at rims of benches and tops ------------------------------
    top = zone & (L["rel_isl"] >= 1) & (sl < 20)
    rim = top & ~nd.binary_erosion(top, iterations=2)
    ry, rx = np.nonzero(rim)
    want = int(D["rim_per_km"] * len(ry) * cell / 1000 / 3)
    for i in rng.choice(len(ry), size=min(len(ry), want), replace=False) if len(ry) else []:
        x0, y0 = (rx[i] + 0.5) * cell, (ry[i] + 0.5) * cell
        for _ in range(int(rng.integers(1, 4))):
            size = size_draw(D["size_m"][0], D["size_m"][1] * 0.5, D["alpha"])
            lay(x0 + rng.normal(0, 2.5), y0 + rng.normal(0, 2.5), size, "rim")

    sizes = np.array([r["size"] for r in rocks]) if rocks else np.zeros(1)
    by = {}
    for r in rocks:
        by[r["kind"]] = by.get(r["kind"], 0) + 1
    res = dict(
        rocks=len(rocks), per_km2=round(len(rocks) / max(zone_km2, 1e-6)), falls=falls, by_kind=by,
        size_m=dict(p10=round(float(np.percentile(sizes, 10)), 2), p50=round(float(np.percentile(sizes, 50)), 2),
                    p90=round(float(np.percentile(sizes, 90)), 2), max=round(float(sizes.max()), 2),
                    over_3m=int((sizes > 3).sum())),
        dropped=dropped,
        checks=dict(standing_out_min_m=round(min((r["out_m"] for r in rocks), default=0), 2),
                    floating=int(sum(1 for r in rocks if r["under_m"] <= 0))),
    )
    (out / "decor.json").write_text(json.dumps(dict(cell_m=cell, rocks=rocks, result=res)), encoding="utf-8")
    print(json.dumps(res, indent=1))
    if args.views:
        subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P", str(HERE / "blender_view.py"),
                        "--", str(out), "--paths", "--decor", "--only", "decor0,decor1,decor2"], check=True,
                       stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
