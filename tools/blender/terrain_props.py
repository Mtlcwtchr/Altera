"""The terrain core's mega structures (the mesh part of the hybrid terrain,
doc/rework/MESH_TERRAIN_PLAN.md): what a heightfield cannot hold - kilometre
spires of fused boulders over the canyon steppe, natural bridges between them,
the thin spire of the river valley - and the megaliths of megaliths.py,
written as plain meshes in three levels of detail for terrain_lab.

    blender -b --factory-startup -P tools/blender/terrain_props.py -- --all
    blender -b --factory-startup -P tools/blender/terrain_props.py -- --kind spire --variants 6 --preview P.png

Output: assets/terrain_core/props/<kind>_<variant>_lod<0..2>.mesh and
props.json (every model: its kind, nominal size, triangles per level).
The format is the foliage's (tools/blender/export_foliage.py):

    char[4] "FOLM", u32 version (1), u32 vertices, u32 indices,
    f32[3] bounds min, f32[3] bounds max,
    vertices: f32 position[3], f32 normal[3], f32 uv[2]   (32 bytes each)
    indices:  u32

Nothing is textured: the lab lays the ground's own rock over them in world
space. uv carries (cavity occlusion 0..1, height share 0..1 up the model).
Every model stands with the ground at z = 0 (its foot reaches below, to be
buried) and is meant to be scaled per instance: a spire's nominal height is
1000 m, a bridge's span 1000 m along x; the megaliths keep megaliths.py's
own sizes and are made mega by the placement rules.
"""
import argparse
import json
import math
import random
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asr_blender as ab  # noqa: E402

import bpy  # noqa: E402
import bmesh  # noqa: E402
from mathutils import Vector, noise  # noqa: E402

OUT = ab.ROOT / "assets" / "terrain_core" / "props"
SPIRE_H = 1000.0
BRIDGE_SPAN = 1000.0


def smooth(obj, iterations=4, factor=0.5):
    ab.select_only(obj)
    m = obj.modifiers.new("relax", "SMOOTH")
    m.iterations = iterations
    m.factor = factor
    ab.apply_all(obj)
    return obj


def voxel(obj, size):
    ab.select_only(obj)
    m = obj.modifiers.new("fuse", "REMESH")
    m.mode = "VOXEL"
    m.voxel_size = size
    ab.apply_all(obj)
    return obj


def cut_above(obj, z):
    """Everything over `z` removed and the hole filled flat: a burnt top."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
    r = bmesh.ops.bisect_plane(bm, geom=geom, plane_co=(0, 0, z), plane_no=(0, 0, 1), clear_outer=True)
    edges = [e for e in r["geom_cut"] if isinstance(e, bmesh.types.BMEdge)]
    if edges:
        bmesh.ops.holes_fill(bm, edges=edges, sides=0)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return obj


def boulders(obj, size, strength, seed):
    """Each surface point pushed out by the cell of a Voronoi field it lies
    in: the mass reads as rounded boulders pressed together, seams between."""
    off = Vector((seed * 13.1 % 97, seed * 7.7 % 89, seed * 3.3 % 83))
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    for v in bm.verts:
        p = Vector((v.co.x, v.co.y, v.co.z * 1.35)) / size + off
        d = noise.voronoi(p, distance_metric="DISTANCE", exponent=2.5)[0]
        # Swollen in a cell's middle, pinched at the seams.
        bulge = math.sqrt(max(d[1] - d[0], 0.0)) - 0.35
        v.co += v.normal * bulge * strength
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return obj


# --- new archetypes -------------------------------------------------------

def spire(rng, seed):
    """A canyon steppe spire (doc/BIOMES_RU.md, 03): ochre, built of fused
    rounded boulders, its base narrower than its body, a burnt flat top.
    1000 m tall; foot buried 60 m."""
    h = SPIRE_H
    rm = h * rng.uniform(0.10, 0.145)          # body radius
    lean = rng.uniform(-0.06, 0.06) * h
    turn = rng.uniform(0, math.tau)
    top_wide = rng.uniform(0.85, 1.25)         # some carry a cap wider than the body

    def radius(t):
        base = 0.42 + 0.58 * min(1.0, t / 0.28) ** 0.7
        swell = 1.0 + 0.10 * math.sin(t * rng_phase[0] + rng_phase[1])
        cap = 1.0 + (top_wide - 1.0) * max(0.0, (t - 0.8) / 0.2)
        return rm * base * swell * cap

    def axis(t):
        a = turn + t * 1.3
        return Vector((math.cos(a) * lean * t * t, math.sin(a) * lean * t * t, 0.0))

    rng_phase = (rng.uniform(5, 11), rng.uniform(0, math.tau))
    balls = []
    z = -0.06 * h
    while z < h * 1.02:
        t = max(0.0, z / h)
        r = radius(t)
        c = axis(t)
        balls.append({"co": (c.x, c.y, z), "radius": r * 0.78, "size": (1.0, 1.0, rng.uniform(0.7, 1.1))})
        for k in range(rng.randint(2, 4)):
            a = rng.uniform(0, math.tau)
            d = r * rng.uniform(0.25, 0.65)
            rr = r * rng.uniform(0.45, 0.9)
            # Boulders of every shape: squat, round, some standing tall.
            squash = rng.choice((rng.uniform(0.55, 0.8), rng.uniform(0.85, 1.1), rng.uniform(1.2, 1.7)))
            balls.append({"co": (c.x + math.cos(a) * d, c.y + math.sin(a) * d, z + rng.uniform(-0.45, 0.45) * r),
                          "radius": rr, "size": (rng.uniform(0.85, 1.2), rng.uniform(0.85, 1.2), squash)})
        z += r * rng.uniform(0.45, 1.0)
    # A core all the way up, so no boulder hangs loose of the rest.
    z = -0.06 * h
    while z < h:
        t = max(0.0, z / h)
        c = axis(t)
        balls.append({"co": (c.x, c.y, z), "radius": radius(t) * 0.6, "size": (1, 1, 1)})
        z += radius(t) * 0.3
    o = ab.metaball_mass("spire", balls, resolution=h / 170.0, threshold=0.6)
    ab.keep_largest(o)
    voxel(o, h / 260.0)
    boulders(o, rm * 0.9, rm * 0.16, seed)
    smooth(o, 3, 0.4)
    cut_above(o, h)
    return o


def bridge(rng, seed):
    """A natural bridge (doc/BIOMES_RU.md, 03): the hard joint left standing
    between two spires. Span 1000 m along x, deck at z = 0, its ends sunk
    into the spires it joins; the underside an arch."""
    s = BRIDGE_SPAN
    balls = []
    n = 28
    for i in range(n + 1):
        u = i / n                                   # 0..1 along the span
        x = (u - 0.5) * s * 1.1
        mid = 1.0 - abs(u - 0.5) * 2.0              # 1 in the middle
        thick = s * (0.17 - 0.09 * mid ** 0.6) * rng.uniform(0.9, 1.1)
        sag = s * 0.015 * mid                        # the deck dips a little
        balls.append({"co": (x, rng.uniform(-0.01, 0.01) * s, -thick * 0.45 - sag), "radius": thick,
                      "size": (1.0, rng.uniform(0.75, 0.95), 0.8)})
        # Boulders hanging under the deck, thicker towards the ends: an arch.
        if 0 < i < n:
            balls.append({"co": (x, rng.uniform(-0.03, 0.03) * s, -thick * (1.0 + 0.6 * (1 - mid))),
                          "radius": thick * rng.uniform(0.45, 0.65), "size": (1.1, 0.8, 0.8)})
    o = ab.metaball_mass("bridge", balls, resolution=s / 220.0, threshold=0.6)
    ab.keep_largest(o)
    voxel(o, s / 300.0)
    boulders(o, s * 0.05, s * 0.012, seed)
    smooth(o, 3, 0.4)
    cut_above(o, 0.0)
    return o


def valley_spire(rng, seed):
    """The river valley's spire (doc/BIOMES_RU.md, 07): unnaturally thin, no
    wider at its base, only the top mass swells like a stone beehive.
    1000 m nominal."""
    h = SPIRE_H
    r = h * rng.uniform(0.028, 0.036)
    balls = []
    z = -0.05 * h
    while z < h * 0.72:
        wob = Vector((noise.noise(Vector((z * 0.004, seed, 0))) * r * 0.5,
                      noise.noise(Vector((seed, z * 0.004, 0))) * r * 0.5, 0))
        balls.append({"co": (wob.x, wob.y, z), "radius": r * rng.uniform(1.0, 1.25), "size": (1, 1, 0.8)})
        z += r * 0.6
    # The beehive: rings narrowing to a domed top.
    hive_r = h * rng.uniform(0.07, 0.09)
    rings = 7
    for k in range(rings):
        t = k / (rings - 1)
        rr = hive_r * (1.0 - 0.55 * t * t) * (0.75 if k == 0 else 1.0)
        balls.append({"co": (0, 0, h * 0.72 + t * h * 0.24), "radius": rr, "size": (1, 1, 0.42)})
    o = ab.metaball_mass("valley_spire", balls, resolution=h / 260.0, threshold=0.6)
    ab.keep_largest(o)
    voxel(o, h / 400.0)
    boulders(o, hive_r * 0.35, hive_r * 0.05, seed)
    smooth(o, 2, 0.4)
    return o


def chain(balls, a, b, r0, r1, step=0.5, size=(1, 1, 1)):
    """Metaballs along a segment, radius r0 at `a` to r1 at `b`: a limb."""
    a, b = Vector(a), Vector(b)
    n = max(2, int((b - a).length / (min(r0, r1) * step)) + 1)
    for i in range(n + 1):
        t = i / n
        p = a.lerp(b, t)
        balls.append({"co": tuple(p), "radius": r0 + (r1 - r0) * t, "size": size})


def finish(o, voxel_m, lumps_m, lumps, smooth_n=3, seed=0):
    ab.keep_largest(o)
    voxel(o, voxel_m)
    if lumps:
        boulders(o, lumps_m, lumps, seed)
    smooth(o, smooth_n, 0.4)
    return o


def cut_with(target, cutters):
    for cc in cutters:
        m = target.modifiers.new("cut", "BOOLEAN")
        m.operation = "DIFFERENCE"
        m.object = cc
        m.solver = "EXACT"
        ab.apply_all(target)
        bpy.data.objects.remove(cc, do_unlink=True)
    return target


def marsh_arch(rng, seed):
    """The marshes' arch (doc/BIOMES_RU.md, 01): eroded, mossy, only
    recalling two giants locked in a wrestle; one has lost his head. ~260 m
    tall, ~300 m across, the gap under their clinch the arch."""
    balls = []
    lean = rng.uniform(0.85, 1.15)
    for side in (-1, 1):
        foot = Vector((side * 125 * lean, rng.uniform(-15, 15), -20))
        hip = Vector((side * 95, rng.uniform(-10, 10), 70))
        shoulder = Vector((side * 42, rng.uniform(-12, 12), 175))
        # Legs apart, a body leaning into the other.
        chain(balls, foot + Vector((0, 30, 0)), hip, 34, 40)
        chain(balls, foot + Vector((0, -30, 0)), hip, 32, 40)
        chain(balls, hip, shoulder, 50, 58, size=(1, 0.8, 1))
        # Arms over the other's back.
        chain(balls, shoulder + Vector((0, 25, 10)), Vector((-side * 60, 30, 150)), 18, 14)
        chain(balls, shoulder + Vector((0, -25, 10)), Vector((-side * 55, -30, 140)), 18, 14)
    # The clinch, and one head.
    balls.append({"co": (0, 0, 195), "radius": 48, "size": (1.2, 0.9, 0.8)})
    side = rng.choice((-1, 1))
    balls.append({"co": (side * 38, rng.uniform(-8, 8), 236), "radius": 30, "size": (0.9, 0.85, 1.1)})
    balls.append({"co": (side * 36, 0, 218), "radius": 18})   # the neck
    o = ab.metaball_mass("marsh_arch", balls, resolution=2.2, threshold=0.6)
    return finish(o, 2.0, 34, 4, 3, seed)


def ruin_tower(rng, seed):
    """A buried tower of the golden desert (doc/BIOMES_RU.md, 04): a tapering
    drum of cyclopean courses, broken off at the top, slit windows. ~180 m
    above its foot; meant to be sunk in the dunes up to half its height."""
    h = rng.uniform(160, 200)
    r = rng.uniform(28, 38)
    bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=r, depth=h, location=(0, 0, h / 2))
    t = bpy.context.active_object
    ab.select_only(t)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.subdivide(number_cuts=10)
    bpy.ops.object.mode_set(mode="OBJECT")
    for v in t.data.vertices:
        k = v.co.z / h
        # Tapering, and stepping in a little at every 14 m course.
        ring = (1.0 - 0.22 * k) * (1.0 + 0.035 * (1 if int(v.co.z / 14) % 2 else -1))
        v.co.x *= ring
        v.co.y *= ring
    cutters = []
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, h + 20))
    c = bpy.context.active_object
    c.scale = (4 * r, 4 * r, 60)
    c.rotation_euler = (rng.uniform(0.25, 0.45), 0, rng.uniform(0, math.tau))
    cutters.append(c)
    for k in range(rng.randint(5, 9)):
        a = rng.uniform(0, math.tau)
        z = rng.uniform(0.35, 0.85) * h
        bpy.ops.mesh.primitive_cube_add(size=1, location=(math.cos(a) * r, math.sin(a) * r, z))
        w = bpy.context.active_object
        w.scale = (r * 0.8, 5, 18)
        w.rotation_euler = (0, 0, a + math.pi / 2)
        cutters.append(w)
    cut_with(t, cutters)
    voxel(t, 1.6)
    ab.weather(t, seed=seed, cracks=0.4, erosion=0.5, chips=0.5, scale=0.15)
    return t


def ruin_wall(rng, seed):
    """A wall between ruin towers, on arches: 1000 m along x (scaled to the
    gap it spans), ~70 m high, foot at z = 0; breached here and there."""
    L = 1000.0
    hgt = 70.0
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, hgt / 2))
    w = bpy.context.active_object
    w.scale = (L * 1.04, 14, hgt)
    ab.select_only(w)
    bpy.ops.object.transform_apply(scale=True)
    cutters = []
    n = 14
    for i in range(n):
        x = (i + 0.5) / n * L - L / 2
        bpy.ops.mesh.primitive_cylinder_add(vertices=20, radius=L / n * 0.36, depth=40,
                                            location=(x, 0, hgt * 0.42), rotation=(math.pi / 2, 0, 0))
        cutters.append(bpy.context.active_object)
    for k in range(rng.randint(2, 4)):
        bpy.ops.mesh.primitive_cube_add(size=1, location=(rng.uniform(-0.4, 0.4) * L, 0, hgt))
        b = bpy.context.active_object
        b.scale = (rng.uniform(40, 90), 40, rng.uniform(40, 80))
        b.rotation_euler = (0, rng.uniform(-0.4, 0.4), 0)
        cutters.append(b)
    cut_with(w, cutters)
    voxel(w, 2.0)
    ab.weather(w, seed=seed, cracks=0.4, erosion=0.4, chips=0.6, scale=0.3)
    return w


def giant_bones(rng, seed):
    """The ribcage of a giant half under the sand (doc/BIOMES_RU.md, 04): its
    ribs hooping out of the ground, the spine along their crowns. ~320 m long,
    ribs ~150 m high."""
    balls = []
    L = rng.uniform(280, 340)
    ribs = rng.randint(8, 11)
    crowns = []
    for i in range(ribs):
        u = i / max(ribs - 1, 1)
        x = (u - 0.5) * L * 0.8
        R = 70 + 40 * math.sin(math.pi * (0.2 + 0.7 * u))
        top = 1.3 * R
        crowns.append((x, top))
        prev = None
        for k in range(17):
            a = k / 16 * math.pi
            p = (x + 8 * math.sin(a), R * math.cos(a), top * math.sin(a) - 25)
            if prev:
                chain(balls, prev, p, 8, 7, step=0.6)
            prev = p
    for (x0, t0), (x1, t1) in zip(crowns, crowns[1:]):
        chain(balls, (x0, 0, t0 - 25), (x1, 0, t1 - 25), 14, 14, step=0.6)
    chain(balls, (crowns[0][0], 0, crowns[0][1] - 25), (crowns[0][0] - L * 0.15, 0, -10), 14, 9, step=0.6)
    o = ab.metaball_mass("giant_bones", balls, resolution=1.8, threshold=0.6)
    return finish(o, 1.6, 10, 1.0, 2, seed)


def white_rotunda(rng, seed):
    """The fallen white round building of the green plateau (doc/BIOMES_RU.md,
    06): a great drum lying on its side, hollow, ribbed with its colonnade.
    ~260 m across; meant to be half hidden between hills."""
    d = rng.uniform(230, 280)
    depth = d * 0.45
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=d / 2, depth=depth, location=(0, 0, d / 2 - 25),
                                        rotation=(math.pi / 2, 0, 0))
    drum = bpy.context.active_object
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=d / 2 - 16, depth=depth * 1.2, location=(0, 4, d / 2 - 25),
                                        rotation=(math.pi / 2, 0, 0))
    cut_with(drum, [bpy.context.active_object])
    ribs = []
    for k in range(40):
        a = k / 40 * math.tau
        bpy.ops.mesh.primitive_cube_add(size=1, location=(math.cos(a) * (d / 2 + 2), 0, d / 2 - 25 + math.sin(a) * (d / 2 + 2)))
        rb = bpy.context.active_object
        rb.scale = (6, depth * 0.9, 6)
        rb.rotation_euler = (0, -a, 0)
        ribs.append(rb)
    o = ab.union(drum, ribs)
    voxel(o, 1.8)
    ab.weather(o, seed=seed, cracks=0.3, erosion=0.2, chips=0.5, scale=0.12)
    return o


def sentinel_peak(rng, seed):
    """One of the two natural peaks guarding the way into the black lands
    (doc/BIOMES_RU.md, 10): gaunt, drawn out, a hooded shoulder near the top.
    ~800 m."""
    h = 800.0
    balls = []
    lean = Vector((rng.uniform(-30, 30), rng.uniform(-30, 30), 0))
    z = -40
    while z < h:
        t = max(0.0, z / h)
        r = 95 * (1 - 0.75 * t) + 8
        c = lean * t * t
        balls.append({"co": (c.x + rng.uniform(-6, 6), c.y + rng.uniform(-6, 6), z), "radius": r,
                      "size": (rng.uniform(0.8, 1.1), rng.uniform(0.6, 0.9), 0.9)})
        z += r * 0.35
    balls.append({"co": (lean.x * 0.5 + 25, lean.y * 0.5, h * 0.7), "radius": 60, "size": (1.1, 0.8, 1.3)})
    o = ab.metaball_mass("sentinel_peak", balls, resolution=5.0, threshold=0.6)
    o = finish(o, 4.0, 40, 9, 2, seed)
    # Knife ridges: grooves pulled in down its height.
    for v in o.data.vertices:
        a = math.atan2(v.co.y, v.co.x)
        k = 1 - 0.12 * max(0.0, math.cos(a * 5 + seed))
        v.co.x *= k
        v.co.y *= k
    return o


def spiral_tower(rng, seed):
    """The tower of the floating isles (doc/BIOMES_RU.md, 05): itself twisted
    into a spiral - a blade of stone wrung round its height. ~420 m."""
    from mathutils import Quaternion
    h = 420.0
    balls = []
    turns = rng.uniform(0.9, 1.4)
    for i in range(110):
        t = i / 109
        a = t * turns * math.tau
        r = 50 * (1 - 0.55 * t) + 6
        balls.append({"co": (0, 0, t * h - 20), "radius": r, "size": (1.5, 0.6, 0.35),
                      "rotation": Quaternion((0, 0, 1), a)})
    balls.append({"co": (0, 0, h - 10), "radius": 22, "size": (1, 1, 1.6)})
    o = ab.metaball_mass("spiral_tower", balls, resolution=2.4, threshold=0.6)
    return finish(o, 2.0, 12, 1.2, 2, seed)


def bridge_piece(rng, seed):
    """A fragment of the broken bridge between the floating isles: a slab of
    deck with its arch below, ragged at both ends. ~140 m long."""
    balls = []
    L = rng.uniform(110, 160)
    for i in range(12):
        x = (i / 11 - 0.5) * L
        balls.append({"co": (x, 0, -8), "radius": 14, "size": (1, 1.2, 0.6)})
        balls.append({"co": (x, 0, -16 - 6 * math.cos((i / 11 - 0.5) * math.pi)), "radius": 10 * rng.uniform(0.8, 1.2)})
    o = ab.metaball_mass("bridge_piece", balls, resolution=1.6, threshold=0.6)
    o = finish(o, 1.5, 8, 1.5, 2, seed)
    cut_above(o, 0.0)
    return o


NEW = {
    "spire": (spire, 26000),
    "bridge": (bridge, 9000),
    "valley_spire": (valley_spire, 14000),
    "marsh_arch": (marsh_arch, 20000),
    "ruin_tower": (ruin_tower, 9000),
    "ruin_wall": (ruin_wall, 12000),
    "giant_bones": (giant_bones, 14000),
    "white_rotunda": (white_rotunda, 14000),
    "sentinel_peak": (sentinel_peak, 18000),
    "spiral_tower": (spiral_tower, 14000),
    "bridge_piece": (bridge_piece, 4000),
}


def megalith_kinds():
    """megaliths.py's archetypes (their baked materials are not used)."""
    import megaliths
    return {k: v for k, v in megaliths.ARCHETYPES.items()}


# --- export ---------------------------------------------------------------

def cavity(obj):
    """Per-vertex occlusion from how concave the surface is round it: seams
    between boulders and the hollows under a lip go dark."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    size = max(obj.dimensions) or 1.0
    out = {}
    for v in bm.verts:
        c, n = 0.0, 0
        for e in v.link_edges:
            w = e.other_vert(v)
            d = w.co - v.co
            L = d.length
            if L > 1e-6:
                c += v.normal.dot(d) / L
                n += 1
        c = c / max(n, 1)
        out[v.index] = max(0.35, min(1.0, 1.0 - 2.2 * max(0.0, c)))
    bm.free()
    # Blur twice over the neighbours: occlusion is broad, not per vertex.
    mesh = obj.data
    nbrs = [[] for _ in mesh.vertices]
    for e in mesh.edges:
        a, b = e.vertices
        nbrs[a].append(b)
        nbrs[b].append(a)
    for _ in range(2):
        out = {i: (out[i] * 2 + sum(out[j] for j in nbrs[i])) / (2 + len(nbrs[i])) for i in out}
    return out


def write_mesh(obj, path):
    mesh = obj.data
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    for p in mesh.polygons:
        p.use_smooth = True
    ao = cavity(obj)
    mesh.calc_loop_triangles()
    try:
        normals = [n.vector for n in mesh.corner_normals]
    except AttributeError:
        mesh.calc_normals_split()
        normals = [l.normal for l in mesh.loops]
    zs = [v.co.z for v in mesh.vertices]
    z0, z1 = min(zs), max(zs)
    vertices, indices, seen = [], [], {}
    for tri in mesh.loop_triangles:
        for loop in tri.loops:
            vi = mesh.loops[loop].vertex_index
            index = seen.get(vi)
            if index is None:
                p = mesh.vertices[vi].co
                n = normals[loop]
                index = len(vertices)
                seen[vi] = index
                vertices.append((p.x, p.y, p.z, n.x, n.y, n.z, ao[vi], (p.z - z0) / max(z1 - z0, 1e-3)))
            indices.append(index)
    lo = [min(v[i] for v in vertices) for i in range(3)]
    hi = [max(v[i] for v in vertices) for i in range(3)]
    with open(path, "wb") as f:
        f.write(b"FOLM")
        f.write(struct.pack("<III", 1, len(vertices), len(indices)))
        f.write(struct.pack("<6f", *lo, *hi))
        for v in vertices:
            f.write(struct.pack("<8f", *v))
        f.write(struct.pack("<%dI" % len(indices), *indices))
    return len(indices) // 3, lo, hi


def levels(obj, budget, name):
    """Three levels: the budget, a quarter, a sixteenth (never under 300)."""
    out = []
    for k, tris in enumerate([budget, budget // 4, max(budget // 16, 300)]):
        copy = obj.copy()
        copy.data = obj.data.copy()
        bpy.context.scene.collection.objects.link(copy)
        ab.reduce(copy, tris)
        t, lo, hi = write_mesh(copy, OUT / f"{name}_lod{k}.mesh")
        out.append(t)
        bpy.data.objects.remove(copy, do_unlink=True)
    return out, lo, hi


def build(kind, variants, seed, catalogue, preview=None):
    ab.reset()
    makers = dict(NEW)
    if kind not in makers:
        makers.update(megalith_kinds())
    make, budget = makers[kind]
    made = []
    for v in range(variants):
        rng = random.Random(seed * 1000 + v + sum(map(ord, kind)) * 7)
        obj = make(rng, seed * 1000 + v)
        name = f"{kind}_{v}"
        obj.name = name
        ab.select_only(obj)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        if kind not in NEW:
            # megaliths.py stands its models on their lowest point: sink them
            # a tenth so a slope never shows their foot.
            for vtx in obj.data.vertices:
                vtx.co.z -= 0.08 * obj.dimensions.z
        # Centred on the footprint at the ground (z = 0).
        near = [vtx.co for vtx in obj.data.vertices if abs(vtx.co.z) < 0.15 * obj.dimensions.z] or \
               [vtx.co for vtx in obj.data.vertices]
        cx = sum(c.x for c in near) / len(near)
        cy = sum(c.y for c in near) / len(near)
        if kind != "bridge":
            for vtx in obj.data.vertices:
                vtx.co.x -= cx
                vtx.co.y -= cy
        if kind in NEW:
            obj.data.materials.clear()
        tris, lo, hi = levels(obj, budget, name)
        catalogue[name] = {"kind": kind, "lo": [round(a, 2) for a in lo], "hi": [round(a, 2) for a in hi],
                           "triangles": tris}
        print(f"{name}: {tris} triangles, {hi[2] - lo[2]:.0f} m tall, {hi[0] - lo[0]:.0f} x {hi[1] - lo[1]:.0f} m")
        made.append(obj)
    if preview:
        render_preview(made, preview)


def render_preview(objects, path):
    scene = bpy.context.scene
    x, tallest = 0.0, 0.0
    for o in objects:
        w = max(o.dimensions.x, o.dimensions.y)
        o.location = (x + w / 2, 0, 0)
        x += w * 1.3
        tallest = max(tallest, o.dimensions.z)
        o.data.materials.clear()
    mat = bpy.data.materials.new("ochre")
    mat.use_nodes = True
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs[0].default_value = (0.62, 0.38, 0.2, 1)
    for o in objects:
        o.data.materials.append(mat)
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    for e in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE", "BLENDER_WORKBENCH"):
        if e in engines:
            scene.render.engine = e
            break
    bpy.ops.object.light_add(type="SUN", rotation=(math.radians(55), 0, math.radians(-40)))
    bpy.context.active_object.data.energy = 4.0
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs[0].default_value = (0.35, 0.45, 0.7, 1)
    scene.world = world
    centre = Vector((x / 2, 0, tallest / 2))
    direction = Vector((0.2, -1.0, 0.25)).normalized()
    bpy.ops.object.camera_add(location=centre + direction * (x + tallest) * 3)
    cam = bpy.context.active_object
    cam.rotation_mode = "QUATERNION"
    cam.rotation_quaternion = (-direction).to_track_quat("-Z", "Y")
    cam.data.type = "ORTHO"
    cam.data.clip_end = (x + tallest) * 10
    aspect = 1280 / 640
    cam.data.ortho_scale = max(x * 1.05, tallest * 1.15 * aspect)
    scene.camera = cam
    scene.render.resolution_x, scene.render.resolution_y = 1280, 640
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


# What a full build makes: kind -> variants.
ALL = {"spire": 6, "bridge": 3, "valley_spire": 1,
       "marsh_arch": 2, "ruin_tower": 3, "ruin_wall": 2, "giant_bones": 2, "white_rotunda": 1,
       "sentinel_peak": 2, "spiral_tower": 1, "bridge_piece": 3,
       "menhir": 3, "fallen_menhir": 2, "marsh_panel": 3, "colossus_head": 2, "colossus_hand": 1,
       "needle": 3, "portal_tower": 2, "mirror_monument": 1, "iron_defender": 1, "cyclopean_block": 2,
       "giant_stair": 1}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--variants", type=int)
    parser.add_argument("--seed", type=int, default=3)
    parser.add_argument("--preview")
    args = parser.parse_args(ab.arguments())
    OUT.mkdir(parents=True, exist_ok=True)
    index = OUT / "props.json"
    catalogue = json.loads(index.read_text()) if index.exists() else {}
    kinds = ALL if args.all else {args.kind: args.variants or ALL.get(args.kind, 3)}
    for kind, variants in kinds.items():
        for name in [n for n, m in catalogue.items() if m["kind"] == kind]:
            del catalogue[name]
        preview = None
        if args.preview:
            p = Path(args.preview)
            preview = p if len(kinds) == 1 else p.with_name(p.stem + "_" + kind + p.suffix)
        build(kind, variants, args.seed, catalogue, preview)
        index.write_text(json.dumps(catalogue, indent=1))


if __name__ == "__main__":
    main()
