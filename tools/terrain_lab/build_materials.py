#!/usr/bin/env python3
"""build_materials - the terrain core's ground pictures, from new_assets/Free.zip.

    python tools/terrain_lab/build_materials.py

Reads content/config/terrain_core/biomes.json, takes each material's base
colour straight out of the archive (or, with "file", from the project's own
pictures: the A-1 hatch for the marshes' red ground) (Free/<group>/<name>/<name>_basecolor.png),
recolours the ones that ask for it (snow, ash) and writes
assets/terrain_core/materials/<material>.png. The renderer loads them as one
texture array in the order of the config. Also extracts the supplied AO,
roughness, height and normal maps into materials/pbr, packing AO/roughness/
height into RGB properties. Normals are converted from the archive's DirectX
convention to OpenGL (+Y). The pack's own AO is white and its normals nearly
flat, so the normal is strengthened from the height map and the AO is made
from the height's cavities (relief_from_height). The renderer loads both.
"""
import hashlib
import io
import json
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "content" / "config" / "terrain_core" / "biomes.json"
ARCHIVE = ROOT / "new_assets" / "Free.zip"
OUT = ROOT / "assets" / "terrain_core" / "materials"
PBR = OUT / "pbr"


def recolour(image, spec):
    a = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    grey = a @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    s = spec.get("saturation", 1.0)
    a = grey[..., None] + (a - grey[..., None]) * s
    a = a * spec.get("brightness", 1.0) * np.array(spec.get("tint", [1, 1, 1]), dtype=np.float32)
    return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")


def relief_from_height(height, source_normal):
    """The pack's own surface maps are near empty - AO is white everywhere and
    the normals tilt by a couple of degrees - but its height maps hold the
    stones, cracks and tufts. So the normal is rebuilt from the height (seamless:
    the tile wraps), steep enough that its 90th percentile leans ~25 degrees,
    and added to the source's; the AO is the height's cavities - how far a
    texel sits below its blurred surroundings."""
    h = np.asarray(height, dtype=np.float32) / 255.0
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    drow = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5
    g = np.hypot(dx, drow)
    strength = float(np.clip(0.45 / max(np.percentile(g, 90), 1e-4), 2.0, 40.0))
    # OpenGL (+Y up the picture): rows run down, so d/dY = -d/drow.
    slope_x = -dx * strength + source_normal[..., 0] / np.maximum(source_normal[..., 2], 0.2)
    slope_y = drow * strength + source_normal[..., 1] / np.maximum(source_normal[..., 2], 0.2)
    n = np.stack([slope_x, slope_y, np.ones_like(h)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    normal = Image.fromarray(np.clip((n + 1.0) * 127.5 + 0.5, 0, 255).astype(np.uint8))
    cavity = ndi.gaussian_filter(h, 3.0, mode="wrap") - h
    k = 0.6 / max(np.percentile(cavity, 95), 1e-4)
    ao = np.clip(1.0 - np.maximum(cavity, 0) * k, 0.35, 1.0)
    return normal, Image.fromarray((ao * 255 + 0.5).astype(np.uint8)), strength


def own_material(name, spec, manifest):
    """A material from the project's own pictures rather than the archive:
    `file` the colour, `height` a picture whose dark is low (e.g. the grooves of
    the A-1 hatch, inverted with `height_invert`). Normal and AO from the height
    as for the archive's; roughness `roughness` (0..1) everywhere."""
    image = Image.open(ROOT / spec["file"]).convert("RGB")
    if "recolour" in spec:
        image = recolour(image, spec["recolour"])
    image.save(OUT / f"{name}.png")
    if "height" in spec:
        height = Image.open(ROOT / spec["height"]).convert("L").resize(image.size, Image.BILINEAR)
        if spec.get("height_invert"):
            height = Image.fromarray(255 - np.asarray(height))
    else:
        height = image.convert("L")
    flat = np.zeros((image.size[1], image.size[0], 3), np.float32)
    flat[..., 2] = 1.0
    normal, ao, strength = relief_from_height(height, flat)
    rough = Image.new("L", image.size, int(round(spec.get("roughness", 0.8) * 255)))
    for kind, img in (("ao", ao), ("roughness", rough), ("height", height), ("normal", normal)):
        img.save(PBR / f"{name}_{kind}.png")
    Image.merge("RGBA", (ao, rough, height, Image.new("L", image.size, 255))).save(PBR / f"{name}_properties.png")
    manifest["materials"][name] = {"file": spec["file"], "size": list(image.size), "properties": f"{name}_properties.png",
                                   "height": spec.get("height"), "normal_strength": round(strength, 1)}
    print(f"{name:16s} <- {spec['file']} {image.size[0]}x{image.size[1]}")


def main():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    OUT.mkdir(parents=True, exist_ok=True)
    PBR.mkdir(parents=True, exist_ok=True)
    manifest = {"archive": "new_assets/Free.zip", "archive_sha256": hashlib.sha256(ARCHIVE.read_bytes()).hexdigest(),
                "normal_convention": "OpenGL (+Y)", "source_normal_convention": "DirectX (-Y)",
                "normal_convention_evidence": "Normal Y negatively correlates with downward image-row height gradient; green inverted for Blender.",
                "properties_channels": {"r": "ao", "g": "roughness", "b": "height", "a": "unused (255)"},
                "renderer_connected": True, "materials": {}}
    with zipfile.ZipFile(ARCHIVE) as archive:
        for name, spec in config["materials"].items():
            if "file" in spec:
                own_material(name, spec, manifest)
                continue
            source = spec["source"]
            group = source.split("_")[0]
            member = f"Free/{group}/{source}/{source}_basecolor.png"
            image = Image.open(io.BytesIO(archive.read(member))).convert("RGB")
            if "recolour" in spec:
                image = recolour(image, spec["recolour"])
            image.save(OUT / f"{name}.png")
            maps = {}
            channels = []
            for kind, suffix in (("ao", "ambientocclusion"), ("roughness", "roughness"),
                                 ("height", "height"), ("normal", "normal")):
                member = f"Free/{group}/{source}/{source}_{suffix}.png"
                raw = archive.read(member)
                mapped = Image.open(io.BytesIO(raw)).convert("RGB" if kind == "normal" else "L")
                if mapped.size != image.size:
                    raise ValueError(f"{name}: {kind} size disagrees with albedo")
                if kind == "normal":
                    vectors = np.asarray(mapped, dtype=np.float32) / 127.5 - 1.0
                    vectors[..., 1] *= -1.0
                    lengths = np.linalg.norm(vectors, axis=2, keepdims=True)
                    if np.any(lengths < 0.01):
                        raise ValueError(f"{name}: zero-length source normal")
                    vectors /= lengths
                    mapped, derived_ao, strength = relief_from_height(channels[2], vectors)
                    channels[0] = derived_ao
                else:
                    channels.append(mapped)
                file = f"{name}_{kind}.png"
                mapped.save(PBR / file)
                maps[kind] = {"file": file, "source": member, "source_sha256": hashlib.sha256(raw).hexdigest()}
                if kind == "normal":
                    maps[kind]["derived"] = "source normal + normal from the height map, strength %.1f" % strength
                    maps["ao"]["derived"] = "cavities of the height map (the source AO is white)"
            Image.merge("RGBA", (*channels, Image.new("L", image.size, 255))).save(PBR / f"{name}_properties.png")
            manifest["materials"][name] = {"source": source, "size": list(image.size), "maps": maps,
                                           "properties": f"{name}_properties.png",
                                           "recolour": spec.get("recolour"),
                                           "note": "Source surface maps retained for recoloured snow/ash; no physical snow/ash surface inferred."}
            print(f"{name:16s} <- {source} {image.size[0]}x{image.size[1]}")
    (PBR / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
