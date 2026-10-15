"""The canyon steppe from the sandbox (tools/canyon_lab, user 15.10: "Да,
переходим") cut into the world: the sandbox's generator run over the
steppe's zone on the world's own land, written back as the game holds
ground - the 16 m macro map everywhere, and fine-relief windows where the
macro map misses the canyon by more than a tolerance (4 m a texel: the
steppe is ~290 km2; at 2 m its windows would take ~240 MB on the GPU).

    python tools/terrain_lab/canyon_relief.py [--world terrain_lab_out/world] [--seed N] [--cell 4]

Replaces steppe_relief.py and steppe_ground.py there: run after them (or
instead of them), before world_features.py; what they did in the steppe is
undone from their records first. Writes:
- <world>_final.r16: the canyon's heights (16 m texel = the mean of its fine ones);
- <world>_relief.bin: the windows as one sparse patch per piece of the zone
  (relief.hpp version 2), the marshes' patches kept;
- <world>_biomes.png: bare tops, burnt grass, floor, washes, scree (ids 3,
  19, 20, 21, 22) by majority over each 64 m cell;
- <world>_canyon.json: the columns and peaks (pinnacle meshes), world
  metres; checks;
- <world>_canyon_<k>/layers.npz etc (one per piece of the zone): the run as the
  sandbox writes it (rows south first), for its analysis and pictures.

How: the zone is the steppe's (ids 3 and 16 as the generator painted them),
its outline rounded. The floor at the rim is the ground round the zone (the
canyon opens onto the land below it and stands under the land above it),
spread smoothly inside, never less than `depth_min_m` under the land, whose
level the islands' tops keep; the old rivers ran off the land as it lay to
the rim. Islands keep `edge_m` inside the zone; the whole eases into the
heights round it over `feather_m` (canyon.json `world`).

Re-run safe: what it replaced (heights, biomes) is kept in
<world>_canyon_relief.npz with a digest of what it wrote; a re-run on its own
output starts from them.
"""

import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as nd

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "canyon_lab"))
import gen  # noqa: E402

PARAMS = ROOT / "tools" / "canyon_lab" / "canyon.json"
WORLD_RULES = ROOT / "content" / "config" / "terrain_core" / "world_rules.json"
ZONE_IDS = [3, 16]                        # canyon_steppe and the stone desert round its mesas
TOP, POCKET, FLOOR, WASH, SCREE = 3, 19, 20, 21, 22


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def sha(a):
    return hashlib.sha1(np.ascontiguousarray(a).tobytes()).hexdigest()


def upsample(a, k):
    """Bilinear, texel centres kept: a k-times finer grid of the same area."""
    ny, nx = a.shape
    yy = (np.arange(ny * k) + 0.5) / k - 0.5
    xx = (np.arange(nx * k) + 0.5) / k - 0.5
    return nd.map_coordinates(a.astype(np.float32), np.meshgrid(yy, xx, indexing="ij"), order=1, mode="nearest")


def read_relief(path):
    """The patches of <world>_relief.bin as raw records (header, payload bytes)."""
    if not path.exists():
        return []
    b = path.read_bytes()
    if b[:4] != b"RELF":
        return []
    version, count = struct.unpack("<II", b[4:12])
    out, o = [], 12
    for _ in range(count):
        if version == 1:
            x0, y0, t, base, w, h = struct.unpack("<ddffII", b[o:o + 32])
            tile = 0
            o += 32
        else:
            x0, y0, t, base, w, h, tile, _pad = struct.unpack("<ddffIIII", b[o:o + 40])
            o += 40
        if tile == 0:
            size = w * h * 4
        else:
            tw, th = -(-w // tile), -(-h // tile)
            nt = struct.unpack("<I", b[o + tw * th * 8:o + tw * th * 8 + 4])[0]
            size = tw * th * 8 + 4 + nt * tile * tile * 4
        out.append(((x0, y0, t, base, w, h, tile), b[o:o + size]))
        o += size
    return out


def write_relief(path, records):
    with open(path, "wb") as fh:
        fh.write(b"RELF")
        fh.write(struct.pack("<II", 2, len(records)))
        for (x0, y0, t, base, w, h, tile), payload in records:
            fh.write(struct.pack("<ddffIIII", x0, y0, t, base, w, h, tile, 0))
            fh.write(payload)


def sparse_patch(hz, wt, tile):
    """One sparse patch's payload: per tile (row-major, row 0 south) a u32
    index into the stored tiles (0xffffffff: none) and an f32 base; the
    count; then each stored tile's tile*tile pairs of f16 (height over the
    tile's base, weight)."""
    ny, nx = hz.shape
    th, tw = -(-ny // tile), -(-nx // tile)
    H = np.zeros((th * tile, tw * tile), np.float32)
    W = np.zeros((th * tile, tw * tile), np.float32)
    H[:ny, :nx], W[:ny, :nx] = hz, wt
    # (padding repeats the edge, so bilinear taps past it read sense)
    H[ny:, :] = H[ny - 1:ny, :]
    H[:, nx:] = H[:, nx - 1:nx]
    Hb = H.reshape(th, tile, tw, tile).transpose(0, 2, 1, 3)
    Wb = W.reshape(th, tile, tw, tile).transpose(0, 2, 1, 3)
    used = Wb.max(axis=(2, 3)) > 0
    index = np.full((th, tw), 0xFFFFFFFF, np.uint32)
    index[used] = np.arange(int(used.sum()), dtype=np.uint32)
    base = np.where(used, np.floor(Hb.min(axis=(2, 3))), 0.0).astype(np.float32)
    table = np.stack([index, base.view(np.uint32)], -1).astype("<u4")
    hs = (Hb[used] - base[used][:, None, None]).astype(np.float16)
    ws = Wb[used].astype(np.float16)
    pairs = np.stack([hs.view("<u2"), ws.view("<u2")], -1).astype("<u2")
    return table.tobytes() + struct.pack("<I", int(used.sum())) + pairs.tobytes(), int(used.sum())


def band_mask(biomes):
    """The canyon zone on the biome grid (rows north first): the steppe's
    zones as the generator painted them (steppe_ground's painting undone)."""
    zone = np.isin(biomes, ZONE_IDS)
    zone = nd.binary_closing(zone, structure=np.ones((3, 3)), iterations=2)
    zone = nd.binary_fill_holes(zone) & (biomes > 0)
    lab, n = nd.label(zone, structure=np.ones((3, 3)))
    sizes = np.bincount(lab.ravel())
    return zone & (sizes >= 300)[lab] & (lab > 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--world", default="terrain_lab_out/world")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--cell", type=float, default=4.0, help="the generator's cell, metres (the windows' texel)")
    ap.add_argument("--height-range", type=float, default=6000)
    ap.add_argument("--sea", type=float, default=0.13)
    ap.add_argument("--set", action="append", default=[], help="canyon.json override a.b=json")
    args = ap.parse_args()
    t_start = time.time()
    P = json.loads(PARAMS.read_text(encoding="utf-8"))
    for kv in args.set:
        k, v = kv.split("=", 1)
        d = P
        for key in k.split(".")[:-1]:
            d = d[key]
        d[k.split(".")[-1]] = json.loads(v)
    P["cell_m"] = args.cell
    P["remnants"]["stamp"] = False          # columns and peaks are meshes
    Wp = P["world"]
    world_m = json.loads(WORLD_RULES.read_text(encoding="utf-8"))["world_km"] * 1000.0

    final = Path(args.world + "_final.r16")
    raw = np.fromfile(final, "<u2")
    before_sha = hashlib.sha1(raw.tobytes()).hexdigest()
    N = int(np.sqrt(raw.size))
    q16 = raw.reshape(N, N).copy()
    T16 = world_m / N
    bio_path = Path(args.world + "_biomes.png")
    biomes = np.array(Image.open(bio_path))
    kb = N // biomes.shape[0]
    T64 = T16 * kb
    k = int(round(T16 / args.cell))                  # fine texels per macro texel

    keep = Path(args.world + "_canyon_relief.npz")
    band = None
    if keep.exists():
        z = np.load(keep)
        y0, y1, x0, x1 = (int(v) for v in z["box"])
        if str(z["wrote_h"]) == sha(q16[y0:y1, x0:x1]) and str(z["wrote_b"]) == sha(biomes):
            q16[y0:y1, x0:x1] = z["heights"]
            biomes = z["biomes"].copy()
            band = z["band"]
            print("canyon relief: starting from the heights and biomes it replaced last time")
    biomes_before = biomes.copy()
    # The canyon replaces steppe_relief and steppe_ground in the steppe: what
    # they did is undone first (their records), where they are in step.
    sg = Path(args.world + "_steppe_ground.png")
    if sg.exists():
        side = np.array(Image.open(sg))
        if side.shape == biomes.shape:
            biomes[side > 0] = side[side > 0]
    if band is None:
        band = band_mask(biomes)
    band_s = band[::-1]                               # rows south first, as the heights
    rows = np.nonzero(band_s.any(axis=1))[0]
    cols = np.nonzero(band_s.any(axis=0))[0]
    pad = int(np.ceil(Wp["floor_window_m"] / T64)) + 2
    b0, b1 = max(rows[0] - pad, 0), min(rows[-1] + pad + 1, band.shape[0])
    c0, c1 = max(cols[0] - pad, 0), min(cols[-1] + pad + 1, band.shape[1])
    sr = Path(args.world + "_steppe_relief.npz")
    steppe = None
    if sr.exists():
        z = np.load(sr)
        sy0, sy1, sx0, sx1 = (int(v) for v in z["box"])
        if str(z["wrote"]) == sha(q16[sy0:sy1, sx0:sx1]):
            steppe = (sy0, sy1, sx0, sx1, z["original"])
            # (the box holds all of it)
            b0, b1 = min(b0, sy0 // kb), max(b1, -(-sy1 // kb))
            c0, c1 = min(c0, sx0 // kb), max(c1, -(-sx1 // kb))
        else:
            print("canyon relief: steppe_relief's record is out of step; its cut left as it is")
    Y0, Y1, X0, X1 = b0 * kb, b1 * kb, c0 * kb, c1 * kb
    heights_before = q16[Y0:Y1, X0:X1].copy()
    if steppe is not None:
        sy0, sy1, sx0, sx1, original = steppe
        q16[sy0:sy1, sx0:sx1] = original
    cur = (q16[Y0:Y1, X0:X1].astype(np.float32) / 65535.0 - args.sea) * args.height_range
    pre = cur

    # The zone on the 16 m grid, its 64 m staircase rounded off.
    m16 = np.repeat(np.repeat(band_s[b0:b1, c0:c1], kb, 0), kb, 1)
    m16 = nd.gaussian_filter(m16.astype(np.float32), Wp["rim_round_m"] / T16) > 0.5
    s = nd.gaussian_filter(pre, Wp["smooth_m"] / T16)
    # The floor: at the rim the ground round the zone (so the canyon opens
    # onto the land below it and stands under the land above it), inside a
    # smooth spread of the rim's heights; never less than `depth_min_m` under
    # the land, whose level the islands' tops keep.
    ring = nd.binary_dilation(m16, iterations=2) & ~m16
    sig = Wp["floor_window_m"] / T16 / 3
    # (never under the shore's sand line: the canyon does not open into the sea)
    rim = nd.gaussian_filter(np.where(ring, np.maximum(cur, Wp["shore_m"]), 0.0), sig) / np.maximum(nd.gaussian_filter(ring.astype(np.float32), sig), 1e-6)
    lo = np.minimum(rim, s - Wp["depth_min_m"])
    # (smoothed broadly: the floor is a floor, not the land's every rise)
    lo = np.maximum(nd.gaussian_filter(lo, Wp["floor_smooth_m"] / T16), Wp["shore_m"])
    top = np.clip(s - lo, Wp["depth_min_m"], Wp["depth_max_m"])

    # Each piece of the zone on its own: generated, eased into the land,
    # written to the macro map, its windows one sparse patch.
    lab64, npieces = nd.label(band_s[b0:b1, c0:c1], structure=np.ones((3, 3)))
    region = biomes[::-1][b0:b1, c0:c1].copy()
    feats = {"columns": [], "peaks": [], "islands": 0, "runs": []}
    tile = Wp["tile"]
    band_px = max(1, int(round(Wp["blend_m"] / args.cell)))
    margin = int(np.ceil(Wp["edge_m"] / T64)) + 1
    kk = kb * k
    records = []
    tot = dict(zone=0, need=0, over=0, left_max=0.0, tiles=0, lo=1e9, hi=-1e9)
    for piece in range(1, npieces + 1):
        pr, pc = np.nonzero(lab64 == piece)
        r0, r1 = max(pr.min() - margin, 0), min(pr.max() + margin + 1, lab64.shape[0])
        q0, q1 = max(pc.min() - margin, 0), min(pc.max() + margin + 1, lab64.shape[1])
        e0, e1, h0, h1 = r0 * kb, r1 * kb, q0 * kb, q1 * kb
        near = np.repeat(np.repeat(lab64[r0:r1, q0:q1] == piece, kb, 0), kb, 1)
        near = nd.binary_dilation(near, iterations=int(Wp["rim_round_m"] / T16) + 2)
        mask = upsample((m16[e0:e1, h0:h1] & near).astype(np.float32), k) > 0.5
        W = dict(mask=mask, surf=upsample(s[e0:e1, h0:h1], k), floor=upsample(lo[e0:e1, h0:h1], k),
                 top=upsample(top[e0:e1, h0:h1], k), edge_m=Wp["edge_m"])
        t0 = time.time()
        g = gen.generate(P, args.seed * 7919 + 101 * piece, W)
        del W
        print(f"piece {piece}: {mask.shape[1] * args.cell / 1000:.1f} x {mask.shape[0] * args.cell / 1000:.1f} km, "
              f"{mask.mean() * mask.size * args.cell ** 2 / 1e6:.1f} km2, {len(g['islands'])} islands, "
              f"{len(g['columns'])} columns, {len(g['peaks'])} peaks, {time.time() - t0:.0f} s", flush=True)
        w = smoothstep(0.0, Wp["feather_m"], nd.distance_transform_edt(mask) * args.cell).astype(np.float32)

        # The ground as written: the canyon eased into what was there.
        cur_p = cur[e0:e1, h0:h1]
        fine = upsample(cur_p, k)
        fine += (g["h"] - fine) * w
        sy, sx = cur_p.shape
        touched = w.reshape(sy, k, sx, k).max(axis=(1, 3)) > 0
        new16 = np.where(touched, fine.reshape(sy, k, sx, k).mean(axis=(1, 3)), cur_p)
        out16 = np.clip(np.round((new16 / args.height_range + args.sea) * 65535.0), 0, 65535).astype("<u2")
        q16[Y0 + e0:Y0 + e1, X0 + h0:X0 + h1] = out16
        # (what the game reads back from the 16-bit map)
        cur[e0:e1, h0:h1] = (out16.astype(np.float32) / 65535.0 - args.sea) * args.height_range

        # Windows: where the macro map misses the ground by more than `tol`.
        coarse = upsample(cur[e0:e1, h0:h1], k)
        need = (np.abs(fine - coarse) > Wp["tol_m"]) & (w > 0)
        grown = nd.binary_dilation(need, iterations=band_px)
        d = nd.distance_transform_edt(~nd.binary_dilation(need, iterations=max(1, band_px // 2))) * args.cell
        wr = np.clip(1 - d / (Wp["blend_m"] * 0.5), 0, 1).astype(np.float32)
        del d
        ty, tx = -(-wr.shape[0] // tile), -(-wr.shape[1] // tile)
        pad_ = np.zeros((ty * tile, tx * tile), bool)
        pad_[:wr.shape[0], :wr.shape[1]] = grown
        tiles = pad_.reshape(ty, tile, tx, tile).any(axis=(1, 3))
        wr *= np.repeat(np.repeat(tiles, tile, 0), tile, 1)[:wr.shape[0], :wr.shape[1]]
        wr = wr * wr * (3 - 2 * wr)
        left = np.abs(coarse + (fine - coarse) * wr - fine)
        payload, stored = sparse_patch(fine, wr, tile)
        records.append((((X0 + h0) * T16, (Y0 + e0) * T16, args.cell, 0.0, fine.shape[1], fine.shape[0], tile), payload))
        zone = w > 0.99
        tot["zone"] += int(zone.sum())
        tot["need"] += int(need[zone].sum())
        tot["over"] += int((left[zone] > Wp["tol_m"]).sum())
        tot["left_max"] = max(tot["left_max"], float(left[zone].max()))
        tot["tiles"] += stored
        tot["lo"] = min(tot["lo"], float(fine[zone].min()))
        tot["hi"] = max(tot["hi"], float(fine[zone].max()))
        del coarse, need, grown, wr, left, pad_

        # Biomes: each 64 m cell of the piece by the majority of its ground.
        c = np.full(mask.shape, TOP, np.uint8)
        c[g["rel"] < 3] = FLOOR
        c[g["wash"] & (g["rel"] < 1)] = WASH
        c[g["talus"] > 1.0] = SCREE
        c[g["grass"]] = POCKET
        c[~mask] = 0
        ry, rx = mask.shape[0] // kk, mask.shape[1] // kk
        cb = c.reshape(ry, kk, rx, kk)
        votes = np.stack([(cb == v).sum(axis=(1, 3)) for v in (TOP, POCKET, FLOOR, WASH, SCREE)], -1)
        vote = np.array([TOP, POCKET, FLOOR, WASH, SCREE], np.uint8)[votes.argmax(-1)]
        has = votes.sum(-1) > kk * kk // 2
        region[r0:r1, q0:q1] = np.where(has, vote, region[r0:r1, q0:q1])
        del c, cb

        feats["islands"] += len(g["islands"])
        for key in ("columns", "peaks"):
            for q in g[key]:
                y, x = q["y"], q["x"]
                if not mask[y, x]:
                    continue
                feats[key].append(dict(q, piece=piece, x=float((X0 + h0) * T16 + (x + 0.5) * args.cell),
                                       y=float((Y0 + e0) * T16 + (y + 0.5) * args.cell),
                                       z=float(fine[y, x])))
        # The run as the sandbox writes it (analysis, pictures).
        run = Path(f"{args.world}_canyon_{piece}")
        feats["runs"].append(str(run))
        run.mkdir(parents=True, exist_ok=True)
        np.savez_compressed(run / "layers.npz", h=fine, rel_isl=g["rel_isl"], rel_col=g["rel_col"],
                            rel_peak=g["rel_peak"], talus=g["talus"], rel_fan=g["rel_fan"], wash=g["wash"],
                            grass=g["grass"], lab=g["lab"], mask=mask)
        (run / "features.json").write_text(json.dumps({key: g[key] for key in ("islands", "columns", "peaks", "fans")},
                                                      default=float), encoding="utf-8")
        (run / "stats.json").write_text(json.dumps({"cell_m": args.cell, "origin_m": [(X0 + h0) * T16, (Y0 + e0) * T16],
                                                    "rows": "south first"}), encoding="utf-8")
        del g, mask, fine, w

    relief_file = Path(args.world + "_relief.bin")
    write_relief(relief_file, [r for r in read_relief(relief_file) if r[0][6] == 0] + records)
    flipped = biomes[::-1].copy()
    flipped[b0:b1, c0:c1] = region
    biomes = flipped[::-1].copy()
    Image.fromarray(biomes).save(bio_path)
    q16.astype("<u2").tofile(final)
    # The marshes' base copy (marsh_islands.py), when it is in step with the
    # heights they wrote: the same change, so a marsh re-run keeps the canyon.
    marsh_json, marsh_base = Path(args.world + "_marsh_islands.json"), Path(args.world + "_final_base.r16")
    if marsh_json.exists() and marsh_base.exists():
        record = json.loads(marsh_json.read_text(encoding="utf-8"))
        if record.get("final_sha1") == before_sha:
            base = np.fromfile(marsh_base, "<u2").reshape(N, N)
            base[Y0:Y1, X0:X1] = q16[Y0:Y1, X0:X1]
            base.astype("<u2").tofile(marsh_base)
            record["final_sha1"] = hashlib.sha1(final.read_bytes()).hexdigest()
            marsh_json.write_text(json.dumps(record, indent=1), encoding="utf-8")
    np.savez_compressed(keep, box=np.array([Y0, Y1, X0, X1]), heights=heights_before, biomes=biomes_before,
                        band=band, wrote_h=sha(q16[Y0:Y1, X0:X1]), wrote_b=sha(biomes))

    # The canyon's meshes' places and the numbers.
    checks = {
        "zone_km2": round(tot["zone"] * args.cell ** 2 / 1e6, 2),
        "pieces": int(npieces),
        "macro_misses_by_tol_share": round(tot["need"] / max(1, tot["zone"]), 3),
        "windows": {"tiles": tot["tiles"], "texels": tot["tiles"] * tile * tile,
                    "mb": round(tot["tiles"] * tile * tile * 4 / 2 ** 20, 1), "tile_m": tile * args.cell},
        "left_over_tol_share": round(tot["over"] / max(1, tot["zone"]), 4),
        "left_max_m": round(tot["left_max"], 2),
        "relief_m": [round(tot["lo"], 1), round(tot["hi"], 1)],
        "biome_cells": {name: int((region == v).sum()) for name, v in
                        (("top", TOP), ("pocket", POCKET), ("floor", FLOOR), ("wash", WASH), ("scree", SCREE))},
        "seconds": round(time.time() - t_start),
    }
    Path(args.world + "_canyon.json").write_text(json.dumps(
        {"comment": "The canyon's columns and peaks (pinnacle meshes), world metres (x east, y north), z the ground "
                    "at the site; tools/terrain_lab/canyon_relief.py", "cell_m": args.cell,
         "runs": feats["runs"], "columns": feats["columns"], "peaks": feats["peaks"], "islands": feats["islands"],
         "checks": checks},
        indent=1, default=float), encoding="utf-8")
    print("canyon relief checks: " + json.dumps(checks))


if __name__ == "__main__":
    main()
