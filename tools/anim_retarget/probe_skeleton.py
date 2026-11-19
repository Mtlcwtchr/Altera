"""Print armature bones (name, parent, world head/tail, rest dir) of an FBX/GLB/BVH.
blender -b --factory-startup -P probe_skeleton.py -- <file> [<file> ...]"""
import bpy, sys, os
from mathutils import Vector
files = sys.argv[sys.argv.index("--") + 1:]
for path in files:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    ext = os.path.splitext(path)[1].lower()
    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=False)
    elif ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".bvh":
        bpy.ops.import_anim.bvh(filepath=path)
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    print("=== FILE", os.path.basename(path), "armatures", [a.name for a in arms],
          "actions", len(bpy.data.actions), "meshes", sum(o.type == 'MESH' for o in bpy.data.objects))
    print("    actions:", [a.name for a in bpy.data.actions][:200])
    for a in arms[:1]:
        print("  obj matrix scale", tuple(round(v, 4) for v in a.matrix_world.to_scale()), "rot", tuple(round(v, 3) for v in a.matrix_world.to_euler()))
        for b in a.data.bones:
            h = a.matrix_world @ b.head_local; t = a.matrix_world @ b.tail_local
            d = (t - h).normalized()
            print(f"  {b.name:40s} p={b.parent.name if b.parent else '-':30s} h=({h.x:.3f},{h.y:.3f},{h.z:.3f}) d=({d.x:.2f},{d.y:.2f},{d.z:.2f})")
        if a.animation_data and a.animation_data.action:
            print("  action range", tuple(a.animation_data.action.frame_range))
