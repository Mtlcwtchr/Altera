"""Magic megalith A-2: the carved stele (doc/BIOME_PLAN.md, kind A:
blockout -> sculpt -> material, user approval at each step).

Reference (user, 12.10, painting: green sky, red moon): one very tall dark rectangular stele standing straight up, its
section almost square (the side face looks narrow, but the slab is thick and blocky), about
4.5-5 times taller than wide. Flat top with a slightly uneven edge, no slant. No flare at
the foot (unlike A-1): the walls drop almost sheer into the ground, the foot only has a torn
ragged edge, a couple of broken chunks and streaks. It stands in a small round pond of
turquoise water on a stony waste with boulders; the mountains behind are small next to it.
A long dark straight band runs from the foot to the right (a shadow, or a slab lying down;
both are shown). The surface is all carved relief with horizontal joints, as if stacked in
tiers (here only the tiers are hinted at: no carving, no material yet).

Blockout (approved 12.10): four proportion variants side by side with 1.8 m figures, plus a
shot repeating the reference composition; V2_tall chosen, the band to the right is its shadow.
Sculpt (12.10): wear of the ages on V2 only (see sculpted_stele); retopo to a 2500-tri game mesh,
the dense mesh kept as the bake source.
Material: AO, roughness and normal carry the look (see the material section): worn, barely
showing Picasso-like carved silhouettes and glyphs, black scratches cut in, dark runs.

    blender -b -P tools/blender/megalith_a2_carved_stele.py                      # material stage
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --stage sculpt
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --stage blockout
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --no-render
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --no-render --export   # game copy + manifests
"""
import argparse
import math
import random
import sys
import zlib
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector, noise

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a2_carved_stele"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "material"

STONE = (0.10, 0.13, 0.11, 1)      # dark green-black, kept light enough to read the form
GROUND = (0.25, 0.25, 0.20, 1)     # stony waste with a mossy tint
BOULDER = (0.24, 0.23, 0.21, 1)
MOUNTAIN = (0.26, 0.29, 0.27, 1)
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
#   pond        pond radius as share of W; the pond lies in front of and left of the stele,
#               whose foot stands at its back edge (as on the reference)
VARIANTS = {
    "V1_block": dict(W=40, D=34, H=180, taper=0.02, tiers=6, lip=1.2, top_rough=0.8, chip=0, pond=1.8),
    "V2_tall": dict(W=38, D=30, H=190, taper=0.03, tiers=7, lip=1.0, top_rough=1.0, chip=3, pond=1.9),
    "V3_slender": dict(W=34, D=24, H=192, taper=0.015, tiers=8, lip=0.8, top_rough=0.7, chip=0, pond=2.1),
    "V4_heavy": dict(W=46, D=42, H=190, taper=0.04, tiers=9, lip=1.6, top_rough=1.4, chip=5, pond=1.6),
}
HERO = "V2_tall"
LINEUP_X = {"V1_block": -300, "V2_tall": -100, "V3_slender": 100, "V4_heavy": 300}
PONDS = []  # (cx, cy, rx, ry) filled by build()
# the reference camera: like the painting, verticals kept parallel (level camera, frame shifted
# up), long lens from 370 m out and a little right, so the right side face just shows
REF_CAM = (74.0, -366.0, 37.0)
REF_SHIFT = 0.22


def pond_of(x, y, v):
    """Oval pond in front of and left of the stele standing at (x, y): centre and radii."""
    r = v["pond"] * v["W"]
    rx, ry = r, r * 0.9
    return x - 0.6 * v["W"], y - ry * 0.8, rx, ry


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
    g, inset = 0.7, 0.5  # groove half height and depth: tiers only hinted at
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


# ---------------------------------------------------------------- sculpt: wear of the ages
# The carving (human silhouettes, glyphs) goes to the texture and the normal map later; the
# mesh only carries what reads from afar: knocked corners and edges, broken tier joints, cracks,
# a few spalls, the waterline gnawed in, the top broken, a slight lean of an old stone.

def fbm(p, octaves=3):
    s, a, f = 0.0, 1.0, 1.0
    for _ in range(octaves):
        s += a * noise.noise(p * f)
        a *= 0.5
        f *= 2.0
    return s


def side_ts(L, step):
    """Samples along one side: dense near both corners (so the corner can be worn round and
    chipped), even in between; the end corner belongs to the next side."""
    n = max(2, math.ceil((L - 2.6) / step))
    return [0.0, 0.5, 1.3] + [1.3 + (L - 2.6) * k / n for k in range(1, n)] + [L - 1.3, L - 0.5]


def sculpted_stele(name, v, mat, seed, low=False):
    """low=False: the dense bake source (every chip, pit, crack and the gentle unevenness of the
    faces). low=True: the game mesh from the same wear events, keeping only what makes the
    silhouette (big chips off corners and the top, the joints, spalls, the waterline); flat parts
    are truly flat and get dissolved into big faces, the rest goes to the normal map."""
    rnd = random.Random(seed)
    W, D, H = v["W"], v["D"], v["H"]
    off = Vector((rnd.uniform(-100, 100), rnd.uniform(-100, 100), rnd.uniform(-100, 100)))

    # tier joints as in the blockout, each a little off from the one below
    n = v["tiers"]
    w = [rnd.uniform(0.8, 1.25) for _ in range(n)]
    w[0] *= 1.35
    joints, acc = [], 0.0
    for k in range(n - 1):
        acc += w[k]
        joints.append(H * acc / sum(w))
    TIERS["joints"] = joints
    shifts, cx, cy = [], 0.0, 0.0
    for k in range(n):
        shifts.append((cx, cy))
        cx += rnd.uniform(-0.6, 0.6)
        cy += rnd.uniform(-0.6, 0.6)

    def tier_of(z):
        return sum(1 for j in joints if z > j)

    def half(z):
        s = 1.0 - v["taper"] * max(z, 0.0) / H
        return W / 2 * s, D / 2 * s

    # ring heights: every 2 m, plus tight rings around each joint and the waterline
    zs = set(round(z, 2) for z in frange(0.0, H - 0.01, 2.0))
    for j in joints:
        zs.update(round(j + dz, 2) for dz in (-1.6, -0.8, -0.3, 0.3, 0.8, 1.6))
    zs.update((-SINK, -1.0, -0.4, 0.3, 1.0, H - 0.8, H))
    zs = sorted(zs)

    # wear events: chips on the corners (more near the top and the foot), on the top edge,
    # along the joints, a few pits on the faces, two big spalls
    corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    chips = []
    for _ in range(46):
        sx, sy = rnd.choice(corners)
        z = H * (rnd.random() ** 0.7 if rnd.random() < 0.5 else 1 - rnd.random() ** 0.6)
        hw, hd = half(z)
        chips.append((Vector((sx * hw, sy * hd, z)), rnd.uniform(1.2, 4.5), rnd.uniform(0.4, 1.8), "corner"))
    for _ in range(18):  # the top edge, broken down
        t = rnd.random()
        hw, hd = half(H)
        side = rnd.randrange(4)
        p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
        chips.append((Vector((p[0], p[1], H)), rnd.uniform(2.0, 6.5), rnd.uniform(0.8, 3.2), "top"))
    for j in joints:  # the joint lips crumble
        for _ in range(rnd.randint(3, 6)):
            hw, hd = half(j)
            side, t = rnd.randrange(4), rnd.random()
            p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
            chips.append((Vector((p[0], p[1], j + rnd.uniform(-0.8, 0.8))), rnd.uniform(1.0, 3.0), rnd.uniform(0.3, 1.0), "lip"))
    for _ in range(14):  # shallow pits on the faces
        z = rnd.uniform(4, H - 6)
        hw, hd = half(z)
        side, t = rnd.randrange(4), rnd.uniform(0.15, 0.85)
        p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
        chips.append((Vector((p[0], p[1], z)), rnd.uniform(1.5, 3.5), rnd.uniform(0.3, 0.8), "pit"))
    for zf in (0.38, 0.71):  # two big spalls off a corner
        sx, sy = rnd.choice(corners)
        z = H * zf + rnd.uniform(-8, 8)
        hw, hd = half(z)
        chips.append((Vector((sx * hw, sy * hd, z)), rnd.uniform(7.0, 10.0), rnd.uniform(2.5, 3.8), "spall"))

    # cracks: jagged vertical runs on the faces, from the top or a joint down 15-50 m
    cracks = []
    for _ in range(5):
        side = rnd.randrange(4)
        z1 = rnd.choice(joints + [H, H])
        z0 = max(2.0, z1 - rnd.uniform(15, 50))
        cracks.append((side, z0, z1, rnd.uniform(0.2, 0.8), rnd.uniform(0.6, 1.1), rnd.random() * 100))

    if low:  # fine wear goes to the normal map
        chips = [c for c in chips if c[3] in ("top", "spall") or (c[3] == "corner" and c[1] >= 2.2)]
        cracks = []
    chips = [c[:3] for c in chips]

    def crack_t(c, z):
        side, z0, z1, t0, depth, ph = c
        return t0 + 0.06 * noise.noise(Vector((z * 0.08, ph, 0))) + 0.02 * math.sin(z * 0.7 + ph)

    def top_drop(p):
        """How far the top surface is broken down at this point by the top edge chips."""
        drop = 0.0
        for c, r, depth in chips:
            if c.z >= H - 0.1:
                dist = Vector((p.x - c.x, p.y - c.y)).length
                if dist < r:
                    drop += 0.8 * depth * min(1.0, (1 - dist / r) * 3.0)
        return drop * min(1.0, max(0.0, (p.z - (H - 8.0)) / 8.0))

    def inward(p, side, t, at_corner):
        """How far (m) this surface point is worn in, and how far it drops (top edge)."""
        z = p.z
        d = 0.0 if low else 0.14 * fbm(Vector((p.x, p.y, z)) * 0.08 + off)  # the faces are not planes
        # corners worn round and chunky, more at the top and the foot
        # (in 7 m pieces, so each piece stays flat on the game mesh and the bake matches it)
        zc = math.floor((z + 2.3 * side) / 7.0) * 7.0 + 3.5
        age = 1.0 + 1.2 * math.exp(-zc / 25.0) + 1.0 * math.exp(-(H - zc) / 30.0)
        if at_corner < 2.0:
            q = 0.5 + 0.5 * noise.noise(Vector((zc * 0.12, side * 7.3, 3.1)) + off)
            d += age * (0.25 + 0.75 * round(q * 3) / 3) * (1 - at_corner / 2.0) ** 1.5
        # tier joints: an uneven groove
        for j in joints:
            if low:  # a plain groove; its uneven edge is in the normal map
                if abs(z - j) < 0.5:
                    d += 0.6
                continue
            gw = 0.8 * (1 + 0.5 * noise.noise(Vector((t * 0.2, j, 1.7)) + off))
            u = abs(z - j) / max(gw, 0.2)
            if u < 1:
                d += 0.7 * (1 - u * u) * (0.6 + 0.8 * abs(noise.noise(Vector((t * 0.3, j, 5.5)) + off)))
        # the waterline gnawed in
        if -1.5 < z < 3.5:
            wob = 0.0 if low else 0.6 * noise.noise(Vector((p.x, p.y, 0)) * 0.2 + off)
            d += math.exp(-((z - 0.6) / 1.1) ** 2) * (0.9 + wob)
        drop = top_drop(p)
        for c, r, depth in chips:
            dist = (p - c).length
            if dist < r:
                k = depth * min(1.0, (1 - dist / r) * 3.0)  # flat-bottomed flake, steep broken wall
                if c.z < H - 0.1:
                    d += k
                else:
                    d += k * 0.6
        for c in cracks:
            if c[0] == side and c[1] < z < c[2]:
                L = (W if side % 2 == 0 else D)
                u = abs(t / L - crack_t(c, z)) * L / 0.9
                if u < 1:
                    d += c[4] * (1 - u)
        return max(d, -0.3), drop

    # build the side rings
    side_fs = [[t / L0 for t in side_ts(L0, 1.8)] for L0 in (W, D, W, D)]
    loops_pts = []
    for z in zs:
        hw, hd = half(z)
        k = tier_of(z)
        sx0, sy0 = shifts[min(k, n - 1)]
        pts = []
        cs = [(-hw, -hd), (hw, -hd), (hw, hd), (-hw, hd)]
        nrm = [Vector((0, -1, 0)), Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((-1, 0, 0))]
        for s in range(4):
            (x0, y0), (x1, y1) = cs[s], cs[(s + 1) % 4]
            L = math.hypot(x1 - x0, y1 - y0)
            for f in side_fs[s]:  # same samples on every ring, scaled with the taper
                t = f * L
                base = Vector((x0 + (x1 - x0) * f + sx0, y0 + (y1 - y0) * f + sy0, z))
                at_corner = min(t, L - t)
                nv = nrm[s] if t > 0 else (nrm[s] + nrm[(s - 1) % 4]).normalized() * 1.2
                if z < -0.5:  # under the ground: plain
                    pts.append(base)
                    continue
                d, drop = inward(base, s, t, at_corner)
                p = base - nv * d
                p.z -= drop
                pts.append(p)
        loops_pts.append(pts)

    # the top: the last ring pulled inward in steps, broken and uneven, then the centre
    top = loops_pts[-1]
    centre = sum(top, Vector()) / len(top)
    centre.z = H
    rim = [Vector((p.x, p.y, H)) for p in top]  # the edge before it broke, for the inner rings
    for f in (0.9, 0.78, 0.64, 0.5, 0.36, 0.22, 0.1):
        ring_pts = []
        for p in rim:
            q = centre + (p - centre) * f
            q.z = H + (0.0 if low else 0.35 * fbm(Vector((q.x, q.y, 0)) * 0.12 + off))
            q.z -= top_drop(q)
            ring_pts.append(q)
        loops_pts.append(ring_pts)

    bm = bmesh.new()
    loops = [[bm.verts.new(p) for p in r] for r in loops_pts]
    m = len(loops[0])
    for a, b in zip(loops, loops[1:]):
        for i in range(m):
            bm.faces.new((a[i], a[(i + 1) % m], b[(i + 1) % m], b[i]))
    bm.faces.new(list(reversed(loops[0])))
    cv = bm.verts.new(centre + Vector((0, 0, 0.0 if low else 0.3 * noise.noise(off))))
    for i in range(m):
        bm.faces.new((loops[-1][i], loops[-1][(i + 1) % m], cv))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = mesh_from(name, bm, mat)
    if low:
        me = retopo(me, LOW_TRIS)
    if not low:
        # smooth over the gentle unevenness, keep the chips and corners hard
        me.shade_smooth()
        me.set_sharp_from_angle(angle=math.radians(22))
    print(f"{name} sculpt: verts {len(me.vertices)}, tris {sum(len(p.vertices) - 2 for p in me.polygons)}, "
          f"chips {len(chips)}, cracks {len(cracks)}")
    return me


LOW_TRIS = 2500


def retopo(me, target):
    """Game mesh: collapse the flat-faced low mesh down to about target triangles. Collapse keeps
    the density where the chips and joints are and lays big, well-shaped triangles over the flat
    faces (planar dissolve made long needle fans, bad for shading and shadows)."""
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    ob = bpy.data.objects.new("_retopo", me)
    bpy.context.scene.collection.objects.link(ob)
    mod = ob.modifiers.new("collapse", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = min(1.0, target / tris)
    mod.use_collapse_triangulate = True
    dg = bpy.context.evaluated_depsgraph_get()
    out = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    out.name = me.name
    bpy.data.objects.remove(ob)
    bpy.data.meshes.remove(me)
    for p in out.polygons:
        p.use_smooth = False
    return out


def frange(a, b, step):
    x = a
    while x <= b:
        yield x
        x += step


def fallen_pieces(tag, x, y, v, mat, rnd, coll):
    """What fell off over the ages: big angular pieces round the foot, in and out of the water,
    a few further out where they rolled."""
    for k in range(14):
        a = rnd.uniform(0, 2 * math.pi)
        d = rnd.uniform(0.55, 1.0) * max(v["W"], v["D"]) * (1.0 if k < 10 else rnd.uniform(1.4, 2.4))
        px, py = x + math.cos(a) * d, y + math.sin(a) * d
        s = rnd.uniform(1.5, 6.0) if k < 10 else rnd.uniform(1.0, 3.0)
        me = chunk(f"piece_{tag}_{k}", (s, s * rnd.uniform(0.5, 1.1), s * rnd.uniform(0.35, 0.8)), mat, rnd)
        link(f"piece_{tag}_{k}", me, (px, py, ground_height(px, py) + s * 0.1),
             rot=(rnd.uniform(-0.6, 0.6), rnd.uniform(-0.6, 0.6), rnd.uniform(0, math.pi)), coll=coll)


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
    """Stony waste, flattened around each pond: an oval pan sunk under the ground with a low rim."""
    h = h_noise(x, y)
    for px, py, rx, ry in PONDS:
        d = math.hypot((x - px) / rx, (y - py) / ry)
        if d < 2.2:
            flat = min(1.0, max(0.0, (2.2 - d) / 0.9))
            h = h * (1 - flat)
            if d < 1.0:
                h -= POND_DEPTH * (1 - d ** 6)
            else:
                h += 1.0 * math.exp(-((d - 1.12) / 0.12) ** 2)
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
    bands_x = [(px - rx * 2.4, px + rx * 2.4) for px, py, rx, ry in PONDS]
    bands_y = [(py - ry * 2.4, py + ry * 2.4) for px, py, rx, ry in PONDS]
    xs = axis(-7000, 7000, 30.0, bands_x, 3.0)
    ys = axis(-7000, 7000, 30.0, bands_y, 3.0)
    bm = bmesh.new()
    grid = [[bm.verts.new((x, y, ground_height(x, y))) for x in xs] for y in ys]
    for j in range(len(ys) - 1):
        for i in range(len(xs) - 1):
            bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
    print("ground verts:", len(xs) * len(ys))
    return link("ground", mesh_from("ground", bm, mat), (0, 0, 0))


def pond(name, x, y, rx, ry, mat, rnd, coll):
    bm = bmesh.new()
    n = 32
    rim = [bm.verts.new((math.cos(a) * rx * rnd.uniform(0.97, 1.05), math.sin(a) * ry * rnd.uniform(0.97, 1.05), 0))
           for a in (2 * math.pi * k / n for k in range(n))]
    bm.faces.new(rim)
    return link(name, mesh_from(name, bm, mat), (x, y, WATER_Z), coll=coll)


def peak(name, loc, r1, hgt, mat, rnd, coll, flat=0.0):
    """Jagged low-poly peak; flat > 0 cuts it into a mesa with a top that wide (share of r1)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=7, radius1=r1, radius2=r1 * flat, depth=1.0)
    for vert in bm.verts:
        if vert.co.z > 0:
            j = 0.15 * r1 if flat else 60
            vert.co = Vector((vert.co.x + rnd.uniform(-j, j), vert.co.y + rnd.uniform(-j, j),
                              hgt + rnd.uniform(-0.03, 0.03) * hgt))
        else:
            vert.co = Vector((vert.co.x * rnd.uniform(0.8, 1.2), vert.co.y * rnd.uniform(0.8, 1.2), -20))
    bmesh.ops.subdivide_edges(bm, edges=[e for e in bm.edges], cuts=1)
    for vert in bm.verts:
        if 0 < vert.co.z < hgt * 0.97:
            vert.co += Vector((rnd.uniform(-0.12, 0.12) * r1, rnd.uniform(-0.12, 0.12) * r1,
                               rnd.uniform(-0.1, 0.1) * hgt))
    link(name, mesh_from(name, bm, mat), loc, coll=coll)


def mountains(mat, rnd, coll):
    """As on the reference: the stele stands in a wide valley between two jagged ridges
    (1-1.6 km off to each side), a flat-topped mesa closing it far behind, low peaks around."""
    k = 0
    for side in (-1, 1):
        y = -1300.0
        while y < 3800:
            x = side * rnd.uniform(950, 1600) + (side * 200 if y > 2000 else 0)
            peak(f"ridge_{k:03d}", (x, y, 0), rnd.uniform(300, 520), rnd.uniform(220, 520), mat, rnd, coll)
            y += rnd.uniform(160, 320)
            k += 1
    peak("mesa", (300, 6500, 0), 900, 500, mat, rnd, coll, flat=0.75)
    peak("mesa_far", (-900, 5200, 0), 700, 300, mat, rnd, coll, flat=0.6)
    for j in range(30):  # low far ring for the other views
        a = 2 * math.pi * j / 30 + rnd.uniform(-0.05, 0.05)
        d = rnd.uniform(4800, 5800)
        peak(f"far_{j:02d}", (math.cos(a) * d, math.sin(a) * d, 0), rnd.uniform(400, 700), rnd.uniform(150, 320),
             mat, rnd, coll)


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
    stone = material(f"stele_{STAGE}", STONE)
    mats = dict(ground=material("ground_blockout", GROUND), boulder=material("boulder_blockout", BOULDER),
                mountain=material("mountain_blockout", MOUNTAIN), water=material("water_blockout", WATER, rough=0.08),
                human=material("human", HUMAN))
    for name, v in VARIANTS.items():
        PONDS.append(pond_of(LINEUP_X[name], LINEUP_Y, v))
    hv = VARIANTS[HERO]
    PONDS.append(pond_of(0.0, 0.0, hv))
    ground(mats["ground"])

    hero, lineup, fallen, land = collection("hero"), collection("lineup"), collection("fallen"), collection("land")
    rnd = random.Random(11)
    mountains(mats["mountain"], rnd, land)
    kinds = [boulder(f"boulder_kind_{k}", mats["boulder"], rnd) for k in range(8)]

    meshes = {name: stele(name, v, stone, zlib.crc32(name.encode())) for name, v in VARIANTS.items()}
    if STAGE != "blockout":  # only the approved variant goes on
        high = sculpted_stele(HERO + "_high", hv, stone, zlib.crc32(HERO.encode()))
        meshes[HERO] = sculpted_stele(HERO + "_low", hv, stone, zlib.crc32(HERO.encode()), low=True)
        uv_unwrap(meshes[HERO], hv["W"], hv["D"], hv["H"])
    for name, v in (VARIANTS.items() if STAGE == "blockout" else ()):
        x, y = LINEUP_X[name], LINEUP_Y
        link(name, meshes[name], (x, y, 0), coll=lineup)
        cx, cy, rx, ry = pond_of(x, y, v)
        pond(f"pond_{name}", cx, cy, rx, ry, mats["water"], rnd, lineup)
        foot_debris(name, x, y, v, stone, rnd, lineup)
        # figures on the pond rim on the camera side, one wading near the foot
        spots = ((cx - 6, cy - ry - 3), (cx + 5, cy - ry - 4), (x + v["W"] * 0.3, y - v["D"] / 2 - 5))
        for k, (px, py) in enumerate(spots):
            human(f"human_{name}_{k}", (px, py, max(ground_height(px, py), WATER_Z - 0.5)), mats["human"], lineup)
    scatter_boulders(kinds, 0, LINEUP_Y, 60, 900, 260, rnd, lineup, "lineup")

    # the hero for the reference shot: the tall variant in its pond on the waste
    # an old stone: it has settled a touch out of plumb
    lean = (math.radians(0.5), math.radians(-0.4), 0.0) if STAGE != "blockout" else (0.0, 0.0, 0.0)
    link("hero_" + HERO, meshes[HERO], (0, 0, 0), rot=lean, coll=hero)
    wire = collection("wire")
    if STAGE != "blockout":
        bake = collection("bake_source")  # the dense mesh the normal map is baked from
        link("hero_" + HERO + "_high", high, (0, 0, 0), rot=lean, coll=bake)
        bake.hide_render = True
        bpy.context.view_layer.layer_collection.children["bake_source"].hide_viewport = True
    if STAGE == "sculpt":
        wob = link("hero_wire", meshes[HERO], (0, 0, 0), rot=lean, coll=wire)
        mod = wob.modifiers.new("wire", "WIREFRAME")
        mod.thickness = 0.15
        mod.use_replace = True
        mod.use_even_offset = False
        wob.data = wob.data  # shared mesh; the wire material goes on the object slot
        wob.material_slots[0].link = "OBJECT"
        wob.material_slots[0].material = material("wire", (0.02, 0.02, 0.02, 1), emit=0.0)
    cx, cy, rx, ry = pond_of(0.0, 0.0, hv)
    pond("pond_hero", cx, cy, rx, ry, mats["water"], rnd, hero)
    foot_debris("hero", 0, 0, hv, stone, rnd, hero)
    if STAGE != "blockout":
        fallen_pieces("hero", 0, 0, hv, stone, rnd, hero)
    scatter_boulders(kinds, cx, cy, rx * 1.35, 1800, 560, rnd, hero, "hero")
    for k, (px, py) in enumerate(((cx - 14, cy - ry - 4), (cx - 11, cy - ry - 5))):
        human(f"human_hero_{k}", (px, py, ground_height(px, py)), mats["human"], hero)
    # blockout option only: the long dark band as a lying slab (user, 12.10: it is the shadow)
    L = hv["H"] * 1.2
    bm = bmesh.new()
    res = bmesh.ops.create_cube(bm, size=1.0)
    for vert in res["verts"]:
        vert.co = Vector((vert.co.x * L, vert.co.y * hv["D"] * 0.5, vert.co.z * 6.0))
    lying = mesh_from("lying_slab", bm, stone)
    if STAGE == "blockout":
        link("lying_slab", lying, (hv["W"] / 2 + L / 2 + 1.0, 4, 1.0), rot=(0, 0, math.radians(-2)), coll=fallen)
    # big red moon right of the stele top in the reference shot: 6 deg right of the view,
    # 24 deg up, about 3.7 deg across
    cam = Vector(REF_CAM)
    yaw = math.atan2(-cam.y, -cam.x) - math.radians(6)
    el, dist = math.radians(24), 7000.0
    loc = cam + dist * Vector((math.cos(el) * math.cos(yaw), math.cos(el) * math.sin(yaw), math.sin(el)))
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=dist * math.tan(math.radians(1.85)))
    link("moon", mesh_from("moon", bm, material("moon", (0.75, 0.04, 0.02, 1), emit=1.4)), loc, coll=land)


# ---------------------------------------------------------------- material: carved dark stone
# One 2048 atlas for the whole stele at about 10 px/m: the four sides unwrapped as one strip
# round the stone (u = metres along the perimeter, v = height), the top as its own island.
# Maps: basecolor (sRGB), normal (OpenGL tangent: baked from the dense mesh, then the carving,
# the scratches and the grain laid on), AO (baked plus cavities), roughness.
# The carving: worn, barely showing Picasso-like silhouettes (profiles with a frontal eye, split
# faces, standing figures, bands of glyphs), raised a little with incised outlines; black
# scratches cut into it, mostly up and down; dark runs below the joints.

TEX = 2048
PXM = 10.0           # pixels per metre
U0, Z0 = 8.0, 2.0    # side strip: u_px = U0 + u_m * PXM, v_px = (z + Z0) * PXM
TOP_U, TOP_V = 1420.0, 140.0


def strip_u(u):
    """Atlas pixel column of perimeter metre u."""
    return U0 + u * PXM
TIERS = {}           # filled by sculpted_stele: joints of the hero

# material knobs (the game reads them from the manifest later)
KNOBS = dict(
    carving_relief_m=0.32,     # how far the bas-relief figures stand out (rounded masses)
    carving_groove_m=0.12,     # how deep the outlines and features are cut
    carving_wear=0.3,         # 0 = crisp everywhere, 1 = mostly worn away
    carving_albedo=0.6,       # how much the carving shows in colour (it is mostly felt in the normal)
    scratch_count=7500,
    scratch_depth_m=0.045,
    scratch_darkness=0.85,
    symbols_per_panel=22,      # small drawn symbols cut over the figures
    soot=0.65,                 # darker toward the foot, as after a fire
    soot_height_m=120.0,       # how high the soot reaches (tongues go higher)
    contrast=1.35,
    streak_strength=0.45,      # dark runs below the joints and the top
    lichen=0.25,               # pale green-grey speckle
    grain=0.07,
)


def perim_u(side, x, y, W, D):
    return [x + W / 2, W + (y + D / 2), W + D + (W / 2 - x), 2 * W + D + (D / 2 - y)][side]


def ring_u(x, y, W, D):
    """Perimeter metres of a point: where the ray from the axis through it meets the rectangle.
    Continuous round the corners (worn corners do not fold onto a neighbour side's pixels)."""
    t = 1.0 / max(abs(x) / (W / 2), abs(y) / (D / 2), 1e-6)
    bx, by = x * t, y * t
    if abs(by + D / 2) < 1e-4:
        return perim_u(0, bx, by, W, D)
    if abs(bx - W / 2) < 1e-4:
        return perim_u(1, bx, by, W, D)
    if abs(by - D / 2) < 1e-4:
        return perim_u(2, bx, by, W, D)
    return perim_u(3, bx, by, W, D)


def uv_unwrap(me, W, D, H):
    """The sides as one strip round the stone (one seam, at the front-left corner), the top
    faces onto the top island."""
    per = 2 * (W + D)
    uvl = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        nz = poly.normal.z
        top = nz > 0.65 and poly.center.z > H - 20
        us = [ring_u(me.vertices[me.loops[li].vertex_index].co.x, me.vertices[me.loops[li].vertex_index].co.y, W, D)
              for li in poly.loop_indices]
        if max(us) - min(us) > per / 2:  # a face on the seam goes past the end of the strip
            us = [u + per if u < per / 2 else u for u in us]
        for li, um in zip(poly.loop_indices, us):
            co = me.vertices[me.loops[li].vertex_index].co
            if top:
                u, v = TOP_U + (co.x + W / 2) * PXM, TOP_V + (co.y + D / 2) * PXM
            else:
                u, v = strip_u(um), (max(co.z, -Z0 + 0.2) + Z0) * PXM
            uvl.data[li].uv = (u / TEX, v / TEX)


# --- small raster toolkit (numpy; rows = v going up, as Blender stores pixels)

def vnoise(rng, cx, cy=None, size=TEX):
    """Smooth value noise with cells cx by cy pixels, 0..1."""
    import numpy as np
    cy = cy or cx
    gh, gw = int(size / cy) + 3, int(size / cx) + 3
    g = rng.random((gh, gw)).astype(np.float32)
    ys = np.arange(size, dtype=np.float32) / cy
    xs = np.arange(size, dtype=np.float32) / cx
    y0, x0 = ys.astype(int), xs.astype(int)
    fy, fx = ys - y0, xs - x0
    fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
    a = g[y0][:, x0] * (1 - fx) + g[y0][:, x0 + 1] * fx
    b = g[y0 + 1][:, x0] * (1 - fx) + g[y0 + 1][:, x0 + 1] * fx
    return a * (1 - fy[:, None]) + b * fy[:, None]


def blur(img, r):
    """Box blur, run twice (about Gaussian)."""
    import numpy as np
    k = 2 * r + 1
    for _ in range(2):
        for t in (False, True):
            a = img.T if t else img
            c = np.cumsum(np.pad(a, ((r + 1, r), (0, 0)), mode="edge"), axis=0)
            a = (c[k:] - c[:-k]) / k
            img = a.T if t else a
    return img.astype(np.float32)


def stamp(img, pts, r, val=1.0):
    """Draw a thick line through pts (N x 2, px) into img with a rounded profile (max)."""
    import numpy as np
    if len(pts) < 2:
        return
    seg = np.diff(pts, axis=0)
    n = np.maximum(1, (np.linalg.norm(seg, axis=1) / max(0.5, r * 0.35)).astype(int))
    p = np.concatenate([pts[i] + seg[i] * (np.arange(n[i])[:, None] / n[i]) for i in range(len(seg))] + [pts[-1:]])
    ri = int(math.ceil(r))
    for dy in range(-ri, ri + 1):
        for dx in range(-ri, ri + 1):
            d = math.hypot(dx, dy)
            if d > r:
                continue
            prof = val * (1 - (d / (r + 0.5)) ** 2)
            xs = np.round(p[:, 0] + dx).astype(int)
            ys = np.round(p[:, 1] + dy).astype(int)
            ok = (xs >= 0) & (xs < TEX) & (ys >= 0) & (ys < TEX)
            np.maximum.at(img, (ys[ok], xs[ok]), prof)


def ellipse(img, c, a, b, rot=0.0, val=1.0):
    import numpy as np
    m = int(max(a, b)) + 2
    x0, x1 = max(0, int(c[0]) - m), min(TEX, int(c[0]) + m)
    y0, y1 = max(0, int(c[1]) - m), min(TEX, int(c[1]) + m)
    if x0 >= x1 or y0 >= y1:
        return
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    dx, dy = xx - c[0], yy - c[1]
    cs, sn = math.cos(rot), math.sin(rot)
    q = ((dx * cs + dy * sn) / a) ** 2 + ((-dx * sn + dy * cs) / b) ** 2
    sub = img[y0:y1, x0:x1]
    np.maximum(sub, np.where(q <= 1, val, 0).astype(np.float32), out=sub)


def bez(p0, p1, p2, p3, n=24):
    import numpy as np
    t = np.linspace(0, 1, n)[:, None]
    p0, p1, p2, p3 = (np.array(p, dtype=np.float32) for p in (p0, p1, p2, p3))
    return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


# --- the carving, drawn in metres on one face panel and placed on the strip

class Carver:
    def __init__(self, rnd):
        import numpy as np
        self.R = np.zeros((TEX, TEX), np.float32)   # raised plateaus
        self.G = np.zeros((TEX, TEX), np.float32)   # incised lines
        self.rnd = rnd

    def P(self, u, z):
        return (strip_u(u), (z + Z0) * PXM)

    def pts(self, ox, oz, s, local, flip=1.0):
        import numpy as np
        return np.array([self.P(ox + flip * x * s, oz + y * s) for x, y in local], dtype=np.float32)

    def groove(self, ox, oz, s, local, w=1.0, flip=1.0):
        stamp(self.G, self.pts(ox, oz, s, local, flip), w)

    def curve(self, ox, oz, s, a, b, c, d, w=1.0, flip=1.0):
        import numpy as np
        pts = bez(*[(ox + flip * x * s, oz + y * s) for x, y in (a, b, c, d)])
        stamp(self.G, np.array([self.P(x, y) for x, y in pts], dtype=np.float32), w)

    def raise_ell(self, ox, oz, s, cx, cy, a, b, rot=0.0, lvl=1.0, flip=1.0):
        ellipse(self.R, self.P(ox + flip * cx * s, oz + cy * s), a * s * PXM, b * s * PXM, rot * flip, lvl)

    def raise_line(self, ox, oz, s, local, r, lvl=1.0, flip=1.0):
        stamp(self.R, self.pts(ox, oz, s, local, flip), r * s * PXM, lvl)

    def eye(self, ox, oz, s, cx, cy, w, flip=1.0):
        """Frontal almond eye with a pupil, the Picasso eye set into a profile too."""
        self.curve(ox, oz, s, (cx - w, cy), (cx - w * 0.4, cy + w * 0.55), (cx + w * 0.4, cy + w * 0.55), (cx + w, cy), 1.0, flip)
        self.curve(ox, oz, s, (cx - w, cy), (cx - w * 0.4, cy - w * 0.45), (cx + w * 0.4, cy - w * 0.45), (cx + w, cy), 1.0, flip)
        self.raise_ell(ox, oz, s, cx, cy, w * 0.22, w * 0.22, lvl=0.4, flip=flip)

    def profile_head(self, ox, oz, s, flip):
        rnd = self.rnd
        self.raise_ell(ox, oz, s, -0.05, 0.0, 0.36, 0.5, rot=rnd.uniform(-0.2, 0.2), lvl=rnd.choice((0.7, 1.0)), flip=flip)
        prof = [(0.0, 0.52), (0.28, 0.38), (0.36, 0.14), (0.52, 0.0), (0.36, -0.05), (0.42, -0.17),
                (0.3, -0.21), (0.35, -0.31), (0.1, -0.5)]
        prof = [(x + rnd.uniform(-0.03, 0.03), y) for x, y in prof]
        self.groove(ox, oz, s, prof, 1.2, flip)
        self.eye(ox, oz, s, 0.16, 0.13, 0.11, flip)
        self.curve(ox, oz, s, (-0.12, 0.12), (-0.2, 0.05), (-0.18, -0.06), (-0.08, -0.04), 1.0, flip)  # ear
        for k in range(rnd.randint(2, 5)):  # hair running back
            y = 0.42 - k * 0.09
            self.curve(ox, oz, s, (0.0, y + 0.1), (-0.2, y + 0.12), (-0.35, y - 0.05), (-0.42, y - 0.25), 1.0, flip)

    def split_face(self, ox, oz, s):
        rnd = self.rnd
        self.raise_ell(ox, oz, s, 0.0, 0.0, 0.32, 0.5, lvl=0.7)
        self.raise_ell(ox, oz, s, 0.1, -0.04, 0.24, 0.42, rot=0.15, lvl=1.0)  # the half pushed out of line
        self.groove(ox, oz, s, [(0.02, 0.5), (-0.03, 0.2), (0.05, 0.0), (-0.02, -0.25), (0.04, -0.5)], 1.2)
        self.eye(ox, oz, s, -0.14, 0.1, 0.1)
        self.eye(ox, oz, s, 0.15, 0.2, 0.07)
        self.groove(ox, oz, s, [(0.0, 0.22), (0.1, -0.08), (-0.03, -0.12)], 1.0)
        self.curve(ox, oz, s, (-0.12, -0.26), (-0.04, -0.32), (0.06, -0.3), (0.12, -0.22), 1.0)
        if rnd.random() < 0.6:  # a second profile cut into the cheek
            self.groove(ox, oz, s, [(0.32, 0.3), (0.4, 0.05), (0.5, -0.02), (0.38, -0.1), (0.3, -0.4)], 1.0)

    def figure(self, ox, oz, s, flip):
        """Standing figure, foot at (ox, oz), s tall: big simple masses, a turned head, long arms."""
        rnd = self.rnd
        lean = rnd.uniform(-0.06, 0.06)
        self.raise_ell(ox, oz, s, lean * 2, 0.88, 0.09, 0.11, lvl=1.0, flip=flip)
        self.raise_line(ox, oz, s, [(lean * 1.6, 0.76), (lean, 0.48), (0.0, 0.38)], 0.13, 0.9, flip)
        self.raise_ell(ox, oz, s, lean * 1.2, 0.6, 0.15, 0.2, rot=lean, lvl=1.0, flip=flip)
        self.raise_line(ox, oz, s, [(0.0, 0.4), (0.06, 0.2), (0.08, 0.0)], 0.06, 0.8, flip)
        self.raise_line(ox, oz, s, [(0.0, 0.4), (-0.07, 0.2), (-0.06, 0.0)], 0.06, 0.8, flip)
        arm = [(lean * 1.6, 0.72), (0.2, 0.68), (0.3, 0.5), (0.22, 0.36)]
        self.raise_line(ox, oz, s, arm, 0.04, 0.95, flip)
        self.raise_line(ox, oz, s, [(lean * 1.6, 0.72), (-0.14, 0.6), (-0.1, 0.45), (-0.18, 0.32)], 0.04, 0.75, flip)
        self.eye(ox, oz, s, lean * 2 + 0.02, 0.9, 0.035, flip)
        for k in range(rnd.randint(1, 3)):  # folds of a robe
            y = 0.62 - k * 0.08
            self.curve(ox, oz, s, (-0.08, y), (-0.02, y - 0.04), (0.04, y - 0.04), (0.1, y + 0.01), 1.0, flip)

    def symbol(self, u, z, s):
        """One small drawn sign: a stroke, an arc, a chevron, a cross, a hooked line, a ring."""
        rnd = self.rnd
        k = rnd.randrange(6)
        if k == 0:
            self.groove(u, z, s, [(0, -1), (0.2, 1)], 0.8)
        elif k == 1:
            self.curve(u, z, s, (-0.8, -0.6), (-0.8, 1.2), (0.8, 1.2), (0.8, -0.6), 0.8)
        elif k == 2:
            self.groove(u, z, s, [(-0.8, 0.8), (0, -0.8), (0.8, 0.8)], 0.8)
        elif k == 3:
            self.groove(u, z, s, [(-0.8, 0), (0.8, 0)], 0.8)
            self.groove(u, z, s, [(0, -0.9), (0, 0.9)], 0.8)
        elif k == 4:
            self.groove(u, z, s, [(0, -1), (0, 0.6), (0.6, 1.0), (0.9, 0.6)], 0.8)
        else:
            self.curve(u, z, s, (-0.7, 0), (-0.7, 1.0), (0.7, 1.0), (0.7, 0), 0.8)
            self.curve(u, z, s, (-0.7, 0), (-0.7, -1.0), (0.7, -1.0), (0.7, 0), 0.8)

    def glyph_band(self, u0, u1, z0, rows=2, cell=1.4):
        """Rows of small glyphs between incised rules, like a written border."""
        rnd = self.rnd
        import numpy as np
        for r in range(rows + 1):
            z = z0 + r * cell
            stamp(self.G, np.array([self.P(u0, z), self.P(u1, z)], dtype=np.float32), 1.0)
        for r in range(rows):
            u = u0 + 0.3
            while u < u1 - cell * 0.6:
                w = cell * rnd.uniform(0.55, 0.9)
                cz = z0 + r * cell + cell * 0.5
                k = rnd.randrange(6)
                s = cell * 0.35
                if k == 0:
                    self.groove(u + w / 2, cz, s, [(0, -1), (0, 1)], 1.0)
                elif k == 1:
                    self.curve(u + w / 2, cz, s, (-0.8, -0.6), (-0.8, 1.2), (0.8, 1.2), (0.8, -0.6), 1.0)
                elif k == 2:
                    self.groove(u + w / 2, cz, s, [(-0.8, 0.8), (0, -0.8), (0.8, 0.8)], 1.0)
                elif k == 3:
                    self.raise_ell(u + w / 2, cz, s, 0, 0, 0.6, 0.6, lvl=0.6)
                elif k == 4:
                    self.groove(u + w / 2, cz, s, [(-0.8, 0), (0.8, 0)], 1.0)
                    self.groove(u + w / 2, cz, s, [(0, -0.9), (0, 0.9)], 1.0)
                else:
                    self.curve(u + w / 2, cz, s, (-0.8, 0.8), (0.9, 0.9), (-0.9, -0.9), (0.8, -0.8), 1.0)
                u += w + cell * 0.25

    def meander(self, u0, u1, z0, z1):
        rnd = self.rnd
        a = (rnd.uniform(u0, u1), rnd.uniform(z0, z1))
        d = (rnd.uniform(u0, u1), rnd.uniform(z0, z1))
        b = (rnd.uniform(u0, u1), rnd.uniform(z0, z1))
        c = (rnd.uniform(u0, u1), rnd.uniform(z0, z1))
        self.curve(0, 0, 1, a, b, c, d, rnd.choice((1.0, 1.5)))

    def panel(self, u0, u1, z0, z1):
        rnd = self.rnd
        w, h = u1 - u0, z1 - z0
        self.glyph_band(u0, u1, z1 - 2 * 1.4 - 0.4)
        top = z1 - 3.6
        for _ in range(rnd.randint(3, 5)):  # big low plates behind, the relief in layers
            a, b = rnd.uniform(4, 9), rnd.uniform(5, 11)
            self.raise_ell(rnd.uniform(u0 + a, u1 - a), rnd.uniform(z0 + b * 0.6, top - b * 0.6), 1, 0, 0,
                           a, b, rot=rnd.uniform(-0.6, 0.6), lvl=rnd.choice((0.35, 0.45, 0.55)))
        for _ in range(rnd.choice((2, 2, 3))):  # standing figures, the bas-relief proper
            s = min(h - 4.5, rnd.uniform(13, 22))
            self.figure(rnd.uniform(u0 + 0.15 * w, u1 - 0.15 * w), z0 + 0.4, s, rnd.choice((-1, 1)))
        for _ in range(rnd.randint(3, 5)):  # heads, overlapping like a collage
            s = rnd.uniform(4.5, 9.5)
            ox, oz = rnd.uniform(u0 + s * 0.45, u1 - s * 0.45), rnd.uniform(z0 + s * 0.55, top - s * 0.55)
            if rnd.random() < 0.55:
                self.profile_head(ox, oz, s, rnd.choice((-1, 1)))
            else:
                self.split_face(ox, oz, s)
        for _ in range(rnd.randint(1, 3)):
            self.meander(u0, u1, z0, top)
        for _ in range(KNOBS["symbols_per_panel"]):  # symbols scratched in over everything
            self.symbol(rnd.uniform(u0 + 1, u1 - 1), rnd.uniform(z0 + 1, top), rnd.uniform(0.25, 0.6))


def carve(rnd, W, D, H, joints):
    """All panels: every tier on every face (a panel can run round a corner a little)."""
    import numpy as np
    cv = Carver(rnd)
    bounds = [1.2] + list(joints) + [H - 1.5]
    starts = [0.0, W, W + D, 2 * W + D]
    lens = [W, D, W, D]
    for k in range(len(bounds) - 1):
        z0, z1 = bounds[k] + 1.0, bounds[k + 1] - 1.0
        for s in range(4):
            cv.panel(starts[s] + 1.2, starts[s] + lens[s] - 1.2, z0, z1)
    mask = blur(cv.R, 1)
    # bas-relief: each mass swells toward its middle (cut edge, rounded body)
    R = mask * (0.35 + 0.65 * np.clip(blur(mask, 6) * 1.4, 0, 1))
    # the outline of every raised mass is cut in: Picasso's line round the shapes
    gy, gx = np.gradient(mask)
    edge = np.clip(np.hypot(gx, gy) * 2.2, 0, 1)
    G = np.maximum(blur(cv.G, 1) * 1.2, edge)
    return R, np.clip(G, 0, 1)


def scratches(rnd, n):
    """Black scratches: mostly up and down, some aslant, thin, a few long."""
    import numpy as np
    S = np.zeros((TEX, TEX), np.float32)
    for _ in range(n):
        x, y = rnd.uniform(0, TEX), rnd.uniform(0, TEX)
        ang = math.radians(90 + (rnd.gauss(0, 6) if rnd.random() < 0.93 else rnd.uniform(-35, 35)))
        ln = rnd.uniform(8, 60) * (3 if rnd.random() < 0.08 else 1)
        k = max(3, int(ln / 6))
        pts = []
        for i in range(k + 1):
            t = i / k
            wob = rnd.uniform(-0.6, 0.6)
            pts.append((x + math.cos(ang) * ln * t + wob, y + math.sin(ang) * ln * t))
        stamp(S, np.array(pts, dtype=np.float32), rnd.choice((0.6, 0.6, 0.9, 1.3)), rnd.uniform(0.6, 1.0))
    return S


def streaks(rng, joints, H):
    """Dark runs below each joint and the top, fading downward."""
    import numpy as np
    n = vnoise(rng, 5, 160)
    n2 = vnoise(rng, 3, 60)
    mask = np.clip((n * 0.7 + n2 * 0.3 - 0.52) * 4.0, 0, 1)
    v = (np.arange(TEX, dtype=np.float32) / PXM) - Z0  # height of each row (m)
    fade = np.zeros(TEX, np.float32)
    for j in list(joints) + [H]:
        below = j - v
        fade = np.maximum(fade, np.where(below > 0, np.exp(-below / 9.0), 0))
    return mask * fade[:, None]


def bake_maps(sc, low_ob, high_ob):
    """Cycles: tangent normal from the dense mesh onto the game mesh, and AO; returns arrays."""
    import numpy as np
    keep = {o.name: o.hide_render for o in sc.objects}
    for o in sc.objects:
        o.hide_render = o not in (low_ob, high_ob)
    lc = bpy.context.view_layer.layer_collection.children["bake_source"]
    lc.hide_viewport = False
    high_ob.users_collection[0].hide_render = False
    try:
        sc.render.engine = "CYCLES"
    except TypeError as e:
        print("engine:", e)
    sc.cycles.device = "CPU"
    tmp = bpy.data.materials.new("bake_target")
    tmp.use_nodes = True
    node = tmp.node_tree.nodes.new("ShaderNodeTexImage")
    tmp.node_tree.nodes.active = node
    old = list(low_ob.data.materials)
    low_ob.data.materials.clear()
    low_ob.data.materials.append(tmp)
    for o in sc.objects:
        o.select_set(o in (low_ob, high_ob))
    bpy.context.view_layer.objects.active = low_ob
    bk = sc.render.bake
    bk.use_selected_to_active = True
    bk.cage_extrusion = 0.5  # small: flat faces split at the corners, a big cage hits the next face
    bk.max_ray_distance = 2.6
    bk.margin = 8
    out = {}
    for kind, samples in (("NORMAL", 1), ("AO", 24)):
        img = bpy.data.images.new(f"bake_{kind}", TEX, TEX, float_buffer=True)
        img.colorspace_settings.name = "Non-Color"
        node.image = img
        sc.cycles.samples = samples
        bpy.ops.object.bake(type=kind, normal_space="TANGENT")
        out[kind] = np.array(img.pixels[:], dtype=np.float32).reshape(TEX, TEX, 4)
        print("baked", kind)
    low_ob.data.materials.clear()
    for m in old:
        low_ob.data.materials.append(m)
    for o in sc.objects:
        o.hide_render = keep.get(o.name, False)
    high_ob.users_collection[0].hide_render = True
    lc.hide_viewport = True
    return out["NORMAL"], out["AO"][:, :, 0]


def save_map(name, arr, srgb):
    """arr: TEX x TEX x 3 (or x 1) in 0..1, rows going up; written as 8-bit PNG next to the blend."""
    import numpy as np
    if arr.ndim == 2:
        arr = np.repeat(arr[:, :, None], 3, axis=2)
    rgba = np.concatenate([np.clip(arr, 0, 1), np.ones((TEX, TEX, 1), np.float32)], axis=2)
    img = bpy.data.images.new(name, TEX, TEX)
    img.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    img.pixels.foreach_set(rgba.astype(np.float32).ravel())
    img.filepath_raw = str(ASSET_DIR / f"{name}.png")
    img.file_format = "PNG"
    img.save()
    print("saved", img.filepath_raw)
    return img


def make_maps(sc, low_ob, high_ob, v):
    import numpy as np
    W, D, H = v["W"], v["D"], v["H"]
    joints = TIERS["joints"]
    rnd = random.Random(23)
    rng = np.random.default_rng(23)
    n_bake, ao_bake = bake_maps(sc, low_ob, high_ob)
    R, G = carve(rnd, W, D, H, joints)
    # wear the carving away in patches, more low down where the water reached
    wear = vnoise(rng, 90) * 0.6 + vnoise(rng, 28) * 0.4
    vz = (np.arange(TEX, dtype=np.float32) / PXM - Z0)[:, None]
    keep = np.clip((wear - KNOBS["carving_wear"] * 0.6) * 2.5, 0.08, 1.0) * np.clip(0.4 + vz / 40.0, 0.4, 1.0)
    R, G = R * keep, G * keep
    S = scratches(rnd, KNOBS["scratch_count"])
    ST = streaks(rng, joints, H)
    grain = rng.random((TEX, TEX)).astype(np.float32)
    mott = vnoise(rng, 40) * 0.5 + vnoise(rng, 12) * 0.3 + vnoise(rng, 4) * 0.2
    lich = np.clip((vnoise(rng, 6) * 0.6 + vnoise(rng, 20) * 0.4 - (1.0 - KNOBS["lichen"] * 0.5)) * 6, 0, 1)

    # height of the fine detail (m) -> tangent normal, laid over the baked one (whiteout blend)
    h = (KNOBS["carving_relief_m"] * R - KNOBS["carving_groove_m"] * G - KNOBS["scratch_depth_m"] * S
         + 0.012 * (grain - 0.5))
    dhv, dhu = np.gradient(h * PXM)  # slope per metre
    n2 = np.dstack([-dhu, -dhv, np.ones_like(h)])
    n1 = n_bake[:, :, :3] * 2 - 1
    n = np.dstack([n1[:, :, 0] + n2[:, :, 0], n1[:, :, 1] + n2[:, :, 1], n1[:, :, 2] * n2[:, :, 2]])
    n /= np.linalg.norm(n, axis=2, keepdims=True) + 1e-6
    # cavity from the baked normal (chips, cracks of the dense mesh)
    curv = np.gradient(n1[:, :, 0], axis=1) + np.gradient(n1[:, :, 1], axis=0)
    cav = np.clip(-blur(curv, 1) * 6.0, 0, 1)

    ao = np.clip(ao_bake, 0, 1) * (1 - 0.6 * G) * (1 - 0.45 * cav) * (1 - 0.45 * S) * (1 - 0.1 * ST)
    ao = np.clip(ao, 0, 1)

    dark = np.array((0.04, 0.075, 0.05), np.float32)
    mid = np.array((0.15, 0.205, 0.16), np.float32)
    mc = np.clip(0.5 + (mott - 0.5) * KNOBS["contrast"] * 1.6, 0, 1)
    col = dark + (mid - dark) * (mc[:, :, None] ** 1.5)
    ca = KNOBS["carving_albedo"]
    col *= (1 + ca * 0.9 * R)[:, :, None]                       # the figures catch the light
    # soot: darker toward the foot as if a fire burned round it, in licking tongues
    tongue = vnoise(rng, 14, 260) * 0.7 + vnoise(rng, 40, 90) * 0.3
    reach = KNOBS["soot_height_m"] * (0.55 + 0.9 * tongue)
    soot = np.clip(1.0 - vz / reach, 0, 1) ** 1.3 * KNOBS["soot"]
    col *= (1 - 0.88 * soot)[:, :, None]
    col *= (1 - ca * 0.9 * G)[:, :, None]                       # cut lines hold dirt
    col = col + (np.array((0.24, 0.29, 0.25), np.float32) - col) * (lich * 0.6)[:, :, None]
    col *= (1 - KNOBS["streak_strength"] * ST)[:, :, None]
    col *= (1 - KNOBS["scratch_darkness"] * S)[:, :, None]      # black scratches
    col *= (1 + KNOBS["grain"] * (grain - 0.5) * 2)[:, :, None]
    rough = (0.84 - 0.14 * R + 0.06 * G - 0.22 * ST + 0.08 * S + 0.05 * (grain - 0.5) - 0.06 * lich
             + 0.08 * soot)  # soot is matte

    imgs = dict(
        base=save_map("stele_basecolor_2048", col, True),
        normal=save_map("stele_normal_2048", n * 0.5 + 0.5, False),
        ao=save_map("stele_ao_2048", ao, False),
        rough=save_map("stele_roughness_2048", np.clip(rough, 0.3, 1.0), False),
    )
    return imgs


def stele_material(imgs):
    m = bpy.data.materials.new("stele_material")
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")

    def tex(img):
        nd = nt.nodes.new("ShaderNodeTexImage")
        nd.image = img
        nd.interpolation = "Closest"  # chunky pixels
        return nd

    base, nrm, ao, rough = tex(imgs["base"]), tex(imgs["normal"]), tex(imgs["ao"]), tex(imgs["rough"])
    mul = nt.nodes.new("ShaderNodeMix")
    mul.data_type = "RGBA"
    mul.blend_type = "MULTIPLY"
    mul.inputs["Factor"].default_value = 1.0
    nt.links.new(base.outputs["Color"], mul.inputs["A"])
    nt.links.new(ao.outputs["Color"], mul.inputs["B"])
    nt.links.new(mul.outputs["Result"], bsdf.inputs["Base Color"])
    nt.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(nrm.outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return m


def debris_material():
    """The fallen pieces: the same dark stone, procedural (no UVs on the chunks)."""
    m = bpy.data.materials.new("stele_debris")
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")
    nz = nt.nodes.new("ShaderNodeTexNoise")
    nz.inputs["Scale"].default_value = 0.6
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (0.006, 0.009, 0.007, 1)
    ramp.color_ramp.elements[1].color = (0.03, 0.04, 0.033, 1)
    nt.links.new(nz.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.85
    return m


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
    d = Vector((math.cos(el), 0.3, -math.sin(el))).normalized()
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.30, 0.45, 0.28, 1)  # green sky
    bg.inputs["Strength"].default_value = 0.6
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
    ("04_foot_close", (-50, Y - 200, 14), (-110, Y - 30, 16), 35, 0, {"lineup"}),
    ("05_reference_shot", REF_CAM, (0, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("06_reference_lying_slab", REF_CAM, (0, 0, REF_CAM[2]), 50, 0, {"hero", "fallen"}),
    ("07_hero_wide", (-380, -560, 30), (40, 0, 85), 30, 0, {"hero"}),
]
SCULPT_VIEWS = [
    ("01_reference_shot", REF_CAM, (0, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("02_three_quarter", (210, -260, 60), (0, 0, 90), 32, 0, {"hero"}),
    ("03_close_top", (70, -95, 205), (0, 0, 180), 35, 0, {"hero"}),
    ("04_close_middle", (-60, -110, 100), (0, 0, 85), 35, 0, {"hero"}),
    ("05_foot_waterline", (-45, -80, 6), (0, 0, 4), 35, 0, {"hero"}),
    ("06_back_left", (-230, 200, 40), (0, 0, 95), 30, 0, {"hero"}),
    ("07_wire_three_quarter", (210, -260, 60), (0, 0, 90), 32, 0, {"hero", "wire"}),
    ("08_wire_top", (70, -95, 205), (0, 0, 180), 35, 0, {"hero", "wire"}),
]
MATERIAL_VIEWS = [
    ("01_reference_shot", REF_CAM, (0, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("02_three_quarter", (210, -260, 60), (0, 0, 90), 32, 0, {"hero"}),
    ("03_close_middle", (-60, -110, 100), (0, 0, 85), 35, 0, {"hero"}),
    ("04_relief_detail", (-14, -48, 40), (-2, 0, 38), 35, 0, {"hero"}),
    ("05_close_top", (70, -95, 205), (0, 0, 180), 35, 0, {"hero"}),
    ("06_foot_waterline", (-45, -80, 6), (0, 0, 6), 35, 0, {"hero"}),
    ("07_back_left", (-230, 200, 40), (0, 0, 95), 30, 0, {"hero"}),
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("hero", "lineup", "fallen", "wire")]
    views = {"blockout": VIEWS, "sculpt": SCULPT_VIEWS, "material": MATERIAL_VIEWS}[STAGE]
    for name, loc, target, lens, ortho, shown in views:
        cam = camera("cam_" + name, loc, target, lens, ortho)
        if not render:
            continue
        sc.camera = cam
        portrait = "reference" in name  # the reference composition is upright
        sc.render.resolution_x, sc.render.resolution_y = (900, 1600) if portrait else (1600, 900)
        cam.data.shift_y = REF_SHIFT if portrait else 0.0
        for c in toggled:
            c.hide_render = c.name not in shown
        bpy.data.collections["land"].hide_render = bool(ortho)  # far peaks would fill an ortho frame
        sc.render.filepath = str(SHOTS / f"a2_stele_{STAGE}_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    for c in toggled + [bpy.data.collections["land"]]:
        c.hide_render = False
    sc.camera = bpy.data.objects["cam_" + next(v[0] for v in views if "reference" in v[0])]


CONFIG_DIR = ROOT / "content/config/terrain_core/megaliths"


def export(v):
    """The game copy: the game mesh as one glTF (one node, Z up as authored, metres, flat
    normals, the atlas UVs), and the two manifests in content/config/terrain_core/megaliths/."""
    import json
    sc = bpy.context.scene
    me = bpy.data.objects["hero_" + HERO].data
    coll = bpy.data.collections.new("export")
    sc.collection.children.link(coll)
    ob = link("a2_carved_stele", me, (0, 0, 0), coll=coll)
    for o in sc.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    out = ASSET_DIR / "a2_carved_stele.gltf"
    bpy.ops.export_scene.gltf(filepath=str(out), export_format="GLTF_SEPARATE", use_selection=True,
                              export_yup=True, export_apply=True, export_normals=True,
                              export_texcoords=True, export_materials="NONE")
    lo = [min(p.co[i] for p in me.vertices) for i in range(3)]
    hi = [max(p.co[i] for p in me.vertices) for i in range(3)]
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    bpy.data.objects.remove(ob)
    bpy.data.collections.remove(coll)
    rel = lambda p: str(p.relative_to(ROOT)).replace("\\", "/")
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    cx, cy, rx, ry = pond_of(0.0, 0.0, v)
    asset = {
        "comment": "Magic megalith A-2, the carved stele (doc/megaliths/A2_CARVED_STELE.md). Generated by "
                   "tools/blender/megalith_a2_carved_stele.py -- --export; edit the script, not this file.",
        "id": "a2_carved_stele", "kind": "A", "approved": "2026-10-12",
        "model": rel(out), "blend": rel(ASSET_DIR / "stele_material.blend"),
        "reference": rel(ASSET_DIR / "reference.png"),
        "units": "metres, Z up in Blender (glTF is Y up)", "origin": "centre of the foot at ground level",
        "material": "a2_carved_stele.material.json",
        "variants": [{"node": "a2_carved_stele", "source_mesh": me.name, "tris": tris,
                      "bounds_m": [[round(x, 2) for x in lo], [round(x, 2) for x in hi]],
                      "shape": {k: v[k] for k in ("W", "D", "H", "taper", "tiers")},
                      "tier_joints_m": [round(j, 2) for j in TIERS["joints"]]}],
        "bake_source": {"object": "hero_" + HERO + "_high", "collection": "bake_source",
                        "tris": sum(len(p.vertices) - 2 for p in bpy.data.objects["hero_" + HERO + "_high"].data.polygons)},
        "placement": {
            "count": "one in the world (unique)", "scale": [0.9, 1.1], "rotation_deg": [0, 360],
            "lean_deg": [0.5, -0.4], "sink_m": SINK,
            "foot": {"pond": {"centre_m": [round(cx, 1), round(cy, 1)], "radii_m": [round(rx, 1), round(ry, 1)],
                              "water_m": WATER_Z, "depth_m": POND_DEPTH, "rim_m": 1.0,
                              "note": "oval pond in front of and left of the stele (in its own frame, -y is the "
                                      "front face), the foot at its back edge; turquoise water"},
                     "debris": "2-4 broken chunks at the foot half in the water, about 14 fallen pieces "
                               "round it (1.5-6 m), a few rolled further out"},
            "setting": "a wide stony valley between jagged ridges, boulders, open sky; seen from everywhere "
                       "in its biome (190 m)",
            "shadow": "a low sun from the left throws the long straight shadow band to the right of the "
                      "foot, as on the reference; the band is the shadow, not geometry"},
        "budget": {"tris": tris, "style_profile_A": [1500, 4000]},
        "engine": "pending: needs the UV-atlas PBR path in import_megalith.py and the props renderer "
                  "(doc/megaliths/A2_CARVED_STELE.md, the in-game section)",
    }
    (CONFIG_DIR / "a2_carved_stele.asset.json").write_text(json.dumps(asset, indent=2, ensure_ascii=False) + "\n",
                                                          encoding="utf-8")
    material = {
        "comment": "Material of A-2 (doc/megaliths/A2_CARVED_STELE.md). Generated by "
                   "tools/blender/megalith_a2_carved_stele.py; the maps are final, the knobs are what made them.",
        "id": "a2_carved_stele_pbr",
        "projection": "the mesh's own UVs: one atlas for the whole stele",
        "atlas": {"size_px": TEX, "px_per_m": PXM,
                  "sides": f"one strip round the stone, u = {U0:g} px + perimeter metres x {PXM:g} "
                           f"(from the front-left corner: front, right, back, left), v = (z + {Z0:g} m) x {PXM:g}; "
                           "one seam at the front-left corner",
                  "top": f"own island at ({TOP_U:g}, {TOP_V:g}) px, x and y in metres x {PXM:g}"},
        "filter": "nearest (hard pixels); mip maps fine",
        "textures": {
            "basecolor": {"file": rel(ASSET_DIR / "stele_basecolor_2048.png"), "space": "sRGB",
                          "what": "dark green-black stone, figures a touch lighter, cut lines darker, black "
                                  "scratches, dark runs below the joints, soot toward the foot; no AO in it"},
            "normal": {"file": rel(ASSET_DIR / "stele_normal_2048.png"), "space": "linear",
                       "convention": "tangent space, OpenGL (+Y up), MikkTSpace from the glTF UVs",
                       "what": "baked from the dense mesh (fine chips, pits, cracks, joint lips) with the "
                               "bas-relief figures, symbols, scratches and grain laid on (whiteout blend)"},
            "ao": {"file": rel(ASSET_DIR / "stele_ao_2048.png"), "space": "linear",
                   "what": "baked AO of the dense mesh x cut lines x cavities x scratches; multiply it into "
                           "ambient (in Blender it multiplies the base colour)"},
            "roughness": {"file": rel(ASSET_DIR / "stele_roughness_2048.png"), "space": "linear",
                          "what": "matte stone 0.84; relief tops worn smoother, runs smoother, cuts and "
                                  "soot rougher"}},
        "metallic": 0.0,
        "knobs": KNOBS,
    }
    (CONFIG_DIR / "a2_carved_stele.material.json").write_text(json.dumps(material, indent=2, ensure_ascii=False) + "\n",
                                                             encoding="utf-8")
    print("exported", out, "and manifests in", CONFIG_DIR)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--stage", choices=("blockout", "sculpt", "material"), default="material")
    ap.add_argument("--export", action="store_true", help="also write the glTF and the manifests (material stage)")
    args = ap.parse_args(argv)
    global STAGE
    STAGE = args.stage
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    build()
    if STAGE == "material":
        sc = bpy.context.scene
        low_ob, high_ob = bpy.data.objects["hero_" + HERO], bpy.data.objects["hero_" + HERO + "_high"]
        mat = stele_material(make_maps(sc, low_ob, high_ob, VARIANTS[HERO]))
        low_ob.data.materials[0] = mat
        deb = debris_material()
        for o in bpy.data.collections["hero"].objects:
            if o.name.startswith(("chunk_hero", "piece_hero")):
                o.data.materials[0] = deb
    setup_render()
    render_views(not args.no_render)
    if args.export and STAGE == "material":
        export(VARIANTS[HERO])
    blend = ASSET_DIR / f"stele_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
