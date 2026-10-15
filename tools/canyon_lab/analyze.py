"""Stage 2 of the canyon sandbox: read the heights a run made and say what
they are, without the generator's own intentions (only the map and the
island ids are read back).

    python tools/canyon_lab/analyze.py --tag T

Writes into terrain_lab_out/canyon_lab/T/: masks.png (what each cell is,
legend at the bottom), walls.png (wall drop: thin = terrain, thick red =
for a cliff mesh), walk.png (walkable ground: the floor's piece, islands'
tops cut off, anything else), analysis.json (checks with pass/fail and the
numbers), mesh_sites.json (columns, peaks, wall segments for stage 3).
"""

import argparse
import json
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]

# What a cell is, by the map alone (draw order: later wins).
CLASSES = [
    ("floor", (196, 112, 60)), ("wash", (236, 200, 140)), ("fan", (226, 160, 96)),
    ("bench", (150, 110, 70)), ("talus", (110, 80, 60)), ("tread", (214, 170, 110)),
    ("wall_terrain", (120, 40, 30)), ("wall_mesh", (230, 30, 30)),
    ("column", (60, 120, 220)), ("peak", (150, 60, 200)), ("grass", (240, 150, 40)),
]
CHECKS = {
    # (user 15.10 asked for talus under the walls: the climb is sheer walls
    # or scree, never a gentle slope)
    "climb_on_sheer_or_talus_min": 0.85,
    "climb_on_gentle_max": 0.08,
    "floor_in_one_piece_min": 0.85,
    "islands_walkable_from_floor_max": 0,
    "grass_off_biggest_max": 0.0,
    "grass_core_depth_min": 0.35,
    "columns_walkable_max": 0,
}


def slope_deg(h, cell):
    gy, gx = np.gradient(h, cell)
    return np.degrees(np.arctan(np.hypot(gx, gy))), gx, gy


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--dir", help="a run directory instead of canyon_lab/<tag> (the world's: canyon_relief.py)")
    ap.add_argument("--mesh-wall-m", type=float, default=25.0,
                    help="a wall at least this tall is a cliff mesh's job, lower ones the terrain's")
    ap.add_argument("--walk-deg", type=float, default=38.0)
    args = ap.parse_args()
    out = Path(args.dir) if args.dir else ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    L = np.load(out / "layers.npz")
    st = json.loads((out / "stats.json").read_text(encoding="utf-8"))
    feats = json.loads((out / "features.json").read_text(encoding="utf-8"))
    cell = st["cell_m"]
    h = L["h"].astype(np.float32)
    n = h.shape
    lab = L["lab"]
    # (in the world only the canyon's own ground counts: the land round it is not its floor)
    zone = L["mask"] if "mask" in L.files else np.ones(n, bool)
    boxes = nd.find_objects(lab)
    sl, gx, gy = slope_deg(h, cell)

    # --- walls: top-edge cells and the drop below them ---------------------
    drop = h - nd.minimum_filter(h, size=5)
    edge = (drop > 4.0) & (sl > 50)
    edge &= h >= nd.maximum_filter(h, size=3) - 1.0          # the upper side of the jump
    wall_cell = sl > 60
    big = nd.maximum_filter(np.where(edge, drop, 0), size=5)
    wall_mesh = wall_cell & (big >= args.mesh_wall_m)
    wall_terrain = wall_cell & ~wall_mesh

    # --- classes ------------------------------------------------------------
    rel_isl, rel_col, rel_peak = L["rel_isl"], L["rel_col"], L["rel_peak"]
    talus, rel_fan, wash, grass = L["talus"], L["rel_fan"], L["wash"], L["grass"]
    c = np.zeros(n, np.uint8)
    names = [k for k, _ in CLASSES]
    put = lambda m, k: c.__setitem__(m, names.index(k))
    put(wash, "wash")
    put(rel_fan > 0.3, "fan")
    put((rel_isl > 1) & (rel_isl <= 20), "bench")
    put(talus > 1, "talus")
    put((rel_isl > 20) & (sl < 30), "tread")
    put(wall_terrain, "wall_terrain")
    put(wall_mesh, "wall_mesh")
    put(rel_col > 4, "column")
    put((rel_peak > 3) & (rel_peak >= np.maximum(rel_isl, rel_col)), "peak")
    put(grass, "grass")
    pal = np.array([col for _, col in CLASSES], np.uint8)
    shade = np.clip(0.55 + 0.45 * (np.clip(-gx * 0.7 + gy * 0.7, -1, 1) + 1) / 2, 0, 1)
    img = (pal[c] * shade[..., None]).astype(np.uint8)
    share = {k: float((c == i).mean()) for i, k in enumerate(names)}

    # --- climbing: how the height is gained ---------------------------------
    dh = np.hypot(gx, gy) * cell
    moving = (sl > 8) & zone
    tot = dh[moving].sum()
    climb = {
        "sheer_gt60": float(dh[moving & (sl > 60)].sum() / tot),
        "steep_45_60": float(dh[moving & (sl > 45) & (sl <= 60)].sum() / tot),
        "talus": float(dh[moving & (talus > 1) & (sl <= 45)].sum() / tot),
        "gentle_8_45": float(dh[moving & (sl <= 45) & (talus <= 1)].sum() / tot),
    }

    # --- walking: what can be reached on foot from the floor ----------------
    # Walkable: the steepest step to any of the 8 neighbours (a central
    # gradient would call a one-cell pit or needle flat).
    st = np.zeros_like(h)
    for dy, dx in ((0, 1), (1, 0), (1, 1), (1, -1)):
        dd = np.hypot(dy, dx) * cell
        a = np.abs(h - np.roll(np.roll(h, -dy, 0), -dx, 1)) / dd
        st = np.maximum(st, a)
        st = np.maximum(st, np.roll(np.roll(a, dy, 0), dx, 1))
    walk = np.degrees(np.arctan(st)) < args.walk_deg
    wl, nw = nd.label(walk, structure=np.ones((3, 3)))
    floor = walk & (rel_isl < 1) & (rel_col < 1) & (rel_peak < 1) & zone
    fl_counts = np.bincount(wl[floor], minlength=nw + 1)
    fl_counts[0] = 0
    main_id = int(fl_counts.argmax())
    floor_cells = floor.sum()
    floor_one = float(fl_counts[main_id] / max(1, floor_cells))
    isl = feats["islands"]
    reachable, isolated = [], 0
    for s in isl:
        i = s["id"]
        if i > len(boxes) or boxes[i - 1] is None:
            continue
        b = boxes[i - 1]
        m = lab[b] == i
        top = m & (h[b] >= h[b][m].max() - 8)
        if (wl[b][top] == main_id).any():
            reachable.append(i)
        else:
            isolated += 1
    # Columns must be too narrow to walk on: their top no wider than walking.
    col_walkable = 0
    for col in feats["columns"]:
        if col["r_m"] > 16.0:
            col_walkable += 1

    # --- risers: heights of the steps ---------------------------------------
    d_e = drop[edge]
    bins = [0, 5, 15, 30, 60, 120, 1000]
    riser_hist = {f"{a}-{b}m": int(((d_e >= a) & (d_e < b)).sum()) for a, b in zip(bins[:-1], bins[1:])}
    wall_len_km = {"terrain": float((edge & (big < args.mesh_wall_m)).sum() * cell / 1000),
                   "mesh": float((edge & (big >= args.mesh_wall_m)).sum() * cell / 1000)}

    # --- grass ----------------------------------------------------------------
    big3 = [s["id"] for s in isl if s.get("grass")]
    on_big = np.isin(lab[grass], big3) if grass.any() else np.array([True])
    depth = []
    for s in isl:
        if not s.get("grass") or s["id"] > len(boxes) or boxes[s["id"] - 1] is None:
            continue
        b = boxes[s["id"] - 1]
        m = lab[b] == s["id"]
        e = nd.distance_transform_edt(np.pad(m, 1)) [1:-1, 1:-1] * cell
        g = grass[b] & m
        if g.any():
            depth.append(float(np.median(e[g]) / max(1.0, e.max())))
    grass_core = float(np.mean(depth)) if depth else 0.0

    # --- mesh sites for stage 3 ------------------------------------------------
    hb = nd.gaussian_filter(h, 2.0)
    by, bx = np.gradient(hb, cell)
    segs = []
    ey, ex = np.nonzero(edge & (big >= args.mesh_wall_m))
    order = np.argsort(-drop[ey, ex])
    taken = np.zeros(n, bool)
    sp = int(30 / cell)
    for o in order:
        y, x = ey[o], ex[o]
        if taken[y, x]:
            continue
        taken[max(0, y - sp):y + sp + 1, max(0, x - sp):x + sp + 1] = True
        g = np.array([by[y, x], bx[y, x]])
        nrm = -g / (np.linalg.norm(g) + 1e-6)       # outward: downhill
        segs.append(dict(y=int(y), x=int(x), top_m=float(h[y, x]), drop_m=float(drop[y, x]),
                         out=[float(nrm[0]), float(nrm[1])]))
    sites = dict(cell_m=cell, columns=feats["columns"], peaks=feats["peaks"], walls=segs)
    (out / "mesh_sites.json").write_text(json.dumps(sites), encoding="utf-8")

    checks = {
        "climb_on_sheer_or_talus": (climb["sheer_gt60"] + climb["talus"],
                                    climb["sheer_gt60"] + climb["talus"] >= CHECKS["climb_on_sheer_or_talus_min"]),
        "climb_on_gentle": (climb["gentle_8_45"], climb["gentle_8_45"] <= CHECKS["climb_on_gentle_max"]),
        "floor_in_one_piece": (floor_one, floor_one >= CHECKS["floor_in_one_piece_min"]),
        "islands_walkable_from_floor": (len(reachable), len(reachable) <= CHECKS["islands_walkable_from_floor_max"]),
        "grass_off_biggest": (float(1 - on_big.mean()), (1 - on_big.mean()) <= CHECKS["grass_off_biggest_max"]),
        "grass_core_depth": (grass_core, grass_core >= CHECKS["grass_core_depth_min"]),
        "columns_walkable": (col_walkable, col_walkable <= CHECKS["columns_walkable_max"]),
    }
    res = dict(
        checks={k: dict(value=v, ok=bool(ok)) for k, (v, ok) in checks.items()},
        share=share, climb=climb, riser_cells=riser_hist, wall_len_km=wall_len_km,
        islands=dict(count=len(isl), isolated=isolated, walkable_from_floor=reachable),
        mesh_sites=dict(columns=len(feats["columns"]), peaks=len(feats["peaks"]), wall_segments=len(segs)),
    )
    (out / "analysis.json").write_text(json.dumps(res, indent=1, default=float), encoding="utf-8")

    # --- pictures ---------------------------------------------------------------
    def legend(im, items):
        w = im.size[0]
        bar = Image.new("RGB", (w, 26 * ((len(items) + 3) // 4)), (24, 20, 18))
        d = ImageDraw.Draw(bar)
        for k, (name, col) in enumerate(items):
            x, y = (k % 4) * (w // 4) + 8, (k // 4) * 26 + 5
            d.rectangle([x, y, x + 16, y + 16], fill=tuple(col))
            d.text((x + 22, y + 3), name, fill=(235, 225, 210))
        page = Image.new("RGB", (w, im.size[1] + bar.size[1]))
        page.paste(im, (0, 0))
        page.paste(bar, (0, im.size[1]))
        return page

    small = lambda a: Image.fromarray(a).resize((1024, max(1, round(1024 * a.shape[0] / a.shape[1]))), Image.LANCZOS)
    legend(small(img), CLASSES).save(out / "masks.png")
    wv = np.full(n + (3,), 40, np.uint8)
    wv[...] = (np.clip(h / max(1.0, h.max()), 0, 1)[..., None] * 90 + 30).astype(np.uint8)
    thick = nd.binary_dilation(edge & (big >= args.mesh_wall_m), iterations=2)
    wv[nd.binary_dilation(edge & (big < args.mesh_wall_m))] = (250, 210, 120)
    wv[thick] = (235, 40, 30)
    legend(small(wv), [("wall, terrain (<%dm)" % args.mesh_wall_m, (250, 210, 120)),
                       ("wall, cliff mesh", (235, 40, 30))]).save(out / "walls.png")
    wk = np.full(n + (3,), 30, np.uint8)
    wk[walk] = (90, 90, 90)
    wk[wl == main_id] = (90, 170, 90)
    for s in isl:
        if s["id"] > len(boxes) or boxes[s["id"] - 1] is None:
            continue
        b = boxes[s["id"] - 1]
        m = lab[b] == s["id"]
        top = m & (h[b] >= h[b][m].max() - 8) & walk[b]
        wk[b][top] = (60, 120, 220) if s["id"] not in reachable else (230, 60, 40)
    legend(small(wk), [("floor, one piece", (90, 170, 90)), ("island top, cut off", (60, 120, 220)),
                       ("island top, walkable from floor", (230, 60, 40)), ("other walkable", (90, 90, 90)),
                       ("too steep", (30, 30, 30))]).save(out / "walk.png")
    print(json.dumps(res, indent=1, default=float))


if __name__ == "__main__":
    main()
