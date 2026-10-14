"""Magic megalith A-3: the mirror gate of the golden desert (doc/BIOME_PLAN.md 3.4, kind A:
blockout -> sculpt -> material, user approval at each step).

Reference (user, 14.10, painting; assets/terrain_core/megaliths/a3_mirror_gate/reference.jpg):
a very tall pale slab standing straight up on a flat pale-yellow plain under a blue sky, about
5 times taller than wide, flat top. Its front face is a thin frame round one tall narrow
round-arched opening, black inside (a deep dark passage), running from the ground almost to the
top; a tiny figure stands in it. The left wall is clean and cut, a touch thicker toward the foot;
the right flank behind the frame is raw broken rock that widens downward into a craggy foot with
fallen blocks. Shallow water sheets lie round the base. A cloaked figure in the foreground.
User: the stone is polished like a mirror (material stage).

Blockout: proportion variants side by side with 1.8 m figures, plus a shot repeating the
reference composition; V1_ref chosen (14.10), 190 m.
Sculpt: one game mesh (about 3.3k tris, no separate dense mesh): the cut block keeps big flat
planes (the mirror) with chamfered edges and a few flat-bottomed chips; the right flank is a smooth
weathered skin flaring down to the foot, fallen blocks as fractured hulls.

    blender -b -P tools/blender/megalith_a3_mirror_gate.py                      # sculpt stage
    blender -b -P tools/blender/megalith_a3_mirror_gate.py -- --stage blockout
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
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a3_mirror_gate"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "sculpt"

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


def flank(v, bm, seed):
    """The right flank as one smooth, slightly weathered skin (user, 14.10: no stuck-on heap of
    stones, a smooth worn transition down to the boulders): a loft of rings flaring out toward
    the foot, nudged only by slow noise and faint wind-cut bands."""
    W, D, H = v["W"], v["D"], v["H"]
    N, levels = 20, 44
    off = Vector((seed % 97, seed % 89, seed % 83)) * 1.7
    rings = []
    for j in range(levels + 1):
        z = -SINK + (H * 0.985 + SINK) * (j / levels) ** 0.9
        t = 1.0 - max(z, 0.0) / H
        ext = v["crag"] * W * (0.10 + 0.90 * t ** 2.6)
        x0, x1 = W / 2 - 0.08 * W, W / 2 + ext + 0.5
        y0, y1 = -D / 2 + 2.5 + 0.06 * W * (1 - t), D / 2 + 0.25 * D * t
        cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
        amp = 0.035 * W * (0.4 + t)
        ring = []
        for i in range(N):
            a = 2 * math.pi * i / N
            c, s = math.cos(a), math.sin(a)
            px = cx + rx * math.copysign(abs(c) ** 0.5, c)
            py = cy + ry * math.copysign(abs(s) ** 0.5, s)
            p = Vector((px, py, z))
            d = fbm(p * 0.018 + off, 2) * amp + 0.35 * math.sin(z * 0.45 + 2.0 * fbm(p * 0.05 + off, 1))
            ring.append(bm.verts.new((px + c * d, py + s * d, z)))
        rings.append(ring)
    for a, b in zip(rings, rings[1:]):
        for i in range(N):
            bm.faces.new((a[i], a[(i + 1) % N], b[(i + 1) % N], b[i]))
    bm.faces.new(list(reversed(rings[0])))
    top = bm.verts.new((sum(p.co.x for p in rings[-1]) / N, sum(p.co.y for p in rings[-1]) / N, H * 0.99))
    for i in range(N):
        bm.faces.new((rings[-1][i], rings[-1][(i + 1) % N], top))
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
        x = W / 2 + fx * s * 0.8
        y = -D / 2 + fy * D + 0.2 * D
        rx, ry, rz = r * s * rnd.uniform(0.9, 1.2), r * s * rnd.uniform(0.8, 1.1), hz * s
        hull(bm, chunk_pts(rnd, x - rx, x + rx, y - ry, y + ry, -1.5, rz * 1.4, n=16,
                           flat="-y" if k in (4, 6) else None))


def chip_cutters(v, rnd):
    """Flat-bottomed wedges biting the frame's edges: most low and on the crag side, a couple at
    the top corners and on the arch's lip."""
    W, D, H = v["W"], v["D"], v["H"]
    r = v["open_w"] * W / 2
    cx = W * 0.03
    yf = -D / 2
    spots = []
    for _ in range(5):   # the outer left front edge, mostly low
        z = H * rnd.random() ** 2.2
        xl = -W / 2 - v["flare"] * W * (1 - z / H) ** 1.5
        spots.append((xl, yf, z, rnd.uniform(1.0, 2.6)))
    for _ in range(5):   # the arch's front lips, low (worn by passage) and a couple high
        z = H * 0.55 * rnd.random() ** 1.8
        spots.append((cx + rnd.choice((-r, r)), yf, z, rnd.uniform(0.8, 2.0)))
    for _ in range(4):   # the right front edge where the falling rock struck
        spots.append((W / 2, yf, H * rnd.uniform(0.05, 0.7), rnd.uniform(1.5, 3.2)))
    spots.append((-W / 2, yf, H, 3.0))           # top corners
    spots.append((W / 2, D / 2, H, 4.0))
    bm = bmesh.new()
    for x, y, z, s in spots:
        hull(bm, [(x + rnd.uniform(-s, s), y + rnd.uniform(-s, s), z + rnd.uniform(-s, s) * 1.6)
                  for _ in range(9)])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def sculpted_gate(name, v, stone, void, rock):
    seed = zlib.crc32(name.encode())
    rnd = random.Random(seed)
    W, D, H = v["W"], v["D"], v["H"]
    bm = bmesh.new()
    block(v, bm)
    ob = link(name + "_block", mesh_from(name + "_block", bm, [stone, void, rock]), (0, 0, 0))
    cut = link(name + "_cut", arch_cutter(v, name + "_cut", void), (0, 0, 0))
    chips = link(name + "_chips", mesh_from(name + "_chips", chip_cutters(v, rnd), stone), (0, 0, 0))
    bm = bmesh.new()
    flank(v, bm, seed)
    rag = link(name + "_crag", mesh_from(name + "_crag", bm, rock), (0, 0, 0))
    for label, op, other in (("arch", "DIFFERENCE", cut), ("chips", "DIFFERENCE", chips), ("crag", "UNION", rag)):
        mod = ob.modifiers.new(label, "BOOLEAN")
        mod.operation = op
        mod.solver = "EXACT"
        mod.object = other
        mod.use_self = label != "arch"   # chips and splinters overlap each other
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
    for o in (ob, cut, chips, rag):
        bpy.data.objects.remove(o)
    bm = bmesh.new()
    bm.from_mesh(me)
    for f in bm.faces:   # the weathered flank shades smooth, the cut planes and the boulders flat
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


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("hero", "lineup", "wire")]
    views = {"blockout": VIEWS, "sculpt": SCULPT_VIEWS}[STAGE]
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


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--stage", choices=("blockout", "sculpt"), default="sculpt")
    args = ap.parse_args(argv)
    global STAGE
    STAGE = args.stage
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    build()
    setup_render()
    render_views(not args.no_render)
    blend = ASSET_DIR / f"gate_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
