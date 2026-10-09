"""Blender (background) side of tools/terrain_lab/build_foliage.py.

    blender --background --factory-startup --python tools/blender/export_foliage.py -- JOBS.json

JOBS.json: {"out": DIR, "thumbs": DIR or null, "jobs": [{"name", "source", "texture"}...]}.
For each job: import the model (glb/gltf/fbx/obj), join it into one mesh with
its transforms applied (Z up, metres), triangulate, and write DIR/<name>.mesh:

    char[4] "FOLM", u32 version (1), u32 vertices, u32 indices,
    f32[3] bounds min, f32[3] bounds max,
    vertices: f32 position[3], f32 normal[3], f32 uv[2]   (32 bytes each)
    indices:  u32

and copy its texture to DIR/<name>.png. With "thumbs", also renders a small
picture of each model there, for choosing what goes where.
"""
import json
import math
import shutil
import struct
import sys
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector


def clear():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    for block in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
        for item in list(block):
            block.remove(item)


def import_model(path):
    suffix = Path(path).suffix.lower()
    if suffix in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif suffix == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif suffix == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise RuntimeError("cannot import " + path)


def joined_mesh():
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        raise RuntimeError("no mesh")
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    # Parents (FBX armature-less roots, glTF nodes) carry rotation and scale.
    bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    return obj


def write_mesh(obj, path):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = obj.evaluated_get(depsgraph).to_mesh()
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(mesh)
    bm.free()
    mesh.calc_loop_triangles()
    uv_layer = mesh.uv_layers.active.data if mesh.uv_layers.active else None
    try:
        normals = [n.vector for n in mesh.corner_normals]
    except AttributeError:
        mesh.calc_normals_split()
        normals = [l.normal for l in mesh.loops]

    # Origin at the base of the model, centred on its footprint: placed on the
    # ground at its origin, a tree stands on its trunk.
    xs = [v.co.x for v in mesh.vertices]
    ys = [v.co.y for v in mesh.vertices]
    zs = [v.co.z for v in mesh.vertices]
    offset = Vector(((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, min(zs)))

    vertices, indices, seen = [], [], {}
    for tri in mesh.loop_triangles:
        for loop in tri.loops:
            p = mesh.vertices[mesh.loops[loop].vertex_index].co - offset
            n = normals[loop]
            uv = uv_layer[loop].uv if uv_layer else (0.0, 0.0)
            key = (round(p.x, 5), round(p.y, 5), round(p.z, 5), round(n.x, 3), round(n.y, 3), round(n.z, 3),
                   round(uv[0], 5), round(uv[1], 5))
            index = seen.get(key)
            if index is None:
                index = len(vertices)
                seen[key] = index
                vertices.append((p.x, p.y, p.z, n.x, n.y, n.z, uv[0], 1.0 - uv[1]))
            indices.append(index)
    lo = [min(v[i] for v in vertices) for i in range(3)]
    hi = [max(v[i] for v in vertices) for i in range(3)]
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<III", 1, len(vertices), len(indices)))
        f.write(struct.pack("<6f", *lo, *hi))
        for v in vertices:
            f.write(struct.pack("<8f", *v))
        f.write(struct.pack("<%dI" % len(indices), *indices))
    obj.evaluated_get(depsgraph).to_mesh_clear()
    return len(vertices), len(indices) // 3, lo, hi


def embedded_texture(out_png):
    for image in bpy.data.images:
        if image.type == "IMAGE" and (image.packed_file or Path(bpy.path.abspath(image.filepath)).exists()):
            image.filepath_raw = str(out_png)
            image.file_format = "PNG"
            image.save()
            return True
    return False


def thumbnail(obj, lo, hi, path):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "TEXTURE"
    scene.render.resolution_x = scene.render.resolution_y = 192
    scene.render.film_transparent = True
    size = max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2])
    centre = Vector(((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2))
    cam_data = bpy.data.cameras.new("thumb")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = size * 1.2
    cam = bpy.data.objects.new("thumb", cam_data)
    scene.collection.objects.link(cam)
    cam.location = obj.matrix_world @ centre + Vector((size * 2, -size * 2, size * 1.2))
    cam.rotation_euler = (math.radians(65), 0, math.radians(45))
    scene.camera = cam
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def main():
    jobs = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
    out = Path(jobs["out"])
    out.mkdir(parents=True, exist_ok=True)
    thumbs = Path(jobs["thumbs"]) if jobs.get("thumbs") else None
    if thumbs:
        thumbs.mkdir(parents=True, exist_ok=True)
    report = {}
    for job in jobs["jobs"]:
        clear()
        try:
            import_model(job["source"])
            obj = joined_mesh()
            vertices, triangles, lo, hi = write_mesh(obj, out / (job["name"] + ".mesh"))
            if job.get("texture"):
                shutil.copyfile(job["texture"], out / (job["name"] + ".png"))
            elif not embedded_texture(out / (job["name"] + ".png")):
                print("no texture for", job["name"])
            if thumbs:
                thumbnail(obj, lo, hi, thumbs / (job["name"] + ".png"))
            report[job["name"]] = {"vertices": vertices, "triangles": triangles, "min": lo, "max": hi}
            print("exported %-14s %6d tris  %.1f x %.1f x %.1f m" % (job["name"], triangles, hi[0] - lo[0],
                                                                    hi[1] - lo[1], hi[2] - lo[2]))
        except Exception as error:   # one bad model must not stop the rest
            print("FAILED", job["name"], error)
    (out / "report.json").write_text(json.dumps(report, indent=1))


main()
