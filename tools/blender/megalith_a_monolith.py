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
Material: red hatched stone with moss laid on by noise (see the material stage section).
1.8 m human figures give the scale. Retro look: low poly, flat shading, hard texture pixels.

    blender -b -P tools/blender/megalith_a_monolith.py                      # material stage
    blender -b -P tools/blender/megalith_a_monolith.py -- --stage sculpt
    blender -b -P tools/blender/megalith_a_monolith.py -- --stage blockout
    blender -b -P tools/blender/megalith_a_monolith.py -- --no-render
    blender -b -P tools/blender/megalith_a_monolith.py -- --no-render --export   # game copy + manifests
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
VARIANT_PARAMS = []  # filled by build(): the giant, then its 12 variations
CONFIG_DIR = ROOT / "content/config/terrain_core/megaliths"

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


# --- material stage --------------------------------------------------------------
# Rough retro look after the reference, not a strict PSX pipeline: low-res tiles with hard
# pixels. Two textures: red hatched stone (uneven strokes and a wobbly broken grid) and moss.
# The material lays moss over the stone by a noise mask (pixel-snapped, thicker on the foot
# and on up-facing ledges) and runs dark red streaks down the body. Every knob is a labelled
# Value node in the material ("moss_coverage", "moss_scale", ...); each object gets its own
# offset and coverage nudge from Object Info > Random, so copies never repeat.

TILE_M = 36.0   # metres covered by one stone tile
TEX = 256       # texture size drawn, px
PIXEL = 2       # saved at TEX / PIXEL: each saved texel is a PIXEL x PIXEL block, chunkier pixels

MOSS_KNOBS = {  # name: default
    "moss_coverage": 0.12,     # 0 none .. 1 all
    "moss_scale": 0.06,        # noise frequency per metre (0.06 = ~17 m patches)
    "moss_softness": 0.06,     # edge width of a patch
    "moss_foot_m": 95.0,       # moss thickens below this height
    "moss_foot_weight": 0.3,   # how much the foot adds
    "moss_ledge_weight": 0.45, # how much up-facing faces add
    "moss_pixel_m": 0.7,       # mask pixel size, m (retro blocky edges)
    "coverage_jitter": 0.3,    # per-object random spread of coverage
    "streak_strength": 0.55,   # dark red streaks running down
    "moss_tile_m": 22.0,       # metres per moss tile
    "stone_saturation": 0.6,   # red of the stone: 1 as painted, lower is duller (toward brown)
    "stone_value": 0.9,        # brightness of the stone
    "scratch_strength": 0.7,   # pale scratch dashes over the stone
    "scratch_tile_m": 60.0,    # metres per scratch tile (off the stone tile, so they never line up)
    "bump_strength": 1.0,      # overall relief of the surface (normal)
    "bump_distance_m": 0.3,    # how tall the relief reads, m
    "stroke_height": 0.6,      # pale strokes stand proud as thin ridges, also under the moss
    "groove_depth": 0.7,       # hatch grooves sink in
    "moss_height": 1.0,        # moss sits on top as a cushion, lumpy by its own texture
    "ao_strength": 0.6,        # occlusion from the surrounding shape (EEVEE AO node)
    "ao_distance_m": 6.0,      # reach of that occlusion, m
    "cavity_strength": 0.45,   # grooves and the moss edges darkened from the relief itself
    "stone_roughness": 0.78,
    "stroke_roughness": 0.55,  # the strokes are a bit glossier, worn smooth
    "moss_roughness": 1.0,
    "specular": 0.3,
    "moss_streak_stretch": 7.0,  # patches this many times taller than wide: moss runs in bands
    "moss_edge_weight": 0.4,   # how much corners, block edges and ledges add
    "moss_groove_weight": 0.8, # moss seeping along the grooves of the hatch grid
    "groove_gate": 0.68,       # 0 every groove .. 1 almost none: where the seeping happens
    "moss_fine_scale": 0.3,    # small noise that frays every moss edge (per metre)
    "moss_ragged": 0.4,        # how hard the small noise frays the edges
    "groove_warp": 0.012,      # how far the seeping wanders off the groove lines (tiles)
}


def tileable(rng, n, fmin, fmax):
    """Isotropic noise that wraps: white noise band-passed to fmin..fmax periods per tile."""
    import numpy as np
    f = np.fft.fftfreq(n) * n
    r = np.hypot(*np.meshgrid(f, f))
    band = ((r >= fmin) & (r <= fmax)) / np.maximum(r, 1.0)
    h = np.real(np.fft.ifft2(np.fft.fft2(rng.normal(size=(n, n))) * band))
    return (h - h.min()) / (h.max() - h.min())


def save_image(name, img, pixel=None):
    import numpy as np
    px = PIXEL if pixel is None else pixel
    n = img.shape[0] // px
    img = img[:n * px, :n * px].reshape(n, px, n, px, 3).mean(axis=(1, 3))
    img = np.clip(np.round(img / 6.0) * 6.0, 0, 255)  # posterize a little: retro, not smooth
    rgba = np.concatenate([img.astype(np.float32) / 255.0, np.ones((n, n, 1), np.float32)], 2)
    im = bpy.data.images.new(name, n, n, alpha=False)
    im.pixels.foreach_set(rgba[::-1].ravel())  # Blender rows run bottom-up
    path = ASSET_DIR / f"{name}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    im.filepath_raw = str(path)
    im.file_format = "PNG"
    im.save()
    print("texture", path)
    return im


def hatch_texture():
    """Red hatched stone: dark red ground, a mass of short vertical strokes, fewer cross strokes,
    and a wobbly, broken grid of grooves with uneven spacing."""
    import numpy as np
    n = TEX
    rng = np.random.default_rng(21)
    base = tileable(rng, n, 1, 5)
    ramp = np.array([(70, 18, 12), (96, 26, 16), (122, 34, 20), (148, 44, 26)], float)
    img = ramp[np.minimum((base * 4).astype(int), 3)] + rng.normal(0, 7, (n, n, 1))
    grooves = np.zeros((n, n, 3))

    def stroke(x, y, ln, col, alpha, vertical=True, width=1):
        for k in range(ln):
            if rng.random() < 0.08:
                x += rng.choice((-1, 1))
            for w in range(width):
                yy, xx = ((y + k) % n, (x + w) % n) if vertical else ((x + w) % n, (y + k) % n)
                img[yy, xx] = img[yy, xx] * (1 - alpha) + np.array(col) * alpha

    cols = [(30, 8, 6), (150, 42, 26), (186, 62, 36), (205, 98, 52), (14, 6, 5)]
    probs = [0.33, 0.3, 0.2, 0.05, 0.12]
    for _ in range(1200):
        c = cols[rng.choice(5, p=probs)]
        stroke(rng.integers(0, n), rng.integers(0, n), rng.integers(3, 18), c, rng.uniform(0.45, 0.85),
               width=2 if rng.random() < 0.15 else 1)
    for _ in range(900):
        stroke(rng.integers(0, n), rng.integers(0, n), rng.integers(3, 12), cols[rng.choice((0, 1, 2, 4))],
               rng.uniform(0.4, 0.8), vertical=False)
    # wobbly broken grid: rows and columns at uneven spacing, gaps in runs
    for axis, gap in ((0, (10, 26)), (1, (10, 30))):
        pos = 0
        while True:
            pos += rng.integers(*gap)
            if pos >= n - 6:
                break
            f, ph, on = rng.integers(1, 4), rng.uniform(0, 6.3), True
            for t in range(n):
                if rng.random() < 0.03:
                    on = not on if rng.random() < 0.6 else on
                if not on:
                    continue
                o = pos + int(round(1.6 * np.sin(2 * np.pi * f * t / n + ph)))
                for d, col, al in ((0, (20, 6, 5), 0.85), (1, (34, 10, 7), 0.6), (2, (184, 74, 42), 0.45)):
                    p = ((o + d) % n, t) if axis == 0 else (t, (o + d) % n)
                    img[p] = img[p] * (1 - al) + np.array(col) * al
                for d in (-1, 0, 1, 2):
                    grooves[((o + d) % n, t) if axis == 0 else (t, (o + d) % n)] = 255
    # soften the groove mask so the shader can eat into it with noise instead of following lines
    f = np.fft.fftfreq(n)
    fx, fy = np.meshgrid(f, f)
    blur = np.exp(-(fx ** 2 + fy ** 2) * (2 * np.pi * 2.5) ** 2 / 2)
    g = np.real(np.fft.ifft2(np.fft.fft2(grooves[..., 0]) * blur))
    g = np.clip(g / max(g.max(), 1e-6), 0, 1) * 255
    return save_image("monolith_hatch_128", img), save_image("monolith_hatch_grooves_128", np.repeat(g[..., None], 3, 2))


def scratch_texture():
    """Pale strokes (mask, white on black): long sweeping dashes, mostly up-down with a lean,
    some across, drifting slightly as they go, thick in the middle and thinning to the ends,
    like the light pen strokes on the reference stone."""
    import numpy as np
    n = TEX
    rng = np.random.default_rng(33)
    img = np.zeros((n, n, 3))
    for _ in range(95):
        y, x = rng.uniform(0, n, 2)
        ang = rng.normal(np.pi / 2, 0.05) if rng.random() < 0.9 else rng.normal(0.0, 0.05)
        ln, val, width = int(rng.integers(70, 170)), rng.uniform(190, 255), 1.0
        bend = rng.normal(0, 0.0015)
        for k in range(ln):
            ang += bend
            y, x = y + np.sin(ang), x + np.cos(ang)
            taper = np.sin(np.pi * (k + 0.5) / ln) ** 0.6  # thick middle, thin ends
            w = max(1.0, width * taper)
            for oy in range(-int(w), int(w) + 1):
                for ox in range(-int(w), int(w) + 1):
                    if oy * oy + ox * ox <= w * w * 0.6:
                        py, px = int(y + oy) % n, int(x + ox) % n
                        img[py, px] = np.maximum(img[py, px], val * (0.55 + 0.45 * taper))
    return save_image("monolith_strokes_256", img, pixel=1)  # full size: hairline strokes


def moss_texture():
    """Moss: clumps in several greens, light and dark speckles, a few holes of red stone."""
    import numpy as np
    n = TEX
    rng = np.random.default_rng(5)
    v = 0.6 * tileable(rng, n, 2, 7) + 0.4 * tileable(rng, n, 9, 30)
    ramp = np.array([(28, 46, 16), (46, 72, 22), (70, 100, 30), (98, 124, 38), (132, 150, 52)], float)
    img = ramp[np.minimum((v * 5).astype(int), 4)] + rng.normal(0, 6, (n, n, 1))
    img[v < 0.1] = (96, 32, 20)
    for _ in range(1800):
        y, x = rng.integers(0, n, 2)
        img[y, x] = (166, 176, 66) if rng.random() < 0.6 else (16, 28, 10)
    for _ in range(260):
        y, x = rng.integers(0, n, 2)
        for k in range(rng.integers(2, 7)):
            img[(y + k) % n, (x + (k // 2) * rng.choice((-1, 1))) % n] = (14, 22, 8)
    return save_image("monolith_moss_128", img)


def stone_material(hatch, grooves, moss, scratches):
    m = bpy.data.materials.new("monolith_hatch_moss")
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    for key in ("Specular IOR Level", "Specular"):
        if key in bsdf.inputs:
            pass  # set from the "specular" knob below
    N = nt.nodes.new
    L = nt.links.new

    def node(kind, **props):
        nd = N(kind)
        for k, val in props.items():
            setattr(nd, k, val)
        return nd

    def put(sock, val):
        if isinstance(val, bpy.types.NodeSocket):
            L(val, sock)
        else:
            sock.default_value = val

    def math(op, a, b=0.0, clamp=False):
        nd = node("ShaderNodeMath", operation=op, use_clamp=clamp)
        put(nd.inputs[0], a)
        put(nd.inputs[1], b)
        return nd.outputs[0]

    def map_range(val, a, b, c, d):
        nd = node("ShaderNodeMapRange")
        for key, x in (("Value", val), ("From Min", a), ("From Max", b), ("To Min", c), ("To Max", d)):
            put(nd.inputs[key], x)
        return nd.outputs["Result"]

    def mix(fac, a, b, blend="MIX"):
        nd = node("ShaderNodeMix", data_type="RGBA", blend_type=blend)
        pin = {i.identifier: i for i in nd.inputs}
        put(pin["Factor_Float"], fac)
        put(pin["A_Color"], a)
        put(pin["B_Color"], b)
        return next(o for o in nd.outputs if o.identifier == "Result_Color")

    knob = {}
    for i, (name, val) in enumerate(MOSS_KNOBS.items()):
        nd = node("ShaderNodeValue", name=name, label=name)
        nd.outputs[0].default_value = val
        nd.location = (-1400, 400 - i * 90)
        knob[name] = nd.outputs[0]

    coords = node("ShaderNodeTexCoord")
    info = node("ShaderNodeObjectInfo")
    rand = info.outputs["Random"]
    # stone and moss: box-projected in object space, so the slanted flare does not stretch them;
    # the moss tile has its own scale so it never lines up with the stone grid
    stone_v = node("ShaderNodeVectorMath", operation="SCALE")
    L(coords.outputs["Object"], stone_v.inputs[0])
    stone_v.inputs["Scale"].default_value = 1.0 / TILE_M
    stone = node("ShaderNodeTexImage", image=hatch, interpolation="Closest", projection="BOX", projection_blend=0.15)
    L(stone_v.outputs["Vector"], stone.inputs["Vector"])
    moss_v = node("ShaderNodeVectorMath", operation="SCALE")
    L(coords.outputs["Object"], moss_v.inputs[0])
    put(moss_v.inputs["Scale"], math("DIVIDE", 1.0, knob["moss_tile_m"]))
    moss_tex = node("ShaderNodeTexImage", image=moss, interpolation="Closest", projection="BOX", projection_blend=0.15)
    L(moss_v.outputs["Vector"], moss_tex.inputs["Vector"])
    # streaks: noise stretched down the stone, darkening toward old blood red
    stretch = node("ShaderNodeMapping")
    L(coords.outputs["Object"], stretch.inputs["Vector"])
    stretch.inputs["Scale"].default_value = (0.12, 0.12, 0.006)
    off = node("ShaderNodeCombineXYZ")
    put(off.inputs[0], math("MULTIPLY", rand, 731.0))
    put(off.inputs[1], math("MULTIPLY", rand, 377.0))
    L(off.outputs[0], stretch.inputs["Location"])
    streak_n = node("ShaderNodeTexNoise")
    L(stretch.outputs["Vector"], streak_n.inputs["Vector"])
    streak_n.inputs["Scale"].default_value = 1.0
    streak = math("MULTIPLY", map_range(streak_n.outputs["Fac"], 0.52, 0.72, 0.0, 1.0), knob["streak_strength"])
    streaked = mix(streak, stone.outputs["Color"], (0.42, 0.13, 0.10, 1.0), blend="MULTIPLY")
    # slow light/dark drift over tens of metres hides the tile repeat
    drift = node("ShaderNodeTexNoise")
    drift_v = node("ShaderNodeVectorMath", operation="ADD")
    L(coords.outputs["Object"], drift_v.inputs[0])
    L(off.outputs[0], drift_v.inputs[1])
    L(drift_v.outputs["Vector"], drift.inputs["Vector"])
    drift.inputs["Scale"].default_value = 0.012
    shade = node("ShaderNodeVectorMath", operation="SCALE")
    L(streaked, shade.inputs[0])
    put(shade.inputs["Scale"], map_range(drift.outputs["Fac"], 0.3, 0.7, 0.72, 1.18))
    # dull the red toward a warm brown of the same brightness (plain desaturation goes pink)
    luma = node("ShaderNodeRGBToBW")
    L(shade.outputs["Vector"], luma.inputs[0])
    brown = node("ShaderNodeVectorMath", operation="SCALE")
    brown.inputs[0].default_value = (1.25, 0.82, 0.6)
    L(luma.outputs[0], brown.inputs["Scale"])
    dulled = mix(math("SUBTRACT", 1.0, knob["stone_saturation"]), shade.outputs["Vector"], brown.outputs["Vector"])
    dim = node("ShaderNodeVectorMath", operation="SCALE")
    L(dulled, dim.inputs[0])
    L(knob["stone_value"], dim.inputs["Scale"])
    # pale scratch dashes, a warm light tone over the dulled stone
    scr_v = node("ShaderNodeVectorMath", operation="SCALE")
    L(coords.outputs["Object"], scr_v.inputs[0])
    put(scr_v.inputs["Scale"], math("DIVIDE", 1.0, knob["scratch_tile_m"]))
    scr = node("ShaderNodeTexImage", image=scratches, interpolation="Closest", projection="BOX", projection_blend=0.15)
    L(scr_v.outputs["Vector"], scr.inputs["Vector"])
    stone_col = mix(math("MULTIPLY", scr.outputs["Color"], knob["scratch_strength"]), dim.outputs["Vector"],
                    (0.46, 0.21, 0.13, 1.0))  # linear: a light warm clay
    # moss mask: pixel-snapped banded noise + foot + ledges + edges + per-object nudge, then grooves
    shifted = node("ShaderNodeVectorMath", operation="ADD")
    L(coords.outputs["Object"], shifted.inputs[0])
    L(off.outputs[0], shifted.inputs[1])
    snap = node("ShaderNodeVectorMath", operation="SNAP")
    L(shifted.outputs["Vector"], snap.inputs[0])
    L(knob["moss_pixel_m"], snap.inputs[1])
    # banded noise: squashed in z, so patches run down the stone as streaks
    squash = node("ShaderNodeMapping")
    L(snap.outputs["Vector"], squash.inputs["Vector"])
    sq = node("ShaderNodeCombineXYZ")
    put(sq.inputs[0], 1.0)
    put(sq.inputs[1], 1.0)
    put(sq.inputs[2], math("DIVIDE", 1.0, knob["moss_streak_stretch"]))
    L(sq.outputs[0], squash.inputs["Scale"])
    moss_n = node("ShaderNodeTexNoise")
    L(squash.outputs["Vector"], moss_n.inputs["Vector"])
    L(knob["moss_scale"], moss_n.inputs["Scale"])
    moss_n.inputs["Detail"].default_value = 4.0
    moss_n.inputs["Roughness"].default_value = 0.55
    moss_n.inputs["Distortion"].default_value = 0.6
    sep = node("ShaderNodeSeparateXYZ")
    L(coords.outputs["Object"], sep.inputs[0])
    foot = math("MULTIPLY", map_range(sep.outputs["Z"], 0.0, knob["moss_foot_m"], 1.0, 0.0), knob["moss_foot_weight"])
    geo = node("ShaderNodeNewGeometry")
    nz = node("ShaderNodeSeparateXYZ")
    L(geo.outputs["Normal"], nz.inputs[0])
    ledge = math("MULTIPLY", map_range(nz.outputs["Z"], 0.35, 0.9, 0.0, 1.0), knob["moss_ledge_weight"])
    nudge = math("MULTIPLY", math("SUBTRACT", rand, 0.5), knob["coverage_jitter"])
    spread = map_range(moss_n.outputs["Fac"], 0.32, 0.68, 0.0, 1.0)  # noise sits near 0.5: stretch it
    # small noise that frays every edge of the moss
    fine = node("ShaderNodeTexNoise")
    L(snap.outputs["Vector"], fine.inputs["Vector"])
    L(knob["moss_fine_scale"], fine.inputs["Scale"])
    fine.inputs["Detail"].default_value = 3.0
    fine.inputs["Distortion"].default_value = 1.2
    ragged = math("MULTIPLY", math("SUBTRACT", fine.outputs["Fac"], 0.5), math("MULTIPLY", knob["moss_ragged"], 2.0))
    # corners, block edges and ledges: baked per vertex into "_edge" (see uv_project); it fades
    # across the neighbouring faces, so the ragged noise lets moss spill off the ledges in clumps
    edge_attr = node("ShaderNodeAttribute", attribute_name="_edge")
    on_edge = math("MULTIPLY", edge_attr.outputs["Fac"], knob["moss_edge_weight"])
    s = math("ADD", math("ADD", spread, foot), math("ADD", math("ADD", ledge, nudge), math("ADD", on_edge, ragged)))
    edge = math("SUBTRACT", 1.15, knob["moss_coverage"])
    mask = map_range(s, math("SUBTRACT", edge, knob["moss_softness"]), math("ADD", edge, knob["moss_softness"]), 0.0, 1.0)
    # moss seeping along the hatch grooves, in runs where the banded noise lets it through
    wobble = node("ShaderNodeTexNoise")
    L(shifted.outputs["Vector"], wobble.inputs["Vector"])
    wobble.inputs["Scale"].default_value = 0.08
    centred = node("ShaderNodeVectorMath", operation="SUBTRACT")
    L(wobble.outputs["Color"], centred.inputs[0])
    centred.inputs[1].default_value = (0.5, 0.5, 0.5)
    warp = node("ShaderNodeVectorMath", operation="SCALE")
    L(centred.outputs["Vector"], warp.inputs[0])
    L(knob["groove_warp"], warp.inputs["Scale"])
    warped = node("ShaderNodeVectorMath", operation="ADD")
    L(stone_v.outputs["Vector"], warped.inputs[0])
    L(warp.outputs["Vector"], warped.inputs[1])
    groove = node("ShaderNodeTexImage", image=grooves, interpolation="Linear", projection="BOX", projection_blend=0.15)
    L(warped.outputs["Vector"], groove.inputs["Vector"])
    gate = map_range(spread, knob["groove_gate"], math("ADD", knob["groove_gate"], 0.15), 0.0, 1.0)
    along = math("ADD", math("MULTIPLY", groove.outputs["Color"], gate), ragged)
    seep = math("MULTIPLY", map_range(along, 0.45, 0.6, 0.0, 1.0), knob["moss_groove_weight"])
    mask = math("MAXIMUM", mask, seep)
    base = mix(mask, stone_col, moss_tex.outputs["Color"])

    # relief: strokes stand proud (and keep lifting the moss laid over them), hatch grooves sink,
    # moss is a lumpy cushion on top; one height field drives normal, cavity and roughness
    groove_flat = node("ShaderNodeTexImage", image=grooves, interpolation="Linear", projection="BOX", projection_blend=0.15)
    L(stone_v.outputs["Vector"], groove_flat.inputs["Vector"])
    moss_luma = node("ShaderNodeRGBToBW")
    L(moss_tex.outputs["Color"], moss_luma.inputs[0])
    strokes = scr.outputs["Color"]
    h_stroke = math("MULTIPLY", strokes, knob["stroke_height"])
    h_groove = math("MULTIPLY", math("MULTIPLY", groove_flat.outputs["Color"], math("SUBTRACT", 1.0, mask)),
                    knob["groove_depth"])
    h_moss = math("MULTIPLY", math("MULTIPLY", mask, math("ADD", 0.45, moss_luma.outputs[0])), knob["moss_height"])
    height = math("ADD", math("SUBTRACT", h_stroke, h_groove), h_moss)
    bump = node("ShaderNodeBump")
    L(knob["bump_strength"], bump.inputs["Strength"])
    L(knob["bump_distance_m"], bump.inputs["Distance"])
    L(height, bump.inputs["Height"])
    L(bump.outputs["Normal"], bsdf.inputs["Normal"])
    # AO: the shape's own occlusion times a cavity term from the relief (grooves, moss rims)
    ao = node("ShaderNodeAmbientOcclusion")
    L(knob["ao_distance_m"], ao.inputs["Distance"])
    L(bump.outputs["Normal"], ao.inputs["Normal"])
    shape_ao = map_range(ao.outputs["AO"], 0.0, 1.0, math("SUBTRACT", 1.0, knob["ao_strength"]), 1.0)
    rim = math("MULTIPLY", math("MULTIPLY", mask, math("SUBTRACT", 1.0, mask)), 4.0)  # 1 at moss edges
    cavity = math("SUBTRACT", 1.0, math("MULTIPLY", math("ADD", h_groove, math("MULTIPLY", rim, 0.35)),
                                        knob["cavity_strength"]))
    shade_ao = node("ShaderNodeVectorMath", operation="SCALE")
    L(base, shade_ao.inputs[0])
    L(math("MULTIPLY", shape_ao, cavity), shade_ao.inputs["Scale"])
    L(shade_ao.outputs["Vector"], bsdf.inputs["Base Color"])
    # roughness: stone, glossier strokes, matte moss
    rough_stone = map_range(strokes, 0.0, 1.0, knob["stone_roughness"], knob["stroke_roughness"])
    rough = map_range(mask, 0.0, 1.0, rough_stone, knob["moss_roughness"])
    L(rough, bsdf.inputs["Roughness"])
    for key in ("Specular IOR Level", "Specular"):
        if key in bsdf.inputs:
            L(knob["specular"], bsdf.inputs[key])
    return m


def uv_project(me):
    """UVs: each face projected on its dominant axis, TILE_M metres per tile, v up the stone.
    Also bakes "_edge" per vertex: 1 around the narrow faces, which are the step faces between
    raised blocks, the ledge strips at band edges and the chamfered corners, so moss can sit
    on corners and ledges and run in lines between the blocks."""
    bm = bmesh.new()
    bm.from_mesh(me)
    edge = [0.0] * len(bm.verts)
    for f in bm.faces:
        shortest = min(e.calc_length() for e in f.edges)
        turn = max((f.normal.angle(o.normal, 0.0) for e in f.edges for o in e.link_faces if o is not f), default=0.0)
        if shortest < 6.0 and turn > 0.45:  # a real step, not a seam
            for vert in f.verts:
                edge[vert.index] = 1.0
    bm.free()
    attr = me.attributes.new("_edge", "FLOAT", "POINT")  # leading _: glTF exports it as _EDGE
    attr.data.foreach_set("value", edge)
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
        uv_project(me)
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
        stone = field_stone = stone_material(*hatch_texture(), moss_texture(), scratch_texture())
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
    params = [dict(variation(rnd), seed=k + 1) for k in range(12)]
    kinds = [slab(f"giant_var_{k:02d}", params[k], field_stone) for k in range(12)]
    VARIANT_PARAMS[:] = [dict(VARIANTS["B_giant"], seed=0)] + params
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
    ("08_relief_detail", (-62, -95, 22), (-18, -38, 30), 35, set()),
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


def export():
    """The game copy: the giant and its 12 variations as one glTF (one mesh node each, Z up as
    authored, metres, flat normals, UVs, the per-vertex "_edge" attribute as _EDGE), the four
    tiles next to it, and the two manifests in content/config/terrain_core/megaliths/."""
    import json
    sc = bpy.context.scene
    coll = bpy.data.collections.new("export")
    sc.collection.children.link(coll)
    names = ["B_giant"] + [f"giant_var_{k:02d}" for k in range(12)]
    objs = [link(f"a1_monolith_{k:02d}", bpy.data.meshes[n], (0, 0, 0), coll=coll) for k, n in enumerate(names)]
    for o in sc.objects:
        o.select_set(o in objs)
    bpy.context.view_layer.objects.active = objs[0]
    out = ASSET_DIR / "a1_monolith.gltf"
    bpy.ops.export_scene.gltf(filepath=str(out), export_format="GLTF_SEPARATE", use_selection=True,
                              export_yup=True, export_apply=True, export_normals=True,
                              export_attributes=True, export_materials="NONE")
    variants = []
    for o, n, prm in zip(objs, names, VARIANT_PARAMS):
        me = o.data
        lo = [min(v.co[i] for v in me.vertices) for i in range(3)]
        hi = [max(v.co[i] for v in me.vertices) for i in range(3)]
        variants.append({
            "node": o.name, "source_mesh": n, "seed": prm["seed"],
            "tris": sum(len(p.vertices) - 2 for p in me.polygons),
            "bounds_m": [[round(x, 2) for x in lo], [round(x, 2) for x in hi]],
            "shape": {k: ([round(x, 3) for x in v] if isinstance(v, tuple) else round(v, 3) if isinstance(v, float) else v)
                      for k, v in prm.items() if k not in ("pos", "seed")}})
    for o in objs:
        bpy.data.objects.remove(o)
    bpy.data.collections.remove(coll)
    rel = lambda p: str(p.relative_to(ROOT)).replace("\\", "/")
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    asset = {
        "comment": "Magic megalith A-1, the giant monolith slabs (doc/megaliths/A1_MONOLITH.md). Generated by "
                   "tools/blender/megalith_a_monolith.py -- --export; edit the script, not this file.",
        "id": "a1_monolith", "kind": "A", "approved": "2026-10-12",
        "model": rel(out), "blend": rel(ASSET_DIR / "monolith_material.blend"),
        "units": "metres, Z up in Blender (glTF is Y up)", "origin": "centre of the foot at ground level",
        "material": "a1_monolith.material.json",
        "variants": variants,
        "placement": {
            "scale": [0.8, 1.15], "rotation_deg": [0, 360], "lean_deg": 7, "mirror_x": True,
            "sink_m": 16.0, "foot": "skirt",
            "field": {"row_gap_m": 680, "row_jitter_m": 120, "spacing_m": [480, 950], "rows_to_km": 7},
            "note": "all copies about the giant's size; the shape is fixed, rules pick place, turn and scale"},
        "budget": {"tris_per_variant": [min(v["tris"] for v in variants), max(v["tris"] for v in variants)],
                   "style_profile_A": [1500, 4000]},
    }
    (CONFIG_DIR / "a1_monolith.asset.json").write_text(json.dumps(asset, indent=2, ensure_ascii=False) + "\n")
    material = {
        "comment": "Material of A-1 (doc/megaliths/A1_MONOLITH.md). Generated by tools/blender/megalith_a_monolith.py; "
                   "the Blender node tree in the .blend is the reference implementation.",
        "id": "a1_monolith_hatch_moss",
        "projection": "box (triplanar) in object space, blend 0.15; hard pixels (nearest)",
        "textures": {
            "hatch": {"file": rel(ASSET_DIR / "monolith_hatch_128.png"), "tile_m": TILE_M,
                      "what": "red hatched stone: strokes and a wobbly broken groove grid"},
            "grooves": {"file": rel(ASSET_DIR / "monolith_hatch_grooves_128.png"), "tile_m": TILE_M,
                        "filter": "linear", "what": "soft mask of the hatch grooves (same layout as hatch)"},
            "moss": {"file": rel(ASSET_DIR / "monolith_moss_128.png"), "tile_m": MOSS_KNOBS["moss_tile_m"],
                     "what": "moss clumps"},
            "strokes": {"file": rel(ASSET_DIR / "monolith_strokes_256.png"), "tile_m": MOSS_KNOBS["scratch_tile_m"],
                        "what": "pale hairline strokes, mostly up-down (mask)"}},
        "vertex_attributes": {"_EDGE": "1 around step faces (ledges, block edges, corners), fades across faces"},
        "per_object": "Object Info Random: noise offset (x731, x377) and coverage nudge",
        "knobs": MOSS_KNOBS,
        "layers": [
            "stone = hatch, darkened by stretched-noise streaks, slow light/dark drift (noise 0.012/m), "
            "dulled toward brown (stone_saturation, stone_value), pale strokes mixed in (scratch_strength)",
            "moss mask = stepped(snap moss_pixel_m) banded noise (z stretched moss_streak_stretch) "
            "+ foot (below moss_foot_m) + up-facing ledges + _EDGE*moss_edge_weight + fine fray (moss_ragged) "
            "+ per-object nudge, thresholded at 1.15-moss_coverage +- moss_softness; "
            "max with grooves seeping (warped groove mask gated by the noise)",
            "base colour = mix(stone, moss, mask) x shape AO x cavity",
            "height = strokes*stroke_height - grooves*(1-mask)*groove_depth + mask*(0.45+moss luma)*moss_height "
            "-> bump normal (bump_strength, bump_distance_m)",
            "roughness = stone_roughness / stroke_roughness on strokes / moss_roughness under moss; specular knob"],
    }
    (CONFIG_DIR / "a1_monolith.material.json").write_text(json.dumps(material, indent=2, ensure_ascii=False) + "\n")
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
    build()
    setup_render()
    render_views(not args.no_render)
    if args.export and STAGE == "material":
        export()
    blend = ASSET_DIR / f"monolith_{STAGE}.blend"
    blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    print("saved", blend)


main()
