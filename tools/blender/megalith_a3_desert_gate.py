"""Magic megalith A-3: the gate of the golden desert (doc/BIOME_PLAN.md 3.4, kind A:
blockout -> sculpt -> material, user approval at each step; doc/megaliths/A3_DESERT_GATE.md).

Reference (user, 14.10, painting; assets/terrain_core/megaliths/a3_desert_gate/reference.jpg):
a very tall pale slab standing straight up on a flat pale-yellow plain under a blue sky, about
5 times taller than wide, flat top. Its front face is a thin frame round one tall narrow
round-arched opening, black inside (a deep dark passage), running from the ground almost to the
top; a tiny figure stands in it. The left wall is clean and cut, a touch thicker toward the foot;
the right flank behind the frame is raw broken rock that widens downward into a craggy foot with
fallen blocks. Shallow water sheets lie round the base. A cloaked figure in the foreground.
User: first "polished like a mirror", then (material stage, 14.10) no mirror after all: plain
worn white stone, a little covered in sand dust.

Blockout: proportion variants side by side with 1.8 m figures, plus a shot repeating the
reference composition; V1_ref chosen (14.10), 190 m.
Sculpt: one game mesh (about 3.6k tris, no separate dense mesh): the cut block keeps big flat
planes with chamfered edges and a few flat-bottomed chips; the right side wall slants out toward
the foot with rare soft folds and a few small corner chips, fallen blocks as fractured hulls at its foot.
Material (approved 14.10): see the material section.

    blender -b -P tools/blender/megalith_a3_desert_gate.py                      # material stage
    blender -b -P tools/blender/megalith_a3_desert_gate.py -- --no-render --export   # game copy + manifests
    blender -b -P tools/blender/megalith_a3_desert_gate.py -- --stage sculpt
    blender -b -P tools/blender/megalith_a3_desert_gate.py -- --stage blockout
"""
import argparse
import math
import random
import sys
import zlib
from pathlib import Path

import bpy
import bmesh
from mathutils import Matrix, Vector, noise

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a3_desert_gate"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "material"

STONE = (0.80, 0.84, 0.88, 1)      # pale, cool: the polished stone takes the sky's blue
VOID = (0.004, 0.004, 0.006, 1)    # the passage
ROCK = (0.74, 0.79, 0.85, 1)       # the broken flank: same stone, rougher
SAND = (0.85, 0.72, 0.30, 1)
WATER = (0.30, 0.50, 0.75, 1)
HUMAN = (0.95, 0.10, 0.05, 1)
CLOAK = (0.012, 0.012, 0.014, 1)

SINK = 5.0          # the foot goes this far under the sand
LINEUP_Y = -2500.0

# Each variant (metres):
#   W, D, H      front width at the top, depth of the cut block, height
#   flare        the left wall thickens toward the foot by this (share of W)
#   open_w       width of the opening (share of W); lintel = solid stone above the arch (share of W)
#   niche        depth of the passage (share of D); >= 1 goes right through
#   crag         how far the broken right flank juts out past the frame at the foot (share of W)
#   rubble       size of the fallen blocks at the right foot (share of W)
VARIANTS = {
    "V1_ref": dict(W=36, D=30, H=190, flare=0.17, open_w=0.58, lintel=0.28, niche=0.8, crag=0.55, rubble=1.0),
    "V2_slender": dict(W=31, D=26, H=210, flare=0.12, open_w=0.52, lintel=0.30, niche=0.8, crag=0.35, rubble=0.8),
    "V3_broad": dict(W=44, D=36, H=175, flare=0.22, open_w=0.60, lintel=0.25, niche=0.8, crag=0.65, rubble=1.3),
    "V4_through": dict(W=36, D=30, H=190, flare=0.17, open_w=0.58, lintel=0.28, niche=1.2, crag=0.55, rubble=1.0),
}
HERO = "V1_ref"
LINEUP_X = {"V1_ref": -330, "V2_slender": -110, "V3_broad": 110, "V4_through": 330}
SITES = [(0.0, 0.0), (0.0, LINEUP_Y)]
# the reference camera: level (verticals parallel, as painted), frame shifted up so the horizon
# sits low at 72 %, 50 mm from about 390 m out and a little left; eye 0.65 m over the sand
# (the cloaked figure near the camera towers over the horizon, as painted)
REF_CAM = (-55.0, -390.0, 0.65)
REF_SHIFT = 0.22
REF_RES = (900, 1756)


def material(name, rgba, rough=0.9, metal=0.0, emit=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
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


def mesh_from(name, bm, mats):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = False
    for m in (mats if isinstance(mats, (list, tuple)) else [mats]):
        me.materials.append(m)
    return me


def tris(me):
    return sum(len(p.vertices) - 2 for p in me.polygons)


def fbm(p, octaves=3):
    s, a, f = 0.0, 1.0, 1.0
    for _ in range(octaves):
        s += a * noise.noise(p * f)
        a *= 0.5
        f *= 2.1
    return s


# ---------------------------------------------------------------- the gate
# Front face at y = -D/2 (the camera looks along +y). The cut block: clean flat faces; its left
# edge leans out toward the foot (the left wall thickens), its right edge stays plumb.

def block(v, bm):
    W, D, H = v["W"], v["D"], v["H"]
    zs = [-SINK] + [H * t for t in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0)]
    rings = []
    for z in zs:
        t = 1.0 - max(z, 0.0) / H
        xl = -W / 2 - v["flare"] * W * t ** 1.5
        y0, y1 = -D / 2, D / 2
        rings.append([bm.verts.new(p) for p in ((xl, y0, z), (W / 2, y0, z), (W / 2, y1, z), (xl, y1, z))])
    for a, b in zip(rings, rings[1:]):
        for i in range(4):
            bm.faces.new((a[i], a[(i + 1) % 4], b[(i + 1) % 4], b[i]))
    bm.faces.new(list(reversed(rings[0])))
    bm.faces.new(rings[-1])


def arch_cutter(v, name, void):
    """Tall round-arched prism cut into the front face."""
    W, D, H = v["W"], v["D"], v["H"]
    r = v["open_w"] * W / 2
    cx = W * 0.03          # the right frame is a touch narrower than the left (as painted)
    spring = H - v["lintel"] * W - r
    pts = [(cx - r, -SINK - 2), (cx + r, -SINK - 2)]
    pts += [(cx + r * math.cos(a), spring + r * math.sin(a)) for a in (math.pi * k / 16 for k in range(17))]
    bm = bmesh.new()
    y0, y1 = -D / 2 - 2, -D / 2 + v["niche"] * D
    front = [bm.verts.new((x, y0, z)) for x, z in pts]
    back = [bm.verts.new((x, y1, z)) for x, z in pts]
    n = len(pts)
    bm.faces.new(front)
    bm.faces.new(list(reversed(back)))
    for i in range(n):
        bm.faces.new((front[i], back[i], back[(i + 1) % n], front[(i + 1) % n]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return mesh_from(name, bm, void)


def crag(v, bm, rnd, seed):
    """The broken right flank: a column of noisy rings behind the frame, widening toward the foot,
    its front a little behind the cut face; reaches up to just under the top right corner."""
    W, D, H = v["W"], v["D"], v["H"]
    N, levels = 14, 26
    off = Vector((seed % 97, seed % 89, seed % 83)) * 1.7
    rings = []
    for j in range(levels + 1):
        z = -SINK + (H * 0.985 + SINK) * (j / levels) ** 0.85
        t = 1.0 - max(z, 0.0) / H
        ext = v["crag"] * W * (0.12 + 0.88 * t ** 2.6)   # a ragged strip from the top, widening low          # past the frame's right edge
        x0, x1 = W / 2 - 0.08 * W, W / 2 + ext + 0.5   # behind the right frame, clear of the opening
        y0, y1 = -D / 2 + 2.5 + 0.06 * W * (1 - t), D / 2 + 0.25 * D * t
        cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
        amp = 0.05 * W * (0.4 + t)
        ring = []
        for i in range(N):
            a = 2 * math.pi * i / N + 0.2 * j
            c, s = math.cos(a), math.sin(a)
            # superellipse: a blocky rock, not a tube
            px = cx + rx * math.copysign(abs(c) ** 0.6, c)
            py = cy + ry * math.copysign(abs(s) ** 0.6, s)
            p = Vector((px, py, z))
            d = fbm(p * 0.035 + off, 3) * amp + rnd.uniform(-0.25, 0.25) * amp
            ring.append(bm.verts.new((px + c * d, py + s * d, z + rnd.uniform(-0.8, 0.8) * (j not in (0, levels)))))
        rings.append(ring)
    for a, b in zip(rings, rings[1:]):
        for i in range(N):
            bm.faces.new((a[i], a[(i + 1) % N], b[(i + 1) % N], b[i]))
    bm.faces.new(list(reversed(rings[0])))
    top = bm.verts.new((sum(p.co.x for p in rings[-1]) / N, sum(p.co.y for p in rings[-1]) / N, H * 0.99))
    for i in range(N):
        bm.faces.new((rings[-1][i], rings[-1][(i + 1) % N], top))


def rock(bm, centre, size, rnd, seed):
    """A fallen block: an icosphere, flattened and knocked about by noise."""
    r = bmesh.ops.create_icosphere(bm, subdivisions=2, radius=1.0)
    off = Vector((seed % 31, seed % 37, seed % 41)) * 2.3
    sx, sy, sz = size
    for vert in r["verts"]:
        p = vert.co.copy()
        k = 1.0 + 0.35 * fbm(p * 1.3 + off, 2)
        vert.co = Vector((centre[0] + p.x * sx * k, centre[1] + p.y * sy * k, centre[2] + p.z * sz * k))


def gate(name, v, stone, void):
    seed = zlib.crc32(name.encode())
    rnd = random.Random(seed)
    W, D, H = v["W"], v["D"], v["H"]
    bm = bmesh.new()
    block(v, bm)
    me = mesh_from(name + "_block", bm, [stone, void])
    ob = link(name + "_block", me, (0, 0, 0))
    cut = link(name + "_cut", arch_cutter(v, name + "_cut", void), (0, 0, 0))
    mod = ob.modifiers.new("arch", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.solver = "EXACT"
    mod.object = cut
    try:
        mod.material_mode = "TRANSFER"
    except (AttributeError, TypeError):
        pass
    dg = bpy.context.evaluated_depsgraph_get()
    cut_me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    bpy.data.objects.remove(ob)
    bpy.data.objects.remove(cut)

    bm = bmesh.new()
    bm.from_mesh(cut_me)
    crag(v, bm, rnd, seed)
    # fallen blocks heaped at the right foot, the biggest against the crag
    s = v["rubble"] * W
    heap = [(0.55, 0.10, 0.40, 0.55), (0.95, -0.10, 0.30, 0.38), (1.30, 0.15, 0.24, 0.26),
            (0.80, 0.55, 0.30, 0.42), (1.55, -0.25, 0.10, 0.08), (1.20, -0.50, 0.08, 0.06)]
    for k, (fx, fy, r, hz) in enumerate(heap):
        x = W / 2 + fx * s * 0.8
        y = -D / 2 + fy * D + 0.2 * D
        rock(bm, (x, y, hz * s * 0.55), (r * s * rnd.uniform(0.9, 1.2), r * s * rnd.uniform(0.8, 1.1), hz * s),
             rnd, seed + k)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    out = bpy.data.meshes.new(name)
    bm.to_mesh(out)
    bm.free()
    for p in out.polygons:
        p.use_smooth = False
    for m in cut_me.materials:
        out.materials.append(m)
    bpy.data.meshes.remove(cut_me)
    print(f"{name}: {W}x{D}x{H} m, H/W {H / W:.2f}, opening {v['open_w'] * W:.1f} m, tris {tris(out)}")
    return out


# ---------------------------------------------------------------- sculpt (V1 approved 14.10)
# The cut block stays cut: big flat planes (they are the mirror), edges chamfered by a polisher,
# a few flat-bottomed chips where the frame met the falling rock and at the foot and the top.
# The broken flank and the fallen blocks are fractured rock: planar facets of convex chunks
# (as the painting's crag: sheer splinters, steps, slabs), not lumpy noise.

CHAMFER = 0.45   # polished edge chamfer (m)


def hull(bm_dst, pts):
    """Convex hull of the points added to bm_dst (a chunk of fractured rock: all faces planar)."""
    tmp = bmesh.new()
    vs = [tmp.verts.new(p) for p in pts]
    bmesh.ops.convex_hull(tmp, input=vs)
    for v in [v for v in tmp.verts if not v.link_faces]:
        tmp.verts.remove(v)
    me = bpy.data.meshes.new("_hull")
    tmp.to_mesh(me)
    tmp.free()
    bm_dst.from_mesh(me)
    bpy.data.meshes.remove(me)


def chunk_pts(rnd, x0, x1, y0, y1, z0, z1, n=14, flat=None, knock=0.25):
    """Points scattered over a box's surface, so the hull is a broken block with big facets;
    flat='-y' keeps one face of it a clean plane (a piece of the cut, polished frame)."""
    pts = []
    for _ in range(n):
        p = [rnd.uniform(x0, x1), rnd.uniform(y0, y1), rnd.uniform(z0, z1)]
        ax = rnd.randrange(3)
        lo, hi = ((x0, x1), (y0, y1), (z0, z1))[ax]
        p[ax] = lo if rnd.random() < 0.5 else hi
        # knock the corners in: bigger facets
        p[ax] += (rnd.uniform(0, knock) * (hi - lo)) * (1 if p[ax] == lo else -1)
        pts.append(p)
    if flat == "-y":
        for k in range(5):
            pts.append([rnd.uniform(x0, x1), y0, rnd.uniform(z0, z1)])
    return pts


def right_ext(v, z):
    """How far the right side wall leans out past the frame at height z (user, 14.10: the side
    face itself goes down at a slight slant, no step)."""
    t = 1.0 - min(max(z, 0.0), v["H"]) / v["H"]
    return v["crag"] * v["W"] * t ** 1.2


def sloped_block(v, bm, seed):
    """The cut block with its right side wall slanting out toward the foot as one face: rare
    folds (soft vertical creases, ridged noise stretched up the height). The front face simply widens with it; the left wall flares as before."""
    W, D, H = v["W"], v["D"], v["H"]
    K = 10                       # segments across the side wall
    off = Vector((seed % 97, seed % 89, seed % 83)) * 1.7
    zs = [-SINK] + [H * (j / 40) for j in range(41)]
    rings = []
    for z in zs:
        t = 1.0 - max(z, 0.0) / H
        xl = -W / 2 - v["flare"] * W * t ** 1.5
        xr = W / 2 + right_ext(v, z)
        y0, y1 = -D / 2, D / 2
        ring = [(xl, y0, z)]
        for k in range(K + 1):
            y = y0 + (y1 - y0) * k / K
            q = Vector((y * 0.07, z * 0.011, 0.0)) + off
            fold = 0.55 - abs(noise.noise(q))                      # ridged: creases and folds
            fold *= 0.5 + 0.5 * noise.noise(Vector((y * 0.02, z * 0.004, 3.0)) + off)  # rare
            edge = 0.35 if k in (0, K) else 1.0                    # keep the corners near true
            ring.append((xr + 1.6 * (0.4 + t) * fold * edge, y, z))
        ring.append((xl, y1, z))
        rings.append([bm.verts.new(p) for p in ring])
    m = len(rings[0])
    for a, b in zip(rings, rings[1:]):
        for i in range(m):
            f = bm.faces.new((a[i], a[(i + 1) % m], b[(i + 1) % m], b[i]))
            if 1 <= i < K + 1:   # the side wall
                f.material_index = 2
    bm.faces.new(list(reversed(rings[0])))
    bm.faces.new(rings[-1])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)


def rubble_chunks(v, bm, rnd):
    """Fallen blocks heaped at the right foot, the biggest against the crag; two are pieces of the
    cut frame (one clean face)."""
    W, D = v["W"], v["D"]
    s = v["rubble"] * W
    heap = [(0.55, 0.10, 0.40, 0.55), (0.95, -0.10, 0.30, 0.38), (1.30, 0.15, 0.24, 0.26),
            (0.80, 0.55, 0.30, 0.42), (1.55, -0.25, 0.10, 0.08), (1.20, -0.50, 0.08, 0.06),
            (1.75, 0.05, 0.07, 0.05), (0.40, -0.55, 0.05, 0.04)]
    for k, (fx, fy, r, hz) in enumerate(heap):
        x = W / 2 + right_ext(v, 0.0) * 0.8 + fx * s * 0.6   # at the foot of the slanted wall
        y = -D / 2 + fy * D + 0.2 * D
        rx, ry, rz = r * s * rnd.uniform(0.9, 1.2), r * s * rnd.uniform(0.8, 1.1), hz * s
        hull(bm, chunk_pts(rnd, x - rx, x + rx, y - ry, y + ry, -1.5, rz * 1.4, n=16,
                           flat="-y" if k in (4, 6) else None))


def chip_cutters(v, rnd):
    """A few small flat-bottomed chips on the corners only (user, 14.10: no holes in the faces):
    the top corners and a couple on the outer vertical edges."""
    W, D, H = v["W"], v["D"], v["H"]
    yf, yb = -D / 2, D / 2
    xl = lambda z: -W / 2 - v["flare"] * W * (1 - z / H) ** 1.5
    xr = lambda z: W / 2 + right_ext(v, z)
    spots = [(xl(H), yf, H, 1.6), (xr(H), yb, H, 1.8),          # top corners
             (xl(H * 0.22), yf, H * 0.22, 1.2), (xr(H * 0.48), yf, H * 0.48, 1.3)]   # outer edges
    bm = bmesh.new()
    for x, y, z, s in spots:
        hull(bm, [(x + rnd.uniform(-s, s), y + rnd.uniform(-s, s), z + rnd.uniform(-s, s) * 1.4)
                  for _ in range(9)])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def sculpted_gate(name, v, stone, void, rock):
    seed = zlib.crc32(name.encode())
    rnd = random.Random(seed)
    W, D, H = v["W"], v["D"], v["H"]
    bm = bmesh.new()
    sloped_block(v, bm, seed)
    ob = link(name + "_block", mesh_from(name + "_block", bm, [stone, void, rock]), (0, 0, 0))
    cut = link(name + "_cut", arch_cutter(v, name + "_cut", void), (0, 0, 0))
    chips = link(name + "_chips", mesh_from(name + "_chips", chip_cutters(v, rnd), stone), (0, 0, 0))
    for label, op, other in (("arch", "DIFFERENCE", cut), ("chips", "DIFFERENCE", chips)):
        mod = ob.modifiers.new(label, "BOOLEAN")
        mod.operation = op
        mod.solver = "EXACT"
        mod.object = other
        mod.use_self = label == "chips"   # the chips overlap each other
        mod.use_hole_tolerant = label == "chips"
        try:
            mod.material_mode = "TRANSFER"
        except (AttributeError, TypeError):
            pass
    # the polisher's chamfer on the cut stone only (crag edges stay sharp: angle limit and the
    # chips' fresh breaks are short)
    bev = ob.modifiers.new("chamfer", "BEVEL")
    bev.width = CHAMFER
    bev.segments = 1
    bev.limit_method = "ANGLE"
    bev.angle_limit = math.radians(60)
    bev.harden_normals = False
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    for o in (ob, cut, chips):
        bpy.data.objects.remove(o)
    bm = bmesh.new()
    bm.from_mesh(me)
    for f in bm.faces:   # the folded side wall shades smooth, the cut planes and the boulders flat
        f.smooth = f.material_index == 2
    n0 = len(bm.faces)
    rubble_chunks(v, bm, rnd)
    for f in list(bm.faces)[n0:]:
        f.material_index = 2
        f.smooth = False
    # nothing under the sand: drop faces wholly below the sink line
    gone = [f for f in bm.faces if all(p.co.z < -SINK + 0.6 for p in f.verts)]
    bmesh.ops.delete(bm, geom=gone, context="FACES")
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    out = bpy.data.meshes.new(name)
    bm.to_mesh(out)
    bm.free()
    for m in me.materials:
        out.materials.append(m)
    bpy.data.meshes.remove(me)
    print(f"{name}: sculpt, tris {tris(out)}")
    return out


# ---------------------------------------------------------------- material: worn white stone
# User, 14.10: no mirror after all; plain worn white stone, a little covered in sand dust.
# Painted procedurally in object space (metres) with labelled knobs, then baked in Cycles onto
# one 2048 atlas (smart UV islands) for the game: basecolor (sRGB), normal (OpenGL tangent: the
# worn relief) and surface (R = AO, G = roughness, B = metallic, kept 0). Desert, so no moss:
# its place is taken by sand dust laid by noise (on what faces up, in a drift at the foot), with
# the same kind of knobs. A sand-blasted band at the foot, jagged at its top.

TEX = 2048
KNOBS = dict(
    rough=0.78,             # roughness of the worn stone
    relief_m=0.12,          # depth of the worn stone's relief (pitting and grit) in the normal map
    veins=0.4,              # faint grey veins in the stone
    mottle=0.5,             # soft white to ivory mottling
    grain=0.07,             # chunky texel grain (retro)
    abrasion=1.0,           # the sand-blasted band at the foot: duller, warmer, rougher
    abrasion_m=14.0,        # its mean height (m); its top edge is jagged by noise
    dust_film=0.18,         # thin film of sand dust over everything
    dust=0.65,              # sand dust patches on what faces up and in a drift at the foot
    dust_scale=0.045,       # dust patch size (1/m)
    dust_foot_m=5.0,        # height of the dust drift at the foot
)
STONE_COOL = (0.72, 0.71, 0.68)
STONE_IVORY = (0.78, 0.74, 0.64)
VEIN = (0.55, 0.55, 0.54)
DUST = (0.78, 0.66, 0.40)


def _set(nt, sock, v):
    if hasattr(v, "is_output"):
        nt.links.new(v, sock)
    else:
        sock.default_value = v


class Graph:
    """Small helper to wire shader nodes without the boilerplate."""

    def __init__(self, nt):
        self.nt = nt

    def node(self, kind, **inputs):
        nd = self.nt.nodes.new(kind)
        for k, v in inputs.items():
            _set(self.nt, nd.inputs[k], v)
        return nd

    def knob(self, name):
        nd = self.nt.nodes.new("ShaderNodeValue")
        nd.label = nd.name = "knob_" + name
        nd.outputs[0].default_value = KNOBS[name]
        return nd.outputs[0]

    def math(self, op, a, b=0.0, clamp=False):
        nd = self.nt.nodes.new("ShaderNodeMath")
        nd.operation = op
        nd.use_clamp = clamp
        _set(self.nt, nd.inputs[0], a)
        _set(self.nt, nd.inputs[1], b)
        return nd.outputs[0]

    def mrange(self, x, a, b, c=0.0, d=1.0):
        nd = self.nt.nodes.new("ShaderNodeMapRange")
        nd.clamp = True
        for k, v in (("Value", x), ("From Min", a), ("From Max", b), ("To Min", c), ("To Max", d)):
            _set(self.nt, nd.inputs[k], v)
        return nd.outputs["Result"]

    def noise(self, vec, scale, detail=2.0, distortion=0.0):
        nd = self.node("ShaderNodeTexNoise", Vector=vec, Scale=scale, Detail=detail, Distortion=distortion)
        return nd.outputs["Fac"]

    def mix(self, a, b, f):
        nd = self.nt.nodes.new("ShaderNodeMix")
        nd.data_type = "RGBA"
        for k, v in (("A", a), ("B", b), ("Factor", f)):
            if not hasattr(v, "is_output") and k != "Factor":
                v = (*v, 1.0)
            _set(self.nt, nd.inputs[k], v)
        return nd.outputs["Result"]


def paint(m, side):
    """The procedural paint of one material slot: side=False the polished cut stone, True the
    slanted side wall and the fallen blocks. Returns the channel sockets for the bake."""
    m.use_nodes = True
    nt = m.node_tree
    g = Graph(nt)
    co = g.node("ShaderNodeTexCoord").outputs["Object"]
    geo = g.node("ShaderNodeNewGeometry")
    z = g.node("ShaderNodeSeparateXYZ", Vector=co).outputs["Z"]
    nz = g.node("ShaderNodeSeparateXYZ", Vector=geo.outputs["Normal"]).outputs["Z"]
    # stone: soft mottling, faint veins in places, chunky grain
    mottle = g.math("MULTIPLY", g.mrange(g.noise(co, 0.012, 3.0), 0.3, 0.7), g.knob("mottle"))
    base = g.mix(STONE_COOL, STONE_IVORY, mottle)
    vn = g.noise(co, 0.006, 2.0, 3.0)
    vein = g.mrange(g.math("ABSOLUTE", g.math("SUBTRACT", vn, 0.5)), 0.0, 0.012, 1.0, 0.0)
    vein = g.math("MULTIPLY", vein, g.mrange(g.noise(co, 0.01), 0.45, 0.6))
    base = g.mix(base, VEIN, g.math("MULTIPLY", vein, g.knob("veins")))
    cell = g.node("ShaderNodeVectorMath", Vector=co)
    cell.operation = "SNAP"
    cell.inputs[1].default_value = (0.3, 0.3, 0.3)
    wn = g.node("ShaderNodeTexWhiteNoise", Vector=cell.outputs["Vector"]).outputs["Value"]
    gr = g.math("ADD", g.math("MULTIPLY", g.math("SUBTRACT", wn, 0.5), g.knob("grain")), 1.0)
    sc = g.node("ShaderNodeVectorMath", Vector=base)
    sc.operation = "SCALE"
    nt.links.new(gr, sc.inputs["Scale"])
    base = sc.outputs["Vector"]
    # sand-blasted foot: matte, duller and warmer below a jagged line
    edge = g.math("ADD", g.knob("abrasion_m"), g.math("MULTIPLY", g.math("SUBTRACT", g.noise(co, 0.03, 3.0), 0.5), 22.0))
    ab = g.math("MULTIPLY", g.mrange(z, g.math("SUBTRACT", edge, 3.0), g.math("ADD", edge, 3.0), 1.0, 0.0),
                g.knob("abrasion"))
    base = g.mix(base, STONE_IVORY, g.math("MULTIPLY", ab, 0.5))
    # sand dust: on what faces up, and a drift at the foot, both broken up by noise
    up = g.mrange(nz, 0.35, 0.8)
    foot = g.mrange(z, 0.0, g.knob("dust_foot_m"), 1.0, 0.0)
    lo = g.math("SUBTRACT", 0.75, g.math("MULTIPLY", g.knob("dust"), 0.4))
    patch = g.mrange(g.noise(co, g.knob("dust_scale"), 3.0), lo, g.math("ADD", lo, 0.1))
    dust = g.math("MULTIPLY", g.math("MAXIMUM", up, foot), patch)
    dust = g.math("MULTIPLY", dust, g.math("MULTIPLY", g.knob("dust"), 2.0), clamp=True)
    base = g.mix(base, DUST, dust)
    # worn white stone with relief (coarse pitting and fine grit), no polish left (user, 14.10:
    # no mirror), under a thin film of sand dust
    relief = g.math("ADD", g.math("MULTIPLY", g.noise(co, 0.9, 4.0), 0.6), g.math("MULTIPLY", g.noise(co, 0.18, 3.0), 0.4))
    base = g.mix(base, (0.62, 0.60, 0.56), g.math("MULTIPLY", g.mrange(relief, 0.55, 0.3), 0.35))   # pits a touch darker
    base = g.mix(base, DUST, g.knob("dust_film"))
    rough = g.math("ADD", g.knob("rough"), g.math("MULTIPLY", g.math("SUBTRACT", relief, 0.5), 0.25))
    rough = g.math("ADD", rough, g.math("MULTIPLY", ab, 0.15))
    rough = g.math("MAXIMUM", rough, g.math("MULTIPLY", dust, 0.92), clamp=True)
    metal = g.math("MULTIPLY", 0.0, 0.0)          # plain stone: the bake keeps a metallic channel at 0
    bump = g.node("ShaderNodeBump", Height=relief, Strength=1.0)
    nt.links.new(g.knob("relief_m"), bump.inputs["Distance"])
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")
    for k, v in (("Base Color", base), ("Roughness", rough), ("Metallic", metal), ("Normal", bump.outputs["Normal"])):
        nt.links.new(v, bsdf.inputs[k])
    return dict(base=base, rough=rough, metal=metal)


def uv_unwrap(ob):
    for o in bpy.context.scene.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(50), island_margin=0.004, area_weight=0.0)
    bpy.ops.object.mode_set(mode="OBJECT")


def bake_all(ob, channels):
    """Cycles: EMIT bakes of the painted channels, NORMAL (tangent) and AO, every slot into one
    image per pass. Returns numpy arrays TEX x TEX x 4."""
    import numpy as np
    sc = bpy.context.scene
    try:
        sc.render.engine = "CYCLES"
    except TypeError as e:
        print("engine:", e)
    sc.cycles.device = "CPU"
    keep = {o.name: o.hide_render for o in sc.objects}
    for o in sc.objects:
        o.hide_render = o is not ob
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    sc.render.bake.use_selected_to_active = False
    sc.render.bake.margin = 8
    out = {}
    for kind in ("base", "rough", "metal", "NORMAL", "AO"):
        img = bpy.data.images.new(f"bake_{kind}", TEX, TEX, float_buffer=True)
        img.colorspace_settings.name = "Non-Color"
        undo = []
        for m in ob.data.materials:
            nt = m.node_tree
            tn = nt.nodes.new("ShaderNodeTexImage")
            tn.image = img
            nt.nodes.active = tn
            em = prev = None
            outn = next(nd for nd in nt.nodes if nd.type == "OUTPUT_MATERIAL")
            src = channels.get(m.name, {}).get(kind)
            if kind in ("base", "rough", "metal"):
                em = nt.nodes.new("ShaderNodeEmission")
                prev = outn.inputs["Surface"].links[0].from_socket if outn.inputs["Surface"].links else None
                if src is not None:
                    nt.links.new(src, em.inputs["Color"])
                else:
                    em.inputs["Color"].default_value = (0, 0, 0, 1)
                nt.links.new(em.outputs["Emission"], outn.inputs["Surface"])
            undo.append((nt, tn, em, outn, prev))
        if kind in ("base", "rough", "metal"):
            sc.cycles.samples = 1
            bpy.ops.object.bake(type="EMIT")
        else:
            sc.cycles.samples = 1 if kind == "NORMAL" else 32
            bpy.ops.object.bake(type=kind, normal_space="TANGENT")
        out[kind] = np.array(img.pixels[:], dtype=np.float32).reshape(TEX, TEX, 4)
        for nt, tn, em, outn, prev in undo:
            nt.nodes.remove(tn)
            if em:
                nt.nodes.remove(em)
                if prev:
                    nt.links.new(prev, outn.inputs["Surface"])
        print("baked", kind)
    for o in sc.objects:
        o.hide_render = keep.get(o.name, False)
    return out


def save_map(name, arr, srgb):
    import numpy as np
    rgba = np.concatenate([np.clip(arr[:, :, :3], 0, 1), np.ones((TEX, TEX, 1), np.float32)], axis=2)
    img = bpy.data.images.new(name, TEX, TEX)
    img.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    img.pixels.foreach_set(rgba.astype(np.float32).ravel())
    img.filepath_raw = str(ASSET_DIR / f"{name}.png")
    img.file_format = "PNG"
    img.save()
    print("saved", img.filepath_raw)
    return img


def make_material(ob):
    """Paint, bake, and put the baked maps on the gate. The painted materials stay in the .blend
    (gate_paint_*, on a hidden copy) for tuning the knobs."""
    import numpy as np
    stone, void, rock = ob.data.materials[0], ob.data.materials[1], ob.data.materials[2]
    stone.name, rock.name = "gate_paint_stone", "gate_paint_side"
    channels = {stone.name: paint(stone, False), rock.name: paint(rock, True)}   # void bakes black
    uv_unwrap(ob)
    b = bake_all(ob, channels)
    base = np.clip(b["base"][:, :, :3], 0, 1)
    void_mask = base.max(axis=2) < 0.02
    base = np.where(void_mask[:, :, None], VOID[0], base)
    base_srgb = np.where(base <= 0.0031308, base * 12.92, 1.055 * np.power(base, 1 / 2.4) - 0.055)
    rough = np.where(void_mask, 0.95, b["rough"][:, :, 0])
    metal = np.where(void_mask, 0.0, b["metal"][:, :, 0])
    surface = np.stack([b["AO"][:, :, 0], rough, metal], axis=2)
    imgs = dict(base=save_map("gate_basecolor_2048", base_srgb, True),
                normal=save_map("gate_normal_2048", b["NORMAL"], False),
                surface=save_map("gate_surface_2048", surface, False))
    keep = bpy.data.objects.new("gate_paint_keeper", ob.data.copy())   # the painted version, hidden
    keep.hide_render = True
    bpy.context.scene.collection.objects.link(keep)
    keep.hide_set(True)

    m = bpy.data.materials.new("gate_material")
    m.use_nodes = True
    nt = m.node_tree
    g = Graph(nt)
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")

    def tex(img):
        nd = nt.nodes.new("ShaderNodeTexImage")
        nd.image = img
        nd.interpolation = "Closest"  # chunky pixels
        return nd

    tb, tn, ts = tex(imgs["base"]), tex(imgs["normal"]), tex(imgs["surface"])
    sep = g.node("ShaderNodeSeparateColor", Color=ts.outputs["Color"])
    mul = nt.nodes.new("ShaderNodeMix")
    mul.data_type = "RGBA"
    mul.blend_type = "MULTIPLY"
    mul.inputs["Factor"].default_value = 1.0
    nt.links.new(tb.outputs["Color"], mul.inputs["A"])
    nt.links.new(sep.outputs["Red"], mul.inputs["B"])
    nt.links.new(mul.outputs["Result"], bsdf.inputs["Base Color"])
    nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    nm = g.node("ShaderNodeNormalMap", Color=tn.outputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    ob.data = ob.data.copy()
    for i in range(len(ob.data.materials)):
        ob.data.materials[i] = m
    return m


# ---------------------------------------------------------------- the land

def ground_height(x, y):
    """Flat pan round each site, low dunes rising beyond."""
    d = min(math.hypot(x - sx, y - sy) for sx, sy in SITES)
    k = min(1.0, max(0.0, (d - 900) / 1500))
    dune = 18 * (math.sin(x * 0.0021 + 0.7 * math.sin(y * 0.0013)) * 0.5 + 0.5) ** 2 + 6 * math.sin(y * 0.0047 + x * 0.001)
    return k * dune


def ground(mat):
    bm = bmesh.new()
    step = 40.0
    xs = [-8000 + step * i for i in range(int(16000 / step) + 1)]
    grid = [[bm.verts.new((x, y, ground_height(x, y))) for x in xs] for y in xs]
    for j in range(len(xs) - 1):
        for i in range(len(xs) - 1):
            bm.faces.new((grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]))
    return link("ground", mesh_from("ground", bm, mat), (0, 0, 0))


def water_sheet(name, x, y, v, mat, rnd, coll):
    """Shallow sheets of water round the foot, mirror-flat (on the reference they reflect the gate)."""
    W, D = v["W"], v["D"]
    bm = bmesh.new()
    n = 40
    pts = []
    for k in range(n):
        a = 2 * math.pi * k / n
        rx = (2.4 * W if math.cos(a) > 0 else 1.6 * W) * rnd.uniform(0.85, 1.15)
        ry = (0.9 * D if math.sin(a) > 0 else 1.1 * D) * rnd.uniform(0.7, 1.2)
        pts.append(bm.verts.new((x + W * 0.4 + math.cos(a) * rx, y + math.sin(a) * ry, 0.06)))
    bm.faces.new(pts)
    return link(name, mesh_from(name, bm, mat), (0, 0, 0), coll=coll)


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


def cloaked(name, loc, mat, coll):
    """The hooded figure of the painting: a long cloak to the ground (1.9 m)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=16, radius1=0.42, radius2=0.17, depth=1.6,
                          matrix=Matrix.Translation((0, 0, 0.8)))
    bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=0.17,
                              matrix=Matrix.Translation((0, 0.02, 1.72)))
    return link(name, mesh_from(name, bm, mat), loc, coll=coll)


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    stone = material(f"gate_{STAGE}", STONE, rough=0.18)
    void = material("void", VOID, rough=1.0)
    sand = material("sand", SAND, rough=0.95)
    water = material("water", WATER, rough=0.02, metal=0.0)
    red = material("human", HUMAN)
    cloak = material("cloak", CLOAK, rough=0.9)
    ground(sand)
    hero, lineup = collection("hero"), collection("lineup")
    rnd = random.Random(7)

    rock = material(f"rock_{STAGE}", ROCK, rough=0.3)
    meshes = {name: gate(name, v, stone, void) for name, v in VARIANTS.items()} if STAGE == "blockout" else {}
    if STAGE != "blockout":
        meshes[HERO] = sculpted_gate(HERO, VARIANTS[HERO], stone, void, rock)
    for name, v in (VARIANTS.items() if STAGE == "blockout" else ()):
        x, y = LINEUP_X[name], LINEUP_Y
        link(name, meshes[name], (x, y, 0), coll=lineup)
        cx = x + v["W"] * 0.03
        for k, px in enumerate((cx - 1.5, cx + 1.5)):  # two in the opening
            human(f"human_{name}_{k}", (px, y - v["D"] / 2 + 3 + 2 * k, 0), red, lineup)
        human(f"human_{name}_front", (x - v["W"] * 0.2, y - v["D"] / 2 - 25, 0), red, lineup)

    hv = VARIANTS[HERO]
    link("hero_" + HERO, meshes[HERO], (0, 0, 0), coll=hero)
    wire = collection("wire")
    if STAGE == "sculpt":
        wob = link("hero_wire", meshes[HERO], (0, 0, 0), coll=wire)
        mod = wob.modifiers.new("wire", "WIREFRAME")
        mod.thickness = 0.12
        mod.use_replace = True
        mod.use_even_offset = False
        for slot in wob.material_slots:
            slot.link = "OBJECT"
            slot.material = material("wire", (0.02, 0.02, 0.02, 1))
    water_sheet("water_hero", 0, -hv["D"] * 0.35, hv, water, rnd, hero)
    human("human_hero_door", (hv["W"] * 0.03, -hv["D"] / 2 + 4, 0), red, hero)
    # the cloaked figure near the camera, left of the gate, as in the painting
    cam = Vector(REF_CAM)
    to = (Vector((0, 0, 0)) - cam).normalized()
    side = Vector((to.y, -to.x, 0)).normalized()
    p = cam + to * 6.0 - side * 0.6
    cloaked("cloak_hero", (p.x, p.y, 0), cloak, hero)


# ---------------------------------------------------------------- shots

def setup_render():
    sc = bpy.context.scene
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = engine
            break
        except TypeError as e:
            print("engine:", e)
    sc.render.image_settings.file_format = "PNG"
    sc.view_settings.view_transform = "Standard"
    try:   # the water sheets reflect the gate
        sc.eevee.use_raytracing = True
        sc.eevee.ray_tracing_options.resolution_scale = "1"
    except (AttributeError, TypeError) as e:
        print("raytracing:", e)
    # high sun from the right on the camera side: the front frame and the broken flank lit, as painted
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 4.0
    sun.angle = math.radians(1.0)
    ob = bpy.data.objects.new("sun", sun)
    el = math.radians(38)
    d = Vector((-0.6, 0.8, 0)).normalized() * math.cos(el) + Vector((0, 0, -math.sin(el)))
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    nt = world.node_tree
    bg = next(n for n in nt.nodes if n.type == "BACKGROUND")
    # blue sky, paler at the horizon
    grad = nt.nodes.new("ShaderNodeTexGradient")
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    nt.links.new(coord.outputs["Generated"], sep.inputs[0])
    nt.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
    ramp.color_ramp.elements[0].position = 0.5
    ramp.color_ramp.elements[0].color = (0.10, 0.30, 0.62, 1)
    ramp.color_ramp.elements[1].position = 0.75
    ramp.color_ramp.elements[1].color = (0.04, 0.16, 0.45, 1)
    nt.links.new(ramp.outputs["Color"], bg.inputs["Color"])
    nt.nodes.remove(grad)
    bg.inputs["Strength"].default_value = 1.0
    sc.world = world


def camera(name, loc, target, lens, ortho=0.0, level=False):
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    cam.clip_end = 14000
    if ortho:
        cam.type = "ORTHO"
        cam.ortho_scale = ortho
    ob = bpy.data.objects.new(name, cam)
    ob.location = loc
    d = Vector(target) - Vector(loc)
    if level:
        d.z = 0.0
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(ob)
    return ob


Y = LINEUP_Y
# name, camera, target, lens, ortho scale, collections shown
VIEWS = [
    ("01_lineup_front", (0, Y - 900, 40), (0, Y, 95), 32, 0, {"lineup"}),
    ("02_lineup_three_quarter", (650, Y - 620, 120), (0, Y, 80), 32, 0, {"lineup"}),
    ("03_proportions_ortho", (0, Y - 700, 105), (0, Y, 105), 50, 900, {"lineup"}),
    ("04_reference_shot", REF_CAM, (6, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("05_hero_right_crag", (300, -260, 40), (20, 0, 80), 32, 0, {"hero"}),
    ("06_hero_back_left", (-260, 300, 50), (0, 0, 90), 32, 0, {"hero"}),
    ("07_opening_foot", (-25, -110, 6), (2, 0, 22), 30, 0, {"hero"}),
]
SCULPT_VIEWS = [
    ("01_reference_shot", REF_CAM, (6, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("02_right_crag", (300, -260, 40), (20, 0, 80), 32, 0, {"hero"}),
    ("03_crag_close_low", (95, -95, 12), (28, 0, 22), 32, 0, {"hero"}),
    ("04_top_close", (-60, -95, 200), (0, 0, 182), 35, 0, {"hero"}),
    ("05_opening_foot", (-25, -110, 6), (2, 0, 22), 30, 0, {"hero"}),
    ("06_back_left", (-260, 300, 50), (0, 0, 90), 32, 0, {"hero"}),
    ("07_wire_right_crag", (300, -260, 40), (20, 0, 80), 32, 0, {"hero", "wire"}),
    ("08_wire_crag_close_low", (95, -95, 12), (28, 0, 22), 32, 0, {"hero", "wire"}),
]
MATERIAL_VIEWS = [
    ("01_reference_shot", REF_CAM, (6, 0, REF_CAM[2]), 50, 0, {"hero"}),
    ("02_right_three_quarter", (300, -260, 40), (20, 0, 80), 32, 0, {"hero"}),
    ("03_front_close", (-40, -120, 40), (0, 0, 45), 35, 0, {"hero"}),
    ("04_side_wall_close", (95, -95, 12), (28, 0, 22), 32, 0, {"hero"}),
    ("05_opening_foot", (-25, -110, 6), (2, 0, 22), 30, 0, {"hero"}),
    ("06_top_close", (-60, -95, 200), (0, 0, 182), 35, 0, {"hero"}),
    ("07_back_left", (-260, 300, 50), (0, 0, 90), 32, 0, {"hero"}),
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("hero", "lineup", "wire")]
    views = {"blockout": VIEWS, "sculpt": SCULPT_VIEWS, "material": MATERIAL_VIEWS}[STAGE]
    for name, loc, target, lens, ortho, shown in views:
        portrait = "reference" in name
        cam = camera("cam_" + name, loc, target, lens, ortho, level=portrait)
        if not render:
            continue
        sc.camera = cam
        sc.render.resolution_x, sc.render.resolution_y = REF_RES if portrait else (1600, 900)
        cam.data.shift_y = REF_SHIFT if portrait else 0.0
        for c in toggled:
            c.hide_render = c.name not in shown
        sc.render.filepath = str(SHOTS / f"a3_gate_{STAGE}_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    for c in toggled:
        c.hide_render = False
    sc.camera = bpy.data.objects["cam_" + next(v[0] for v in views if "reference" in v[0])]


CONFIG_DIR = ROOT / "content/config/terrain_core/megaliths"
IDENT = "a3_desert_gate"


def export(v):
    """The game copy: the game mesh as one glTF node (Z up as authored, metres, its own normals,
    the atlas UVs) and the two manifests in content/config/terrain_core/megaliths/."""
    import json
    sc = bpy.context.scene
    me = bpy.data.objects["hero_" + HERO].data
    coll = bpy.data.collections.new("export")
    sc.collection.children.link(coll)
    ob = link(IDENT, me, (0, 0, 0), coll=coll)
    for o in sc.objects:
        o.select_set(o is ob)
    bpy.context.view_layer.objects.active = ob
    out = ASSET_DIR / f"{IDENT}.gltf"
    bpy.ops.export_scene.gltf(filepath=str(out), export_format="GLTF_SEPARATE", use_selection=True,
                              export_yup=True, export_apply=True, export_normals=True,
                              export_texcoords=True, export_materials="NONE")
    lo = [min(p.co[i] for p in me.vertices) for i in range(3)]
    hi = [max(p.co[i] for p in me.vertices) for i in range(3)]
    bpy.data.objects.remove(ob)
    bpy.data.collections.remove(coll)
    rel = lambda p: str(p.relative_to(ROOT)).replace("\\", "/")
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    asset = {
        "comment": "Magic megalith A-3, the gate of the golden desert (doc/megaliths/A3_DESERT_GATE.md). Generated "
                   "by tools/blender/megalith_a3_desert_gate.py -- --export; edit the script, not this file.",
        "id": IDENT, "kind": "A", "approved": "2026-10-14",
        "model": rel(out), "blend": rel(ASSET_DIR / "gate_material.blend"),
        "reference": rel(ASSET_DIR / "reference.jpg"),
        "units": "metres, Z up in Blender (glTF is Y up)",
        "origin": "centre of the foot at ground level; the front (the arch) faces -y",
        "material": f"{IDENT}.material.json",
        "variants": [{"node": IDENT, "source_mesh": me.name, "tris": tris(me),
                      "bounds_m": [[round(x, 2) for x in lo], [round(x, 2) for x in hi]],
                      "shape": {k: v[k] for k in ("W", "D", "H", "open_w", "lintel", "niche", "crag")}}],
        "placement": {
            "count": "one in the world (unique)", "zone": "golden_desert", "scale": 1.0,
            "sink_m": 0.5, "foot_below_origin_m": SINK,
            "site": {"flat_radius_m": 160, "max_relief_m": 6.0, "edge_depth_share": 0.15,
                     "candidates": 60, "eye_points": 400, "range_m": 30000,
                     "note": "flat sand deep in the biome, not on a dune crest; the candidate seen from most "
                             "of the biome wins (BIOME_PLAN 3.4: the monument seen from >= 80 % of it)"},
            "turn": "the arch faces the biome's centre of mass (most of it sees the passage)",
            "setting": "open sand, dunes beyond, blue sky; seen from everywhere in its biome (190 m)"},
        "budget": {"tris": tris(me), "style_profile_A": [1500, 4000]},
        "engine": "uv_atlas",
    }
    (CONFIG_DIR / f"{IDENT}.asset.json").write_text(json.dumps(asset, indent=2, ensure_ascii=False) + "\n",
                                                    encoding="utf-8")
    material = {
        "comment": "Material of A-3 (doc/megaliths/A3_DESERT_GATE.md). Generated by "
                   "tools/blender/megalith_a3_desert_gate.py; the maps are final, the knobs are what made them "
                   "(the painted materials gate_paint_* stay in the blend).",
        "id": IDENT + "_pbr",
        "projection": "the mesh's own UVs: one smart-UV atlas for the whole gate",
        "atlas": {"size_px": TEX, "unwrap": "smart UV project, 50 deg, margin 0.004"},
        "filter": "nearest (hard pixels); mip maps fine",
        "textures": {
            "basecolor": {"file": rel(ASSET_DIR / "gate_basecolor_2048.png"), "space": "sRGB",
                          "what": "worn white stone, ivory mottling, faint grey veins, pits a touch darker, "
                                  "a film of sand dust, dust patches on what faces up and at the foot, the "
                                  "passage black; no AO in it; true albedo (no gain needed)"},
            "normal": {"file": rel(ASSET_DIR / "gate_normal_2048.png"), "space": "linear",
                       "convention": "tangent space, OpenGL (+Y up), MikkTSpace from the glTF UVs",
                       "what": "the worn relief: coarse pitting and fine grit"},
            "surface": {"file": rel(ASSET_DIR / "gate_surface_2048.png"), "space": "linear",
                        "what": "R = baked AO (multiply into ambient), G = roughness (worn 0.78, dust rougher), "
                                "B = metallic (0)"}},
        "metallic": 0.0,
        "knobs": KNOBS,
    }
    (CONFIG_DIR / f"{IDENT}.material.json").write_text(json.dumps(material, indent=2, ensure_ascii=False) + "\n",
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
        make_material(bpy.data.objects["hero_" + HERO])
    setup_render()
    render_views(not args.no_render)
    if args.export and STAGE == "material":
        export(VARIANTS[HERO])
    blend = ASSET_DIR / f"gate_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
