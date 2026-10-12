"""Magic megalith A-1: the giant monolith slabs (doc/BIOME_PLAN.md, kind A:
blockout -> sculpt -> material, user approval at each step).

Reference (user, 12.10): thick rectangular monoliths standing in rows on a green plain
to the horizon; the sides sweep out in a long concave flare into the grass (on the giant
in the foreground the flare starts at about half height and is stronger on one side),
the tops are cut flat but slanted and chipped.

Blockout (approved 12.10): the giant's shape and proportions; every monolith in the field
is a variation of that giant at about the same size (the game may rescale them): thickness,
foot flare, top cut and bend differ, the copies turn any way and lean up to 7 degrees.
Sculpt (approved 12.10): surface relief on the same silhouettes (see sculpted_loops).
Material: PSX lattice tile and vertex colours (see the material stage section).
1.8 m human figures give the scale. PSX profile: low poly, flat shading, no smoothing.

    blender -b -P tools/blender/megalith_a_monolith.py                      # material stage
    blender -b -P tools/blender/megalith_a_monolith.py -- --stage sculpt
    blender -b -P tools/blender/megalith_a_monolith.py -- --stage blockout
    blender -b -P tools/blender/megalith_a_monolith.py -- --no-render
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
ASSET_DIR = ROOT / "assets/terrain_core/megaliths/a_monolith"
SHOTS = ROOT / "terrain_lab_out/shots_for_user/megaliths"
STAGE = "sculpt"

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


COLS = 6    # raised slab columns on a broad face (2 on a narrow face)
BAND = 22.0  # m, height of one band of raised slabs
GAP = 0.025  # share of a side between the two vertices of a column edge: the step face


def side_samples(cols):
    """(u, column) along one side; every column edge is two close vertices so steps stay sharp."""
    out = [(0.0, 0)]
    for k in range(1, cols):
        out += [(k / cols - GAP / 2, k - 1), (k / cols + GAP / 2, k)]
    return out


def perimeter(x0, x1, y0, y1, chamfer):
    """The chamfered rectangle resampled, counter-clockwise. Yields (x, y, outward normal, side,
    column on that side); corners are their own one-piece sides."""
    c = min(chamfer, (x1 - x0) * 0.22, (y1 - y0) * 0.22)
    r = 0.7071
    segs = [((x1, y0 + c), (x1, y1 - c), 2, (1, 0), "E"), ((x1, y1 - c), (x1 - c, y1), 1, (r, r), "NE"),
            ((x1 - c, y1), (x0 + c, y1), COLS, (0, 1), "N"), ((x0 + c, y1), (x0, y1 - c), 1, (-r, r), "NW"),
            ((x0, y1 - c), (x0, y0 + c), 2, (-1, 0), "W"), ((x0, y0 + c), (x0 + c, y0), 1, (-r, -r), "SW"),
            ((x0 + c, y0), (x1 - c, y0), COLS, (0, -1), "S"), ((x1 - c, y0), (x1, y0 + c), 1, (r, -r), "SE")]
    out = []
    for (ax, ay), (bx, by), n, nrm, side in segs:
        for u, col in side_samples(n):
            out.append((ax + (bx - ax) * u, ay + (by - ay) * u, nrm, side, col))
    return out


def cell_hash(*key):
    """Stable 0..1 per key (str hash is salted per process, crc32 is not)."""
    return random.Random(zlib.crc32(repr(key).encode())).random()


def sculpt_heights(v):
    """Ring heights (z, band index): dense through the flare, and a pair at every band edge
    (the lower ring keeps the band below, the upper one 1.5 m higher takes the band above)."""
    H = v["H"]
    pairs = {}
    zb = BAND
    while zb < H - 10:
        pairs[zb], pairs[zb + 1.5] = int(zb / BAND) - 1, int(zb / BAND)
        zb += BAND
    flare = [(i / 14) ** 1.5 * v["tf"] * H for i in range(15)]
    flare = [z for z in flare if z == 0.0 or min(abs(z - q) for q in pairs) > 3.0]
    return sorted([(z, int(z / BAND)) for z in flare] + list(pairs.items()))


def sculpted_loops(bm, v, extents, ch, sx, sy):
    """Sculpt stage, still PSX: a denser cage (2-3k tris) displaced by hand-set rules.
    - every face is cut into a grid of great blocks (columns x 22 m bands); a block stands
      proud by 0 / 1.5 / 3 m (narrow faces 0 / 1.2 m), with sharp ledges between blocks: the
      ground for the lattice pattern of the material stage;
    - the blocks fade into the flare, which slumps into the grass in slow lumps;
    - corners broken away in rare runs (notches 2-6 m deep), clean on the flare;
    - the top cut torn: jagged rim plus the big chip."""
    from mathutils import noise
    H, seed = v["H"], v.get("seed", 0)
    o = Vector((seed * 17.3, seed * 5.1, seed * 9.7))
    rings = [(-v.get("sink", SINK), 0)] + sculpt_heights(v) + [(H, None)]
    loops = []
    for i, (z, bnd) in enumerate(rings):
        t = max(z, 0.0) / H
        x0, x1, y0, y1 = extents(t)
        loop = []
        for k, (x, y, (nxv, nyv), side, col) in enumerate(perimeter(x0, x1, y0, y1, ch)):
            foot = max(0.0, 1.0 - z / (v["tf"] * H * 0.55))
            d = 1.4 * noise.noise(Vector((x, y, z)) / 140.0 + o)                 # slow swell
            d += foot * 4.5 * noise.noise(Vector((x, y, z)) / 30.0 + o)          # slumped foot
            if bnd is not None and len(side) == 1 and z < H - 8:
                if side in ("N", "S"):
                    step = int(cell_hash(seed, side, col, bnd) * 3) * 1.5
                else:
                    step = int(cell_hash(seed, side, col, bnd // 2) * 2) * 1.2
                d += step * (1.0 - foot) ** 2
            if len(side) == 2:                                                   # corners: broken runs
                n = noise.noise(Vector((z / 26.0, k * 3.7, 0.0)) + o)
                if n > 0.3:
                    d -= (2.0 + 4.0 * (n - 0.3) / 0.7) * (1.0 - foot)
            if z <= 0.0:
                d = 0.0
            px, py = x + nxv * d, y + nyv * d
            if bnd is None and i == len(rings) - 1:  # torn top rim, slanted, the big chip on one corner run
                pz = H + sx * px + sy * py + 4.5 * noise.noise(Vector((px, py, 0.0)) / 18.0 + o)
                if side in ("E", "SE") or (side == "S" and col >= COLS - 2):
                    pz -= v["chip"] * (0.6 + 0.4 * cell_hash(seed, "chip", k))
            else:
                pz = z
            loop.append(bm.verts.new((px, py, pz)))
        loops.append(loop)
    return loops


# --- material stage: PSX profile -----------------------------------------------
# One 128 px tile drawn in a 16-colour palette (no dithering, nearest filtering) carries the
# lattice: a schematic grid of grooves with traces, pads and sunken chips, on rust-brown stone.
# Big-scale colour rides on vertex colours the PS1 way (texture x vertex colour x 2, 0.5 is
# neutral): moss climbing the foot, red and green streaks running down from the ledges.

TILE_M = 48.0  # metres covered by one texture tile: lattice cells of 6 m
PALETTE = [  # sRGB 0..255
    (20, 14, 12), (45, 26, 18), (92, 40, 22), (120, 52, 28), (146, 68, 34), (170, 88, 44),
    (196, 118, 60), (224, 150, 70), (150, 34, 22), (186, 52, 30), (48, 70, 24), (78, 104, 34),
    (112, 134, 46), (100, 88, 40), (28, 44, 22), (210, 170, 120)]
DARKEST, DARK, RUST, HL, ORANGE, RED, RED2, MOSS, MOSS2, DEEP, PALE = 0, 1, (2, 3, 4, 5), 6, 7, 8, 9, 10, 11, 14, 15


def lattice_texture():
    import numpy as np
    n, cell = 128, 16
    rng = np.random.default_rng(11)
    yy, xx = np.mgrid[0:n, 0:n]
    h = np.zeros((n, n))
    for amp, fmin, fmax in ((1.0, 1, 4), (0.35, 5, 14)):  # tileable: whole periods only
        for _ in range(6):
            fx, fy = rng.integers(fmin, fmax, 2) * rng.choice([-1, 1], 2)
            h += amp * rng.uniform(0.3, 1.0) * np.sin(2 * np.pi * (fx * xx + fy * yy) / n + rng.uniform(0, 6.3))
    h = (h - h.min()) / (h.max() - h.min())
    img = np.array(RUST)[np.minimum((h * 4).astype(int), 3)]
    img[h > 0.80] = MOSS
    img[h > 0.90] = MOSS2
    # grid grooves with a lit lower/right lip, a few segments missing
    for a in range(n // cell):
        for b in range(n // cell):
            if rng.random() < 0.82:
                img[a * cell, b * cell:(b + 1) * cell] = DARKEST
                img[(a * cell + 1) % n, b * cell:(b + 1) * cell] = HL
            if rng.random() < 0.82:
                img[a * cell:(a + 1) * cell, b * cell] = DARKEST
                img[a * cell:(a + 1) * cell, (b * cell + 1) % n] = HL
    # inside each cell: traces ending in pads, or a sunken chip
    for a in range(n // cell):
        for b in range(n // cell):
            y0, x0 = a * cell, b * cell
            if rng.random() < 0.22:
                h_, w_ = rng.integers(3, 6), rng.integers(5, 10)
                cy, cx = y0 + rng.integers(3, cell - h_ - 1), x0 + rng.integers(3, cell - w_ - 1)
                img[cy:cy + h_, cx:cx + w_] = DARK
                img[cy, cx:cx + w_] = DARKEST
                img[cy:cy + h_, cx] = DARKEST
                continue
            for _ in range(rng.integers(1, 4)):
                off, s, ln = rng.integers(4, cell - 3), rng.integers(3, 8), rng.integers(4, 10)
                if rng.random() < 0.5:
                    img[y0 + off, x0 + s:x0 + min(s + ln, cell - 2)] = DARK
                    py, px = y0 + off, x0 + min(s + ln, cell - 3)
                else:
                    img[y0 + s:y0 + min(s + ln, cell - 2), x0 + off] = DARK
                    py, px = y0 + min(s + ln, cell - 3), x0 + off
                img[py - 1:py + 2, px - 1:px + 2] = DARKEST
                img[py, px] = ORANGE if rng.random() < 0.7 else PALE
    # drips running down from the grooves (rows grow downward in the image = down the stone)
    for _ in range(14):
        x, y, ln = rng.integers(0, n), rng.integers(0, n // cell) * cell + 2, rng.integers(8, 40)
        col = (RED, RED2) if rng.random() < 0.55 else (MOSS, DEEP)
        for k in range(ln):
            img[(y + k) % n, x] = col[0] if k < ln * 0.7 else col[1]
    rgba = np.concatenate([np.array(PALETTE, dtype=np.float32)[img] / 255.0, np.ones((n, n, 1), np.float32)], 2)
    im = bpy.data.images.new("monolith_lattice_128", n, n, alpha=False)
    im.pixels.foreach_set(rgba[::-1].ravel())  # Blender rows run bottom-up
    path = ASSET_DIR / "monolith_lattice_128.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    im.filepath_raw = str(path)
    im.file_format = "PNG"
    im.save()
    print("texture", path, "colours", len(np.unique(img)))
    return im


def psx_material(image):
    m = bpy.data.materials.new("monolith_psx")
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Roughness"].default_value = 1.0
    for key in ("Specular IOR Level", "Specular"):
        if key in bsdf.inputs:
            bsdf.inputs[key].default_value = 0.0
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image
    tex.interpolation = "Closest"
    vcol = nt.nodes.new("ShaderNodeVertexColor")
    vcol.layer_name = "Col"
    mul = nt.nodes.new("ShaderNodeVectorMath")
    mul.operation = "MULTIPLY"
    x2 = nt.nodes.new("ShaderNodeVectorMath")
    x2.operation = "SCALE"
    x2.inputs["Scale"].default_value = 2.0
    nt.links.new(tex.outputs["Color"], mul.inputs[0])
    nt.links.new(vcol.outputs["Color"], mul.inputs[1])
    nt.links.new(mul.outputs["Vector"], x2.inputs[0])
    # moss: the tile's brightness tinted green, laid over by the per-vertex moss mask
    luma = nt.nodes.new("ShaderNodeRGBToBW")
    green = nt.nodes.new("ShaderNodeVectorMath")
    green.operation = "MULTIPLY"
    green.inputs[1].default_value = (0.55, 1.45, 0.38)
    mask = nt.nodes.new("ShaderNodeVertexColor")
    mask.layer_name = "Moss"
    mix = nt.nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    pin = {i.identifier: i for i in mix.inputs}
    nt.links.new(tex.outputs["Color"], luma.inputs[0])
    nt.links.new(luma.outputs[0], green.inputs[0])
    nt.links.new(mask.outputs["Color"], pin["Factor_Float"])
    nt.links.new(x2.outputs["Vector"], pin["A_Color"])
    nt.links.new(green.outputs["Vector"], pin["B_Color"])
    nt.links.new(next(o for o in mix.outputs if o.identifier == "Result_Color"), bsdf.inputs["Base Color"])
    return m


def paint(me, v):
    """UVs: each face projected on its dominant axis, TILE_M metres per tile, v up the stone.
    Vertex colours: red/green streaks down the body, dark ground contact; a second layer\n    "Moss" masks the green over the foot and in patches."""
    from mathutils import noise
    H, seed = v["H"], v.get("seed", 0)
    o = Vector((seed * 7.7, seed * 3.3, seed * 1.9))
    uv = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        nx, ny, nz = (abs(c) for c in poly.normal)
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            if nz > max(nx, ny):
                uv.data[li].uv = (co.x / TILE_M, co.y / TILE_M)
            elif nx > ny:
                uv.data[li].uv = (co.y / TILE_M, co.z / TILE_M)
            else:
                uv.data[li].uv = (co.x / TILE_M, co.z / TILE_M)
    attr = me.color_attributes.new("Col", "BYTE_COLOR", "POINT")
    moss_attr = me.color_attributes.new("Moss", "BYTE_COLOR", "POINT")
    red_c, green_c = Vector((0.85, 0.30, 0.22)), Vector((0.30, 0.66, 0.26))
    for vert in me.vertices:
        x, y, z = vert.co
        c = Vector((0.5, 0.5, 0.5))
        foot = min(1.0, max(0.0, 1.0 - z / (v["tf"] * H * 0.6)))
        moss = min(1.0, max(0.0, foot * 1.2 + 0.7 * noise.noise(Vector((x, y, z)) / 30.0 + o) - 0.15))
        moss_attr.data[vert.index].color = (moss, moss, moss, 1.0)
        streak = noise.noise(Vector(((x + y) / 9.0, seed * 3.1, z / 260.0)))
        if streak > 0.2:
            amount = min(1.0, (streak - 0.2) / 0.5) * (0.35 + 0.65 * z / H)
            hue = red_c if noise.noise(Vector(((x - y) / 30.0, 5.0 + seed, z / 500.0))) > -0.1 else green_c
            c = c.lerp(hue, amount)
        c *= 0.7 + 0.3 * min(1.0, max(z, 0.0) / 14.0)
        attr.data[vert.index].color = (c.x, c.y, c.z, 1.0)  # linear: the shader multiplies as-is


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
    if STAGE in ("sculpt", "material"):
        loops = sculpted_loops(bm, v, extents, ch, sx, sy)
    for i, t in enumerate([] if loops else [-v.get("sink", SINK) / H] + T):
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
    if STAGE == "material":
        paint(me, v)
    me.materials.append(mat)
    return me


def link(name, me, loc, rot_z=0.0, lean=0.0, scale=1.0, coll=None, tilt_y=0.0):
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.rotation_euler = (math.radians(lean), math.radians(tilt_y), rot_z)
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
    size, step, cy = 10000.0, 20.0, 2000.0
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


def variation(rnd):
    """The giant with its width-to-height kept, everything else nudged: thickness, how hard and
    to which side the foot flares, the slant and chip of the top cut, a sideways bend."""
    b = VARIANTS["B_giant"]
    fl = [f * rnd.uniform(0.6, 1.4) for f in b["flare"]]
    if rnd.random() < 0.5:  # the strong flare goes to the other side
        fl[0], fl[1] = fl[1], fl[0]
    return dict(b, D=b["D"] * rnd.uniform(0.7, 1.35), flare=tuple(fl), tf=b["tf"] * rnd.uniform(0.85, 1.15),
                p=rnd.uniform(2.0, 2.8), top=rnd.uniform(0.0, 0.04), slant=(rnd.uniform(-9, 9), rnd.uniform(-5, 5)),
                chip=rnd.uniform(0, 28), bend=rnd.uniform(-0.04, 0.05), sink=16.0)


def collection(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def build():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if STAGE == "material":
        stone = field_stone = psx_material(lattice_texture())
    else:
        stone, field_stone = material("stone_blockout", STONE), material("field_stone_blockout", FIELD_STONE)
    grass, red = material("ground_blockout", GROUND), material("human", HUMAN)
    ground(grass)
    lineup, field, people, sheet = collection("lineup"), collection("field"), collection("people"), collection("sheet")
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

    # variations of the giant; a sheet of the first six side by side for comparison
    rnd = random.Random(7)
    kinds = [slab(f"giant_var_{k:02d}", dict(variation(rnd), seed=k + 1), field_stone) for k in range(12)]
    for k in range(6):
        x = -650 + k * 260
        link(f"sheet_{k:02d}", kinds[k], (x, -700, ground_height(x, -700) - 1.0), coll=sheet,
             rot_z=math.radians(-50 + k * 20), lean=rnd.uniform(-7, 7), tilt_y=rnd.uniform(-7, 7))
    # field: variations at about the giant's size, turned any way, leaning a little, in loose rows
    count = 0
    for r in range(9):
        y = 1000 + r * 680 + rnd.uniform(-120, 120)
        x = -3800 + rnd.uniform(0, 400)
        while x < 3800:
            x += rnd.uniform(480, 950)
            px, py = x, y + rnd.uniform(-150, 150)
            ob = link(f"field_{count:03d}", rnd.choice(kinds), (px, py, ground_height(px, py) - 1.0),
                      rot_z=math.radians(rnd.uniform(0, 360)), scale=rnd.uniform(0.8, 1.15), coll=field,
                      lean=rnd.uniform(-7, 7), tilt_y=rnd.uniform(-7, 7))
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
    ob.rotation_euler = (math.radians(68), 0, math.radians(-62))
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


# name, camera, target, lens, collections shown besides the giant and the seated pair
VIEWS = [
    ("01_front_all", (0, -760, 60), (0, 0, 120), 35, {"lineup", "field"}),
    ("02_three_quarter", (430, -600, 110), (0, 0, 100), 35, {"lineup"}),
    ("03_side_profiles", (680, -30, 40), (0, 0, 110), 30, {"lineup"}),
    ("04_reference_shot", (-150, -172, 1.6), (20, 810, 110), 22, {"field"}),
    ("05_variation_sheet", (0, -1520, 40), (0, -700, 120), 20, {"sheet"}),
    ("06_close_face", (-150, -230, 40), (0, 0, 110), 35, {"lineup"}),
    ("07_close_top", (150, -260, 330), (0, 0, 262), 35, set()),
]


def render_views(render):
    SHOTS.mkdir(parents=True, exist_ok=True)
    sc = bpy.context.scene
    toggled = [bpy.data.collections[n] for n in ("lineup", "field", "sheet")]
    for name, loc, target, lens, shown in VIEWS:
        cam = camera("cam_" + name, loc, target, lens)
        if not render:
            continue
        sc.camera = cam
        for c in toggled:
            c.hide_render = c.name not in shown
        sc.render.filepath = str(SHOTS / f"a1_monolith_{STAGE}_{name}.png")
        bpy.ops.render.render(write_still=True)
        print("wrote", sc.render.filepath)
    for c in toggled:
        c.hide_render = False
    sc.camera = bpy.data.objects["cam_" + VIEWS[0][0]]


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--stage", choices=("blockout", "sculpt", "material"), default="material")
    args = ap.parse_args(argv)
    global STAGE
    STAGE = args.stage
    build()
    setup_render()
    render_views(not args.no_render)
    blend = ASSET_DIR / f"monolith_{STAGE}.blend"
    blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
