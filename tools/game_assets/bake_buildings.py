"""Bake the village's buildings and works for the game: blueprints assembled from the
fantasy_european kit (kit_fe.py: MegaKit walls, doors, windows, floors, procedural logs,
thatch roofs) and a few procedural pieces (hearths, fire, shrine stones, beds, field rows).

blender -b --factory-startup -P bake_buildings.py -- --repo D:/AlteraGame/altera

Writes assets/altera_game/buildings/buildings.astm and a preview of every piece.
ASTM v1 (little endian), read by src/game_view/statics.cpp:
  u32 meshes, vertices, indices, layers, layerSize
  meshes x {char name[32], u32 firstIndex, u32 indexCount, f32 min[3], f32 max[3]}
  vertices x {f32 pos[3], f32 nrm[3], f32 uv[2], u32 layer}   (36 bytes; uv repeats)
  indices x u32;  layers x layerSize^2 RGBA8, rows top-down
Each mesh: origin at the centre of its footprint, z = 0 the ground, front (door) towards -y."""
import bpy, sys, os, math, struct, random
import numpy as np
from mathutils import Vector, Matrix

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit_fe as K

a = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
REPO = a[a.index('--repo') + 1] if '--repo' in a else 'D:/AlteraGame/altera'
OUT = REPO + '/assets/altera_game/buildings'
os.makedirs(OUT, exist_ok=True)
LS = 128


def rect_walls(w, h, front, back, left, right):
    """walls of a w x h cell rectangle; front is the y=0 side, lists run along +x / +y."""
    walls = {}
    for x in range(w):
        walls['%d,-1|%d,0' % (x, x)] = front[x]
        walls['%d,%d|%d,%d' % (x, h - 1, x, h)] = back[x]
    for y in range(h):
        walls['-1,%d|0,%d' % (y, y)] = left[y]
        walls['%d,%d|%d,%d' % (w - 1, y, w, y)] = right[y]
    return walls


BLUEPRINTS = {
    'house_daub': dict(cells=[(x, y) for x in range(4) for y in range(3)],
                       walls=rect_walls(4, 3, ['wall_window', 'wall_door', 'wall', 'wall_window'],
                                        ['wall', 'wall_window', 'wall', 'wall'], ['wall', 'wall_window', 'wall'],
                                        ['wall', 'wall_window', 'wall']),
                       materials=dict(wall='wattle_daub', floor='plank', foundation='rubble_stone'),
                       foundation=dict(type='foundation'),
                       roof=dict(material='thatch', pitch_deg=48, parts=[dict(rect=[0, 0, 4, 3], type='gable', along='x')]),
                       furniture=[dict(chimney=True, rect=[12, 4, 2, 2])]),
    'house_stone': dict(cells=[(x, y) for x in range(3) for y in range(3)],
                        walls=rect_walls(3, 3, ['wall_window', 'wall_door', 'wall'], ['wall', 'wall', 'wall_window'],
                                         ['wall', 'wall_window_small', 'wall'], ['wall', 'wall_window', 'wall']),
                        materials=dict(wall='rubble_stone', floor='plank', foundation='rubble_stone'),
                        foundation=dict(type='foundation'),
                        roof=dict(material='thatch', pitch_deg=45, parts=[dict(rect=[0, 0, 3, 3], type='hip', along='x')]),
                        furniture=[dict(chimney=True, rect=[2, 8, 2, 2])]),
    'house_log': dict(cells=[(x, y) for x in range(3) for y in range(2)],
                      walls=rect_walls(3, 2, ['wall', 'wall_door', 'wall_window_small'], ['wall', 'wall', 'wall'],
                                       ['wall', 'wall_window_small'], ['wall', 'wall']),
                      materials=dict(wall='log', floor='plank', foundation='rubble_stone'),
                      foundation=dict(type='foundation'),
                      roof=dict(material='thatch', pitch_deg=42, parts=[dict(rect=[0, 0, 3, 2], type='gable', along='x')]),
                      furniture=[]),
    'storage': dict(cells=[(x, y) for x in range(2) for y in range(2)],
                    walls=rect_walls(2, 2, ['wall_door', 'wall'], ['wall', 'wall'], ['wall', 'wall'], ['wall', 'wall']),
                    materials=dict(wall='log', floor='plank', foundation='rubble_stone'),
                    foundation=dict(type='foundation_stilt', drop_m=0.2),
                    roof=dict(material='thatch', pitch_deg=40, parts=[dict(rect=[0, 0, 2, 2], type='gable', along='y')]),
                    furniture=[]),
}


def stone(rng, loc, size, tilt=0.12, mat='rubble_stone'):
    o = K.box('stone', (-size[0] / 2, -size[1] / 2, 0), (size[0] / 2, size[1] / 2, size[2]), K.M(mat), 1.0)
    bv = o.modifiers.new('b', 'BEVEL'); bv.width = min(size) * 0.18; bv.segments = 1
    o.rotation_euler = (rng.uniform(-tilt, tilt), rng.uniform(-tilt, tilt), rng.uniform(0, math.pi))
    o.location = loc
    return o


def tilted_log(p0, p1, r):
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    o = K.log_cyl('log', 0, d.length, 0, 0, r)
    q = d.to_track_quat('X', 'Z')
    o.matrix_world = Matrix.Translation(p0) @ q.to_matrix().to_4x4()
    return o


def ring_fire(n, radius, logs, seed):
    rng = random.Random(seed)
    objs = []
    for k in range(n):
        a = 2 * math.pi * k / n
        objs.append(stone(rng, (math.cos(a) * radius, math.sin(a) * radius, -0.05), (0.34, 0.26, 0.24)))
    for k in range(logs):
        a = 2 * math.pi * k / logs + 0.3
        objs.append(tilted_log((math.cos(a) * radius * 0.75, math.sin(a) * radius * 0.75, 0.02), (0, 0, radius * 0.9), 0.07))
    objs.append(K.box('ash', (-radius * 0.7, -radius * 0.7, -0.02), (radius * 0.7, radius * 0.7, 0.03), K.M('soil'), 1.0))
    return objs


def shrine():
    rng = random.Random(11)
    objs = []
    for k in range(5):
        a = 2 * math.pi * k / 5 + 0.2
        h = rng.uniform(1.9, 2.6)
        objs.append(stone(rng, (math.cos(a) * 2.3, math.sin(a) * 2.3, -0.3), (0.7, 0.45, h + 0.3), 0.08))
    objs.append(stone(rng, (0, 0, -0.1), (1.3, 0.8, 0.7), 0.03))
    return objs


def beds(w, h, rows, ridge_w, ridge_h, fence):
    objs = []
    gap = h / rows
    for k in range(rows):
        y = -h / 2 + gap * (k + 0.5)
        objs.append(K.box('bed', (-w / 2, y - ridge_w / 2, -0.15), (w / 2, y + ridge_w / 2, ridge_h), K.M('soil'), 2.0))
    if fence:
        posts = []
        for side in range(4):
            L = w if side % 2 == 0 else h
            n = max(2, int(L / 2.0))
            for k in range(n):
                t = -L / 2 + L * k / n
                if side == 0: posts.append((t, -h / 2 - 0.6))
                elif side == 1: posts.append((w / 2 + 0.6, t))
                elif side == 2: posts.append((-t, h / 2 + 0.6))
                else: posts.append((-w / 2 - 0.6, -t))
        for i, (x, y) in enumerate(posts):
            objs.append(K.box('post', (x - 0.07, y - 0.07, -0.2), (x + 0.07, y + 0.07, 1.05), K.M('plank'), 1.0))
            nx, ny = posts[(i + 1) % len(posts)]
            if i == 1: continue   # a gap for the gate
            for z in (0.45, 0.85):
                objs.append(tilted_log((x, y, z), (nx, ny, z), 0.035))
    return objs


PIECES = {
    'hearth': lambda: ring_fire(8, 0.6, 3, 3),
    'village_fire': lambda: ring_fire(12, 1.1, 6, 5),
    'shrine': shrine,
    'garden': lambda: beds(11.0, 9.0, 6, 0.9, 0.22, True),
    'field': lambda: beds(39.0, 29.0, 16, 1.2, 0.16, False),
}


# ------------------------------------------------------------------ export
layers, layer_of = [], {}


def lin2srgb(x):
    return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def layer_for(mat):
    img, tint = None, (1, 1, 1)
    if mat and mat.use_nodes:
        bsdf = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        src = bsdf.inputs['Base Color'].links[0].from_node if bsdf and bsdf.inputs['Base Color'].links else None
        if src is not None and src.type == 'MIX':
            b = src.inputs['B']
            if not b.links: tint = tuple(lin2srgb(c) for c in b.default_value[:3])
            src = next((l.from_node for i in src.inputs if i.links for l in i.links), None)
        if src is not None and src.type == 'TEX_IMAGE': img = src.image
        if img is None:
            img = next((n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image
                        and 'basecolor' in n.image.name.lower()), None)
        flat = tuple(lin2srgb(c) for c in bsdf.inputs['Base Color'].default_value[:3]) if bsdf else (0.6, 0.6, 0.6)
    else:
        flat = (0.6, 0.6, 0.6)
    key = (img.name if img else 'flat%.3f,%.3f,%.3f' % flat, tint)
    if key in layer_of: return layer_of[key]
    if img is not None and img.size[0] > 0:
        w, h = img.size
        px = np.array(img.pixels[:], np.float32).reshape(h, w, 4)[::-1]   # top-down
        ys = (np.arange(LS) * h // LS); xs = (np.arange(LS) * w // LS)
        px = px[ys][:, xs].copy()
        px[..., :3] *= np.array(tint, np.float32)
        px[..., 3] = 1
    else:
        px = np.ones((LS, LS, 4), np.float32)
        px[..., :3] = flat
    layers.append((np.clip(px, 0, 1) * 255 + 0.5).astype(np.uint8))
    layer_of[key] = len(layers) - 1
    return layer_of[key]


meshes, verts, idx = [], bytearray(), []
nv = 0


def export(name, objs):
    global nv
    dg = bpy.context.evaluated_depsgraph_get()
    dg.update()
    pts = []
    first = len(idx)
    for o in objs:
        if o is None or o.type != 'MESH': continue
        eo = o.evaluated_get(dg)
        me = eo.to_mesh()
        me.calc_loop_triangles()
        M = o.matrix_world
        N = M.to_3x3().inverted().transposed()
        uvl = me.uv_layers.active
        cn = me.corner_normals
        lay = [layer_for(sl.material) for sl in o.material_slots] or [layer_for(None)]
        for t in me.loop_triangles:
            L = lay[min(t.material_index, len(lay) - 1)]
            for li, vi in zip(t.loops, t.vertices):
                p = M @ me.vertices[vi].co
                n = (N @ Vector(cn[li].vector)).normalized()
                u, v = (uvl.data[li].uv if uvl else (0, 0))
                verts.extend(struct.pack('<8fI', p.x, p.y, p.z, n.x, n.y, n.z, u, 1.0 - v, L))
                idx.append(nv)
                nv += 1
                pts.append((p.x, p.y, p.z))
        eo.to_mesh_clear()
    pts = np.array(pts)
    mn, mx = pts.min(0), pts.max(0)
    # recentre on the footprint
    c = np.array([(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, 0.0], np.float32)
    start = (nv - (len(idx) - first)) * 36
    arr = np.frombuffer(verts, np.uint8)[start:].copy().view(np.float32).reshape(-1, 9)
    arr[:, 0:3] -= c
    verts[start:] = arr.tobytes()
    meshes.append((name, first, len(idx) - first, mn - c, mx - c))
    print('PIECE', name, 'tris', (len(idx) - first) // 3, 'size', np.round(mx - mn, 2))


def clear_scene():
    for o in list(bpy.context.scene.objects): bpy.data.objects.remove(o)


bpy.ops.wm.read_factory_settings(use_empty=True)
for name, bp in BLUEPRINTS.items():
    clear_scene()
    export(name, K.assemble(bp))
for name, fn in PIECES.items():
    clear_scene()
    export(name, fn())

with open(OUT + '/buildings.astm', 'wb') as f:
    f.write(b'ASTM' + struct.pack('<6I', 1, len(meshes), nv, len(idx), len(layers), LS))
    for name, fi, ic, mn, mx in meshes:
        f.write(name.encode()[:31].ljust(32, b'\0') + struct.pack('<2I6f', fi, ic, *mn, *mx))
    f.write(bytes(verts))
    f.write(np.array(idx, np.uint32).tobytes())
    for L in layers: f.write(L.tobytes())
print('WROTE buildings.astm', len(meshes), 'pieces', nv, 'verts', len(layers), 'layers')
