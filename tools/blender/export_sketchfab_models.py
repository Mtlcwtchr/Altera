"""Re-export picks from the processed Sketchfab library for the scene catalogue.

The library (assets/generated/sketchfab_fantasy_6080, tools/prepare_sketchfab.py)
keeps each download as it came: arbitrary units, parent transforms, whole packs
in one file, display bases. The catalogue (tools/prepare_environment_models.py
via bake_model_impostors) wants what tools/simplify_models.py writes: one mesh
node per variant with its transform applied, metres, Y-up, under
assets/generated/simplified_models/<asset>/. This is that adapter, driven by
content/config/sketchfab_models.json:

    blender -b --factory-startup --python tools/blender/export_sketchfab_models.py -- [--only ASSET ...]

Per pick: import the pack, keep the named objects, apply every transform, join
them, scale to the configured real height, put the base centre at the origin,
decimate to the triangle budget and export with the source and licence records.
"""
import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "assets/generated/sketchfab_fantasy_6080"
OUT = ROOT / "assets/generated/simplified_models"
CONFIG = ROOT / "content/config/sketchfab_models.json"


def arguments():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for block in (bpy.data.meshes, bpy.data.materials, bpy.data.images, bpy.data.cameras, bpy.data.lights):
        for item in list(block):
            if item.users == 0:
                block.remove(item)


def bounds(objects):
    points = [o.matrix_world @ Vector(c) for o in objects for c in o.bound_box]
    low = Vector([min(p[k] for p in points) for k in range(3)])
    high = Vector([max(p[k] for p in points) for k in range(3)])
    return low, high


def triangles(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def select(objects, active=None):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active or objects[0]


def prepare(pick):
    clear()
    source_dir = LIBRARY / pick["sf"]
    bpy.ops.import_scene.gltf(filepath=str(source_dir / (pick["sf"] + ".gltf")))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    wanted = pick["objects"]
    keep = meshes if wanted == ["*"] else [o for o in meshes if o.name in wanted]
    missing = [] if wanted == ["*"] else sorted(set(wanted) - {o.name for o in keep})
    if missing or not keep:
        raise RuntimeError("%s: objects not in %s: %s" % (pick["asset"], pick["sf"], missing or wanted))

    # Every parent transform into the meshes, then nothing but the meshes.
    select(keep)
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    for o in list(bpy.context.scene.objects):
        if o not in keep:
            bpy.data.objects.remove(o, do_unlink=True)
    select(keep)
    bpy.ops.object.make_single_user(object=True, obdata=True)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(keep) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = pick["asset"]

    # Real height in metres, base centre at the origin.
    low, high = bounds([obj])
    height = high.z - low.z
    if height <= 0:
        raise RuntimeError(pick["asset"] + ": flat model")
    scale = pick["height_m"] / height
    centre = Vector(((low.x + high.x) / 2, (low.y + high.y) / 2, low.z))
    obj.data.transform(__import__("mathutils").Matrix.Translation(-centre))
    obj.scale = (scale, scale, scale)
    select([obj])
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    # The bake reads TEXCOORD_0 on every primitive, textured or not.
    if not obj.data.uv_layers:
        obj.data.uv_layers.new(name="UVMap")

    before = triangles(obj)
    if before > pick["budget"]:
        modifier = obj.modifiers.new("budget", "DECIMATE")
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = pick["budget"] / before
        modifier.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    after = triangles(obj)

    folder = OUT / pick["asset"]
    folder.mkdir(parents=True, exist_ok=True)
    for stale in folder.glob("*"):
        if stale.is_file():
            stale.unlink()
    select([obj])
    bpy.ops.export_scene.gltf(filepath=str(folder / (pick["asset"] + ".gltf")), export_format="GLTF_SEPARATE",
                              use_selection=True, export_texture_dir="textures", export_yup=True,
                              export_apply=True, export_normals=True, export_materials="EXPORT")

    # Foliage cards arrive as BLEND; the catalogue draws leaves as cutouts.
    gltf = folder / (pick["asset"] + ".gltf")
    document = json.loads(gltf.read_text(encoding="utf-8"))
    for material in document.get("materials", []):
        if material.get("alphaMode") == "BLEND":
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = 0.5
    gltf.write_text(json.dumps(document, indent=2), encoding="utf-8")

    info = json.loads((source_dir / "source.json").read_text(encoding="utf-8"))
    dims = [round(float(d), 3) for d in obj.dimensions]
    record = {"site": "sketchfab", "asset_id": pick["asset"], "name": pick["name"], "license": info["license"],
              "authors": [info["author"]], "asset_url": info["url"], "role": pick["kind"],
              "source": "tools/blender/export_sketchfab_models.py from " + pick["sf"] + " (" + info["name"] + ")",
              "objects": wanted, "variants": 1, "dimensions_metres": dims,
              "triangles": {"source": before, "exported": after}}
    (folder / "source.json").write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    meta = {"name": pick["asset"], "source": info["url"], "author": info["author"], "license": info["license"],
            "modifications": ["painted restyle (tools/prepare_sketchfab.py)", "parts selected, transforms applied",
                              "scaled to %.2f m" % pick["height_m"], "decimated to %d triangles" % after],
            "category": pick["kind"], "tags": ["sketchfab", "fantasy_6080"]}
    (folder / (pick["asset"] + ".meta.json")).write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                                                         encoding="utf-8")
    print("EXPORTED %-20s %-24s %6d -> %6d tris  %s m" % (pick["asset"], pick["name"], before, after, dims))


def main():
    args = arguments()
    only = set(args[args.index("--only") + 1:]) if "--only" in args else None
    picks = json.loads(CONFIG.read_text(encoding="utf-8"))["models"]
    failed = 0
    for pick in picks:
        if only and pick["asset"] not in only:
            continue
        try:
            prepare(pick)
        except Exception as error:   # one bad pick must not stop the rest
            failed += 1
            print("FAILED %s: %s" % (pick["asset"], error))
    print("done, %d failed" % failed)


main()
