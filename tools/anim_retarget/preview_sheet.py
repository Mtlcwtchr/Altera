"""Contact sheet of clips on the UAL mannequin (Workbench, front 3/4 view).
blender -b --factory-startup -P preview_sheet.py -- <UAL1.glb> <out_dir> <cols> <glb> [<glb> ...]
Writes <out_dir>/<clip>.png: one row of <cols> frames spread over the clip."""
import bpy, sys, os, math
from mathutils import Vector
a = sys.argv[sys.argv.index("--") + 1:]
ual, out, cols, files = a[0], os.path.abspath(a[1]), int(a[2]), a[3:]
os.makedirs(out, exist_ok=True)
for p in files:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = 30
    bpy.ops.import_scene.gltf(filepath=ual)
    rig = next(o for o in bpy.data.objects if o.type == "ARMATURE")
    keep = set(bpy.data.objects)
    for x in list(bpy.data.actions):
        bpy.data.actions.remove(x)
    bpy.ops.import_scene.gltf(filepath=p)
    clip = bpy.data.actions[0]
    for o in [o for o in bpy.data.objects if o not in keep]:
        bpy.data.objects.remove(o)
    rig.animation_data_create(); rig.animation_data.action = clip
    if not rig.animation_data.action_slot and clip.slots:
        rig.animation_data.action_slot = clip.slots[0]
    f0, f1 = (int(round(v)) for v in clip.frame_range)
    bpy.ops.mesh.primitive_plane_add(size=6)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.lens = 35
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"; sc.display.shading.color_type = "OBJECT"
    sc.render.resolution_x, sc.render.resolution_y = 360, 480
    tiles = []
    for i in range(cols):
        f = f0 + round(i * (f1 - f0) / max(1, cols - 1))
        sc.frame_set(f)
        c = rig.matrix_world @ rig.pose.bones["pelvis"].head
        cam.location = c + Vector((2.2, -3.6, 0.5)); cam.location.z = 1.2
        d = (Vector((c.x, c.y, 0.9)) - cam.location); cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
        fp = os.path.join(out, f"_tile{i}.png"); sc.render.filepath = fp
        bpy.ops.render.render(write_still=True); tiles.append(fp)
    imgs = [bpy.data.images.load(t) for t in tiles]
    W, H = imgs[0].size
    sheet = bpy.data.images.new("sheet", W * cols, H)
    px = [0.0] * (W * cols * H * 4)
    for i, im in enumerate(imgs):
        src = list(im.pixels)
        for y in range(H):
            px[(y * W * cols + i * W) * 4:(y * W * cols + i * W + W) * 4] = src[y * W * 4:(y + 1) * W * 4]
    sheet.pixels = px
    sheet.filepath_raw = os.path.join(out, os.path.splitext(os.path.basename(p))[0] + ".png"); sheet.file_format = "PNG"; sheet.save()
    for t in tiles: os.remove(t)
