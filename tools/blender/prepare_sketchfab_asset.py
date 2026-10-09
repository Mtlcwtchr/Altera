"""Preserve imported materials, UVs and rigs while preparing painted Sketchfab assets.

Invoked by tools/prepare_sketchfab.py. No UV reprojection or material bake:
the original texture coordinates must keep sampling the source paintings.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asr_blender as ab


def bounds(objects):
    points = [o.matrix_world @ Vector(c) for o in objects for c in o.bound_box]
    lo = Vector(tuple(min(p[k] for p in points) for k in range(3)))
    hi = Vector(tuple(max(p[k] for p in points) for k in range(3)))
    return lo, hi


def triangles(objects):
    total = 0
    for mesh in {o.data for o in objects}:
        mesh.calc_loop_triangles()
        total += len(mesh.loop_triangles)
    return total


def prepare(args):
    ab.reset()
    bpy.ops.import_scene.gltf(filepath=str(args.source))
    objects = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not objects:
        raise RuntimeError("No surface mesh")
    before = triangles(objects)
    protected = args.protected or any(o.data.shape_keys or o.find_armature() for o in objects)
    if args.triangles and before > args.triangles and not protected:
        # A single ratio across unique meshes keeps relative detail in asset packs.
        seen = set()
        for obj in objects:
            if obj.data in seen:
                continue
            seen.add(obj.data)
            ab.select_only(obj)
            obj.data.calc_loop_triangles()
            count = len(obj.data.loop_triangles)
            target = max(12, int(count * args.triangles / before))
            ab.reduce(obj, target)
    after = triangles(objects)
    # Parent only the top-level imported objects. Bone and animation relationships
    # remain intact; normalize the pivot through one common static parent.
    lo, hi = bounds(objects)
    root = bpy.data.objects.new(args.asset + "_root", None)
    bpy.context.collection.objects.link(root)
    roots = [o for o in bpy.context.scene.objects if o != root and o.parent is None]
    for obj in roots:
        world = obj.matrix_world.copy()
        obj.parent = root
        obj.matrix_world = world
    root.location = (-0.5 * (lo.x + hi.x), -0.5 * (lo.y + hi.y), -lo.z)
    bpy.context.view_layer.update()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(args.output), export_format="GLTF_SEPARATE",
        export_texture_dir="textures", export_yup=True,
        export_normals=True, export_materials="EXPORT", export_animations=True,
        export_skins=True, export_morph=True, export_extras=True,
        export_cameras=False, export_lights=False)
    report = {"triangles_in": before, "triangles_out": after,
              "geometry_protected": protected,
              "dimensions_metres": [round(float(x), 4) for x in hi - lo],
              "scale": "source units preserved; metric scale requires placement review",
              "pivot": "bottom centre via static root, glTF Y-up"}
    args.output.with_suffix(".blender.json").write_text(
        json.dumps(report, indent=2), encoding="utf8")


def render(source, output, side):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    scene = bpy.context.scene
    scene.frame_set(1)
    objects = [o for o in scene.objects if o.type == "MESH"]
    lo, hi = bounds(objects)
    centre = (lo + hi) * 0.5
    radius = max((hi - lo).length * 0.5, 0.01)
    # Identical neutral studio setup on both sides: the material conversion is
    # visible without lighting changes disguising the difference.
    for obj in list(scene.objects):
        if obj.type in {"LIGHT", "CAMERA"}:
            bpy.data.objects.remove(obj, do_unlink=True)
    camera_data = bpy.data.cameras.new("review_camera")
    camera = bpy.data.objects.new("review_camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = centre + Vector((1.25, -1.7, 1.0)).normalized() * radius * 3
    camera.rotation_euler = (centre - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = radius * 2.3
    camera_data.clip_start = max(radius * 0.0001, 0.00001)
    camera_data.clip_end = radius * 20
    scene.camera = camera
    world = bpy.data.worlds.new("review_world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.12, 0.15, 0.23, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.55
    scene.world = world
    for name, direction, energy, color in (
            ("key", (-3, -4, 6), 3.0, (1.0, 0.88, 0.68)),
            ("fill", (4, 1, 3), 1.0, (0.55, 0.65, 1.0))):
        light_data = bpy.data.lights.new(name, "SUN")
        light_data.energy = energy
        light_data.color = color
        light_data.angle = math.radians(20)
        light = bpy.data.objects.new(name, light_data)
        scene.collection.objects.link(light)
        light.rotation_euler = (-Vector(direction)).to_track_quat("-Z", "Y").to_euler()
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 12
    scene.cycles.use_denoising = True
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 6
    scene.render.resolution_x = side
    scene.render.resolution_y = side
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.view_settings.view_transform = "AgX"
    scene.render.filepath = str(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.render.render(write_still=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--asset", default="sketchfab_asset")
    parser.add_argument("--triangles", type=int, default=0)
    parser.add_argument("--protected", action="store_true")
    parser.add_argument("--preview-source", type=Path)
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--side", type=int, default=480)
    args = parser.parse_args(ab.arguments())
    if args.preview_dir:
        render(args.preview_source, args.preview_dir / "before.png", args.side)
        render(args.source, args.preview_dir / "after.png", args.side)
    else:
        prepare(args)


if __name__ == "__main__":
    main()
