#!/usr/bin/env python3
"""build_materials - the terrain core's ground pictures, from new_assets/Free.zip.

    python tools/terrain_lab/build_materials.py

Reads content/config/terrain_core/biomes.json, takes each material's base
colour straight out of the archive (Free/<group>/<name>/<name>_basecolor.png),
recolours the ones that ask for it (snow, ash) and writes
assets/terrain_core/materials/<material>.png. The renderer loads them as one
texture array in the order of the config.
"""
import io
import json
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "content" / "config" / "terrain_core" / "biomes.json"
ARCHIVE = ROOT / "new_assets" / "Free.zip"
OUT = ROOT / "assets" / "terrain_core" / "materials"


def recolour(image, spec):
    a = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    grey = a @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    s = spec.get("saturation", 1.0)
    a = grey[..., None] + (a - grey[..., None]) * s
    a = a * spec.get("brightness", 1.0) * np.array(spec.get("tint", [1, 1, 1]), dtype=np.float32)
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def main():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    OUT.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ARCHIVE) as archive:
        for name, spec in config["materials"].items():
            source = spec["source"]
            group = source.split("_")[0]
            member = f"Free/{group}/{source}/{source}_basecolor.png"
            image = Image.open(io.BytesIO(archive.read(member))).convert("RGB")
            if "recolour" in spec:
                image = recolour(image, spec["recolour"])
            image.save(OUT / f"{name}.png")
            print(f"{name:16s} <- {source} {image.size[0]}x{image.size[1]}")


if __name__ == "__main__":
    main()
