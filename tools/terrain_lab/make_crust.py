"""Cracked clay crust for the canyon floors (doc/BIOME_PLAN.md 3.3): a seamless
128 px tile (4 m in the ground's tile_m) of dry ochre plates split by cracks.

    python tools/terrain_lab/make_crust.py

Writes assets/terrain_core/materials/own/crust_ochre_128.png (colour) and
crust_ochre_height_128.png (height: plates high and a little domed, cracks
low, the rims curled up); biomes.json `crust_ochre` points at them and
build_materials.py makes the normal and AO from the height.

Plates are the cells of a jittered grid on the torus (so the tile repeats
without a seam), their walls pushed about by a noise so they are not
polygons; a second, finer net of hairline cracks splits some plates.
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "terrain_core" / "materials" / "own"


def tile_noise(n, cells, rng):
    """Smooth value noise with `cells` cells across, seamless on an n x n tile."""
    grid = rng.random((cells, cells))
    t = np.arange(n) / n * cells
    i0 = np.floor(t).astype(int)
    f = t - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    i0 %= cells
    a = grid[i0][:, i0] * (1 - f)[None, :] + grid[i0][:, i1] * f[None, :]
    b = grid[i1][:, i0] * (1 - f)[None, :] + grid[i1][:, i1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def cells_on_torus(n, count, jitter, warp, rng):
    """Distance to the nearest and second-nearest seed of a jittered grid of
    count x count seeds on the torus, after warping the lookup by `warp` px."""
    g = np.stack(np.meshgrid(np.arange(count), np.arange(count), indexing="ij"), -1).reshape(-1, 2).astype(float)
    seeds = (g + 0.5 + (rng.random(g.shape) - 0.5) * jitter) * (n / count)
    y, x = np.mgrid[0:n, 0:n].astype(float)
    y = y + (tile_noise(n, 6, rng) - 0.5) * warp
    x = x + (tile_noise(n, 6, rng) - 0.5) * warp
    d1 = np.full((n, n), 1e9)
    d2 = np.full((n, n), 1e9)
    ident = np.zeros((n, n), int)
    for k, (sy, sx) in enumerate(seeds):
        dy = np.abs(y - sy)
        dy = np.minimum(dy, n - dy)
        dx = np.abs(x - sx)
        dx = np.minimum(dx, n - dx)
        d = np.hypot(dy, dx)
        closer = d < d1
        d2 = np.where(closer, d1, np.minimum(d2, d))
        ident = np.where(closer, k, ident)
        d1 = np.where(closer, d, d1)
    return d1, d2, ident


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=128)
    ap.add_argument("--plates", type=int, default=7, help="plates across the 4 m tile")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    n = args.size
    rng = np.random.default_rng(args.seed)

    d1, d2, ident = cells_on_torus(n, args.plates, 0.9, n * 0.06, rng)
    edge = d2 - d1                                  # 0 on a crack
    crack = np.clip(1.0 - edge / 1.6, 0, 1)         # main cracks ~1.5 px
    # Hairline cracks: a finer net, only through some plates.
    f1, f2, _ = cells_on_torus(n, args.plates * 2, 1.0, n * 0.04, rng)
    hair = np.clip(1.0 - (f2 - f1) / 0.6, 0, 1) * (tile_noise(n, 5, rng) > 0.68)
    # Plate tops domed toward their middle, rims curled up by the crack.
    dome = np.clip(edge / (n / args.plates * 0.5), 0, 1)
    rim = np.exp(-((edge - 2.2) / 1.4) ** 2)
    height = 0.55 + 0.25 * np.sqrt(dome) + 0.12 * rim - 0.75 * crack - 0.3 * hair
    plate_tone = rng.random(args.plates * args.plates)[ident] - 0.5
    grain = rng.random((n, n)) - 0.5
    mottle = tile_noise(n, 8, rng) - 0.5
    height += grain * 0.06
    height = np.clip(height, 0, 1)

    # Pale dry ochre clay, dusty on the plates, dark rust in the cracks.
    clay = np.array([0.80, 0.63, 0.45])
    dark = np.array([0.36, 0.22, 0.15])
    light = 1.0 + plate_tone[..., None] * 0.10 + mottle[..., None] * 0.16 + grain[..., None] * 0.10 + rim[..., None] * 0.08
    colour = clay[None, None, :] * light
    shade = np.clip(crack * 0.95 + hair * 0.55, 0, 1)[..., None]
    colour = colour * (1 - shade) + dark[None, None, :] * shade
    colour = np.clip(colour, 0, 1)

    OUT.mkdir(parents=True, exist_ok=True)
    Image.fromarray((colour * 255 + 0.5).astype(np.uint8)).save(OUT / f"crust_ochre_{n}.png")
    Image.fromarray((height * 255 + 0.5).astype(np.uint8)).save(OUT / f"crust_ochre_height_{n}.png")
    print(f"crust_ochre {n}x{n}: {args.plates} plates across -> {OUT}")


if __name__ == "__main__":
    main()
