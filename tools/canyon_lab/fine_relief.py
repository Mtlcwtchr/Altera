"""The canyon's ground as the game would hold it (user 15.10: a 2 m cell where
it is needed): the macro map at 16 m everywhere, and fine-relief windows at
2 m only where the 16 m map misses the sandbox's ground by more than a
tolerance (walls, terrace edges, benches), eased into the coarse ground.

    python tools/canyon_lab/fine_relief.py --tag T [--tol 1.0] [--tile 64]

Writes into the run dir: h_game.npy (what the game would draw), relief_mask.png
(grey: coarse is enough, orange: 2 m windows, red: missed even so),
fine_relief.json (shares, texels, bytes, the error left over).
"""

import argparse
import json
from pathlib import Path

import numpy as np
import scipy.ndimage as nd
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
CANYON_KM2 = 86.7      # canyon_steppe + mesa_pocket + canyon_floor + dry_wash in the world


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="dev")
    ap.add_argument("--macro", type=float, default=16.0, help="the macro map's texel, metres")
    ap.add_argument("--tol", type=float, default=1.0, help="metres the coarse ground may miss by")
    ap.add_argument("--tile", type=float, default=64.0, help="fine-relief window size, metres")
    ap.add_argument("--blend", type=float, default=16.0, help="easing band round the needed cells, metres")
    args = ap.parse_args()
    out = ROOT / "terrain_lab_out" / "canyon_lab" / args.tag
    st = json.loads((out / "stats.json").read_text(encoding="utf-8"))
    cell = st["cell_m"]
    h = np.load(out / "layers.npz")["h"].astype(np.float32)
    n = h.shape[0]
    k = int(round(args.macro / cell))

    # The macro map: a 16 m texel the mean of its ground, read back bilinear.
    m = h.reshape(n // k, k, n // k, k).mean(axis=(1, 3))
    yy = (np.arange(n) + 0.5) / k - 0.5
    coarse = nd.map_coordinates(m, np.meshgrid(yy, yy, indexing="ij"), order=1, mode="nearest").astype(np.float32)
    err = np.abs(h - coarse)
    need = err > args.tol
    # Windows: whole tiles round every needed cell (and its easing band).
    t = int(round(args.tile / cell))
    band = int(round(args.blend / cell))
    reach = nd.binary_dilation(need, iterations=band)
    tiles = reach.reshape(n // t, t, n // t, t).any(axis=(1, 3))
    win = np.repeat(np.repeat(tiles, t, 0), t, 1)
    # Weight: 1 on the needed cells grown by half the band, easing to 0 over
    # the rest of it (and never past its window).
    d = nd.distance_transform_edt(~nd.binary_dilation(need, iterations=max(1, band // 2))) * cell
    w = np.clip(1 - d / (args.blend * 0.5), 0, 1) * win
    w = w * w * (3 - 2 * w)
    game = coarse + (h - coarse) * w
    left = np.abs(game - h)
    np.save(out / "h_game.npy", game.astype(np.float32))

    texels = int(win.sum())
    share = float(tiles.mean())
    res = dict(
        tol_m=args.tol, tile_m=args.tile, macro_m=args.macro,
        coarse_only=dict(cells_missed_share=float(need.mean()), max_err_m=float(err.max()),
                         p99_err_m=float(np.percentile(err, 99))),
        windows=dict(count=int(tiles.sum()), area_share=share, texels=texels,
                     mb_this_patch=texels * 4 / 2 ** 20,
                     mb_whole_canyon_est=share * CANYON_KM2 * 1e6 / (cell * cell) * 4 / 2 ** 20),
        with_windows=dict(max_err_m=float(left.max()), p99_err_m=float(np.percentile(left, 99)),
                          cells_over_tol_share=float((left > args.tol).mean())),
    )
    (out / "fine_relief.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    img = np.full((n, n, 3), 70, np.uint8)
    img[...] = (40 + 120 * np.clip(h / max(1, h.max()), 0, 1))[..., None].astype(np.uint8)
    img[win] = (img[win] * 0.5 + np.array([230, 140, 50]) * 0.5).astype(np.uint8)
    img[left > args.tol] = (230, 30, 30)
    Image.fromarray(img).resize((1024, 1024), Image.LANCZOS).save(out / "relief_mask.png")
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
