"""Dead trees for the stele's glade (doc/megaliths/A2_CARVED_STELE.md): bare,
crooked trunks with a few broken limbs, some snapped off at the top, low poly
(hexagonal tubes), and a dark weathered bark picture they all share.

    blender -b -P tools/blender/dead_trees.py

Writes assets/source/dead_trees/dead0<k>.glb and bark.png; build_foliage.py
--only dead01 ... turns them into the foliage models (foliage.json names
them).
"""
import math
import random
from pathlib import Path

import bpy
import bmesh
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets/source/dead_trees"
KINDS = 5
SIDES = 6
BARK_M = 2.4   # metres of trunk per repeat of the bark picture, up the trunk


def bark():
    """256 px: near-black grey-green bark in long cracked plates, pale lichen
    flecks, a darker char low down (the picture's bottom)."""
    import numpy as np
    rng = np.random.default_rng(5)
    n = 256
    img = np.zeros((n, n, 4), np.float32)
    x = np.arange(n)
    cols = np.zeros(n, np.float32)
    k = 0
    while k < n:   # plates: columns of random width, each its own shade
        w = int(rng.integers(5, 16))
        cols[k:k + w] = rng.uniform(0.75, 1.15)
        k += w
    v = np.linspace(0, 1, n)[:, None]
    base = np.array([0.20, 0.21, 0.18], np.float32)
    shade = cols[None, :] * (0.85 + 0.15 * np.sin(v * 40 + rng.uniform(0, 6, n)[None, :]))
    img[..., :3] = base * shade[..., None]
    # the cracks between the plates, long and dark
    edges = np.abs(np.diff(cols, append=cols[0])) > 0
    for c in np.nonzero(edges)[0]:
        img[:, c, :3] *= 0.35
        img[:, (c + 1) % n, :3] *= 0.6
    # cross cracks
    for _ in range(140):
        r, c = int(rng.integers(n)), int(rng.integers(n))
        img[r, c:c + int(rng.integers(2, 7)), :3] *= 0.45
    # lichen flecks
    for _ in range(320):
        r, c = int(rng.integers(n)), int(rng.integers(n))
        img[r:r + 2, c:c + 2, :3] = img[r:r + 2, c:c + 2, :3] * 0.4 + np.array([0.32, 0.36, 0.28]) * 0.6
    img[..., :3] *= (0.9 + 0.2 * rng.random((n, n, 1)))
    img[..., 3] = 1.0
    im = bpy.data.images.new("bark", n, n)
    im.pixels.foreach_set(np.clip(img, 0, 1).ravel())
    im.filepath_raw = str(OUT / "bark.png")
    im.file_format = "PNG"
    im.save()


def tube(bm, uv, path, radii, twist=0.0, u0=0.0):
    """A tapering tube along `path` (points), `SIDES` sides, capped at the end;
    bark UVs: u round it, v up it in BARK_M repeats."""
    rings = []
    length = 0.0
    for i, p in enumerate(path):
        if i:
            length += (p - path[i - 1]).length
        d = (path[min(i + 1, len(path) - 1)] - path[max(i - 1, 0)]).normalized()
        a = d.orthogonal().normalized()
        b = d.cross(a)
        ring = []
        for s in range(SIDES + 1):   # (the seam duplicated, for the UVs)
            t = s / SIDES * math.tau + twist
            ring.append((bm.verts.new(p + (a * math.cos(t) + b * math.sin(t)) * radii[i]), u0 + s / SIDES * 0.5,
                         length / BARK_M))
        rings.append(ring)
    for r0, r1 in zip(rings, rings[1:]):
        for s in range(SIDES):
            f = bm.faces.new((r0[s][0], r0[s + 1][0], r1[s + 1][0], r1[s][0]))
            for loop, (vert, uu, vv) in zip(f.loops, (r0[s], r0[s + 1], r1[s + 1], r1[s])):
                loop[uv].uv = (uu, vv)
    end = rings[-1]
    centre = bm.verts.new(path[-1] + (path[-1] - path[-2]).normalized() * radii[-1] * 0.6)
    for s in range(SIDES):
        f = bm.faces.new((end[s][0], end[s + 1][0], centre))
        for loop, (uu, vv) in zip(f.loops, ((end[s][1], end[s][2]), (end[s + 1][1], end[s + 1][2]), (end[s][1], end[s][2] + 0.05))):
            loop[uv].uv = (uu, vv)


def crooked(rng, start, direction, length, steps, wander):
    pts = [start]
    d = direction.normalized()
    for _ in range(steps):
        d = (d + Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-0.5, 0.6))) * wander).normalized()
        pts.append(pts[-1] + d * (length / steps))
    return pts


def tree(k):
    rng = random.Random(40 + k)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    height = rng.uniform(9.0, 15.0)
    r0 = rng.uniform(0.32, 0.5)
    snapped = k in (1, 4)   # broken off at the top: a tall snag
    trunk_len = height * (0.7 if snapped else 1.0)
    trunk = crooked(rng, Vector((0, 0, -0.4)), Vector((rng.uniform(-0.15, 0.15), rng.uniform(-0.15, 0.15), 1)),
                    trunk_len, 5, 0.22)
    radii = [r0 * (1.25 if i == 0 else 1.0) * (1 - 0.75 * i / (len(trunk) - 1)) for i in range(len(trunk))]
    if snapped:
        radii[-1] = radii[-2] * 0.8   # a broken stump top, not a point
    tube(bm, uv, trunk, radii)
    limbs = rng.randint(2, 3) if snapped else rng.randint(3, 6)
    for b in range(limbs):
        i = rng.randint(2, len(trunk) - 2)
        start = trunk[i]
        out = Vector((math.cos(b * 2.4 + rng.uniform(-0.4, 0.4)), math.sin(b * 2.4 + rng.uniform(-0.4, 0.4)),
                      rng.uniform(0.25, 1.1)))
        ln = height * rng.uniform(0.18, 0.4) * (0.6 if rng.random() < 0.3 else 1.0)   # some limbs broken short
        path = crooked(rng, start, out, ln, 3, 0.35)
        rb = radii[i] * rng.uniform(0.35, 0.55)
        tube(bm, uv, path, [rb * (1 - 0.8 * j / (len(path) - 1)) for j in range(len(path))], u0=0.5)
        if rng.random() < 0.6:   # a twig off the limb
            sub = crooked(rng, path[2], out + Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0.6)), ln * 0.5, 2, 0.4)
            tube(bm, uv, sub, [rb * 0.45, rb * 0.25, rb * 0.08], u0=0.5)
    me = bpy.data.meshes.new(f"dead0{k + 1}")
    bm.to_mesh(me)
    bm.free()
    mat = bpy.data.materials.get("bark") or bpy.data.materials.new("bark")
    me.materials.append(mat)
    ob = bpy.data.objects.new(me.name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    OUT.mkdir(parents=True, exist_ok=True)
    bark()
    for k in range(KINDS):
        ob = tree(k)
        for o in bpy.context.scene.objects:
            o.select_set(o is ob)
        bpy.context.view_layer.objects.active = ob
        path = OUT / f"{ob.name}.glb"
        bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLB", use_selection=True, export_yup=True,
                                  export_texcoords=True, export_normals=True, export_materials="NONE")
        print("wrote", path, len(ob.data.polygons), "faces")


main()
