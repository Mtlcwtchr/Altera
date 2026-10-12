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
Sculpt: wear of the ages on V2 only (see sculpted_stele); the carved silhouettes and glyphs will
be texture and normal map, not mesh.

    blender -b -P tools/blender/megalith_a2_carved_stele.py                      # sculpt stage
    blender -b -P tools/blender/megalith_a2_carved_stele.py -- --stage blockout
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
from mathutils import Vector, noise

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a2_carved_stele"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "sculpt"

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


def sculpted_stele(name, v, mat, seed):
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
        chips.append((Vector((sx * hw, sy * hd, z)), rnd.uniform(1.2, 4.5), rnd.uniform(0.4, 1.8)))
    for _ in range(18):  # the top edge, broken down
        t = rnd.random()
        hw, hd = half(H)
        side = rnd.randrange(4)
        p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
        chips.append((Vector((p[0], p[1], H)), rnd.uniform(2.0, 6.5), rnd.uniform(0.8, 3.2)))
    for j in joints:  # the joint lips crumble
        for _ in range(rnd.randint(3, 6)):
            hw, hd = half(j)
            side, t = rnd.randrange(4), rnd.random()
            p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
            chips.append((Vector((p[0], p[1], j + rnd.uniform(-0.8, 0.8))), rnd.uniform(1.0, 3.0), rnd.uniform(0.3, 1.0)))
    for _ in range(14):  # shallow pits on the faces
        z = rnd.uniform(4, H - 6)
        hw, hd = half(z)
        side, t = rnd.randrange(4), rnd.uniform(0.15, 0.85)
        p = [(-hw + 2 * hw * t, -hd), (hw, -hd + 2 * hd * t), (hw - 2 * hw * t, hd), (-hw, hd - 2 * hd * t)][side]
        chips.append((Vector((p[0], p[1], z)), rnd.uniform(1.5, 3.5), rnd.uniform(0.3, 0.8)))
    for zf in (0.38, 0.71):  # two big spalls off a corner
        sx, sy = rnd.choice(corners)
        z = H * zf + rnd.uniform(-8, 8)
        hw, hd = half(z)
        chips.append((Vector((sx * hw, sy * hd, z)), rnd.uniform(7.0, 10.0), rnd.uniform(2.5, 3.8)))

    # cracks: jagged vertical runs on the faces, from the top or a joint down 15-50 m
    cracks = []
    for _ in range(5):
        side = rnd.randrange(4)
        z1 = rnd.choice(joints + [H, H])
        z0 = max(2.0, z1 - rnd.uniform(15, 50))
        cracks.append((side, z0, z1, rnd.uniform(0.2, 0.8), rnd.uniform(0.6, 1.1), rnd.random() * 100))

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
        d = 0.14 * fbm(Vector((p.x, p.y, z)) * 0.08 + off)  # the faces are not planes
        # corners worn round and chunky, more at the top and the foot
        age = 1.0 + 1.2 * math.exp(-z / 25.0) + 1.0 * math.exp(-(H - z) / 30.0)
        if at_corner < 2.0:
            q = 0.5 + 0.5 * noise.noise(Vector((z * 0.12, side * 7.3, 3.1)) + off)
            d += age * (0.25 + 0.75 * round(q * 3) / 3) * (1 - at_corner / 2.0) ** 1.5
        # tier joints: an uneven groove
        for j in joints:
            gw = 0.8 * (1 + 0.5 * noise.noise(Vector((t * 0.2, j, 1.7)) + off))
            u = abs(z - j) / max(gw, 0.2)
            if u < 1:
                d += 0.7 * (1 - u * u) * (0.6 + 0.8 * abs(noise.noise(Vector((t * 0.3, j, 5.5)) + off)))
        # the waterline gnawed in
        if -1.5 < z < 3.5:
            d += math.exp(-((z - 0.6) / 1.1) ** 2) * (0.9 + 0.6 * noise.noise(Vector((p.x, p.y, 0)) * 0.2 + off))
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
            q.z = H + 0.35 * fbm(Vector((q.x, q.y, 0)) * 0.12 + off)
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
    cv = bm.verts.new(centre + Vector((0, 0, 0.3 * noise.noise(off))))
    for i in range(m):
        bm.faces.new((loops[-1][i], loops[-1][(i + 1) % m], cv))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = mesh_from(name, bm, mat)
    # smooth over the gentle unevenness, keep the chips and corners hard
    me.shade_smooth()
    me.set_sharp_from_angle(angle=math.radians(22))
    print(f"{name} sculpt: verts {len(me.vertices)}, tris {sum(len(p.vertices) - 2 for p in me.polygons)}, "
          f"chips {len(chips)}, cracks {len(cracks)}")
    return me


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
    if STAGE == "sculpt":  # only the approved variant goes on
        meshes[HERO] = sculpted_stele(HERO + "_sculpt", hv, stone, zlib.crc32(HERO.encode()))
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
    lean = (math.radians(0.5), math.radians(-0.4), 0.0) if STAGE == "sculpt" else (0.0, 0.0, 0.0)
    link("hero_" + HERO, meshes[HERO], (0, 0, 0), rot=lean, coll=hero)
    cx, cy, rx, ry = pond_of(0.0, 0.0, hv)
    pond("pond_hero", cx, cy, rx, ry, mats["water"], rnd, hero)
    foot_debris("hero", 0, 0, hv, stone, rnd, hero)
    if STAGE == "sculpt":
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
    d = Vector((math.cos(el), 0.12, -math.sin(el))).normalized()
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
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("hero", "lineup", "fallen")]
    views = VIEWS if STAGE == "blockout" else SCULPT_VIEWS
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


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--stage", choices=("blockout", "sculpt"), default="sculpt")
    args = ap.parse_args(argv)
    global STAGE
    STAGE = args.stage
    build()
    setup_render()
    render_views(not args.no_render)
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    blend = ASSET_DIR / f"stele_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
