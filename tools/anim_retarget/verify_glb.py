"""Re-import exported clips and check them against the UAL rig.
blender -b --factory-startup -P verify_glb.py -- <ual_template.blend> <out.json> <glb> [<glb> ...]
Per clip: action present, bone names all in UAL, frame count, ankle min height and
pelvis height range (world, after a glTF round trip)."""
import bpy, sys, json, os
args = sys.argv[sys.argv.index("--") + 1:]
tmpl, out, files = args[0], args[1], args[2:]
bpy.ops.wm.open_mainfile(filepath=tmpl)
ual = {b.name for b in bpy.data.objects["UAL"].data.bones}
res = []
for p in files:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = 30
    try:
        bpy.ops.import_scene.gltf(filepath=p)
        arm = next(o for o in bpy.data.objects if o.type == "ARMATURE")
        names = {b.name for b in arm.data.bones}
        act = arm.animation_data.action if arm.animation_data else None
        f0, f1 = (int(round(v)) for v in act.frame_range)
        sc = bpy.context.scene
        ank, pel = 1e9, []
        for f in range(f0, f1 + 1, max(1, (f1 - f0) // 40 or 1)):
            sc.frame_set(f)
            for n in ("foot_l", "foot_r"):
                ank = min(ank, (arm.matrix_world @ arm.pose.bones[n].head).z)
            pel.append((arm.matrix_world @ arm.pose.bones["pelvis"].head).z)
        res.append({"file": os.path.basename(p), "ok": True, "actions": len(bpy.data.actions), "action": act.name,
                    "frames": f1 - f0 + 1, "extra_bones": sorted(names - ual), "missing_bones": sorted(ual - names),
                    "ankle_min_z": round(ank, 3), "pelvis_z": [round(min(pel), 3), round(max(pel), 3)]})
    except Exception as e:
        res.append({"file": os.path.basename(p), "ok": False, "error": str(e)})
json.dump(res, open(out, "w"), indent=1)
