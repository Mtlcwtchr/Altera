"""Canyon sandbox (user 15.10): the canyon's relief generated on a small patch
and looked at in pictures, seconds per run, nothing of the client built.

    python tools/canyon_lab/canyon_lab.py [--seed 7] [--cell 4] [--tag v1] [--set islands.top_m=220]

Writes terrain_lab_out/canyon_lab/<tag>/: heights.r16 + heights16.png (16-bit,
range in stats.json), hillshade.png, slope.png, map.png (coloured, lit, cast
shadows), view_<name>.png (perspective), sheet.png (all on one page), and
stats.json (the numbers). Parameters in canyon.json next to this file.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen  # noqa: E402
import render  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def set_path(P, key, val):
    keys = key.split(".")
    d = P
    for k in keys[:-1]:
        d = d[int(k)] if isinstance(d, list) else d[k]
    d[keys[-1]] = json.loads(val)


def stats(g, sl):
    cell, h = g["cell"], g["h"]
    rel = g["rel"]
    gy, gx = np.gradient(h, cell)
    dh = np.hypot(gx, gy) * cell                     # vertical change across a cell
    moving = sl > 8
    sheer = sl > 60
    talus = g["talus"] > 1
    climb = dh[moving].sum()
    floor = rel < 3
    flab, nf = nd.label(floor, structure=np.ones((3, 3)))
    fsz = np.bincount(flab.ravel())[1:] if nf else np.array([0])
    high = g["rel_isl"] > 8
    hlab, nh = nd.label(high, structure=np.ones((3, 3)))
    isl = g["islands"]
    r = np.array([s["r_max_m"] for s in isl]) if isl else np.zeros(0)
    a = np.array([s["area_m2"] for s in isl]) if isl else np.zeros(0)
    grass = g["grass"]
    out = {
        "patch_km": g["n"] * cell / 1000, "cell_m": cell,
        "relief_m": float(np.ptp(h)),
        "share": {
            "high_ground": float((g["rel_isl"] > 20).mean()),
            "canyon_floor": float(floor.mean()),
            "floor_in_one_piece": float(fsz.max() / max(1, fsz.sum())),
            "wash": float((g["wash"] & (rel < 1)).mean()),
            "talus": float(talus.mean()),
            "grass": float(grass.mean()),
        },
        "climb": {
            "on_sheer_walls_gt60": float(dh[moving & sheer].sum() / climb),
            "on_talus": float(dh[moving & talus].sum() / climb),
            "on_gentle_8_45_not_talus": float(dh[moving & (sl < 45) & ~talus].sum() / climb),
        },
        "islands": {
            "count": len(isl),
            "high_pieces_after_slabs": int(nh),
            "mesas_r_gt_80m": int((r > 80).sum()),
            "buttes_r_30_80m": int(((r > 30) & (r <= 80)).sum()),
            "small_r_le_30m": int((r <= 30).sum()),
            "largest_km2": float(a.max() / 1e6) if a.size else 0,
            "top_m_range": [float(min(s["top_m"] for s in isl)), float(max(s["top_m"] for s in isl))] if isl else [],
            "rings_hist": np.bincount([s["rings"] for s in isl], minlength=5).tolist() if isl else [],
            "with_grass": int(sum(1 for s in isl if s.get("grass"))),
        },
        "columns": len(g["columns"]),
        "peaks": {k: sum(1 for p in g["peaks"] if p["kind"] == k) for k in ("remnant", "floor", "satellite")},
        "fans": len(g["fans"]),
    }
    if grass.any():
        on_big = np.isin(g["lab"][grass], [s["id"] for s in isl if s.get("grass")]).mean()
        out["share"]["grass_on_biggest_islands"] = float(on_big)
    return out


def sheet(paths, out, tile=640):
    ims = [Image.open(p).convert("RGB") for p in paths]
    rows = []
    for im in ims:
        w, h_ = im.size
        rows.append(im.resize((tile, int(tile * h_ / w)), Image.LANCZOS))
    cols = 2
    hs = [max(r.size[1] for r in rows[i:i + cols]) for i in range(0, len(rows), cols)]
    page = Image.new("RGB", (tile * cols, sum(hs)), (20, 16, 14))
    y = 0
    for k, i in enumerate(range(0, len(rows), cols)):
        for j, r in enumerate(rows[i:i + cols]):
            page.paste(r, (j * tile, y))
        y += hs[k]
    page.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--params", default=str(Path(__file__).with_name("canyon.json")))
    ap.add_argument("--seed", type=int)
    ap.add_argument("--cell", type=float)
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--no-views", action="store_true")
    args = ap.parse_args()
    P = json.loads(Path(args.params).read_text(encoding="utf-8"))
    if args.cell:
        P["cell_m"] = args.cell
    for kv in args.set:
        k, v = kv.split("=", 1)
        set_path(P, k, v)
    seed = args.seed if args.seed is not None else P["seed"]
    out = ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    out.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    g = gen.generate(P, seed)
    t1 = time.time()
    cell, h = g["cell"], g["h"]
    rng = np.random.default_rng(seed + 1000)
    Rr = P["render"]
    col, cls, names, sl = render.lit(g, cell, rng, Rr["sun_az"], Rr["sun_el"])
    t2 = time.time()

    hi = float(h.max())
    (h / max(hi, 1e-6) * 65535).astype("<u2").tofile(out / "heights.r16")
    Image.fromarray((h / max(hi, 1e-6) * 65535).astype(np.uint16)).save(out / "heights16.png")
    small = lambda im: im.resize((1024, 1024), Image.LANCZOS) if im.size[0] > 1024 else im
    small(render.gray(h)).save(out / "heights.png")
    small(render.gray(render.hillshade(h, cell, Rr["sun_az"], 40))).save(out / "hillshade.png")
    small(render.gray(sl, 0, 90)).save(out / "slope.png")
    small(Image.fromarray(col.astype(np.uint8))).save(out / "map.png")
    views = []
    if not args.no_views:
        span = g["n"] * cell
        for v in Rr["views"]:
            cam = dict(x=v["u"] * span, y=v["v"] * span, z=v["z"], yaw=v["yaw"], pitch=v["pitch"])
            p = out / f"view_{v['name']}.png"
            render.perspective(h, col, cell, cam, g['_rock'], g['_wob'], g['_wall']).save(p)
            views.append(p)
    t3 = time.time()

    st = stats(g, sl)
    st["seed"] = seed
    st["height_range_m"] = [0.0, hi]
    st["seconds"] = {"generate": round(t1 - t0, 1), "colour": round(t2 - t1, 1), "views": round(t3 - t2, 1)}
    (out / "stats.json").write_text(json.dumps(st, indent=1), encoding="utf-8")
    feats = {k: g[k] for k in ("islands", "columns", "peaks", "fans")}
    (out / "features.json").write_text(json.dumps(feats, default=float), encoding="utf-8")
    np.savez_compressed(out / "layers.npz", h=h, rel_isl=g["rel_isl"], rel_col=g["rel_col"],
                        rel_peak=g["rel_peak"], talus=g["talus"], rel_fan=g["rel_fan"], wash=g["wash"],
                        grass=g["grass"], lab=g["lab"], cls=cls)
    sheet([out / "map.png", out / "hillshade.png"] + views, out / "sheet.png")
    print(json.dumps(st, indent=1))
    print("out:", out)


if __name__ == "__main__":
    main()
