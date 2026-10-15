"""Magic megalith A-4: the arch-bridge of the canyon (kind A: blockout -> sculpt -> material,
user approval at each step; doc/megaliths/A4_CANYON_ARCH.md).

Reference (user, 15.10, a flat graphic painting in the manner of Moebius; described by the
project's coordinator, the file not yet in the repository): a tall frame looking up from the
floor of a slot canyon. Sheer orange-red walls in big smooth planes with dark cracks, very tall
and close together; bright yellow sand with small red fragments, a heap of red blocks at the
right foot, three small dark hooded figures for scale. High up, about two thirds of the walls'
height, a colossal grey-blue stone arch-bridge spans between the walls: a semicircular opening
nearly the whole width, the springers grown into the rock and running down the walls as cladding,
a thick solid mass over the opening, flat top with a thin low parapet. The surface is small
rectangular bosses, niches and plates; tiny warm orange lights; an orange bloom on the edge.
Ancient, heavy, faintly technological. Through the opening a second, smaller and paler arch
further down the canyon, then pale domed towers (later options, not built here).

The arch is fitted to a real slot of the world's canyon (tools/terrain_lab/canyon_relief.py,
terrain_lab_out/world_canyon_1): SITE below, the ground round it is built from the run's heights.

Blockout: variants fitted to the slot, each shot from the canyon floor as in the painting.

    blender -b -P tools/blender/megalith_a4_canyon_arch.py -- --stage blockout
"""
import argparse
import json
import math
import random
import sys
import zlib
from pathlib import Path

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[2]
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a4_canyon_arch"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
RUN = ROOT / "terrain_lab_out/world_canyon_1"
STAGE = "blockout"

# linear colours (the painting's flat fills, sRGB in the comments)
STONE = (0.15, 0.21, 0.32, 1)      # grey-blue          #6b7d97
WALL = (0.60, 0.12, 0.025, 1)      # orange-red rock    #cb612b
SAND = (0.86, 0.50, 0.04, 1)       # bright yellow sand #eebc3a
FIGURE = (0.02, 0.02, 0.025, 1)
STONE_DARK = (0.035, 0.05, 0.085, 1)   # niches and soffit ribs
STONE_WORN = (0.21, 0.27, 0.36, 1)     # paler plates, fallen blocks
SKY = (0.03, 0.24, 0.85, 1)        # flat bright blue   #3386ef

# The slot (world metres): a straight stretch about 700 m long, 140 m between thick walls 260 m
# tall; ACROSS is the wall-to-wall direction (degrees from east, counter-clockwise).
SITE = (82976.0, 44704.0)
ACROSS = 165.0
HALF = 1200.0                      # the ground built round the site, metres each way

# Each variant (metres or shares):
#   deck       height of the deck over the floor (share of the slot's depth)
#   T          thickness of the bridge along the canyon
#   lintel     solid stone over the crown of the opening (share of the opening's radius)
#   fill       the opening's width (share of the gap between the walls at its springing)
#   embed      how far the ends run into the walls
#   drop       how far the cladding runs down the walls below the springing (share of radius)
#   parapet    height of the parapet (0 = none)
# The painting itself (arrived 15.10 after V1-V3, assets/.../reference.png) shows a portal, not a
# thin bridge: a wall of grey-blue masonry closes the slot from the floor to the rim, the deck on
# top at the rim, and a tall round-headed opening goes through it down to the floor.
#   portal     the portal form: deck share of depth; open = opening width (share of the gap at the
#              springing); head = stone over the crown (share of the radius)
#   second     another portal this far along the slot, away from the reference camera (0 = none)
PORTALS = {
    "V4_portal": dict(deck=0.98, T=44, open=0.62, head=0.32, embed=26, parapet=2.6, second=0),
    "V5_portal_pair": dict(deck=0.98, T=44, open=0.62, head=0.32, embed=26, parapet=2.6, second=360),
    # user 15.10: a set of such bridges along the narrow canyon, 2-5 as many as fit, one megalith
    "V6_chain": dict(deck=0.98, T=44, open=0.62, head=0.32, embed=26, parapet=2.6, second=0,
                     chain=dict(spacing=130.0, most=5, max_gap=180.0, min_depth=150.0)),
}
VARIANTS = {
    "V1_ref": dict(deck=0.67, T=46, lintel=0.38, fill=0.96, embed=30, drop=0.9, parapet=2.6),
    "V2_rim": dict(deck=0.93, T=52, lintel=0.30, fill=0.96, embed=36, drop=1.1, parapet=2.6),
    "V3_slender": dict(deck=0.62, T=26, lintel=0.20, fill=0.98, embed=24, drop=0.6, parapet=1.8),
}
HERO = "V4_portal"
REF_RES = (800, 1600)


# ---------------------------------------------------------------- helpers

def material(name, rgba, rough=0.9):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = rgba
    bsdf.inputs["Roughness"].default_value = rough
    m.diffuse_color = rgba
    return m


def collection(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def link(name, me, loc=(0, 0, 0), rot=(0.0, 0.0, 0.0), coll=None):
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.rotation_euler = rot
    (coll or bpy.context.scene.collection).objects.link(ob)
    return ob


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


# ---------------------------------------------------------------- the slot

class Slot:
    """Heights round the site, local metres: x east, y north, z over the slot's floor."""

    def __init__(self):
        st = json.loads((RUN / "stats.json").read_text())
        self.cell = st["cell_m"]
        ox, oy = st["origin_m"]
        h = np.load(RUN / "layers.npz")["h"]
        n = int(HALF / self.cell)
        cx, cy = int((SITE[0] - ox) / self.cell), int((SITE[1] - oy) / self.cell)
        self.h = h[cy - n:cy + n + 1, cx - n:cx + n + 1].astype(np.float32)   # rows south first
        self.n = n
        self.floor = float(self.h[n, n])
        self.h -= self.floor
        a = math.radians(ACROSS)
        self.u = Vector((math.cos(a), math.sin(a), 0))     # across, wall to wall
        self.a = Vector((-self.u.y, self.u.x, 0))          # along the canyon
        rim = [self.at(self.u * s) for s in range(-400, 401, 8)]
        self.depth = float(np.percentile(rim, 80))

    def at(self, p):
        x = p[0] / self.cell + self.n
        y = p[1] / self.cell + self.n
        i0, j0 = int(math.floor(x)), int(math.floor(y))
        fx, fy = x - i0, y - j0
        m = self.h.shape[0] - 1
        i0, j0 = max(0, min(m - 1, i0)), max(0, min(m - 1, j0))
        h = self.h
        return float((h[j0, i0] * (1 - fx) + h[j0, i0 + 1] * fx) * (1 - fy)
                     + (h[j0 + 1, i0] * (1 - fx) + h[j0 + 1, i0 + 1] * fx) * fy)

    def wall(self, z, side, along=0.0, reach=300.0, p=None, u=None):
        """Distance across from the slot's axis (or from p along u) to the first ground above z
        (side +1/-1)."""
        p = self.a * along if p is None else p
        u = self.u if u is None else u
        for s in np.arange(0.0, reach, 1.0):
            if self.at(u * (side * s) + p) > z:
                return float(s)
        return reach

    def gap(self, z, along=0.0, p=None, u=None):
        """(centre offset across, width) of the slot at height z."""
        l, r = self.wall(z, -1, along, p=p, u=u), self.wall(z, 1, along, p=p, u=u)
        return (r - l) / 2, l + r

    def rim(self, p, u):
        """The slot's rim round p across u (80th percentile of the ground 400 m each way)."""
        return float(np.percentile([self.at(u * float(s) + p) for s in range(-400, 401, 8)], 80))

    def trace(self, step=20.0, reach=1200.0):
        """The slot's floor line both ways from the site, kept in the middle of the walls and
        turning with them: [(point, along unit)] from one end to the other, and the site's index.
        It stops where the floor climbs, the slot opens out or gets shallow."""
        ends = []
        for sign in (-1, 1):
            p = Vector((0.0, 0.0, 0.0))
            d = self.a * sign
            line = []
            z_prev = self.at(p)
            for _ in range(int(reach / step)):
                q = p + d * step
                u = Vector((d.y, -d.x, 0.0))
                z0 = self.at(q)
                off, w = self.gap(z0 + 30.0, p=q, u=u)
                q = q + u * off
                z0 = self.at(q)
                if z0 > z_prev + 15.0 or w > 240.0 or self.rim(q, u) - z0 < 140.0:
                    break
                z_prev = z0
                nd = (q - p).normalized()
                d = (d * 0.6 + nd * 0.4).normalized()
                p = q
                line.append((Vector(p), Vector(d * sign)))
            ends.append(line)
        back = ends[0][::-1]
        line = back + [(Vector((0.0, 0.0, 0.0)), Vector(self.a))] + ends[1]
        return line, len(back)


def ground(slot, mat_wall, mat_sand):
    h = slot.h[::2, ::2]
    n = h.shape[0]
    c = slot.cell * 2
    yy, xx = np.mgrid[0:n, 0:n]
    co = np.stack([(xx - n // 2) * c, (yy - n // 2) * c, h], -1).reshape(-1, 3).astype(np.float32)
    me = bpy.data.meshes.new("ground")
    me.vertices.add(n * n)
    me.vertices.foreach_set("co", co.ravel())
    i = (yy[:-1, :-1] * n + xx[:-1, :-1]).ravel()
    quads = np.stack([i, i + 1, i + n + 1, i + n], 1)
    me.loops.add(quads.size)
    me.loops.foreach_set("vertex_index", quads.ravel().astype(np.int32))
    me.polygons.add(len(quads))
    me.polygons.foreach_set("loop_start", (np.arange(len(quads)) * 4).astype(np.int32))
    me.polygons.foreach_set("loop_total", np.full(len(quads), 4, np.int32))
    me.update()
    me.polygons.foreach_set("use_smooth", np.zeros(len(quads), bool))
    # one material that turns from sand on the flat to rock on the steep
    m = bpy.data.materials.new("canyon_ground")
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED")
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Normal"], sep.inputs[0])
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = 0.8
    mr.inputs["From Max"].default_value = 0.6
    nt.links.new(sep.outputs[2], mr.inputs["Value"])
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.inputs["A"].default_value = SAND
    mix.inputs["B"].default_value = WALL
    nt.links.new(mr.outputs[0], mix.inputs["Factor"])
    nt.links.new(mix.outputs["Result"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 1.0
    me.materials.append(m)
    return link("ground", me)


# ---------------------------------------------------------------- the arch

def fit(slot, v):
    """The variant's measures in the slot (local metres, z over the floor)."""
    deck = v["deck"] * slot.depth
    off, gap_deck = slot.gap(deck)
    # the opening's radius: the crown sits lintel*R under the deck, the springing R lower; the
    # opening takes `fill` of the gap at the springing (walls widen upward, so iterate)
    R = gap_deck / 2
    for _ in range(20):
        spring = deck - R * (1 + v["lintel"])
        off, g = slot.gap(spring)
        R = 0.5 * (R + g * v["fill"] / 2)
    spring = deck - R * (1 + v["lintel"])
    off, g = slot.gap(spring)
    _, g_deck = slot.gap(deck)
    return dict(deck=deck, spring=spring, R=R, off=off, gap_spring=g, gap_deck=g_deck,
                half=max(g_deck, g) / 2 + v["embed"])


def prism(bm, prof, y0, y1, mat=0):
    """Extrude a profile in the (x, z) plane from y0 to y1."""
    a = [bm.verts.new((x, y0, z)) for x, z in prof]
    b = [bm.verts.new((x, y1, z)) for x, z in prof]
    faces = [bm.faces.new(a[::-1]), bm.faces.new(b)]
    n = len(prof)
    for i in range(n):
        j = (i + 1) % n
        faces.append(bm.faces.new((a[i], a[j], b[j], b[i])))
    for fc in faces:
        fc.material_index = mat
    return faces


def box(bm, x0, x1, y0, y1, z0, z1, mat=0):
    return prism(bm, [(x0, z0), (x1, z0), (x1, z1), (x0, z1)], y0, y1, mat)


def inside(prof, x, z):
    """Point in the (x, z) profile polygon (even-odd)."""
    n, c = len(prof), False
    for i in range(n):
        (x1, z1), (x2, z2) = prof[i], prof[(i + 1) % n]
        if (z1 > z) != (z2 > z) and x < x1 + (z - z1) * (x2 - x1) / (z2 - z1):
            c = not c
    return c


# ---------------------------------------------------------------- sculpt: the masonry
# The painting's surface: big plates of different sizes, small rectangular bosses and niches,
# horizontal courses, a ring of voussoirs, a parapet, blocky works and masts on the deck. Kept to
# a few big readable shapes with irregular sizes and gaps (weathered, not a circuit board).
# Material slots: 0 stone, 1 dark (niches, soffit ribs), 2 worn (plates, rubble).
SCULPT = dict(
    plates=70,          # big flat plates per face (6-30 m), 0.6-1.6 m proud
    bosses=150,         # small bosses per face (1.5-6 m), 0.4-2.2 m proud
    niches=26,          # framed niches per face (3-10 m)
    courses=4,          # horizontal courses across each face
    voussoirs=19,       # stones of the ring
    rubble=14,          # fallen blocks at each foot
)


def masonry(bm, rnd, prof, R, spring, deck, bot, T, reach, rail):
    def free(x0, x1, z0, z1, margin=1.0):
        """The rectangle lies on the face (inside the profile) and clear of the ring."""
        for x in (x0 - margin, x1 + margin, (x0 + x1) / 2):
            for z in (z0 - margin, z1 + margin, (z0 + z1) / 2):
                if not inside(prof, x, z):
                    return False
                if z > spring and math.hypot(x, z - spring) < R + 12:
                    return False
                if z <= spring and abs(x) < R + 12:
                    return False
        return True

    for face in (-1, 1):
        y = face * T / 2

        def proud(x0, x1, z0, z1, d, mat=0):
            y0, y1 = sorted((y - face * 0.3, y + face * d))
            box(bm, x0, x1, y0, y1, z0, z1, mat)

        def place(n, wmin, wmax, aspect, tries=40):
            out = []
            for _ in range(n):
                for _ in range(tries):
                    w = rnd.uniform(wmin, wmax)
                    h = w * rnd.uniform(*aspect)
                    x = rnd.uniform(-reach, reach)
                    z = rnd.uniform(bot + 4, deck - 2)
                    if free(x - w / 2, x + w / 2, z - h / 2, z + h / 2):
                        out.append((x - w / 2, x + w / 2, z - h / 2, z + h / 2))
                        break
            return out

        for x0, x1, z0, z1 in place(SCULPT["plates"], 6, 30, (0.3, 1.4)):
            proud(x0, x1, z0, z1, rnd.uniform(0.6, 1.6), 2 if rnd.random() < 0.35 else 0)
        for x0, x1, z0, z1 in place(SCULPT["bosses"], 1.5, 6, (0.4, 1.8)):
            proud(x0, x1, z0, z1, rnd.uniform(0.4, 2.2))
        for x0, x1, z0, z1 in place(SCULPT["niches"], 3, 10, (0.8, 2.2)):
            # a raised frame round a dark sunk panel
            t = max(0.5, (x1 - x0) * 0.12)
            d = rnd.uniform(1.0, 1.8)
            proud(x0, x1, z1 - t, z1, d)
            proud(x0, x1, z0, z0 + t, d)
            proud(x0, x0 + t, z0, z1, d)
            proud(x1 - t, x1, z0, z1, d)
            proud(x0 + t, x1 - t, z0 + t, z1 - t, 0.15, 1)
        # courses: horizontal bands across the piers (and over the crown under the deck)
        levels = [deck - 3.0] + [rnd.uniform(bot + 15, spring) for _ in range(SCULPT["courses"] - 1)]
        for z in levels:
            xs = [float(x) for x in np.arange(-reach - 60, reach + 60, 2.0)
                  if inside(prof, x, z) and inside(prof, x, z + 2.0)]
            run = []
            for x in xs + [None]:
                if run and (x is None or x - run[-1] > 2.5):
                    if run[-1] - run[0] > 6:
                        proud(run[0], run[-1], z, z + 2.0, 1.4)
                    run = []
                if x is not None:
                    run.append(x)

    # the ring: voussoirs of alternating depth and length round the head, jambs in long blocks
    n = SCULPT["voussoirs"]
    for k in range(n):
        t0, t1 = math.pi * (1 - k / n), math.pi * (1 - (k + 1) / n)
        ext = 7 + (3 if k % 2 else 0) + rnd.uniform(-0.8, 0.8)
        pts = [(R * math.cos(t0), spring + R * math.sin(t0)),
               ((R + ext) * math.cos(t0), spring + (R + ext) * math.sin(t0)),
               ((R + ext) * math.cos(t1), spring + (R + ext) * math.sin(t1)),
               (R * math.cos(t1), spring + R * math.sin(t1))]
        d = 2.0 + (k % 2) * 1.2
        prism(bm, pts, -T / 2 - d, T / 2 + d)
    z = bot
    k = 0
    while z < spring - 1:
        h = min(spring - z, rnd.uniform(9, 16))
        w = 7 + (2.5 if k % 2 else 0)
        for side in (-1, 1):
            x0, x1 = sorted((side * R, side * (R + w)))
            box(bm, x0, x1, -T / 2 - 2.2, T / 2 + 2.2, z, z + h)
        z += h
        k += 1
    # ribs under the vault (soffit), dark
    for yy in np.linspace(-T / 2 + 6, T / 2 - 6, 3):
        for kk in range(16):
            t0, t1 = math.pi * kk / 16, math.pi * (kk + 1) / 16
            pts = [(R * math.cos(t0), spring + R * math.sin(t0)),
                   ((R - 1.5) * math.cos(t0), spring + (R - 1.5) * math.sin(t0)),
                   ((R - 1.5) * math.cos(t1), spring + (R - 1.5) * math.sin(t1)),
                   (R * math.cos(t1), spring + R * math.sin(t1))]
            prism(bm, pts, float(yy) - 1.2, float(yy) + 1.2, 1)
    # parapet: a low wall with posts, broken in a few places
    for y0 in (-T / 2, T / 2 - 1.4):
        x = -rail
        while x < rail:
            L = rnd.uniform(14, 40)
            if rnd.random() > 0.12:
                box(bm, x, min(rail, x + L), y0, y0 + 1.4, deck, deck + 2.2)
            x += L + (rnd.uniform(2, 6) if rnd.random() < 0.15 else 0)
        for x in np.arange(-rail, rail, rnd.uniform(9, 13)):
            box(bm, float(x) - 0.6, float(x) + 0.6, y0 - 0.2, y0 + 1.6, deck, deck + 3.4)
    # works on the deck: stacked blocks over the piers, a few masts
    for side in (-1, 1):
        if rnd.random() < 0.25:
            continue
        x = side * (R + rnd.uniform(10, 30))
        z = deck
        w, dd = rnd.uniform(12, 22), rnd.uniform(14, T - 8)
        for _ in range(rnd.randint(1, 3)):
            h = rnd.uniform(5, 14)
            box(bm, x - w / 2, x + w / 2, -dd / 2, dd / 2, z, z + h, 2 if rnd.random() < 0.4 else 0)
            z += h
            w *= rnd.uniform(0.5, 0.85)
            dd *= rnd.uniform(0.6, 0.9)
            x += rnd.uniform(-2, 2)
        for _ in range(rnd.randint(0, 2)):
            mx = x + rnd.uniform(-w, w)
            box(bm, mx - 0.4, mx + 0.4, -0.4, 0.4, z, z + rnd.uniform(8, 20))
    # fallen masonry at each foot of the opening
    for side in (-1, 1):
        for _ in range(SCULPT["rubble"]):
            sz = rnd.uniform(1.5, 6)
            cx = side * (R - rnd.uniform(0, 14) ** 1.2)
            cy = rnd.uniform(-T / 2 - 25, T / 2 + 25)
            cz = bot + 7.0
            faces = box(bm, cx - sz / 2, cx + sz / 2, cy - sz * 0.4, cy + sz * 0.4, cz, cz + sz * 0.6, 2)
            vs = {vt for fc in faces for vt in fc.verts}
            axis = Vector((rnd.random() - 0.5, rnd.random() - 0.5, rnd.random() - 0.5)).normalized()
            rot = Matrix.Rotation(rnd.uniform(-0.5, 0.5), 3, axis)
            c = Vector((cx, cy, cz + sz * 0.3))
            for vt in vs:
                vt.co = rot @ (vt.co - c) + c


def arch(name, slot, v, stone):
    """The bridge in its own frame: x across (wall to wall), y along the canyon, z up."""
    f = fit(slot, v)
    R, deck, spring, L = f["R"], f["deck"], f["spring"], f["half"]
    bot = spring - R * v["drop"] * 0.35
    prof = [(-L, bot), (-R, bot), (-R, spring)]
    for k in range(1, 24):
        t = math.pi * (1 - k / 24)
        prof.append((R * math.cos(t), spring + R * math.sin(t)))
    prof += [(R, spring), (R, bot), (L, bot), (L, deck), (-L, deck)]
    bm = bmesh.new()
    T = v["T"]
    prism(bm, prof, -T / 2, T / 2)
    # parapet along both edges, over the gap only
    if v["parapet"]:
        p = f["gap_deck"] / 2 + 4
        for y in (-T / 2, T / 2 - 1.6):
            box(bm, -p, p, y, y + 1.6, deck, deck + v["parapet"])
    # the cladding: the springers run down the walls, a column of slabs following the wall
    # inward (the walls are not plumb), sunk into the rock
    for side in (-1, 1):
        z = bot
        k = 0
        while z > spring - R * v["drop"] - 1e-3:
            z0 = z - 14
            w = slot.wall(z0 + 7, side) - f["off"] * side
            inner = w - 3.5 + (k % 2) * 1.2
            x0, x1 = sorted((side * inner, side * (w + 16)))
            t = T * (0.92 - 0.25 * k / 10)
            box(bm, x0, x1, -t / 2, t / 2, z0, z + 0.5)
            z = z0
            k += 1
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = mesh_from(name, bm, stone)
    rot = (0, 0, math.radians(ACROSS))
    loc = slot.u * f["off"]
    ob = link(name, me, (loc.x, loc.y, 0), rot)
    print(f"{name}: deck {f['deck']:.0f} m over the floor (slot {slot.depth:.0f} m deep), opening "
          f"{2 * R:.0f} m wide springing at {spring:.0f} m, gap at the deck {f['gap_deck']:.0f} m, "
          f"length {2 * L:.0f} m, {tris(me)} tris")
    return ob, f


def narrowest(slot, p, u, z):
    """The across direction within 40 degrees of u that cuts the slot shortest at height z."""
    best = (slot.gap(z, p=p, u=u)[1], u)
    for deg in range(-40, 41, 5):
        a = math.radians(deg)
        w = Vector((u.x * math.cos(a) - u.y * math.sin(a), u.x * math.sin(a) + u.y * math.cos(a), 0.0))
        g = slot.gap(z, p=p, u=w)[1]
        if g < best[0] - 0.5:
            best = (g, w)
    return best[1]


def fit_portal(slot, v, along=0.0, p=None, u=None):
    p = slot.a * along if p is None else p
    u = slot.u if u is None else u
    floor = slot.at(p)
    rim = slot.rim(p, u)
    if v.get("chain"):
        u = narrowest(slot, p, u, floor + 0.5 * (rim - floor))
    deck = floor + v["deck"] * (rim - floor)
    # the deck no higher than where the slot starts to open out (the walls step back near the rim)
    g_mid = slot.gap(floor + 0.5 * (rim - floor), p=p, u=u)[1]
    z = floor + 0.5 * (rim - floor)
    while z + 4 <= deck and slot.gap(z + 4, p=p, u=u)[1] <= 1.45 * g_mid:
        z += 4
    deck = min(deck, z)
    R = 0.0
    spring = floor + (deck - floor) * 0.6
    for _ in range(20):
        off, g = slot.gap(spring, p=p, u=u)
        R = 0.5 * (R + g * v["open"] / 2)
        spring = deck - R * (1 + v["head"])
    off, g = slot.gap(spring, p=p, u=u)
    _, g_deck = slot.gap(deck, p=p, u=u)
    return dict(deck=deck, spring=spring, R=R, off=off, gap_spring=g, gap_deck=g_deck, along=along,
                floor=floor, p=p, u=u)


def portal(name, slot, v, stone, along=0.0, p=None, u=None):
    """A wall of masonry closing the slot from below the floor to the deck at the rim, its sides
    following the walls into the rock, a round-headed opening through it down to the floor."""
    f = fit_portal(slot, v, along, p, u)
    R, deck, spring, off = f["R"], f["deck"], f["spring"], f["off"]
    p, u = f["p"], f["u"]
    bot = f["floor"] - 8.0
    zs = list(np.arange(deck, bot - 1e-3, -10.0)) + [bot]

    def outer(z, side):
        w = slot.wall(z, side, p=p, u=u) - off * side
        return side * max(w + v["embed"], R + 12)

    prof = [(outer(z, -1), z) for z in zs]                     # left side, top down
    prof += [(-R, bot), (-R, spring)]
    for k in range(1, 24):
        t = math.pi * (1 - k / 24)
        prof.append((R * math.cos(t), spring + R * math.sin(t)))
    prof += [(R, spring), (R, bot)]
    prof += [(outer(z, 1), z) for z in zs[::-1]]               # right side, bottom up
    bm = bmesh.new()
    T = v["T"]
    prism(bm, prof, -T / 2, T / 2)
    if STAGE != "blockout":
        rnd = random.Random(zlib.crc32(name.encode()))
        masonry(bm, rnd, prof, R, spring, deck, bot, T, f["gap_deck"] / 2 + 40, f["gap_deck"] / 2 + 4)
    elif v["parapet"]:
        rail = f["gap_deck"] / 2 + 4
        for y in (-T / 2, T / 2 - 1.6):
            box(bm, -rail, rail, y, y + 1.6, deck, deck + v["parapet"])
    # the ring of the arch stands proud of the face, and a pilaster down each side of the opening
    for y0, y1 in (((-T / 2 - 2.5, -T / 2), (T / 2, T / 2 + 2.5)) if STAGE == "blockout" else ()):
        ts = [math.pi * (1 - k / 24) for k in range(25)]
        outer_pts = [((R + 7) * math.cos(t), spring + (R + 7) * math.sin(t)) for t in ts]
        inner_pts = [(R * math.cos(t), spring + R * math.sin(t)) for t in ts]
        for i in range(24):
            prism(bm, [inner_pts[i], outer_pts[i], outer_pts[i + 1], inner_pts[i + 1]][::-1], y0, y1)
        box(bm, R, R + 7, y0, y1, bot, spring)
        box(bm, -(R + 7), -R, y0, y1, bot, spring)
    # a few blocks on the deck over the piers, as in the painting
    for side, h in (((-1, 16), (1, 9)) if STAGE == "blockout" else ()):
        x = side * (R + 16)
        box(bm, x - 9, x + 9, -T / 2 + 3, T / 2 - 3, deck, deck + h)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    me = mesh_from(name, bm, stone)
    loc = u * off + p
    ob = link(name, me, (loc.x, loc.y, 0), (0, 0, math.atan2(u.y, u.x)))
    f["centre"] = Vector((loc.x, loc.y, 0.0))
    print(f"{name}: floor {f['floor']:.0f}, deck {deck:.0f} m, opening {2 * R:.0f} m wide, crown "
          f"{spring + R:.0f} m, gap at the springing {f['gap_spring']:.0f} m, at the deck {f['gap_deck']:.0f} m, "
          f"{tris(me)} tris")
    return ob, f


def hooded(name, loc, mat, coll):
    """A small hooded figure: a cloak to the ground and a pointed hood (1.8 m)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=8, radius1=0.42, radius2=0.2, depth=1.45,
                          matrix=__import__("mathutils").Matrix.Translation((0, 0, 0.725)))
    bmesh.ops.create_cone(bm, cap_ends=True, segments=8, radius1=0.2, radius2=0.0, depth=0.5,
                          matrix=__import__("mathutils").Matrix.Translation((0, 0, 1.6)))
    return link(name, mesh_from(name, bm, mat), loc, coll=coll)


# ---------------------------------------------------------------- shots

def setup_render(slot):
    sc = bpy.context.scene
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            sc.render.engine = engine
            break
        except TypeError as e:
            print("engine:", e)
    sc.render.image_settings.file_format = "PNG"
    sc.view_settings.view_transform = "Standard"
    sun = bpy.data.lights.new("sun", "SUN")
    sun.energy = 4.5
    sun.angle = math.radians(1.0)
    ob = bpy.data.objects.new("sun", sun)
    # down the slot from behind the reference camera, a little across: the arch's face and the
    # walls ahead lit, as painted
    el = math.radians(78)   # high: the slot floor in sun, as painted
    d = (slot.a + slot.u * 0.15).normalized() * math.cos(el) + Vector((0, 0, -math.sin(el)))
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(ob)
    # a weak second sun from across the slot: the painting's walls are lit on both sides
    fill = bpy.data.lights.new("fill", "SUN")
    fill.energy = 1.6
    ob = bpy.data.objects.new("fill", fill)
    d = (slot.a * 0.3 - slot.u).normalized() * math.cos(el) + Vector((0, 0, -math.sin(el)))
    ob.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(ob)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = SKY
    bg.inputs["Strength"].default_value = 1.0
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


def clear(slot, eye, target, margin=2.0):
    """No ground between the eye and the target (the last stretch, inside the arch, not counted)."""
    for t in np.linspace(0.02, 0.9, 120):
        p = eye.lerp(target, float(t))
        if slot.at(p) > p.z - margin:
            return False
    return True


def spots(slot, target, test):
    """Points of the ground round the slot that pass test(p, along) and see the target."""
    out = []
    for along in np.arange(-1100.0, 1101.0, 20.0):
        for lat in np.arange(-500.0, 501.0, 20.0):
            q = slot.a * float(along) + slot.u * float(lat)
            p = Vector((q.x, q.y, slot.at(q)))
            if test(p, along, lat) and clear(slot, p + Vector((0, 0, 1.7)), target):
                out.append((float(along), float(lat), p))
    return out


def views(slot, f):
    """name, camera, target, lens, portrait."""
    R, deck = f["R"], f["deck"]
    c = slot.u * f["off"]
    mid = Vector((c.x, c.y, deck - R * 0.5))
    eye = Vector((0, 0, 1.7))
    out = []
    on_floor = spots(slot, mid, lambda p, al, lat: p.z < 12 and abs(lat) < 120)
    # the painting: from the floor some way along the slot, looking up at the arch
    for sign, tag in ((-1, "ref"), (1, "ref_back")):
        side = [s for s in on_floor if s[0] * sign > 0]
        if side:
            # as far back along the slot as the arch is still seen (the painting looks a long way
            # down the floor), near the slot's middle
            s = max(side, key=lambda s: abs(s[0]) - abs(s[1]) * 3)
            look = Vector((c.x, c.y, deck * 0.5))
            out.append((f"reference_{tag}", s[2] + eye, look, 24, True))
            far = max(side, key=lambda s: abs(s[0]))
            out.append((f"floor_far_{tag}", far[2] + eye, mid, 40, False))
    air = slot.u * 170 - slot.a * 230 + Vector((0, 0, slot.depth + 70))
    out.append(("aerial", air, Vector((c.x, c.y, deck * 0.6)), 30, False))
    return out


def second_along(slot, f, v):
    """The second portal: the farthest place down the slot (up to v["second"] + 180 m) whose
    middle the reference camera sees through the first opening."""
    c = slot.u * f["off"]
    mid = Vector((c.x, c.y, f["deck"] - f["R"] * 0.5))
    on_floor = spots(slot, mid, lambda p, al, lat: p.z < 12 and abs(lat) < 120 and al < 0)
    eye = max(on_floor, key=lambda s: abs(s[0]) - abs(s[1]) * 3)[2] + Vector((0, 0, 1.7))
    best = 120.0
    for along in np.arange(120.0, v["second"] + 181.0, 10.0):
        g = fit_portal(slot, v, float(along))
        q = slot.u * g["off"] + slot.a * float(along)
        if clear(slot, eye, Vector((q.x, q.y, g["spring"])), margin=1.0):
            best = float(along)
    print(f"second portal {best:.0f} m down the slot")
    return best


def chain_stations(slot, v):
    """Where the portals of the chain stand along the slot's floor line: places where the slot is
    narrow and deep enough, at least `spacing` apart, at most `most`, spread over the stretch."""
    c = v["chain"]
    line, site = slot.trace()
    arc = [0.0]
    for i in range(1, len(line)):
        arc.append(arc[-1] + (line[i][0] - line[i - 1][0]).length)
    ok = []
    for i, (p, d) in enumerate(line):
        u = Vector((d.y, -d.x, 0.0))
        g = fit_portal(slot, v, p=p, u=u)
        if g["gap_spring"] <= c["max_gap"] and g["deck"] - g["floor"] >= c["min_depth"]:
            ok.append(i)
    print(f"slot line {arc[-1]:.0f} m, {len(ok)} of {len(line)} points fit a portal")
    spacing = c["spacing"]
    while True:
        picked = []
        for i in ok:
            if not picked or arc[i] - arc[picked[-1]] >= spacing:
                picked.append(i)
        if len(picked) <= c["most"]:
            break
        spacing += 10.0
    # centre the run in the stretch that fits
    if picked and ok:
        slack = (arc[ok[-1]] - arc[picked[-1]]) / 2
        picked = [min(ok, key=lambda j: abs(arc[j] - (arc[i] + slack))) for i in picked]
    return line, arc, picked


def chain_views(slot, line, fs):
    """From the floor beyond each end looking down the chain (the painting), between portals, and
    from the air over the whole chain."""
    eye = Vector((0, 0, 1.7))
    out = []
    # the painting: from the floor just inside an end portal, looking down the chain at the next
    # one (the slot's ends are blocked by its bends and climbs, so not from outside)
    for tag, k0, k1 in (("ref", 0, 1), ("ref_back", len(fs) - 1, len(fs) - 2)):
        if len(fs) < 2:
            break
        near, far = fs[k0], fs[k1]
        look = far["centre"] + Vector((0, 0, far["floor"] + (far["deck"] - far["floor"]) * 0.45))
        i0, i1 = near["i"], far["i"]
        step = 1 if i1 > i0 else -1
        for i in range(i0 + step, i1, step):
            q = line[i][0]
            if (q - near["centre"]).length < 18:
                continue
            e = Vector((q.x, q.y, slot.at(q))) + eye
            if clear(slot, e, look):
                out.append((f"reference_{tag}", e, look, 20, True))
                break
    if len(fs) > 1:
        a, b = fs[0]["centre"], fs[1]["centre"]
        m = (a + b) / 2
        e = Vector((m.x, m.y, slot.at(m))) + eye
        out.append(("between", e, b + Vector((0, 0, fs[1]["deck"] * 0.6)), 20, True))
    c = sum((f["centre"] for f in fs), Vector()) / len(fs)
    span = max((f["centre"] - c).length for f in fs) + 150
    d = (fs[-1]["centre"] - fs[0]["centre"]).normalized() if len(fs) > 1 else slot.a
    side = Vector((d.y, -d.x, 0))
    air = c + side * span * 0.9 - d * span * 0.5 + Vector((0, 0, slot.depth + span * 0.7))
    out.append(("aerial", air, c + Vector((0, 0, slot.depth * 0.5)), 30, False))
    top = c + Vector((0, 0, slot.depth + span * 2.2))
    out.append(("plan", top, c + Vector((0, 0.01, 0)), 35, False))
    if STAGE != "blockout" and len(fs) > 1:
        # close: the second portal's deck from above
        g = fs[1]
        toward = (fs[0]["centre"] - g["centre"]).normalized()
        side = Vector((toward.y, -toward.x, 0))
        q = g["centre"] + toward * 70 + side * 50 + Vector((0, 0, g["deck"] + 35))
        out.append(("deck_close", q, g["centre"] + Vector((0, 0, g["deck"] + 4)), 32, False))
    return out


def figures(slot, eye, mid, mat, coll):
    to = (mid - eye)
    to.z = 0
    to.normalize()
    side = Vector((to.y, -to.x, 0))
    for k, (fwd, lat) in enumerate(((22, -2.0), (26, 1.5), (31, -0.4))):
        p = eye + to * fwd + side * lat
        hooded(f"figure_{coll.name}_{k}", (p.x, p.y, slot.at(p) - 0.1), mat, coll)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--stage", choices=("blockout", "sculpt"), default="blockout")
    ap.add_argument("--only", default="")
    args = ap.parse_args(argv)
    global STAGE
    STAGE = args.stage
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    SHOTS.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    slot = Slot()
    print(f"slot: floor {slot.floor:.0f} m, depth {slot.depth:.0f} m, gap at the floor+10 "
          f"{slot.gap(10)[1]:.0f} m")
    stone = [material(f"arch_{STAGE}", STONE, rough=0.8), material("arch_dark", STONE_DARK),
             material("arch_worn", STONE_WORN, rough=0.95)]
    fig = material("figure", FIGURE)
    ground(slot, None, None)
    setup_render(slot)
    sc = bpy.context.scene
    every = {**PORTALS, **VARIANTS}
    names = args.only.split(",") if args.only else list(PORTALS)
    built = {}
    for name in names:
        coll = collection(name)
        v = every[name]
        obs = []
        if v.get("chain"):
            line, arc, picked = chain_stations(slot, v)
            fs = []
            for k, i in enumerate(picked):
                p, d = line[i]
                ob, g = portal(f"arch_{name}_{k}", slot, v, stone, p=p, u=Vector((d.y, -d.x, 0.0)))
                obs.append(ob)
                g["i"] = i
                fs.append(g)
                print(f"  portal {k} at {arc[i]:.0f} m along the slot line")
            f = dict(chain=fs, line=line)
        elif name in PORTALS:
            ob, f = portal("arch_" + name, slot, v, stone)
            obs.append(ob)
            if v["second"]:
                obs.append(portal("arch_" + name + "_second", slot, v, stone, second_along(slot, f, v))[0])
        else:
            ob, f = arch("arch_" + name, slot, v, stone)
            obs.append(ob)
        for ob in obs:
            bpy.context.scene.collection.objects.unlink(ob)
            coll.objects.link(ob)
        built[name] = (coll, f)
    figs = collection("figures")
    for name in names:
        coll, f = built[name]
        vs = chain_views(slot, f["line"], f["chain"]) if "chain" in f else views(slot, f)
        for vname, loc, target, lens, portrait in vs:
            if vname == "reference_ref":
                figures(slot, loc, target, fig, coll)
            cam = camera(f"cam_{name}_{vname}", loc, target, lens)
            if args.no_render:
                continue
            for n2, (c2, _) in built.items():
                c2.hide_render = n2 != name
            sc.camera = cam
            sc.render.resolution_x, sc.render.resolution_y = REF_RES if portrait else (1600, 900)
            sc.render.filepath = str(SHOTS / f"a4_arch_{STAGE}_{name}_{vname}.png")
            bpy.ops.render.render(write_still=True)
            print("wrote", sc.render.filepath)
    for c2, _ in built.values():
        c2.hide_render = False
    blend = ASSET_DIR / f"arch_{STAGE}.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
