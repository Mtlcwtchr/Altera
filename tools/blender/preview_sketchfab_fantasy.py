"""A small review scene made from the actual converted Sketchfab meshes.

blender -b -P tools/blender/preview_sketchfab_fantasy.py -- --output IMAGE.png
Illustrates the asset treatment together with the project's lighting and
composition rules. Does not change the asset library or game placement.
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

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "assets/generated/sketchfab_fantasy_6080"


def material(name, color, roughness=0.9):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1)
    shader.inputs["Roughness"].default_value = roughness
    return mat


def import_asset(uid, height, position, yaw=0):
    asset = "sf_" + uid[:16]
    folder = LIBRARY / asset
    before = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=str(folder / (asset + ".gltf")))
    imported = set(bpy.context.scene.objects) - before
    bpy.context.view_layer.update()
    meshes = [o for o in imported if o.type == "MESH"]
    points = [o.matrix_world @ Vector(p) for o in meshes for p in o.bound_box]
    lo = Vector(tuple(min(p[k] for p in points) for k in range(3)))
    hi = Vector(tuple(max(p[k] for p in points) for k in range(3)))
    group = bpy.data.objects.new(asset + "_review", None)
    bpy.context.collection.objects.link(group)
    for obj in imported:
        if obj.parent not in imported:
            world = obj.matrix_world.copy()
            obj.parent = group
            obj.matrix_world = world
    group.scale = (height / max(hi.z - lo.z, 0.01),) * 3
    group.rotation_euler.z = math.radians(yaw)
    group.location = (*position, 0)
    return json.loads((folder / "source.json").read_text(encoding="utf8"))


def ridge(name, y, peaks, color):
    # Hand-shaped rising ridgelines, deliberately staged in separate planes.
    vertices = [(x, y - 8, 0) for x, z in peaks] + [(x, y, z) for x, z in peaks] + [(x, y + 10, 0) for x, z in peaks]
    n = len(peaks)
    faces = [(i, i + 1, n + i + 1, n + i) for i in range(n - 1)]
    faces += [(n + i, n + i + 1, 2 * n + i + 1, 2 * n + i) for i in range(n - 1)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material(name, color))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=LIBRARY / "fantasy_scene_preview.png")
    args = parser.parse_args(ab.arguments())
    scene = ab.reset()
    credits = []
    for uid, height, position, yaw in (
            ("f7e507f0b0654579accf5dbf05745f7d", 7.8, (5, 5), -15),
            ("4ec56df1c24d422ea85d1cfbf21bbe8c", 4.0, (-4, 11), 15),
            ("3cb4d59eb4844dc4802480e9ee53785e", 11.0, (-8, 2), -15),
            ("08a018e4e7b3486dbec7b0341be3aeb4", 5.5, (4, 28), 0),
            ("392342e94ee94c1b823a367a8ddcf9ee", 1.7, (-0.8, 4), 35)):
        credits.append(import_asset(uid, height, position, yaw))
    bpy.ops.mesh.primitive_plane_add(size=180, location=(0, 20, -0.04))
    ground = bpy.context.object
    ground.name = "quiet_ochre_olive_field"
    mat = material("broad_pigment_masses", (0.18, 0.20, 0.075))
    n, links = mat.node_tree.nodes, mat.node_tree.links
    noise = n.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 0.32
    noise.inputs["Detail"].default_value = 1
    coord = n.new("ShaderNodeTexCoord")
    links.new(coord.outputs["Object"], noise.inputs["Vector"])
    ramp = n.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.055, 0.075, 0.04, 1)
    ramp.color_ramp.elements[1].color = (0.28, 0.26, 0.11, 1)
    links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], n.get("Principled BSDF").inputs["Base Color"])
    ground.data.materials.append(mat)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=16, location=(-2, 21, -0.16))
    lake = bpy.context.object
    lake.name = "quiet_lake_negative_space"
    lake.scale = (8, 8, 0.18)
    lake.data.materials.append(material("cool_water_accent", (0.04, 0.12, 0.15), 0.22))
    ridge("far_lavender_ridge", 54, [(-50, 9), (-31, 16), (-16, 13), (-3, 23), (11, 15), (24, 20), (45, 8)], (0.24, 0.29, 0.37))
    ridge("middle_blue_ridge", 43, [(-40, 4), (-28, 11), (-19, 7), (-11, 14), (1, 9), (13, 15), (23, 8), (40, 4)], (0.13, 0.19, 0.24))
    world = bpy.data.worlds.new("cool_painted_sky")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.07, 0.13, 0.26, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.4
    scene.world = world
    for name, direction, energy, color in (
            ("late_gold_key", (-4, -3, 7), 2.0, (1.0, 0.87, 0.68)),
            ("violet_sky_fill", (3, 1, 5), 0.65, (0.43, 0.57, 1.0))):
        data = bpy.data.lights.new(name, "SUN")
        data.energy = energy
        data.color = color
        data.angle = math.radians(12)
        obj = bpy.data.objects.new(name, data)
        scene.collection.objects.link(obj)
        obj.rotation_euler = (-Vector(direction)).to_track_quat("-Z", "Y").to_euler()
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 30, 14))
    fog = bpy.context.object
    fog.name = "layered_cool_haze"
    fog.scale = (130, 110, 35)
    fog_material = bpy.data.materials.new("lavender_atmosphere")
    fog_material.use_nodes = True
    nodes, links = fog_material.node_tree.nodes, fog_material.node_tree.links
    nodes.clear()
    volume = nodes.new("ShaderNodeVolumeScatter")
    volume.inputs["Color"].default_value = (0.35, 0.50, 0.85, 1)
    volume.inputs["Density"].default_value = 0.004
    volume.inputs["Anisotropy"].default_value = 0.1
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(volume.outputs["Volume"], output.inputs["Volume"])
    fog.data.materials.append(fog_material)
    camera_data = bpy.data.cameras.new("review_camera")
    camera = bpy.data.objects.new("review_camera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = (16, -25, 10)
    target = Vector((0, 9, 4.7))
    camera.rotation_euler = (target - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera_data.lens = 42
    camera_data.clip_end = 300
    scene.camera = camera
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.exposure = 0.25
    scene.cycles.samples = 24
    scene.cycles.use_denoising = True
    scene.cycles.volume_bounces = 1
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 6
    scene.render.resolution_x = 1280
    scene.render.resolution_y = 800
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(args.output)
    (args.output.parent / "scene_preview_sources.json").write_text(
        json.dumps([{"name": m["name"], "author": m["author"], "license": m["license"], "url": m["url"]}
                    for m in credits], ensure_ascii=False, indent=2), encoding="utf8")
    bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    main()
