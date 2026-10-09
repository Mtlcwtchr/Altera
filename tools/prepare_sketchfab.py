#!/usr/bin/env python3
"""Downloaded Sketchfab surfaces -> optimized, painted 1960s-80s fantasy library.

Run with the Python environment containing NumPy, Pillow and optionally SciPy:
    python tools/prepare_sketchfab.py --only f7e507f0b0654579accf5dbf05745f7d
    python tools/prepare_sketchfab.py --workers 2

Uses model_simplify where its supported attributes are sufficient, otherwise
Blender's UV-preserving reducer. Animated/skinned/morph geometry is retained.
Sources are read-only; output is assets/generated/sketchfab_fantasy_6080.
"""
import argparse
import concurrent.futures
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import stylize_textures as style

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "content/config/style/sketchfab_pipeline.json"
HELPER = ROOT / "tools/blender/prepare_sketchfab_asset.py"
OUT = ROOT / "assets/generated/sketchfab_fantasy_6080"
INPUT = ROOT / "assets/downloaded/sketchfab"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf8")
    temp.replace(path)


def primitive_triangles(doc):
    count = 0
    for mesh in doc.get("meshes", []):
        for p in mesh.get("primitives", []):
            n = doc["accessors"][p["indices"]]["count"] if "indices" in p else doc["accessors"][p["attributes"]["POSITION"]]["count"]
            mode = p.get("mode", 4)
            count += n // 3 if mode == 4 else max(0, n - 2) if mode in (5, 6) else 0
    return count


def protected(doc):
    return bool(doc.get("skins") or doc.get("animations") or
                any(mesh.get("weights") or p.get("targets") for mesh in doc.get("meshes", []) for p in mesh["primitives"]))


def native_safe(doc):
    if protected(doc):
        return False
    if any("uri" not in image for image in doc.get("images", [])):
        return False
    if any("uri" not in b or b["uri"].startswith("data:") for b in doc.get("buffers", [])):
        return False
    if any("sparse" in a for a in doc.get("accessors", [])):
        return False
    for mesh in doc.get("meshes", []):
        for p in mesh["primitives"]:
            if p.get("mode", 4) != 4 or p.get("extensions") or p.get("targets"):
                return False
            if set(p["attributes"]) - {"POSITION", "NORMAL", "TEXCOORD_0", "TANGENT"}:
                return False
            for key in ("POSITION", "NORMAL", "TEXCOORD_0"):
                if key in p["attributes"] and doc["accessors"][p["attributes"][key]]["componentType"] != 5126:
                    return False
    return True


def local_path(folder, uri):
    if urllib.parse.urlparse(uri).scheme or uri.startswith("data:"):
        raise ValueError("Expected local external resource")
    result = (folder / urllib.parse.unquote(uri)).resolve()
    if not result.is_relative_to(folder.resolve()):
        raise ValueError("External resource escapes asset directory")
    return result


def run(cmd, log, timeout=600):
    with log.open("w", encoding="utf8") as handle:
        result = subprocess.run([str(x) for x in cmd], stdout=handle, stderr=subprocess.STDOUT,
                                timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError(f"{Path(str(cmd[0])).name} exited {result.returncode}; see {log.name}")


def srgb(value):
    return np.where(value <= 0.0031308, 12.92 * value, 1.055 * np.maximum(value, 0) ** (1 / 2.4) - 0.055)


def linear(value):
    return np.where(value <= 0.04045, value / 12.92, ((np.maximum(value, 0) + 0.055) / 1.055) ** 2.4)


def paint_factor(factor, preset):
    img = np.array([[list(srgb(np.asarray(factor[:3], dtype=np.float32))) + [factor[3]]]], dtype=np.float32)
    color_preset = {"steps": [s for s in preset["steps"] if s["op"] in {"palette", "posterize"}]}
    painted = style.stylize(img, color_preset, "albedo")[0, 0]
    return list(map(float, linear(painted[:3]))) + [float(factor[3])]


def paint_vertex_colours(doc, folder, preset):
    # Rewrite only COLOR attributes in copied buffers, including interleaved and
    # normalized integer layouts. JOINTS, WEIGHTS, morphs and UVs stay byte-identical.
    ids = {v for mesh in doc.get("meshes", []) for p in mesh["primitives"]
           for k, v in p["attributes"].items() if k.startswith("COLOR_")}
    vertex_preset = {"steps": [s for s in preset["steps"]
                               if s["op"] in {"gradient_map", "palette", "posterize"}]}
    buffers = {}
    for index in ids:
        a = doc["accessors"][index]
        if "sparse" in a or "bufferView" not in a:
            continue
        view = doc["bufferViews"][a["bufferView"]]
        b = view["buffer"]
        path = local_path(folder, doc["buffers"][b]["uri"])
        raw = buffers.setdefault(b, bytearray(path.read_bytes()))
        dtype = {5121: np.dtype("u1"), 5123: np.dtype("<u2"), 5126: np.dtype("<f4")}[a["componentType"]]
        channels = {"VEC3": 3, "VEC4": 4}[a["type"]]
        values = np.ndarray((a["count"], channels), dtype=dtype, buffer=raw,
                            offset=view.get("byteOffset", 0) + a.get("byteOffset", 0),
                            strides=(view.get("byteStride", channels * dtype.itemsize), dtype.itemsize))
        divisor = np.iinfo(dtype).max if dtype.kind != "f" else 1
        img = np.ones((a["count"], 1, 4), dtype=np.float32)
        img[:, 0, :channels] = values.astype(np.float32) / divisor
        img[..., :3] = srgb(img[..., :3])
        img = style.stylize(img, vertex_preset, "albedo")
        img[..., :3] = linear(img[..., :3])
        result = np.clip(img[:, 0, :channels], 0, 1) * divisor
        values[:] = np.rint(result) if dtype.kind != "f" else result
    for b, raw in buffers.items():
        local_path(folder, doc["buffers"][b]["uri"]).write_bytes(raw)
    return len(ids)


def paint_materials(doc, source_dir, stage, preset, size):
    cache = {}
    original_textures = copy.deepcopy(doc.get("textures", []))
    original_images = copy.deepcopy(doc.get("images", []))

    def repaint(ref, kind):
        old_index = ref["index"]
        key = (old_index, kind)
        if key in cache:
            ref["index"] = cache[key]
            return
        texture = copy.deepcopy(original_textures[old_index])
        if "source" not in texture:
            raise ValueError("Compressed texture requires a decoded fallback")
        image_index = texture["source"]
        image = original_images[image_index]
        path = local_path(source_dir, image["uri"])
        rgba = Image.open(path).convert("RGBA")
        rgba.thumbnail((size, size), Image.Resampling.LANCZOS)
        img = np.asarray(rgba, dtype=np.float32) / 255
        if kind == "orm":
            # glTF packs AO/Roughness/Metalness in R/G/B. Process G alone;
            # never run color grading over packed data.
            rough = np.repeat(img[..., 1:2], 3, axis=2)
            rough = style.stylize(rough, preset, "roughness")
            img[..., 1] = np.clip(0.25 + rough[..., 0] * 0.75, 0.25, 1)
        elif kind == "specgloss":
            # Legacy SpecularGlossiness packs gloss in alpha, not opacity.
            rough = np.repeat(1 - img[..., 3:4], 3, axis=2)
            rough = style.stylize(rough, preset, "roughness")
            img[..., 3] = 1 - np.clip(0.25 + rough[..., 0] * 0.75, 0.25, 1)
        elif kind == "ao":
            img[..., 0] = style.blur(img[..., 0:1], 2)[..., 0]
        elif kind != "copy":
            img = style.stylize(img, preset, kind)
        name = f"textures/paint_{image_index}_{kind}.png"
        (stage / "textures").mkdir(exist_ok=True)
        style.save(img, stage / name)
        texture["source"] = len(doc["images"])
        doc["images"].append({"uri": name, "mimeType": "image/png"})
        ref["index"] = len(doc["textures"])
        doc["textures"].append(texture)
        cache[key] = ref["index"]

    for material in doc.get("materials", []):
        legacy = material.get("extensions", {}).get("KHR_materials_pbrSpecularGlossiness")
        if legacy:
            if legacy.get("diffuseTexture"):
                repaint(legacy["diffuseTexture"], "albedo")
            else:
                legacy["diffuseFactor"] = paint_factor(legacy.get("diffuseFactor", [1, 1, 1, 1]), preset)
            if legacy.get("specularGlossinessTexture"):
                repaint(legacy["specularGlossinessTexture"], "specgloss")
            legacy["glossinessFactor"] = min(legacy.get("glossinessFactor", 1), 0.45)
            legacy["specularFactor"] = [v * 0.7 for v in legacy.get("specularFactor", [1, 1, 1])]
        pbr = material.setdefault("pbrMetallicRoughness", {})
        base = pbr.get("baseColorTexture")
        if base:
            repaint(base, "albedo")
        else:
            pbr["baseColorFactor"] = paint_factor(pbr.get("baseColorFactor", [1, 1, 1, 1]), preset)
        orm = pbr.get("metallicRoughnessTexture")
        if orm:
            repaint(orm, "orm")
        else:
            pbr["roughnessFactor"] = max(0.65, pbr.get("roughnessFactor", 1))
        pbr["metallicFactor"] = pbr.get("metallicFactor", 1) * 0.75
        normal = material.get("normalTexture")
        if normal:
            repaint(normal, "normal")
            normal["scale"] = normal.get("scale", 1) * 0.65
        if material.get("occlusionTexture"):
            repaint(material["occlusionTexture"], "ao")
            material["occlusionTexture"]["strength"] = min(material["occlusionTexture"].get("strength", 1), 0.8)
        def remaining_maps(value):
            if isinstance(value, dict):
                for k, v in value.items():
                    if k.endswith("Texture") and isinstance(v, dict) and "index" in v:
                        if v["index"] < len(original_textures):
                            repaint(v, "copy")
                    else:
                        remaining_maps(v)
            elif isinstance(value, list):
                for item in value:
                    remaining_maps(item)
        remaining_maps(material)
    return len(cache)


def validate(path, original):
    doc = read(path)
    if not primitive_triangles(doc):
        raise ValueError("Export has no surface triangles")
    for entry in doc.get("buffers", []) + doc.get("images", []):
        if "uri" in entry and not entry["uri"].startswith("data:"):
            p = local_path(path.parent, entry["uri"])
            if not p.is_file():
                raise ValueError("Missing exported resource " + entry["uri"])
    if original.get("animations") and not doc.get("animations"):
        raise ValueError("Animations were lost")
    if original.get("skins") and not doc.get("skins"):
        raise ValueError("Skin was lost")
    old_morph = any(p.get("targets") for m in original.get("meshes", []) for p in m["primitives"])
    new_morph = any(p.get("targets") for m in doc.get("meshes", []) for p in m["primitives"])
    if old_morph and not new_morph:
        raise ValueError("Morph targets were lost")
    return doc


def process(model, args, config, presets, fingerprint):
    uid = model["uid"]
    asset = "sf_" + uid[:16]
    output = args.dest / asset
    output.mkdir(parents=True, exist_ok=True)
    target = output / (asset + ".gltf")
    meta = output / "source.json"
    if meta.exists() and read(meta).get("processing_fingerprint") == fingerprint and target.exists() and not args.force:
        return read(output / "processing.json") | {"status": "existing"}
    folder = INPUT / uid
    source = next(iter(sorted(folder.rglob("*.gltf"))), None)
    if source is None:
        return {"uid": uid, "name": model["name"], "status": "unsupported", "reason": "No external glTF source"}
    original = read(source)
    count = primitive_triangles(original)
    if not count:
        return {"uid": uid, "name": model["name"], "status": "unsupported", "reason": "Point cloud or lines; no surface mesh"}
    started = time.time()
    profile_name = model["cat"]
    name = model["name"].lower()
    if any(word in name for word in config["stone_keywords"]):
        profile_name = "stone"
    elif any(word in name for word in ("tree", "forest", "mushroom", "plant", "oak", "pine")):
        profile_name = "flora"
    profile = dict(config["profiles"][profile_name])
    override = config.get("asset_overrides", {}).get(uid)
    if override:
        profile.update(override)
    preset = presets[profile["preset"]]
    stage = ROOT / ".cache/sketchfab-pipeline/jobs" / asset
    stage.mkdir(parents=True, exist_ok=True)
    row = {"uid": uid, "asset_id": asset, "name": model["name"], "category": model["cat"],
           "profile": profile_name, "preset": profile["preset"], "triangles_in": count}
    if override:
        row["processing_override"] = override
    try:
        doc = copy.deepcopy(original)
        reducer = "geometry retained for rig/animation/morph" if protected(original) else "Blender decimate"
        if profile["triangles"] == 0:
            reducer = "Blender; authored component geometry retained"
        simplify = None
        if args.optimizer and profile["triangles"] > 0 and native_safe(original):
            per_mesh = max(400, profile["triangles"] // max(1, len(original["meshes"])))
            geometry = stage / "optimized.gltf"
            simplify_path = output / "simplify.json"
            for multiplier in (1, 2, 4):
                run([args.optimizer, "--in", source, "--out", geometry, "--tris", per_mesh * multiplier,
                     "--max-growth", "2", "--solid-share", "0.4", "--res", "64", "--report", simplify_path],
                    output / "optimizer.log")
                simplify = read(simplify_path)
                worst = min(min(m["silhouette_iou"]) for m in simplify["meshes"])
                if worst >= config["silhouette_min_iou"]:
                    doc = read(geometry)
                    reducer = "meshoptimizer (silhouette checked)"
                    row["worst_silhouette_iou"] = round(worst, 4)
                    break
            else:
                reducer = "original geometry retained after silhouette check"
                row["reduction_rejected"] = "Silhouette IoU below threshold"
        if reducer != "meshoptimizer (silhouette checked)":
            for buffer in doc.get("buffers", []):
                buffer_target = local_path(stage, buffer["uri"])
                buffer_target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(local_path(source.parent, buffer["uri"]), buffer_target)
        # Copy untouched source images needed by extra material extensions, then
        # create independent painted maps for each actual glTF texture role.
        for image in doc.get("images", []):
            if "uri" in image and not image["uri"].startswith("data:"):
                image_source = local_path(source.parent, image["uri"])
                image_target = local_path(stage, image["uri"])
                image_target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(image_source, image_target)
        row["painted_maps"] = paint_materials(doc, source.parent, stage, preset, config["texture_size"])
        row["painted_color_accessors"] = paint_vertex_colours(doc, stage, preset)
        staged = stage / "painted.gltf"
        write(staged, doc)
        command = [args.blender, "--background", "--factory-startup", "--threads", "6",
                   "--python-exit-code", "1", "--python", HELPER, "--", staged, target, "--asset", asset]
        if protected(original) or "retained" in reducer:
            command.append("--protected")
        elif reducer == "Blender decimate":
            command += ["--triangles", str(profile["triangles"])]
        run(command, output / "blender.log")
        result = validate(target, original)
        row.update({"status": "processed", "optimizer": reducer,
                    "triangles_out": primitive_triangles(result),
                    "materials": len(result.get("materials", [])),
                    "animations": len(result.get("animations", [])), "skins": len(result.get("skins", [])),
                    "gltf": str(target.relative_to(ROOT)).replace("\\", "/")})
        if uid in config["preview_uids"] and not args.no_previews:
            run([args.blender, "--background", "--factory-startup", "--threads", "6", "--python-exit-code", "1",
                 "--python", HELPER, "--", target, target, "--preview-source", source,
                 "--preview-dir", output / "preview"], output / "preview.log")
            row["preview"] = str((output / "preview").relative_to(ROOT)).replace("\\", "/")
        license_file = next(iter(folder.glob("*license*.txt")), None)
        license_name = "CC0-1.0" if model["license"] == "cc0" else "CC-BY (see license.txt)"
        if license_file:
            shutil.copy2(license_file, output / "license.txt")
            match = re.search(r"license type:\s*([^\s(]+)", license_file.read_text(encoding="utf8"))
            if match:
                license_name = match[1]
        row["seconds"] = round(time.time() - started, 2)
        write(output / "processing.json", row)
        write(meta, model | {"site": "Sketchfab", "asset_id": asset, "license": license_name,
                            "original_source": str(source.relative_to(ROOT)).replace("\\", "/"),
                            "gltf": target.name, "processing_fingerprint": fingerprint,
                            "processing_override": override,
                            "modifications": [reducer, "UV-preserving Blender cleanup and bottom-centre pivot",
                                              "painted albedo and vertex colors; warm/cool reference palette",
                                              "suppressed micro normals; broader roughness; textures <= 1024"],
                            "style": config["style"], "runtime_registered": False,
                            "dimensions": read(target.with_suffix(".blender.json"))["dimensions_metres"]})
        write(output / (asset + ".meta.json"), {"name": model["name"], "source": model["url"],
              "author": model["author"], "license": license_name, "category": model["cat"],
              "tags": ["painted", "fantasy_6080"], "modifications": read(meta)["modifications"]})
        # Scratch is generated by this job under one fixed workspace subtree.
        if stage.resolve().is_relative_to((ROOT / ".cache/sketchfab-pipeline/jobs").resolve()):
            shutil.rmtree(stage)
        return row
    except Exception as error:
        row.update({"status": "failed", "error": str(error), "seconds": round(time.time() - started, 2)})
        write(output / "processing.json", row)
        return row


def contact_sheet(dest, rows):
    selected = [r for r in rows if r.get("preview")]
    if not selected:
        return
    panels = []
    for row in selected:
        folder = ROOT / row["preview"]
        if not (folder / "before.png").exists() or not (folder / "after.png").exists():
            continue
        panel = Image.new("RGB", (640, 352), (20, 25, 34))
        for x, name in ((0, "before"), (320, "after")):
            image = Image.open(folder / (name + ".png")).convert("RGB")
            image.thumbnail((320, 320), Image.Resampling.LANCZOS)
            panel.paste(image, (x, 32))
        draw = ImageDraw.Draw(panel)
        label = row["name"].encode("ascii", "replace").decode()[:55]
        draw.text((8, 5), label, fill="white")
        draw.text((8, 19), f"SOURCE {row['triangles_in']} tris", fill=(190, 199, 215))
        draw.text((328, 19), f"PAINTED {row['triangles_out']} tris", fill=(234, 206, 156))
        panels.append(panel)
    if panels:
        sheet = Image.new("RGB", (1280, 352 * ((len(panels) + 1) // 2)), (20, 25, 34))
        for i, panel in enumerate(panels):
            sheet.paste(panel, ((i % 2) * 640, (i // 2) * 352))
        sheet.save(dest / "before_after.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blender", type=Path, default=Path(r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"))
    parser.add_argument("--optimizer", type=Path, default=ROOT / "cmake-build-msvc-relwithdebinfo/RelWithDebInfo/model_simplify.exe")
    parser.add_argument("--dest", type=Path, default=OUT)
    parser.add_argument("--only", nargs="*")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--no-previews", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if not args.blender.is_file():
        parser.error("Blender executable was not found")
    if not args.optimizer.is_file():
        args.optimizer = None
    args.dest = args.dest.resolve()
    if not args.dest.is_relative_to(ROOT):
        parser.error("Output must be inside this workspace")
    args.dest.mkdir(parents=True, exist_ok=True)
    config = read(CONFIG)
    presets = read(style.PRESETS)["presets"]
    digest = hashlib.sha256()
    for path in (CONFIG, style.PRESETS, ROOT / "content/config/style/reference.json", HELPER,
                 Path(__file__), ROOT / "tools/stylize_textures.py"):
        digest.update(path.read_bytes())
    fingerprint = digest.hexdigest()
    models = [m for m in read(ROOT / "doc/sketchfab_candidates.json") if (INPUT / m["uid"] / "source.json").exists()]
    if args.only:
        models = [m for m in models if m["uid"] in args.only]
    if args.limit is not None:
        models = models[:args.limit]
    existing_report = read(args.dest / "report.json") if (args.dest / "report.json").exists() else {"models": []}
    rows = {r["uid"]: r for r in existing_report["models"]}
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(process, m, args, config, presets, fingerprint): m for m in models}
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            row = future.result()
            rows[row["uid"]] = row
            print(f"[{i}/{len(models)}] {row['status']} {row['name']} "
                  f"{row.get('triangles_in', 0)}->{row.get('triangles_out', 0)} {row.get('error', '')}", flush=True)
            values = list(rows.values())
            summary = {s: sum(r["status"] == s for r in values)
                       for s in ("processed", "existing", "unsupported", "failed")}
            write(args.dest / "report.json", {"style": config["style"], "summary": summary, "models": values})
    contact_sheet(args.dest, list(rows.values()))
    print("Report: " + str(args.dest / "report.json"), flush=True)
    return int(any(r["status"] == "failed" for r in rows.values()))


if __name__ == "__main__":
    sys.exit(main())
