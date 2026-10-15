"""Stage 3 of the canyon sandbox: the meshes put on the sites the analysis
found (mesh_sites.json), on the ground the run made, and checked.

    python tools/canyon_lab/place.py --tag T [--views]

Columns -> canyon_pinnacle columns/towers, peaks -> needles/towers, heaps of
slab_rubble at the feet of the big ones. Each instance: kind, variant, x, y
(metres, x right, y down the pictures), z (its ground level: the lowest
ground under its foot, the foot itself reaches below and is buried), yaw,
uniform scale s and a footprint stretch k. Writes instances.json and
placement.json (checks); with --views renders the scene in Blender
(blender_view.py) to view3d_<name>.png.
"""

import argparse
import json
import struct
import subprocess
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

ROOT = Path(__file__).resolve().parents[2]
PROPS = ROOT / "assets" / "terrain_core" / "props"
BLENDER = Path("C:/Program Files/Blender Foundation/Blender 5.2/blender.exe")
PIN_KINDS = ("needle", "column", "tower", "twin", "needle", "column", "tower", "twin")


def read_mesh(path):
    b = path.read_bytes()
    _, nv, ni = struct.unpack("<III", b[4:16])
    v = np.frombuffer(b, "<f4", count=nv * 8, offset=40).reshape(nv, 8)
    return v[:, :3]


def variant_info():
    meta = json.loads((PROPS / "props.json").read_text(encoding="utf-8"))
    out = {}
    for name, m in meta.items():
        if m["kind"] not in ("canyon_pinnacle", "slab_rubble"):
            continue
        p = read_mesh(PROPS / f"{name}_lod0.mesh")
        hgt = m["hi"][2]
        foot = p[(p[:, 2] > 0) & (p[:, 2] < 0.05 * hgt)]
        r = float(np.median(np.hypot(foot[:, 0], foot[:, 1]))) if len(foot) else 0.5 * (m["hi"][0] - m["lo"][0])
        rmax = float(np.hypot(p[:, 0], p[:, 1]).max())
        ring = p[(p[:, 2] > -0.005 * hgt) & (p[:, 2] < 0.02 * hgt)]
        out[name] = dict(kind=m["kind"], h=hgt, r=r, rmax=rmax, below=-m["lo"][2], ring=ring,
                         sub=PIN_KINDS[int(name.rsplit("_", 1)[1]) % 8] if m["kind"] == "canyon_pinnacle" else "rubble")
    return out


def ground_under(h, cell, x, y, r):
    n = h.shape[0]
    rp = max(1.0, r / cell)
    y0, y1 = int(max(0, y / cell - rp)), int(min(n, y / cell + rp + 1))
    x0, x1 = int(max(0, x / cell - rp)), int(min(n, x / cell + rp + 1))
    yy, xx = np.mgrid[y0:y1, x0:x1]
    m = (yy - y / cell) ** 2 + (xx - x / cell) ** 2 <= rp * rp
    v = h[y0:y1, x0:x1][m]
    return (float(v.min()), float(v.max())) if v.size else (0.0, 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--views", action="store_true")
    args = ap.parse_args()
    out = ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    sites = json.loads((out / "mesh_sites.json").read_text(encoding="utf-8"))
    L = np.load(out / "layers.npz")
    h = L["h"]
    cell = sites["cell_m"]
    rng = np.random.default_rng(args.seed)
    V = variant_info()
    pins = {k: [n for n, v in V.items() if v["kind"] == "canyon_pinnacle" and v["sub"] in k] for k in
            (("column", "tower"), ("needle", "tower"), ("tower", "twin"))}
    rubble = [n for n, v in V.items() if v["kind"] == "slab_rubble"]

    inst = []

    dropped = {}
    gyy, gxx = np.gradient(h, cell)
    placed = []          # (x, y, r) of pinnacles so far

    def fits(x, y, r):
        return all(np.hypot(x - px_, y - py_) >= 0.85 * (r + pr) for px_, py_, pr in placed)

    def put(name, x, y, height, radius, role):
        v = V[name]
        s = height / v["h"]
        if radius is None:
            k = float(rng.uniform(0.85, 1.25))
        else:
            k = float(np.clip(radius / max(1e-6, v["r"] * s), 0.75, 1.5))
        foot_r = v["r"] * s * k
        yaw = float(rng.uniform(0, 360))
        a = np.radians(yaw)
        q = v["ring"]
        n = h.shape[0]
        for t in range(4):
            # The real foot: the model's vertices at its ground level, placed.
            px = x + (q[:, 0] * np.cos(a) - q[:, 1] * np.sin(a)) * s * k
            py = y - (q[:, 0] * np.sin(a) + q[:, 1] * np.cos(a)) * s * k
            gi = np.clip(np.rint(py / cell).astype(int), 0, n - 1)
            gj = np.clip(np.rint(px / cell).astype(int), 0, n - 1)
            g_ring = h[gi, gj] - q[:, 2] * s
            z = float(g_ring.min()) - 0.3                      # sunk so no side of the foot is in the air
            g0, g1 = ground_under(h, cell, x, y, foot_r * 1.1)
            span = max(g1, float(h[gi, gj].max())) - z
            ok_wall = role == "rubble" or span <= 0.25 * height
            ok_room = role == "rubble" or fits(x, y, foot_r)
            if ok_wall and ok_room:
                gap = float(np.max(z + q[:, 2] * s - h[gi, gj]))
                inst.append(dict(name=name, kind=v["kind"], role=role, x=x, y=y, z=z, yaw=yaw,
                                 s=s, k=k, foot_r=foot_r, top=z + height, ground_span=span,
                                 bury=v["below"] * s, hang_m=max(0.0, gap), moved_m=t * foot_r))
                if role != "rubble":
                    placed.append((x, y, foot_r))
                return True
            # Nudge off the wall (downhill) or out of a neighbour's way.
            j, i = int(np.clip(x / cell, 0, n - 1)), int(np.clip(y / cell, 0, n - 1))
            g = np.array([gxx[i, j], gyy[i, j]])
            d = -g / (np.linalg.norm(g) + 1e-6) if np.linalg.norm(g) > 0.05 else rng.normal(size=2)
            d /= np.linalg.norm(d) + 1e-9
            x, y = x + d[0] * foot_r, y + d[1] * foot_r
        dropped[role] = dropped.get(role, 0) + 1
        return False

    for c in sites["columns"]:
        name = rng.choice(pins[("column", "tower")])
        put(name, c["x"] * cell, c["y"] * cell, c["h_m"], c["r_m"], "column")
    for p in sites["peaks"]:
        group = ("needle", "tower") if p["kind"] != "satellite" else ("tower", "twin")
        name = rng.choice(pins[group])
        put(name, p["x"] * cell, p["y"] * cell, p["h_m"], None, "peak_" + p["kind"])
    # Heaps of fallen slabs at the feet of the tall ones.
    for i in [i for i in range(len(inst)) if inst[i]["top"] - inst[i]["z"] > 60]:
        b = inst[i]
        for _ in range(int(rng.integers(1, 4))):
            a = rng.uniform(0, 2 * np.pi)
            d = b["foot_r"] * rng.uniform(1.0, 1.6)
            x, y = b["x"] + d * np.cos(a), b["y"] + d * np.sin(a)
            name = rng.choice(rubble)
            hh = rng.uniform(6, 14) * (b["foot_r"] / 10.0) ** 0.5
            put(name, x, y, hh, None, "rubble")

    # --- checks -------------------------------------------------------------
    xy = np.array([[i["x"], i["y"]] for i in inst])
    rr = np.array([i["foot_r"] for i in inst])
    role = np.array([i["role"] for i in inst])
    tree = cKDTree(xy)
    clash = 0
    for a, b in tree.query_pairs(float(rr.max() * 2)):
        if "rubble" in (role[a], role[b]):
            continue
        if np.hypot(*(xy[a] - xy[b])) < 0.85 * (rr[a] + rr[b]):
            clash += 1
    span = np.array([i["ground_span"] for i in inst])
    tall = np.array([i["top"] - i["z"] for i in inst])
    bury = np.array([i["bury"] for i in inst])
    hang = np.array([i["hang_m"] for i in inst]) > 0.3
    # In a wall: the ground under the foot rises by a big share of the model.
    in_wall = span > 0.25 * tall
    res = dict(
        instances=len(inst), dropped=dropped,
        moved=int(sum(1 for i in inst if i["moved_m"] > 0)),
        ground=dict(max_ground_span_share=float(max(i["ground_span"] / max(1e-6, i["top"] - i["z"]) for i in inst if i["role"] != "rubble"))),
        by_role={r: int((role == r).sum()) for r in sorted(set(role))},
        checks=dict(
            hanging=dict(value=int((hang & (role != "rubble")).sum()), ok=bool((hang & (role != "rubble")).sum() == 0)),
            rubble_hanging=dict(value=int((hang & (role == "rubble")).sum()), ok=bool((hang & (role == "rubble")).sum() == 0)),
            pinnacles_overlapping=dict(value=clash, ok=clash == 0),
            standing_in_a_wall=dict(value=int((in_wall & (role != "rubble")).sum()), ok=bool((in_wall & (role != "rubble")).sum() == 0)),
        ),
        footprint_stretch_k=dict(min=float(min(i["k"] for i in inst)), max=float(max(i["k"] for i in inst)),
                                 clamped=int(sum(1 for i in inst if i["k"] in (0.75, 1.5)))),
        height_m=dict(min=float(tall.min()), median=float(np.median(tall)), max=float(tall.max())),
    )
    (out / "instances.json").write_text(json.dumps(dict(cell_m=cell, instances=inst), default=float), encoding="utf-8")
    (out / "placement.json").write_text(json.dumps(res, indent=1, default=float), encoding="utf-8")
    print(json.dumps(res, indent=1, default=float))
    if args.views:
        subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P", str(Path(__file__).with_name("blender_view.py")),
                        "--", str(out)], check=True, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
