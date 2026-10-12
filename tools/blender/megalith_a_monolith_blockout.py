"""Magic megalith A-1, blockout stage: the giant rectangular monolith slabs.

Reference (user, 12.10): huge flat slabs standing in rows on a green plain, slightly
widening at the bottom where the hem runs into the grass. This stage settles only the
shape: proportions, silhouette and foot. No lattice, no sculpt, no material yet
(doc/BIOME_PLAN.md, kind A: blockout -> sculpt -> material, user approval at each step).

Three proportion variants stand side by side with 1.8 m human figures for scale.
PSX profile: low poly, flat shading, no smoothing.

    blender -b -P tools/blender/megalith_a_monolith_blockout.py
    blender -b -P tools/blender/megalith_a_monolith_blockout.py -- --no-render
"""
import argparse
import math
import sys
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
BLEND = ROOT / "assets/terrain_core/megaliths/a_monolith/monolith_blockout.blend"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"

STONE = (0.60, 0.61, 0.56, 1)
GROUND = (0.46, 0.62, 0.30, 1)
HUMAN = (0.95, 0.10, 0.05, 1)

# name, x, y, depth D, width W, height H, top taper, lean (deg, around X)
VARIANTS = [
    ("A_stele_1_4_9", -125.0, -70.0, 12.0, 48.0, 108.0, 0.06, 0.0),
    ("B_broad_1_4_5", 0.0, 0.0, 16.0, 64.0, 80.0, 0.04, 0.0),
    ("C_blade_1_3_14", 110.0, 70.0, 10.0, 28.0, 140.0, 0.08, 1.5),
]
SINK = 4.0          # hem goes this far under the ground
FLARE_W = 0.16      # hem flare as share of width (each side)
FLARE_D = 0.45      # hem flare as share of depth (each side): the slab gets a real foot
HEM = 0.13          # hem height as share of H
# hem profile: (height share of the hem, flare share); concave, the curve meets the slab tangentially
HEM_PROFILE = [(0.0, 1.0), (0.18, 0.62), (0.42, 0.30), (0.70, 0.10), (1.0, 0.0)]


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def material(name, rgba):
    m = bpy.data.materials.new(name)
    m.diffuse_color = rgba
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = 0.95
    return m


def ring(hw, hd, chamfer):
    """Rectangle with chamfered corners, 8 points, counter-clockwise."""
    c = min(chamfer, hw * 0.45, hd * 0.45)
    return [(hw, -hd + c), (hw, hd - c), (hw - c, hd), (-hw + c, hd),
            (-hw, hd - c), (-hw, -hd + c), (-hw + c, -hd), (hw - c, -hd)]


def slab(name, D, W, H, taper, mat):
    hw, hd = W / 2, D / 2
    fw, fd = W * FLARE_W, D * FLARE_D
    hem = H * HEM
    ch = 0.10 * D
    rings = [(-SINK, hw + fw, hd + fd)]
    for t, f in HEM_PROFILE:
        rings.append((t * hem, hw + fw * f, hd + fd * f))
    # body tapers slightly to the top; a mid ring keeps the long faces from being one quad
    tw, td = hw * (1 - taper), hd * (1 - taper * 0.5)
    rings.append((hem + (H - hem) * 0.55, hw + (tw - hw) * 0.55, hd + (td - hd) * 0.55))
    top_c = 0.18 * D
    rings.append((H - top_c, tw, td))
    rings.append((H, tw - top_c * 0.6, td - top_c * 0.6))

    bm = bmesh.new()
    loops = []
    for z, w, d in rings:
        loops.append([bm.verts.new((x, y, z)) for x, y in ring(w, d, ch)])
    for a, b in zip(loops, loops[1:]):
        n = len(a)
        for i in range(n):
            bm.faces.new((a[i], a[(i + 1) % n], b[(i + 1) % n], b[i]))
    bm.faces.new(loops[-1])
    bm.faces.new(list(reversed(loops[0])))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = False
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    bpy.context.collection.objects.link(ob)
    return ob


def human(name, loc, mat):
    """1.8 m low-poly figure: legs, torso, head boxes."""
    bm = bmesh.new()
    for (sx, sy, sz), (cx, cy, cz) in [((0.42, 0.25, 0.85), (0, 0, 0.425)),
                                       ((0.50, 0.28, 0.65), (0, 0, 1.18)),
                                       ((0.22, 0.22, 0.25), (0, 0, 1.66))]:
        r = bmesh.ops.create_cube(bm, size=1.0)
        for v in r["verts"]:
            v.co = Vector((v.co.x * sx + cx, v.co.y * sy + cy, v.co.z * sz + cz))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.data.materials.append(mat)
    bpy.context.collection.objects.link(ob)
    return ob


def ground_height(x, y):
    """Plain with soft rolls and a low mound of earth around each foot."""
    h = 1.2 * math.sin(x * 0.011) * math.cos(y * 0.014) + 0.6 * math.sin(x * 0.031 + y * 0.022)
    for _, vx, vy, D, W, H, _, _ in VARIANTS:
        rx, ry = W * 0.85, D * 2.6
        q = ((x - vx) / rx) ** 2 + ((y - vy) / ry) ** 2
        h += 2.5 * math.exp(-q * 1.6)
    return h


def ground(mat):
    size_x, size_y, step = 900.0, 900.0, 6.0
    nx, ny = int(size_x / step), int(size_y / step)
    bm = bmesh.new()
    grid = []
    for j in range(ny + 1):
        row = []
        for i in range(nx + 1):
            x = -size_x / 2 + i * step
            y = -size_y / 2 + j * step + 150.0
            row.append(bm.verts.new((x, y, ground_height(x, y))))
        grid.append(row)
    for j in range(ny):
        for i in range(nx):
            bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
    me = bpy.data.meshes.new("ground")
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new("ground", me)
    ob.data.materials.append(mat)
    bpy.context.collection.objects.link(ob)
    return ob


def build():
    clear()
    stone, grass, red = material("stone_blockout", STONE), material("ground_blockout", GROUND), material("human", HUMAN)
    ground(grass)
    made = []
    for name, x, y, D, W, H, taper, lean in VARIANTS:
        ob = slab(name, D, W, H, taper, stone)
        ob.location = (x, y, 0)
        ob.rotation_euler = (math.radians(lean), 0, 0)
        made.append(ob)
        # figures at the hem's outer edge on the camera side, and one at mid-width
        hd = D / 2 * (1 + 2 * FLARE_D)
        for k, dx in enumerate((-W * 0.25, W * 0.18)):
            px, py = x + dx, y - hd - 2.0 - k * 3
            human(f"human_{name}_{k}", (px, py, ground_height(px, py)), red)
    # a walker in the foreground of the eye-level view
    human("human_walker", (-9.0, -62.0, ground_height(-9.0, -62.0)), red)
    # report
    for ob in made:
        me = ob.data
        print(f"{ob.name}: tris {sum(len(p.vertices) - 2 for p in me.polygons)}, "
              f"dims {tuple(round(v, 1) for v in ob.dimensions)}")
    return made


def setup_render():
    sc = bpy.context.scene
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = engine
            break
        except TypeError as e:
            print("engine:", e)
    sc.render.resolution_x, sc.render.resolution_y = 1600, 900
    sc.render.film_transparent = False
    sc.render.image_settings.file_format = "PNG"
    # low afternoon sun from the left-front, cool sky fill
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 2.2
    sun.angle = math.radians(2)
    ob = bpy.data.objects.new("sun", sun)
    ob.rotation_euler = (math.radians(55), 0, math.radians(-40))
    bpy.context.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.55, 0.64, 0.74, 1)
    bg.inputs["Strength"].default_value = 0.45
    sc.world = world


def camera(name, loc, target, lens=35):
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    cam.clip_end = 5000
    ob = bpy.data.objects.new(name, cam)
    ob.location = loc
    d = Vector(target) - Vector(loc)
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    bpy.context.collection.objects.link(ob)
    return ob


VIEWS = [
    ("01_front_all", (0, -480, 40), (0, 0, 60), 35),
    ("02_three_quarter", (330, -400, 80), (-10, 0, 55), 35),
    ("03_side_profiles", (430, -10, 30), (-20, 0, 60), 30),
    ("04_human_eye_under_B", (-16, -78, 1.7), (2, -5, 36), 18),
]


def render_views():
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    for name, loc, target, lens in VIEWS:
        cam = camera("cam_" + name, loc, target, lens)
        sc.camera = cam
        sc.render.filepath = str(SHOTS / f"a1_monolith_blockout_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    sc.camera = bpy.data.objects["cam_" + VIEWS[0][0]]


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args(argv)
    build()
    setup_render()
    if not args.no_render:
        render_views()
    else:
        for name, loc, target, lens in VIEWS:
            camera("cam_" + name, loc, target, lens)
    BLEND.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND))
    print("saved", BLEND)


main()
