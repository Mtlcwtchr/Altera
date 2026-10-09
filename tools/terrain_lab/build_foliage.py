#!/usr/bin/env python3
"""build_foliage - the terrain core's foliage models, from new_assets.

    python tools/terrain_lab/build_foliage.py            # what foliage.json names
    python tools/terrain_lab/build_foliage.py --all      # every model in the packs, with thumbnails

Unpacks the archives into .cache/foliage_src, has Blender (in the background,
tools/blender/export_foliage.py) turn each model into a .mesh and a .png in
assets/terrain_core/foliage, and with --all writes a contact sheet of every
model to .cache/foliage_thumbs/sheet.png.
"""
import argparse
import json
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / ".cache" / "foliage_src"
OUT = ROOT / "assets" / "terrain_core" / "foliage"
CONFIG = ROOT / "content" / "config" / "terrain_core" / "foliage.json"
BLENDER = Path(r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe")
ARCHIVES = ["retro_nature_pack.zip", "tree_pack_1.1.zip"]


def unpack():
    for name in ARCHIVES:
        marker = SRC / (name + ".done")
        if marker.exists():
            continue
        with zipfile.ZipFile(ROOT / "new_assets" / name) as z:
            z.extractall(SRC)
        marker.write_text("ok")


def every_model():
    jobs = []
    retro = SRC / "retro_nature_pack"
    for glb in sorted((retro / "models" / "glTF").rglob("*.glb")):
        if "winter" in glb.stem:
            continue
        kind = glb.parent.name
        # Textures are by model: tree01 -> tree01_summer, bush01 -> bush1_summer, grass01 -> grass_summer.
        stem = glb.stem
        candidates = [stem + "_summer.png", stem.replace("0", "", 1) + "_summer.png",
                      stem.rstrip("0123456789") + "_summer.png", "grass_summer.png"]
        texture = next((retro / "textures" / kind / c for c in candidates if (retro / "textures" / kind / c).exists()), None)
        jobs.append({"name": "rn_" + stem, "source": str(glb), "texture": str(texture) if texture else None})
    pack = SRC / "tree_pack_1.1"
    for fbx in sorted((pack / "models").glob("*.fbx")):
        texture = pack / "textures" / (fbx.stem + ".png")
        jobs.append({"name": "tp_" + fbx.stem, "source": str(fbx), "texture": str(texture) if texture.exists() else None})
    return jobs


def sheet(thumbs, report):
    from PIL import Image, ImageDraw
    names = sorted(report)
    cols, tile = 10, 192
    rows = (len(names) + cols - 1) // cols
    out = Image.new("RGB", (cols * tile, rows * (tile + 28)), (60, 64, 70))
    draw = ImageDraw.Draw(out)
    for i, name in enumerate(names):
        x, y = (i % cols) * tile, (i // cols) * (tile + 28)
        path = thumbs / (name + ".png")
        if path.exists():
            image = Image.open(path).convert("RGBA")
            out.paste(image, (x, y), image)
        r = report[name]
        size = [r["max"][k] - r["min"][k] for k in range(3)]
        draw.text((x + 3, y + tile), name, fill="white")
        draw.text((x + 3, y + tile + 13), "%d tris, %.1fm tall" % (r["triangles"], size[2]), fill=(200, 200, 200))
    out.save(thumbs / "sheet.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    unpack()
    if args.all:
        jobs = every_model()
        thumbs = ROOT / ".cache" / "foliage_thumbs"
        out = ROOT / ".cache" / "foliage_all"
    else:
        config = json.loads(CONFIG.read_text(encoding="utf-8"))
        jobs = [{"name": name, "source": str(SRC / spec["source"]),
                 "texture": str(SRC / spec["texture"]) if spec.get("texture") else None}
                for name, spec in config["models"].items()]
        thumbs, out = None, OUT
    job_file = ROOT / ".cache" / "foliage_jobs.json"
    job_file.write_text(json.dumps({"out": str(out), "thumbs": str(thumbs) if thumbs else None, "jobs": jobs}))
    run = subprocess.run([str(BLENDER), "--background", "--factory-startup", "--python",
                          str(ROOT / "tools" / "blender" / "export_foliage.py"), "--", str(job_file)],
                         capture_output=True, text=True)
    for line in run.stdout.splitlines():
        if line.startswith(("exported", "FAILED", "no texture")):
            print(line)
    if run.returncode != 0:
        print(run.stderr[-3000:], file=sys.stderr)
        sys.exit(run.returncode)
    if not args.all:
        # One size for the texture array; nearest, so the painted pixels stay crisp.
        from PIL import Image
        for png in out.glob("*.png"):
            image = Image.open(png).convert("RGBA")
            if image.size != (256, 256):
                image.resize((256, 256), Image.NEAREST).save(png)
    if thumbs:
        sheet(thumbs, json.loads((out / "report.json").read_text()))
        print("contact sheet:", thumbs / "sheet.png")


if __name__ == "__main__":
    main()
