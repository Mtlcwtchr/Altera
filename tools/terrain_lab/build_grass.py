#!/usr/bin/env python3
"""build_grass - the grass cover's sprites, from new_assets/kenney_foliageSprites.zip.

    python tools/terrain_lab/build_grass.py

Takes the white, shaded tufts (PNG/Shaded) the grass is drawn with - they are
tinted by the ground's own colour in the shader, so white is what is wanted -
and writes them, 256 a side with the tuft's foot at the bottom edge, to
assets/terrain_core/grass/NN.png in the order of SPRITES: grass first, then
the flowering ones.
"""
import io
import zipfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / "new_assets" / "kenney_foliageSprites.zip"
OUT = ROOT / "assets" / "terrain_core" / "grass"
GRASS = ["0052", "0053", "0054", "0055", "0056", "0057", "0062", "0063", "0064", "0065", "0066", "0067", "0068", "0069"]
FLOWERS = ["0061", "0071", "0072", "0073"]
SPRITES = GRASS + FLOWERS


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ARCHIVE) as z:
        for i, name in enumerate(SPRITES):
            image = Image.open(io.BytesIO(z.read(f"PNG/Shaded/sprite_{name}.png"))).convert("RGBA")
            # Crop to the tuft, keep it standing on the bottom edge, square it.
            box = image.getbbox()
            tuft = image.crop(box)
            side = max(tuft.size)
            square = Image.new("RGBA", (side, side), (255, 255, 255, 0))
            square.paste(tuft, ((side - tuft.size[0]) // 2, side - tuft.size[1]))
            square.resize((256, 256), Image.LANCZOS).save(OUT / f"{i:02d}.png")
    print(f"{len(SPRITES)} sprites ({len(GRASS)} grass, {len(FLOWERS)} flowering) -> {OUT}")


if __name__ == "__main__":
    main()
