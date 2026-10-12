"""Magic megalith A-2: the carved stele (doc/BIOME_PLAN.md, kind A:
blockout -> sculpt -> material, user approval at each step).

Reference (user, 12.10): one very tall dark rectangular stele standing straight up, its
section almost square (the side face looks narrow, but the slab is thick and blocky), about
4.5-5 times taller than wide. Flat top with a slightly uneven edge, no slant. No flare at
the foot (unlike A-1): the walls drop almost sheer into the ground, the foot only has a torn
ragged edge, a couple of broken chunks and streaks. It stands in a small round pond of
turquoise water on a stony waste with boulders; the mountains behind are small next to it.
A long dark straight band runs from the foot to the right (a shadow, or a slab lying down;
both are shown). The surface is all carved relief with horizontal joints, as if stacked in
tiers (here only the tiers are hinted at: no carving, no material yet).

Blockout: four proportion variants side by side with 1.8 m figures, plus a shot repeating the
reference composition (from below, stele in the middle, the pond at its foot).

    blender -b -P tools/blender/megalith_a2_carved_stele.py
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --no-render
"""
import argparse
import math
import random
import sys
import zlib
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a2_carved_stele"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "blockout"

STONE = (0.10, 0.13, 0.11, 1)      # dark green-black, kept light enough to read the form
GROUND = (0.30, 0.27, 0.23, 1)     # stony waste
BOULDER = (0.24, 0.23, 0.21, 1)
MOUNTAIN = (0.22, 0.24, 0.23, 1)
WATER = (0.10, 0.62, 0.56, 1)
HUMAN = (0.95, 0.10, 0.05, 1)

SINK = 6.0        # the foot goes this far under the ground
POND_DEPTH = 1.4  # pond floor under the ground level
WATER_Z = -0.35
LINEUP_Y = -2000.0

# Each variant:
#   W, D, H     width, depth, height (m)
#   taper       the top is this share narrower than the foot (almost sheer walls)
#   tiers       number of stacked tiers (joints are uneven in height)
#   lip         how far the ragged foot edge juts out (m)
#   top_rough   uneven top edge (m); chip = one top corner knocked down (m)
#   pond        pond radius as share of max(W, D)
VARIANTS = {
    "V1_block": dict(W=40, D=34, H=180, taper=0.02, tiers=6, lip=1.2, top_rough=0.8, chip=0, pond=1.15),
    "V2_tall": dict(W=38, D=30, H=190, taper=0.03, tiers=7, lip=1.0, top_rough=1.0, chip=3, pond=1.2),
    "V3_slender": dict(W=34, D=24, H=192, taper=0.015, tiers=8, lip=0.8, top_rough=0.7, chip=0, pond=1.3),
    "V4_heavy": dict(W=46, D=42, H=190, taper=0.04, tiers=9, lip=1.6, top_rough=1.4, chip=5, pond=1.05),
}
HERO = "V2_tall"
LINEUP_X = {"V1_block": -300, "V2_tall": -100, "V3_slender": 100, "V4_heavy": 300}
PONDS = []  # (x, y, r) filled by build()


def material(name, rgba, rough=0.9, emit=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = rough
    if emit:
        bsdf.inputs["Emission Color"].default_value = rgba
        bsdf.inputs["Emission Strength"].default_value = emit
    m.diffuse_color = rgba
    return m


def link(name, me, loc, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0), coll=None):
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.rotation_euler = rot
    ob.scale = scale
    (coll or bpy.context.collection).objects.link(ob)
    return ob


def collection(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def mesh_from(name, bm, mat):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = False
    me.materials.append(mat)
    return me


# ---------------------------------------------------------------- the stele

SEG = 5  # perimeter segments per side: enough to make walls and edges a little uneven


def ring(hw, hd, z, cx=0.0, cy=0.0, jit=0.0, zjit=0.0, rnd=None):
    """Rectangle loop around (cx, cy) at height z, corners kept, SEG points per side,
    each point pushed out/in by up to jit and up/down by up to zjit."""
    pts = []
    corners = [(-hw, -hd), (hw, -hd), (hw, hd), (-hw, hd)]
    normals = [(0, -1), (1, 0), (0, 1), (-1, 0)]
    for s in range(4):
        (x0, y0), (x1, y1) = corners[s], corners[(s + 1) % 4]
        nx, ny = normals[s]
        for k in range(SEG):
            t = k / SEG
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            o = rnd.uniform(-jit, jit) if rnd and jit else 0.0
            if k == 0:  # the corner goes out along both side normals
                pnx, pny = normals[(s - 1) % 4]
                x, y = x + (nx + pnx) * o * 0.7, y + (ny + pny) * o * 0.7
            else:
                x, y = x + nx * o, y + ny * o
            dz = rnd.uniform(-zjit, zjit) if rnd and zjit else 0.0
            pts.append(Vector((cx + x, cy + y, z + dz)))
    return pts


def stele(name, v, mat, seed):
    """Stacked-tier slab: near-sheer walls with a small taper, a shallow groove at every tier
    joint, each tier a touch off from the one below; ragged lip at the foot, uneven flat top."""
    rnd = random.Random(seed)
    W, D, H = v["W"], v["D"], v["H"]

    def half(z):
        s = 1.0 - v["taper"] * max(z, 0.0) / H
        return W / 2 * s, D / 2 * s

    # uneven tier joints, the lowest tier tallest
    n = v["tiers"]
    w = [rnd.uniform(0.8, 1.25) for _ in range(n)]
    w[0] *= 1.35
    joints, acc = [], 0.0
    for k in range(n - 1):
        acc += w[k]
        joints.append(H * acc / sum(w))

    rings = []
    # under the ground, then the torn lip just above the water
    hw, hd = half(0)
    rings.append(ring(hw, hd, -SINK))
    rings.append(ring(hw + v["lip"], hd + v["lip"], WATER_Z - 0.3, jit=v["lip"] * 0.8, rnd=rnd))
    rings.append(ring(hw + v["lip"] * 0.6, hd + v["lip"] * 0.6, 1.2, jit=v["lip"] * 0.9, zjit=0.8, rnd=rnd))
    rings.append(ring(hw, hd, 3.0, jit=0.35, zjit=1.0, rnd=rnd))
    cx = cy = 0.0
    g, inset = 0.9, 0.9  # groove half height and depth
    z_prev = 3.0
    for z in joints + [None]:
        top = H - 2.5 if z is None else z - g
        mid = (z_prev + top) / 2
        hw, hd = half(mid)
        rings.append(ring(hw, hd, mid, cx, cy, jit=0.3, rnd=rnd))
        hw, hd = half(top)
        rings.append(ring(hw, hd, top, cx, cy, jit=0.15, rnd=rnd))
        if z is None:
            break
        rings.append(ring(hw - inset, hd - inset, z - g, cx, cy))
        cx += rnd.uniform(-0.6, 0.6)
        cy += rnd.uniform(-0.6, 0.6)
        hw, hd = half(z + g)
        rings.append(ring(hw - inset, hd - inset, z + g, cx, cy))
        rings.append(ring(hw, hd, z + g, cx, cy, jit=0.15, rnd=rnd))
        z_prev = z + g
    # uneven top edge, a chipped corner on some
    hw, hd = half(H)
    edge = ring(hw, hd, H, cx, cy, jit=0.4, zjit=v["top_rough"], rnd=rnd)
    if v["chip"]:
        c = rnd.randrange(4) * SEG
        for k in (c - 1, c, c + 1):
            edge[k % len(edge)].z -= v["chip"] * (1.0 if k == c else 0.45)
    rings.append(edge)

    bm = bmesh.new()
    loops = [[bm.verts.new(p) for p in r] for r in rings]
    m = len(loops[0])
    for a, b in zip(loops, loops[1:]):
        for i in range(m):
            bm.faces.new((a[i], a[(i + 1) % m], b[(i + 1) % m], b[i]))
    bm.faces.new(list(reversed(loops[0])))
    centre = bm.verts.new((cx, cy, H + rnd.uniform(-0.3, 0.3)))
    top = loops[-1]
    for i in range(m):
        bm.faces.new((top[i], top[(i + 1) % m], centre))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = mesh_from(name, bm, mat)
    print(f"{name}: {W}x{D}x{H} m, H/W {H / W:.2f}, D/W {D / W:.2f}, tris {sum(len(p.vertices) - 2 for p in me.polygons)}")
    return me


def chunk(name, size, mat, rnd):
    """A broken block: a box with its corners knocked about."""
    bm = bmesh.new()
    r = bmesh.ops.create_cube(bm, size=1.0)
    sx, sy, sz = size
    for vert in r["verts"]:
        vert.co = Vector((vert.co.x * sx * rnd.uniform(0.75, 1.15), vert.co.y * sy * rnd.uniform(0.75, 1.15),
                          vert.co.z * sz * rnd.uniform(0.7, 1.2)))
    return mesh_from(name, bm, mat)


def boulder(name, mat, rnd):
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=1, radius=1.0)
    for vert in bm.verts:
        vert.co *= rnd.uniform(0.75, 1.2)
        vert.co.z = vert.co.z * 0.7 + 0.15
    return mesh_from(name, bm, mat)


# ---------------------------------------------------------------- the land

def h_noise(x, y):
    return (2.2 * math.sin(x * 0.007 + 1.3) * math.cos(y * 0.009) + 1.1 * math.sin(x * 0.023 + y * 0.017)
            + 0.5 * math.sin(x * 0.061 - y * 0.047) + 0.25 * math.cos(x * 0.13 + y * 0.11))


def ground_height(x, y):
    """Stony waste, flattened around each pond: a round pan sunk under the ground with a low rim."""
    h = h_noise(x, y)
    for px, py, r in PONDS:
        d = math.hypot(x - px, y - py) / r
        if d < 2.2:
            flat = min(1.0, max(0.0, (2.2 - d) / 0.9))
            h = h * (1 - flat)
            if d < 1.0:
                h -= POND_DEPTH * (1 - d ** 6)
            else:
                h += 0.6 * math.exp(-((d - 1.12) / 0.12) ** 2)
    return h


def axis(lo, hi, coarse, fine_bands, fine):
    """Grid coordinates: coarse steps, fine steps inside the pond bands."""
    xs, x = [], lo
    while x <= hi:
        xs.append(x)
        inside = any(a <= x < b for a, b in fine_bands)
        x += fine if inside else coarse
    return xs


def ground(mat):
    bands_x = [(px - r * 2.4, px + r * 2.4) for px, py, r in PONDS]
    bands_y = [(py - r * 2.4, py + r * 2.4) for px, py, r in PONDS]
    xs = axis(-6000, 6000, 30.0, bands_x, 3.0)
    ys = axis(-6000, 6000, 30.0, bands_y, 3.0)
    bm = bmesh.new()
    grid = [[bm.verts.new((x, y, ground_height(x, y))) for x in xs] for y in ys]
    for j in range(len(ys) - 1):
        for i in range(len(xs) - 1):
            bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
    print("ground verts:", len(xs) * len(ys))
    return link("ground", mesh_from("ground", bm, mat), (0, 0, 0))


def pond(name, x, y, r, mat, rnd, coll):
    bm = bmesh.new()
    n = 28
    rim = [bm.verts.new((math.cos(a) * r * rnd.uniform(0.97, 1.05), math.sin(a) * r * rnd.uniform(0.97, 1.05), 0))
           for a in (2 * math.pi * k / n for k in range(n))]
    bm.faces.new(rim)
    return link(name, mesh_from(name, bm, mat), (x, y, WATER_Z), coll=coll)


def mountains(mat, rnd, coll):
    """A ring of jagged peaks far away (4.6-5.8 km): small next to the stele."""
    for k in range(46):
        a = 2 * math.pi * k / 46 + rnd.uniform(-0.05, 0.05)
        d = rnd.uniform(4600, 5800)
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, segments=7, radius1=rnd.uniform(350, 700), radius2=0,
                              depth=1.0)
        hgt = rnd.uniform(180, 420)
        for vert in bm.verts:
            if vert.co.z > 0:
                vert.co = Vector((rnd.uniform(-60, 60), rnd.uniform(-60, 60), hgt))
            else:
                vert.co = Vector((vert.co.x * rnd.uniform(0.8, 1.2), vert.co.y * rnd.uniform(0.8, 1.2), -20))
        bmesh.ops.subdivide_edges(bm, edges=[e for e in bm.edges], cuts=1)
        for vert in bm.verts:
            if 0 < vert.co.z < hgt:
                vert.co += Vector((rnd.uniform(-50, 50), rnd.uniform(-50, 50), rnd.uniform(-40, 40)))
        link(f"mountain_{k:02d}", mesh_from(f"mountain_{k:02d}", bm, mat),
             (math.cos(a) * d, math.sin(a) * d, 0), coll=coll)


def human(name, loc, mat, coll=None):
    """1.8 m low-poly figure: legs, torso, head boxes."""
    parts = [((0.42, 0.25, 0.85), (0, 0, 0.425)), ((0.50, 0.28, 0.65), (0, 0, 1.18)),
             ((0.22, 0.22, 0.25), (0, 0, 1.66))]
    bm = bmesh.new()
    for (sx, sy, sz), (cx, cy, cz) in parts:
        r = bmesh.ops.create_cube(bm, size=1.0)
        for vert in r["verts"]:
            vert.co = Vector((vert.co.x * sx + cx, vert.co.y * sy + cy, vert.co.z * sz + cz))
    return link(name, mesh_from(name, bm, mat), loc, coll=coll)


def foot_debris(tag, x, y, v, mat, rnd, coll):
    """A couple of broken chunks at the foot, half in the water."""
    for k in range(rnd.randint(2, 4)):
        side = rnd.choice((-1, 1))
        along = rnd.uniform(-0.5, 0.5)
        if rnd.random() < 0.5:
            px, py = x + along * v["W"], y + side * (v["D"] / 2 + rnd.uniform(2, 9))
        else:
            px, py = x + side * (v["W"] / 2 + rnd.uniform(2, 9)), y + along * v["D"]
        s = rnd.uniform(2.5, 7.0)
        me = chunk(f"chunk_{tag}_{k}", (s, s * rnd.uniform(0.6, 1.2), s * rnd.uniform(0.5, 0.9)), mat, rnd)
        link(f"chunk_{tag}_{k}", me, (px, py, ground_height(px, py) + s * 0.15),
             rot=(rnd.uniform(-0.4, 0.4), rnd.uniform(-0.4, 0.4), rnd.uniform(0, math.pi)), coll=coll)


def scatter_boulders(kinds, cx, cy, r0, r1, count, rnd, coll, tag):
    for k in range(count):
        a = rnd.uniform(0, 2 * math.pi)
        d = r0 + (r1 - r0) * rnd.random() ** 1.6
        px, py = cx + math.cos(a) * d, cy + math.sin(a) * d
        s = rnd.uniform(0.6, 2.5) * (1 if rnd.random() < 0.85 else rnd.uniform(2, 4))
        link(f"boulder_{tag}_{k:03d}", rnd.choice(kinds), (px, py, ground_height(px, py) - s * 0.2),
             rot=(0, 0, rnd.uniform(0, 2 * math.pi)), scale=(s * rnd.uniform(0.8, 1.4), s, s * rnd.uniform(0.6, 1.1)),
             coll=coll)


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    stone = material("stele_blockout", STONE)
    mats = dict(ground=material("ground_blockout", GROUND), boulder=material("boulder_blockout", BOULDER),
                mountain=material("mountain_blockout", MOUNTAIN), water=material("water_blockout", WATER, rough=0.08),
                human=material("human", HUMAN))
    for name, v in VARIANTS.items():
        PONDS.append((LINEUP_X[name], LINEUP_Y, v["pond"] * max(v["W"], v["D"])))
    hv = VARIANTS[HERO]
    PONDS.append((0.0, 0.0, hv["pond"] * max(hv["W"], hv["D"])))
    ground(mats["ground"])

    hero, lineup, fallen, land = collection("hero"), collection("lineup"), collection("fallen"), collection("land")
    rnd = random.Random(11)
    mountains(mats["mountain"], rnd, land)
    kinds = [boulder(f"boulder_kind_{k}", mats["boulder"], rnd) for k in range(8)]

    meshes = {name: stele(name, v, stone, zlib.crc32(name.encode())) for name, v in VARIANTS.items()}
    for name, v in VARIANTS.items():
        x, y = LINEUP_X[name], LINEUP_Y
        link(name, meshes[name], (x, y, 0), coll=lineup)
        r = v["pond"] * max(v["W"], v["D"])
        pond(f"pond_{name}", x, y, r, mats["water"], rnd, lineup)
        foot_debris(name, x, y, v, stone, rnd, lineup)
        # figures on the pond rim on the camera side, one wading near the foot
        for k, (dx, dy) in enumerate(((-6, -r - 3), (5, -r - 4), (v["W"] * 0.3, -v["D"] / 2 - 5))):
            px, py = x + dx, y + dy
            human(f"human_{name}_{k}", (px, py, max(ground_height(px, py), WATER_Z - 0.5)), mats["human"], lineup)
    scatter_boulders(kinds, 0, LINEUP_Y, 60, 900, 260, rnd, lineup, "lineup")

    # the hero for the reference shot: the tall variant in its pond on the waste
    link("hero_" + HERO, meshes[HERO], (0, 0, 0), coll=hero)
    r = hv["pond"] * max(hv["W"], hv["D"])
    pond("pond_hero", 0, 0, r, mats["water"], rnd, hero)
    foot_debris("hero", 0, 0, hv, stone, rnd, hero)
    scatter_boulders(kinds, 0, 0, r * 1.3, 1800, 520, rnd, hero, "hero")
    for k, (px, py) in enumerate(((-14, -r - 4), (-11, -r - 5))):
        human(f"human_hero_{k}", (px, py, ground_height(px, py)), mats["human"], hero)
    # option: the long dark band is a slab lying on the ground to the right of the foot
    L = hv["H"] * 0.9
    bm = bmesh.new()
    res = bmesh.ops.create_cube(bm, size=1.0)
    for vert in res["verts"]:
        vert.co = Vector((vert.co.x * L, vert.co.y * hv["D"] * 0.55, vert.co.z * 5.0))
    lying = mesh_from("lying_slab", bm, stone)
    link("lying_slab", lying, (hv["W"] / 2 + r * 0.6 + L / 2, 8, 0.5), rot=(0, 0, math.radians(-4)), coll=fallen)
    # red moon low in the green sky, right of the stele in the reference shot
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=260)
    link("moon", mesh_from("moon", bm, material("moon", (0.75, 0.06, 0.03, 1), emit=1.2)),
         (1300, 5200, 3600), coll=land)


# ---------------------------------------------------------------- shots

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
    sc.view_settings.view_transform = "Standard"
    # low sun from the left: the stele throws its long shadow band to the right of the foot
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 4.0
    sun.angle = math.radians(1.5)
    ob = bpy.data.objects.new("sun", sun)
    el = math.radians(22)
    d = Vector((math.cos(el), 0.35, -math.sin(el))).normalized()
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.30, 0.45, 0.28, 1)  # green sky
    bg.inputs["Strength"].default_value = 0.45
    sc.world = world


def camera(name, loc, target, lens, ortho=0.0):
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    cam.clip_end = 12000
    if ortho:
        cam.type = "ORTHO"
        cam.ortho_scale = ortho
    ob = bpy.data.objects.new(name, cam)
    ob.location = loc
    ob.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(ob)
    return ob


Y = LINEUP_Y
# name, camera, target, lens, ortho scale, collections shown besides land
VIEWS = [
    ("01_lineup_front", (0, Y - 780, 45), (0, Y, 95), 32, 0, {"lineup"}),
    ("02_lineup_three_quarter", (-560, Y - 560, 140), (0, Y, 80), 32, 0, {"lineup"}),
    ("03_proportions_ortho", (0, Y - 700, 100), (0, Y, 100), 50, 760, {"lineup"}),
    ("04_foot_close", (-55, Y - 85, 9), (-100, Y, 10), 30, 0, {"lineup"}),
    ("05_reference_shot", (-20, -215, 16.0), (0, 0, 88), 24, 0, {"hero"}),
    ("06_reference_lying_slab", (-20, -215, 16.0), (0, 0, 88), 24, 0, {"hero", "fallen"}),
    ("07_hero_wide", (-380, -560, 30), (40, 0, 85), 30, 0, {"hero"}),
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("hero", "lineup", "fallen")]
    for name, loc, target, lens, ortho, shown in VIEWS:
        cam = camera("cam_" + name, loc, target, lens, ortho)
        if not render:
            continue
        sc.camera = cam
        portrait = "reference" in name  # the reference composition is upright
        sc.render.resolution_x, sc.render.resolution_y = (900, 1600) if portrait else (1600, 900)
        for c in toggled:
            c.hide_render = c.name not in shown
        bpy.data.collections["land"].hide_render = bool(ortho)  # far peaks would fill an ortho frame
        sc.render.filepath = str(SHOTS / f"a2_stele_{STAGE}_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    for c in toggled + [bpy.data.collections["land"]]:
        c.hide_render = False
    sc.camera = bpy.data.objects["cam_" + VIEWS[4][0]]


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args(argv)
    build()
    setup_render()
    render_views(not args.no_render)
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    blend = ASSET_DIR / f"stele_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
