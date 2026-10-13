"""world_megaliths - the megaliths (doc/rework/MESH_TERRAIN_PLAN.md): strange,
surreal standing things that belong to one biome each and do not repeat
elsewhere (doc/BIOMES_RU.md; the user's list of 2026-10-11):

- the white mountains: the broken ring;
- the great marshes: the giant monolith slabs A-1 (doc/megaliths/A1_MONOLITH.md)
  in loose rows on the dry islands all over them (they replaced the prism row
  and the panels, 2026-10-12);
- the green shore at the desert's edge: a cluster of huge white cubes;
- the plain: the remains of giants - heads, hands, a spine, fallen iron
  defenders;
- the golden desert: one mirroring prism, the rest giant remains - spines,
  ribcages, hands, iron defenders;
- the black coast: grey spikes all along it;
- the river valley: one towering straight grey spire (a tower will stand on it).
The isles and the crescent have none.

Placed by world_features.py from the "megaliths" section of
content/config/terrain_core/props.json. Sizes of the great ones follow their
biome (a share of its extent, the root of its area), so each is seen from
anywhere in it whatever the map's size.
"""
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
CHECKS = {}  # the automatic checks (doc/BIOME_PLAN.md 1.5) by element; world_features.py writes them out


def _names(catalogue, kind):
    names = sorted(n for n, m in catalogue.items() if m["kind"] == kind)
    if not names:
        raise SystemExit(f"no models of kind {kind}: run tools/blender/terrain_props.py --all")
    return names


def _extent(world, mask):
    return math.sqrt(mask.sum()) * world.cell


def heroes(world, rules, catalogue, rng, add):
    """One kind per biome, `count` of it, height a `share` of the biome's extent."""
    r = rules["heroes"]
    gy, gx = np.gradient(world.hz, world.cell)
    flat = np.hypot(gx, gy) < r["max_slope"]
    made = 0
    for zone, spec in r["zones"].items():
        inside = world.zones == zone
        if not inside.any():
            continue
        extent = _extent(world, inside)
        height = float(np.clip(spec.get("share", r["share"]) * extent, *spec.get("height_m", r["height_m"])))
        depth = ndimage.distance_transform_edt(inside) * world.cell
        ok = inside & flat & (world.hz > 1.0) & (depth > min(depth.max() * 0.5, r["edge_share"] * extent))
        ys, xs = np.nonzero(ok)
        if not len(xs):
            continue
        names = _names(catalogue, spec["kind"])
        placed = []
        for k in rng.permutation(len(xs)):
            x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
            if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < (r["spacing_share"] * extent) ** 2 for p in placed):
                continue
            placed.append((x, y))
            name = names[int(rng.integers(len(names)))]
            m = catalogue[name]
            s = height * rng.uniform(0.9, 1.1) / max(m["hi"][2], 1e-3)
            add(spec["kind"], name, x, y, rng.uniform(0, math.tau), (s, s, s), sink=round(0.03 * height, 1))
            made += 1
            if len(placed) >= spec.get("count", 1):
                break
    return made


def _border(world, a, b, depth_m):
    """Cells of zone `a` within `depth_m` of zone `b`."""
    near_b = ndimage.distance_transform_edt(world.zones != b) * world.cell < depth_m
    return (world.zones == a) & near_b & (world.hz > 0.5)


def prism_row(world, rules, catalogue, rng, add):
    """A row of towering prisms standing along a border (the marshes against
    the dark forest): the border's longest stretch, prisms spaced along it."""
    r = rules["prism_row"]
    edge = _border(world, r["zone"], r["along"], r["depth_m"])
    lab, n = ndimage.label(edge)
    if not n:
        return 0
    sizes = ndimage.sum(edge, lab, range(1, n + 1))
    piece = lab == 1 + int(np.argmax(sizes))
    ys, xs = np.nonzero(piece)
    pts = np.stack([(xs + 0.5) * world.cell, (ys + 0.5) * world.cell], 1)
    # Along the border: its main direction, a run of `count` spaced `gap_m`.
    c = pts.mean(0)
    _, _, vt = np.linalg.svd(pts - c, full_matrices=False)
    t = (pts - c) @ vt[0]
    mid = float(np.median(t))
    names = _names(catalogue, r["kind"])
    height = float(np.clip(r["share"] * _extent(world, world.zones == r["zone"]), *r["height_m"]))
    made = 0
    for k in range(r["count"]):
        want = mid + (k - (r["count"] - 1) / 2) * r["gap_m"]
        i = int(np.argmin(np.abs(t - want)))
        x, y = pts[i]
        name = names[int(rng.integers(len(names)))]
        m = catalogue[name]
        s = height * rng.uniform(0.85, 1.15) / m["hi"][2]
        add(r["kind"], name, x, y, math.atan2(vt[0][1], vt[0][0]) + rng.uniform(-0.1, 0.1), (s, s, s),
            sink=round(0.04 * height, 1))
        made += 1
    return made


def cube_cluster(world, rules, catalogue, rng, add):
    """A tight cluster of huge white cubes on the green shore where it meets
    the desert."""
    r = rules["cube_cluster"]
    edge = _border(world, r["zone"], r["near"], r["depth_m"])
    ys, xs = np.nonzero(edge)
    if not len(xs):
        ys, xs = np.nonzero((world.zones == r["zone"]) & (world.hz > 1.0))
        if not len(xs):
            return 0
    k = int(rng.integers(len(xs)))
    cx, cy = (xs[k] + 0.5) * world.cell, (ys[k] + 0.5) * world.cell
    names = _names(catalogue, r["kind"])
    n = int(rng.integers(r["count"][0], r["count"][1] + 1))
    made = 0
    for q in range(n):
        a = rng.uniform(0, math.tau)
        d = r["cluster_m"] * math.sqrt(rng.random())
        name = names[int(rng.integers(len(names)))]
        m = catalogue[name]
        s = rng.uniform(*r["size_m"]) / max(m["hi"][i] - m["lo"][i] for i in range(3))
        add(r["kind"], name, cx + math.cos(a) * d, cy + math.sin(a) * d, rng.uniform(0, math.tau), (s, s, s))
        made += 1
    return made


def coast_spikes(world, rules, catalogue, rng, add):
    """Grey spikes stuck all along a coast (the black coast): every few
    hundred metres to a couple of kilometres of its shore, a spike or two."""
    r = rules["coast_spikes"]
    land = world.hz > 1.0
    # The open sea: the water that reaches the map's edge (not a lake or a
    # river bed inland).
    lab, _ = ndimage.label(~land)
    edge = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
    sea = np.isin(lab, edge[edge > 0])
    zone = world.zones == r["zone"]
    shore = zone & land & (ndimage.distance_transform_edt(~sea) * world.cell < r["shore_m"])
    ys, xs = np.nonzero(shore)
    if not len(xs):
        return 0
    names = _names(catalogue, r["kind"])
    made = []
    for k in rng.permutation(len(xs)):
        x, y = (xs[k] + rng.random()) * world.cell, (ys[k] + rng.random()) * world.cell
        gap = rng.uniform(*r["gap_m"])
        if any((x - p[0]) ** 2 + (y - p[1]) ** 2 < gap * gap for p in made):
            continue
        made.append((x, y))
        if len(made) > r.get("max_sites", 400):
            break
        for q in range(int(rng.integers(1, r["group"] + 1))):
            name = names[int(rng.integers(len(names)))]
            m = catalogue[name]
            s = rng.uniform(*r["height_m"]) / m["hi"][2]
            add(r["kind"], name, x + rng.uniform(-60, 60), y + rng.uniform(-60, 60), rng.uniform(0, math.tau),
                (s, s, s))
    return len(made)


def water_mask(world):
    """Where the world's water stands, on the zone grid: the lab's own
    hydrology (<world>_water_preview.png, written by terrain_lab
    --water-preview: lakes blue, rivers red), else the painted skeleton;
    and the sea."""
    n = world.zones.shape[0]
    wet = world.hz <= 0.5
    skeleton = Path(str(world.stem) + "_water_skeleton.png")
    for name in ("_water_preview.png", "_water_skeleton.png"):
        f = Path(str(world.stem) + name)
        if not f.exists():
            continue
        # (a preview older than the skeleton - marsh_islands.py dried cells since - is stale)
        if name == "_water_preview.png" and skeleton.exists() and f.stat().st_mtime < skeleton.stat().st_mtime:
            continue
        im = np.asarray(Image.open(f).convert("RGB")).astype(np.int32)[::-1]
        if name == "_water_preview.png":
            w = ((im[..., 0] == 40) & (im[..., 1] == 120) & (im[..., 2] == 255)) | \
                ((im[..., 1] == 30) & (im[..., 2] == 25))
        else:
            w = (im[..., 2] > 200) & (im[..., 0] < 80)
        k = w.shape[0] // n
        if k >= 1:
            w = w.reshape(n, k, n, k).any(axis=(1, 3))
        else:
            up = n // w.shape[0]
            w = np.repeat(np.repeat(w, up, 0), up, 1)
        return wet | w, f.name
    return wet, "sea only"


def a1_monoliths(world, rules, catalogue, rng, add):
    """The giant monolith slabs A-1 (doc/megaliths/A1_MONOLITH.md): loose rows
    across the marshes, each slab on dry ground - an island or a ridge between
    the pools - its whole footprint clear of the water, inside the biome, on
    ground flat enough that its skirt reaches under every edge. A slab that
    fails is moved along its row (the next salt, up to `tries`), then dropped
    and counted in the report. The rows' gap, the spacing along them and the
    scale come from the asset's own `placement` (a1_monolith.asset.json)."""
    r = rules["a1_monoliths"]
    asset = json.loads((ROOT / r["manifest"]).read_text(encoding="utf-8"))
    pl = asset["placement"]
    field = dict(pl["field"])
    field["row_gap_m"] *= r.get("row_gap_scale", 1.0)
    field["spacing_m"] = [v * r.get("spacing_scale", 1.0) for v in field["spacing_m"]]
    names = _names(catalogue, r["kind"])
    inside = np.isin(world.zones, r["zones"])
    depth = ndimage.distance_transform_edt(inside) * world.cell
    wet, source = water_mask(world)
    # The water grown by a margin: a slab's flare must not dip into a pool.
    wet_near = ndimage.distance_transform_edt(~wet) * world.cell < r["water_gap_m"]
    ys, xs = np.nonzero(inside)
    pts = np.stack([(xs + 0.5) * world.cell, (ys + 0.5) * world.cell], 1)
    theta = rng.uniform(0, math.pi)
    d = np.array([math.cos(theta), math.sin(theta)])
    nrm = np.array([-d[1], d[0]])
    across, along = pts @ nrm, pts @ d
    n = world.zones.shape[0]

    def cell(x, y):
        return min(max(int(y / world.cell), 0), n - 1), min(max(int(x / world.cell), 0), n - 1)

    def footprint(x, y, yaw, m, s):
        """Points over the slab's footprint (its plan box, flare and all)."""
        c, si = math.cos(yaw), math.sin(yaw)
        out = []
        for u in np.linspace(m["lo"][0] * s, m["hi"][0] * s, 6):
            for v in np.linspace(m["lo"][1] * s, m["hi"][1] * s, 5):
                out.append((x + u * c - v * si, y + u * si + v * c))
        return out

    def judge(x, y, yaw, m, s, level=None):
        """(None, its numbers) when the slab may stand here, else (why not, None).
        `level`: on an island, its water's level - the footprint must stand
        above it (the water grid is too coarse for an island's top)."""
        i, j = cell(x, y)
        if not inside[i, j] or depth[i, j] < r["edge_m"]:
            return "edge", None
        foot = footprint(x, y, yaw, m, s)
        for fx, fy in foot:
            a, b = cell(fx, fy)
            if not inside[a, b]:
                return "edge", None
            if level is None and wet_near[a, b]:
                return "water", None
        # Stood as the lab stands it: the lowest ground under 0.7 of its reach.
        reach = 0.5 * max((m["hi"][0] - m["lo"][0]) * s, (m["hi"][1] - m["lo"][1]) * s)
        ring = [world.height(x, y)] + [world.height(x + math.cos(k * math.pi / 4) * reach * 0.7,
                                                     y + math.sin(k * math.pi / 4) * reach * 0.7) for k in range(8)]
        base = min(ring) - r["sink_m"]
        ground = np.array([world.height(fx, fy) for fx, fy in foot])
        bottom = base + m["lo"][2] * s
        # Hanging: the skirt's bottom must be under the ground everywhere.
        if (ground - bottom).min() < 0.3:
            return "hanging", None
        if level is not None and ground.min() < level + 0.5:
            return "water", None
        if ground.max() - ground.min() > r["max_relief_m"]:
            return "slope", None
        if base + r["sink_m"] < r["min_height_m"]:
            return "low", None
        return None, {"base": round(base, 1), "relief_m": round(float(ground.max() - ground.min()), 1)}

    islands = Path(str(world.stem) + "_marsh_islands.json")
    if islands.exists():
        # Each slab on the island raised for it (marsh_islands.py): its
        # variant, scale and turn were chosen there, to fit the island's top.
        placed, why_not = [], {}
        for q in json.loads(islands.read_text(encoding="utf-8"))["islands"]:
            why, check = judge(q["x"], q["y"], q["yaw"], catalogue[q["m"]], q["s"], q["level"])
            if why is not None:
                why_not[why] = why_not.get(why, 0) + 1
                continue
            placed.append({"x": q["x"], "y": q["y"], "m": q["m"], "s": q["s"], "yaw": q["yaw"], **check,
                           "top": round(check["base"] + catalogue[q["m"]]["hi"][2] * q["s"], 1), "salt": 0})
        return finish(world, r, inside, wet, source, placed, why_not, add, rng,
                      json.loads(islands.read_text(encoding="utf-8")).get("rows_deg", 0))

    order = list(rng.permutation(len(names)))
    placed, why_not = [], {}
    gap = field["row_gap_m"]
    for row in np.arange(across.min() + gap * 0.5, across.max(), gap):
        row_off = row + rng.uniform(-1, 1) * field["row_jitter_m"]
        band = np.abs(across - row_off) < world.cell
        if not band.any():
            continue
        t = along[band].min() + rng.uniform(0, field["spacing_m"][1])
        t_end = along[band].max()
        while t < t_end:
            why = None
            for salt in range(r["tries"]):
                tt = t + salt * r["try_step_m"]
                lateral = rng.uniform(-1, 1) * r["lateral_m"]
                x, y = d * tt + nrm * (row_off + lateral)
                name = names[order[len(placed) % len(names)]]
                m = catalogue[name]
                s = float(rng.uniform(*r.get("scale", pl["scale"])))
                yaw = float(rng.uniform(0, math.tau))
                why, check = judge(x, y, yaw, m, s)
                if why is None and any((x - q["x"]) ** 2 + (y - q["y"]) ** 2 < r["min_gap_m"] ** 2 for q in placed):
                    why = "gap"
                if why is None:
                    placed.append({"x": float(x), "y": float(y), "m": name, "s": s, "yaw": yaw, **check,
                                   "top": round(check["base"] + m["hi"][2] * s, 1), "salt": salt})
                    break
            if why is not None:
                why_not[why] = why_not.get(why, 0) + 1
            t += rng.uniform(*field["spacing_m"])
    return finish(world, r, inside, wet, source, placed, why_not, add, rng, math.degrees(theta))


def finish(world, r, inside, wet, source, placed, why_not, add, rng, rows_deg):
    """Adds the slabs and writes their checks (doc/BIOME_PLAN.md 1.5)."""
    for q in placed:
        add(r["kind"], q["m"], q["x"], q["y"], q["yaw"], (q["s"], q["s"], q["s"]), sink=r["sink_m"], look=r["look"])

    # The checks of doc/BIOME_PLAN.md 1.5 for these slabs, into the world's report.
    vis = visibility(world, inside & ~wet, placed, rng, r["visibility"])
    area = inside.sum() * (world.cell / 1000) ** 2
    CHECKS["a1_monoliths"] = {
        "placed": len(placed), "per_km2": round(len(placed) / max(area, 1e-6), 2), "zones_km2": round(area, 1),
        "water_from": source, "spots_dropped_after_tries": why_not,
        "moved_along_row": sum(1 for q in placed if q["salt"] > 0),
        "relief_under_foot_m_max": max((q["relief_m"] for q in placed), default=0),
        "visibility": vis,
        "rows_deg": round(rows_deg, 1),
    }
    return len(placed)


def visibility(world, ground_ok, placed, rng, spec):
    """Share of eye points (1.8 m over the biome's dry ground) from which the
    upper half of at least one slab stands clear of the relief within
    `range_m`."""
    if not placed:
        return {"share": 0.0, "points": 0, "need": spec["need"], "ok": False}
    ys, xs = np.nonzero(ground_ok)
    pick = rng.choice(len(xs), size=min(spec["points"], len(xs)), replace=False)
    tops = np.array([(q["x"], q["y"], q["base"] + 0.5 * (q["top"] - q["base"])) for q in placed])
    seen = 0
    for k in pick:
        ex, ey = (xs[k] + 0.5) * world.cell, (ys[k] + 0.5) * world.cell
        ez = world.height(ex, ey) + 1.8
        dist = np.hypot(tops[:, 0] - ex, tops[:, 1] - ey)
        ok = False
        for i in np.argsort(dist)[:spec["nearest"]]:
            if dist[i] > spec["range_m"]:
                break
            tx, ty, mid = tops[i]
            clear = True
            for f in np.linspace(0.02, 0.97, max(int(dist[i] / (world.cell * 0.5)), 2)):
                if world.height(ex + (tx - ex) * f, ey + (ty - ey) * f) > ez + (mid - ez) * f:
                    clear = False
                    break
            if clear:
                ok = True
                break
        seen += ok
    share = seen / len(pick)
    return {"share": round(share, 3), "points": int(len(pick)), "need": spec["need"], "ok": bool(share >= spec["need"])}


def place(world, rules, catalogue, rng, add):
    import world_features as wf
    counts = {}
    for name, fn in (("heroes", heroes), ("prism_row", prism_row), ("cube_cluster", cube_cluster),
                     ("coast_spikes", coast_spikes), ("a1_monoliths", a1_monoliths)):
        if name in rules:
            counts[name] = fn(world, rules, catalogue, rng, add)
    counts["groups"] = wf.place_groups(world, rules, catalogue, rng, add)
    # The carved stele A-2 in its glade (world_stele.py), on its own draws so
    # the rest stays as it was.
    import world_stele
    counts["a2_stele"] = world_stele.place(world, catalogue, np.random.default_rng(18), add, CHECKS)
    return counts
