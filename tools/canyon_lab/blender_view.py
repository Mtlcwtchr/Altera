"""The canyon sandbox's scene with its meshes, rendered by Blender without a
window (stage 3: the meshes on the ground, seen in perspective).

    blender -b --factory-startup -P tools/canyon_lab/blender_view.py -- <run dir> [--step 2]

Reads the run's layers.npz (heights), albedo.png (ground colours),
instances.json (place.py) and the views of canyon.json; writes
view3d_<name>.png into the run dir. The ground is a grid every `step` cells.
"""

import json
import math
import struct
import sys
from pathlib import Path

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROPS = ROOT / "assets" / "terrain_core" / "props"

argv = sys.argv[sys.argv.index("--") + 1:]
RUN = Path(argv[0]).resolve()
STEP = int(argv[argv.index("--step") + 1]) if "--step" in argv else 2
ONLY = argv[argv.index("--only") + 1].split(",") if "--only" in argv else None
GAME = "--game" in argv          # the ground as the game holds it (fine_relief.py)
SUFFIX = "_game" if GAME else ""
WALL_TEX = ROOT / "assets" / "terrain_core" / "materials" / "rock_orange.png"


def srgb_to_lin(c):
    c = np.asarray(c, np.float32)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def ground(h, alb, cell):
    hs = h[::STEP, ::STEP]
    n = hs.shape[0]
    c = cell * STEP
    yy, xx = np.mgrid[0:n, 0:n]
    co = np.stack([xx * c, -yy * c, hs], -1).reshape(-1, 3).astype(np.float32)
    me = bpy.data.meshes.new("ground")
    me.vertices.add(n * n)
    me.vertices.foreach_set("co", co.ravel())
    i = (yy[:-1, :-1] * n + xx[:-1, :-1]).ravel()
    quads = np.stack([i, i + n, i + n + 1, i + 1], 1)
    me.loops.add(quads.size)
    me.loops.foreach_set("vertex_index", quads.ravel().astype(np.int32))
    me.polygons.add(len(quads))
    me.polygons.foreach_set("loop_start", (np.arange(len(quads)) * 4).astype(np.int32))
    me.polygons.foreach_set("loop_total", np.full(len(quads), 4, np.int32))
    me.update()
    me.polygons.foreach_set("use_smooth", np.zeros(len(quads), bool))
    a = alb[::STEP, ::STEP].reshape(-1, 3) / 255.0
    col = np.concatenate([srgb_to_lin(a), np.ones((n * n, 1), np.float32)], 1)
    attr = me.color_attributes.new("albedo", "FLOAT_COLOR", "POINT")
    attr.data.foreach_set("color", col.ravel())
    ob = bpy.data.objects.new("ground", me)
    bpy.context.scene.collection.objects.link(ob)
    mat = bpy.data.materials.new("ground")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")
    ca = nt.nodes.new("ShaderNodeVertexColor")
    ca.layer_name = "albedo"
    # Steep ground wears the rock projected from three sides (triplanar,
    # world metres), bedding level on every face; the flat keeps its colour.
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (1 / 14.0, 1 / 14.0, 1 / 11.0)
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(str(WALL_TEX))
    tex.projection = "BOX"
    tex.projection_blend = 0.25
    tex.interpolation = "Closest"
    nt.links.new(mp.outputs["Vector"], tex.inputs["Vector"])
    tint = nt.nodes.new("ShaderNodeMix"); tint.data_type = "RGBA"; tint.blend_type = "MULTIPLY"
    tint.inputs["Factor"].default_value = 1.0
    tint.inputs["B"].default_value = (0.62, 0.36, 0.30, 1)
    nt.links.new(tex.outputs["Color"], tint.inputs["A"])
    # Beds of different shades by height, and a slow noise, so the tiles of
    # the texture do not read as brickwork.
    sp = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(tc.outputs["Object"], sp.inputs[0])
    wv = nt.nodes.new("ShaderNodeTexWave")
    wv.wave_type = "BANDS"; wv.bands_direction = "Z"
    wv.inputs["Scale"].default_value = 0.004
    wv.inputs["Distortion"].default_value = 6.0
    wv.inputs["Detail"].default_value = 3.0
    nt.links.new(tc.outputs["Object"], wv.inputs["Vector"])
    ns = nt.nodes.new("ShaderNodeTexNoise")
    ns.inputs["Scale"].default_value = 0.02
    nt.links.new(tc.outputs["Object"], ns.inputs["Vector"])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.55, 0.42, 0.40, 1)
    ramp.color_ramp.elements[1].color = (1.15, 1.0, 0.92, 1)
    add = nt.nodes.new("ShaderNodeMath"); add.operation = "ADD"
    mul = nt.nodes.new("ShaderNodeMath"); mul.operation = "MULTIPLY"; mul.inputs[1].default_value = 0.5
    nt.links.new(wv.outputs["Fac"], add.inputs[0])
    nt.links.new(ns.outputs["Fac"], add.inputs[1])
    nt.links.new(add.outputs[0], mul.inputs[0])
    nt.links.new(mul.outputs[0], ramp.inputs[0])
    shade = nt.nodes.new("ShaderNodeMix"); shade.data_type = "RGBA"; shade.blend_type = "MULTIPLY"
    shade.inputs["Factor"].default_value = 1.0
    nt.links.new(tint.outputs["Result"], shade.inputs["A"])
    nt.links.new(ramp.outputs[0], shade.inputs["B"])
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sz = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Normal"], sz.inputs[0])
    st = nt.nodes.new("ShaderNodeMapRange")
    st.inputs["From Min"].default_value = 0.75
    st.inputs["From Max"].default_value = 0.55
    nt.links.new(sz.outputs[2], st.inputs["Value"])
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"
    nt.links.new(st.outputs[0], mix.inputs["Factor"])
    nt.links.new(ca.outputs["Color"], mix.inputs["A"])
    nt.links.new(shade.outputs["Result"], mix.inputs["B"])
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 1.0
    me.materials.append(mat)


def rock_material():
    mat = bpy.data.materials.new("rock")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Roughness"].default_value = 1.0
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Position"], sep.inputs[0])
    # Bedded rock: bands by world height, as the ground's walls.
    m1 = nt.nodes.new("ShaderNodeMath"); m1.operation = "SINE"
    sc = nt.nodes.new("ShaderNodeMath"); sc.operation = "MULTIPLY"; sc.inputs[1].default_value = 1 / 6.5
    nt.links.new(sep.outputs[2], sc.inputs[0])
    nt.links.new(sc.outputs[0], m1.inputs[0])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[0].color = (*srgb_to_lin(np.array([100, 36, 24]) / 255), 1)
    ramp.color_ramp.elements[1].color = (*srgb_to_lin(np.array([140, 58, 36]) / 255), 1)
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = -1
    nt.links.new(m1.outputs[0], mr.inputs["Value"])
    nt.links.new(mr.outputs[0], ramp.inputs[0])
    # Cavity (the mesh's uv.x) darkens the flutes.
    uv = nt.nodes.new("ShaderNodeUVMap")
    sx = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(uv.outputs[0], sx.inputs[0])
    mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"; mix.blend_type = "MULTIPLY"
    mix.inputs["Factor"].default_value = 0.6
    nt.links.new(ramp.outputs[0], mix.inputs["A"])
    cv = nt.nodes.new("ShaderNodeCombineColor")
    for k in ("Red", "Green", "Blue"):
        nt.links.new(sx.outputs[0], cv.inputs[k])
    nt.links.new(cv.outputs[0], mix.inputs["B"])
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    return mat


def load_mesh(name, mat):
    b = (PROPS / f"{name}_lod0.mesh").read_bytes()
    ver, nv, ni = struct.unpack("<III", b[4:16])
    stride = 8 if ver == 1 else 9
    v = np.frombuffer(b, "<f4", count=nv * stride, offset=40).reshape(nv, stride)
    idx = np.frombuffer(b, "<u4", count=ni, offset=40 + nv * stride * 4)
    me = bpy.data.meshes.new(name)
    me.vertices.add(nv)
    me.vertices.foreach_set("co", v[:, :3].ravel())
    nt = ni // 3
    me.loops.add(ni)
    me.loops.foreach_set("vertex_index", idx.astype(np.int32))
    me.polygons.add(nt)
    me.polygons.foreach_set("loop_start", (np.arange(nt) * 3).astype(np.int32))
    me.polygons.foreach_set("loop_total", np.full(nt, 3, np.int32))
    me.update()
    uvl = me.uv_layers.new(name="uv")
    uvl.data.foreach_set("uv", v[idx, 6:8].ravel())
    me.materials.append(mat)
    return me


def main():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    sc = bpy.context.scene
    L = np.load(RUN / "layers.npz")
    h = np.load(RUN / "h_game.npy") if GAME else L["h"].astype(np.float32)
    n = h.shape[0]
    img = bpy.data.images.load(str(RUN / "albedo.png"))
    px = np.empty(img.size[0] * img.size[1] * 4, np.float32)
    img.pixels.foreach_get(px)
    alb = px.reshape(img.size[1], img.size[0], 4)[::-1, :, :3] * 255.0     # rows from the top
    inst = json.loads((RUN / "instances.json").read_text(encoding="utf-8"))
    cell = inst["cell_m"]
    ground(h, alb, cell)
    rock = rock_material()
    meshes = {}
    coll = sc.collection
    for it in inst["instances"]:
        me = meshes.get(it["name"]) or meshes.setdefault(it["name"], load_mesh(it["name"], rock))
        ob = bpy.data.objects.new(it["name"], me)
        ob.location = (it["x"], -it["y"], it["z"])
        ob.rotation_euler = (0, 0, math.radians(it["yaw"]))
        ob.scale = (it["s"] * it["k"], it["s"] * it["k"], it["s"])
        coll.objects.link(ob)

    P = json.loads((HERE / "canyon.json").read_text(encoding="utf-8"))
    R = P["render"]
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 4.5
    sun.color = (1.0, 0.9, 0.78)
    sun.angle = math.radians(1.5)
    so = bpy.data.objects.new("sun", sun)
    # Sun towards az (clockwise from north = +y), el over the horizon.
    so.rotation_euler = (math.radians(90 - R["sun_el"]), 0, math.radians(180 - R["sun_az"]))
    coll.objects.link(so)
    world = bpy.data.worlds.new("w")
    sc.world = world
    world.use_nodes = True
    bg = next(nd for nd in world.node_tree.nodes if nd.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (*srgb_to_lin(np.array([150, 170, 196]) / 255), 1)
    bg.inputs["Strength"].default_value = 0.9
    engines = [i.identifier for i in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
    sc.render.engine = next(e for e in engines if "EEVEE" in e)
    try:
        sc.eevee.taa_render_samples = 16
    except AttributeError:
        pass
    sc.render.resolution_x, sc.render.resolution_y = 1280, 720
    sc.view_settings.view_transform = "Standard"
    span = n * cell
    views = list(R["views"])
    # Plus one close to the tallest column.
    cols = sorted([i for i in inst["instances"] if i["role"] == "column"], key=lambda i: -(i["top"] - i["z"]))
    # (the first tall one with open floor between it and the camera)
    for c in cols:
        d = 5 * c["foot_r"] + 100
        for a in range(0, 360, 45):
            ra = math.radians(a)
            x, y = c["x"] - d * math.sin(ra), c["y"] + d * math.cos(ra)
            if not (0 < x < n * cell and 0 < y < n * cell):
                continue
            t = np.linspace(0.15, 0.85, 12)
            line = h[np.clip(((y + (c["y"] - y) * t) / cell).astype(int), 0, n - 1),
                     np.clip(((x + (c["x"] - x) * t) / cell).astype(int), 0, n - 1)]
            g = h[int(y / cell), int(x / cell)]
            if np.ptp(line) < 8 and abs(g - c["z"]) < 8:
                views.append(dict(name="column", x=x, y=y, z=10, yaw=a, pitch=-10))
                break
        else:
            continue
        break
    for v in views:
        if ONLY and v["name"] not in ONLY:
            continue
        x = v.get("x", v.get("u", 0) * span)
        y = v.get("y", v.get("v", 0) * span)
        gz = float(h[int(np.clip(y / cell, 0, n - 1)), int(np.clip(x / cell, 0, n - 1))])
        cam = bpy.data.cameras.new("cam")
        cam.sensor_fit = "HORIZONTAL"
        cam.angle = math.radians(62)
        cam.clip_end = 20000
        co = bpy.data.objects.new("cam", cam)
        co.location = (x, -y, gz + v["z"])
        co.rotation_euler = (math.radians(90 - v["pitch"]), 0, math.radians(-v["yaw"]))
        coll.objects.link(co)
        sc.camera = co
        sc.render.filepath = str(RUN / f"view3d_{v['name']}{SUFFIX}.png")
        bpy.ops.render.render(write_still=True)


main()
