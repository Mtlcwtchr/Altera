"""Magic megalith A-1, blockout stage: the giant monolith slabs.

Reference (user, 12.10): thick rectangular monoliths standing in rows on a green plain
to the horizon; the sides sweep out in a long concave flare into the grass (on the giant
in the foreground the flare starts at about half height and is stronger on one side),
the tops are cut flat but slanted and chipped. This stage settles only the shape:
proportions, silhouette and foot. No lattice, no sculpt, no material yet
(doc/BIOME_PLAN.md, kind A: blockout -> sculpt -> material, user approval at each step).

The user picked the giant's shape and proportions (12.10): every monolith in the field is
that giant at about the same size (the game may rescale them). 1.8 m human figures give
the scale; copies of the giant, turned and mirrored, run in rows to the horizon.
PSX profile: low poly, flat shading, no smoothing.

    blender -b -P tools/blender/megalith_a_monolith_blockout.py
    blender -b -P tools/blender/megalith_a_monolith_blockout.py -- --no-render
"""
import argparse
import math
import random
import sys
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
BLEND = ROOT / "assets/terrain_core/megaliths/a_monolith/monolith_blockout.blend"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"

STONE = (0.60, 0.61, 0.56, 1)
FIELD_STONE = (0.55, 0.53, 0.50, 1)
GROUND = (0.28, 0.55, 0.16, 1)
HUMAN = (0.95, 0.10, 0.05, 1)

SINK = 4.0  # the foot goes this far under the ground
# height samples (share of H), denser low where the flare curves
T = [0.0, 0.03, 0.07, 0.12, 0.18, 0.25, 0.33, 0.42, 0.52, 0.63, 0.75, 0.87, 1.0]

# Each variant:
#   W, D, H        width, depth, height (m)
#   flare          extra half-width at the ground per side (-x, +x, -y, +y), as share of W or D
#   tf, p          the flare dies out at tf*H, with curve power p (bigger p = more concave)
#   top            the slab widens again near the top by this share
#   slant          top cut slope (deg around y, deg around x); chip = one top corner knocked down (m)
#   bend           sideways drift of the top as share of H; lean in degrees around x
VARIANTS = {
    "B_giant": dict(pos=(0, 0), W=95, D=62, H=280, flare=(0.60, 0.22, 0.50, 0.40), tf=0.62, p=2.4,
                    top=0.0, slant=(3, -1.5), chip=12, bend=0.03, lean=0.0),
}


def material(name, rgba):
    m = bpy.data.materials.new(name)
    m.diffuse_color = rgba
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = 0.95
    return m


def ring(x0, x1, y0, y1, chamfer):
    """Rectangle from x0..x1, y0..y1 with chamfered corners, 8 points, counter-clockwise."""
    c = min(chamfer, (x1 - x0) * 0.22, (y1 - y0) * 0.22)
    return [(x1, y0 + c), (x1, y1 - c), (x1 - c, y1), (x0 + c, y1),
            (x0, y1 - c), (x0, y0 + c), (x0 + c, y0), (x1 - c, y0)]


def slab(name, v, mat):
    W, D, H = v["W"], v["D"], v["H"]
    hw, hd = W / 2, D / 2
    fx0, fx1, fy0, fy1 = v["flare"][0] * W, v["flare"][1] * W, v["flare"][2] * D, v["flare"][3] * D
    tf, p, top = v["tf"], v["p"], v["top"]
    sx, sy = math.tan(math.radians(v["slant"][0])), math.tan(math.radians(v["slant"][1]))

    def extents(t):
        f = max(0.0, 1.0 - t / tf) ** p
        g = 1.0 + top * max(0.0, (t - 0.7) / 0.3) ** 2
        off = v["bend"] * H * t * t
        return (-hw * g - fx0 * f + off, hw * g + fx1 * f + off, -hd * g - fy0 * f, hd * g + fy1 * f)

    ch = 0.08 * D
    bm = bmesh.new()
    loops = []
    for i, t in enumerate([-SINK / H] + T):
        x0, x1, y0, y1 = extents(max(t, 0.0))
        pts = ring(x0, x1, y0, y1, ch)
        if i == len(T):  # top: slanted cut, one corner chipped
            loop = [bm.verts.new((x, y, H + sx * x + sy * y - (v["chip"] if k in (0, 7) else 0.0)))
                    for k, (x, y) in enumerate(pts)]
        else:
            loop = [bm.verts.new((x, y, t * H)) for x, y in pts]
        loops.append(loop)
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
    for poly in me.polygons:
        poly.use_smooth = False
    me.materials.append(mat)
    return me


def link(name, me, loc, rot_z=0.0, lean=0.0, scale=1.0, coll=None):
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.rotation_euler = (math.radians(lean), 0, rot_z)
    ob.scale = (scale, scale, scale)
    (coll or bpy.context.collection).objects.link(ob)
    return ob


def human(name, loc, mat, coll=None, seated=False):
    """1.8 m low-poly figure (about 1.0 m seated): legs, torso, head boxes."""
    parts = ([((0.45, 0.70, 0.30), (0, 0.2, 0.15)), ((0.48, 0.28, 0.60), (0, 0, 0.60)),
              ((0.22, 0.22, 0.24), (0, 0, 1.02))] if seated else
             [((0.42, 0.25, 0.85), (0, 0, 0.425)), ((0.50, 0.28, 0.65), (0, 0, 1.18)),
              ((0.22, 0.22, 0.25), (0, 0, 1.66))])
    bm = bmesh.new()
    for (sx, sy, sz), (cx, cy, cz) in parts:
        r = bmesh.ops.create_cube(bm, size=1.0)
        for vert in r["verts"]:
            vert.co = Vector((vert.co.x * sx + cx, vert.co.y * sy + cy, vert.co.z * sz + cz))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(mat)
    return link(name, me, loc, coll=coll)


def ground_height(x, y):
    """Plain with soft rolls and a low mound of earth around each lineup foot."""
    h = 1.2 * math.sin(x * 0.011) * math.cos(y * 0.014) + 0.6 * math.sin(x * 0.031 + y * 0.022)
    for v in VARIANTS.values():
        vx, vy = v["pos"]
        rx, ry = v["W"] * 1.0, v["D"] * 2.2
        q = ((x - vx) / rx) ** 2 + ((y - vy) / ry) ** 2
        h += 3.0 * math.exp(-q * 1.6)
    return h


def ground(mat):
    size, step, cy = 8000.0, 20.0, 3000.0
    n = int(size / step)
    bm = bmesh.new()
    grid = []
    for j in range(n + 1):
        row = []
        for i in range(n + 1):
            x, y = -size / 2 + i * step, -size / 2 + j * step + cy
            row.append(bm.verts.new((x, y, ground_height(x, y))))
        grid.append(row)
    for j in range(n):
        for i in range(n):
            bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
    me = bpy.data.meshes.new("ground")
    bm.to_mesh(me)
    bm.free()
    me.materials.append(mat)
    return link("ground", me, (0, 0, 0))


def collection(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    stone, field_stone = material("stone_blockout", STONE), material("field_stone_blockout", FIELD_STONE)
    grass, red = material("ground_blockout", GROUND), material("human", HUMAN)
    ground(grass)
    lineup, field, people = collection("lineup"), collection("field"), collection("people")
    meshes = {}
    for name, v in VARIANTS.items():
        meshes[name] = slab(name, v, stone)
        x, y = v["pos"]
        # the giant stays in every shot; A and C sit out the reference shot
        link(name, meshes[name], (x, y, 0), lean=v["lean"], coll=people if name == "B_giant" else lineup)
        # figures at the outer edge of the foot on the camera side
        hd = v["D"] / 2 + v["flare"][2] * v["D"]
        for k, dx in enumerate((-v["W"] * 0.25, v["W"] * 0.18)):
            px, py = x + dx, y - hd - 3.0 - k * 4
            human(f"human_{name}_{k}", (px, py, ground_height(px, py)), red, coll=lineup)
    # two people sitting on the grass in the foreground of the reference shot
    for k, dx in enumerate((0.0, 1.1)):
        px, py = -136.0 + dx, -152.0
        human(f"human_seated_{k}", (px, py, ground_height(px, py)), red, coll=people, seated=True)

    # field: copies of the giant at about its size, turned and mirrored, in loose rows to the horizon
    fb = slab("field_B", VARIANTS["B_giant"], field_stone)
    rnd = random.Random(7)
    count = 0
    for r in range(9):
        y = 1000 + r * 680 + rnd.uniform(-120, 120)
        x = -3800 + rnd.uniform(0, 400)
        while x < 3800:
            x += rnd.uniform(480, 950)
            px, py = x, y + rnd.uniform(-150, 150)
            ob = link(f"field_{count:03d}", fb, (px, py, ground_height(px, py) - 1.0),
                      rot_z=math.radians(rnd.uniform(-35, 35)), scale=rnd.uniform(0.8, 1.15), coll=field)
            if rnd.random() < 0.5:
                ob.scale.x = -ob.scale.x
            count += 1
    for name, me in meshes.items():
        print(f"{name}: tris {sum(len(p.vertices) - 2 for p in me.polygons)}")
    print("field copies:", count)


def setup_render():
    sc = bpy.context.scene
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = engine
            break
        except TypeError as e:
            print("engine:", e)
    sc.render.resolution_x, sc.render.resolution_y = 1600, 900
    sc.render.image_settings.file_format = "PNG"
    # afternoon sun from the left-front, deep blue sky fill
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 2.4
    sun.angle = math.radians(2)
    ob = bpy.data.objects.new("sun", sun)
    ob.rotation_euler = (math.radians(50), 0, math.radians(-35))
    bpy.context.scene.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.20, 0.32, 0.62, 1)
    bg.inputs["Strength"].default_value = 0.8
    sc.world = world


def camera(name, loc, target, lens):
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    cam.clip_end = 8000
    ob = bpy.data.objects.new(name, cam)
    ob.location = loc
    ob.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(ob)
    return ob


# name, camera, target, lens, show the lineup (A, C and their figures), show the field
VIEWS = [
    ("01_front_all", (0, -760, 60), (0, 0, 120), 35, True, True),
    ("02_three_quarter", (430, -600, 110), (0, 0, 100), 35, True, False),
    ("03_side_profiles", (680, -30, 40), (0, 0, 110), 30, True, False),
    ("04_reference_shot", (-150, -172, 1.6), (20, 810, 110), 22, False, True),
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    lineup, field = bpy.data.collections["lineup"], bpy.data.collections["field"]
    for name, loc, target, lens, show_lineup, show_field in VIEWS:
        cam = camera("cam_" + name, loc, target, lens)
        if not render:
            continue
        sc.camera = cam
        lineup.hide_render = not show_lineup
        field.hide_render = not show_field
        sc.render.filepath = str(SHOTS / f"a1_monolith_blockout_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    lineup.hide_render = field.hide_render = False
    sc.camera = bpy.data.objects["cam_" + VIEWS[0][0]]


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args(argv)
    build()
    setup_render()
    render_views(not args.no_render)
    BLEND.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND))
    print("saved", BLEND)


main()
