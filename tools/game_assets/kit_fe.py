"""Kit fantasy_european: assemble a blueprint (2 m cells, walls on cell edges, roof parts)
from Quaternius Medieval Village MegaKit modules and procedural logs/roofs.
From the blueprint prototype (gdd/blueprints/proto/kit_fe/kit_build.py), with local paths:
VMK modules and textures, Kenney retro textures for thatch, planks, logs.
Used by bake_buildings.py; `assemble(bp)` returns the Blender objects of one building."""
import sys, os, math, json
import bpy, bmesh
import numpy as np
from mathutils import Vector, Matrix

SRC = os.environ.get('LR_ASSET_SRC', 'D:/AlteraGame/_asset_src')
VMK = SRC + '/vmk/Medieval Village MegaKit[Standard]/glTF'
VT = SRC + '/vmk/Medieval Village MegaKit[Standard]/Textures'
KRT = SRC + '/krt/PNG'
TEX = 128

TINT = dict(log='#6b4a2e', plank='#8a6440', wattle_daub='#cdbb92', rubble_stone='#8d8a80',
            cut_stone='#a7a39a', turf='#4f5a2e', thatch='#a08a52', reed='#8a7b4a',
            shingle='#5a4a3a', hide='#9a7a5a', door='#3a2a1a', window='#2a3038')

IMG = {
    'plaster': VT + '/T_Plaster_BaseColor.png',
    'stone': VT + '/T_UnevenBrick_BaseColor.png',
    'cutstone': VT + '/T_Brick_BaseColor.png',
    'bark': KRT + '/wall_timber.png',
    'logend': KRT + '/timber_square_planks.png',
    'planks': KRT + '/floor_wood_planks.png',
    'shingle': KRT + '/roof_clay_grey_center.png',
    'thatch': KRT + '/roof_thatch_center.png',
    'dirt': KRT + '/floor_ground_dirt.png',
}

def hexc(h, a=1.0):
    h = h.lstrip('#'); return tuple(int(h[i:i+2], 16) / 255 for i in (0, 2, 4)) + (a,)

def srgb2lin(c):
    return tuple(((x + 0.055) / 1.055) ** 2.4 if x > 0.04045 else x / 12.92 for x in c[:3]) + (1,)

# ---------------------------------------------------------------- materials
_imgcache = {}
def load_img(path, grade=True):
    if path in _imgcache: return _imgcache[path]
    im = bpy.data.images.load(path, check_existing=False)
    if im.size[0] > TEX:
        im.scale(TEX, max(1, int(im.size[1] * TEX / im.size[0])))
    if grade: grade_img(im)
    im.pack()
    _imgcache[path] = im
    return im

def grade_img(im):
    if im.size[0] == 0: return
    px = np.array(im.pixels[:], dtype=np.float32).reshape(-1, 4)
    lum = (px[:, :3] @ np.array([0.3, 0.59, 0.11]))[:, None]
    px[:, :3] = (px[:, :3] * 0.6 + lum * 0.4) * np.array([0.82, 0.80, 0.72])
    im.pixels[:] = px.ravel()

_matcache = {}
def tex_mat(name, img_key=None, tint=None):
    key = (name, img_key, tint)
    if key in _matcache: return _matcache[key]
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; p = nt.nodes['Principled BSDF']
    p.inputs['Roughness'].default_value = 0.95
    for k in ('Specular IOR Level', 'Specular'):
        if k in p.inputs: p.inputs[k].default_value = 0.1
    if img_key:
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = load_img(IMG[img_key]); t.interpolation = 'Closest'
        if tint:
            mix = nt.nodes.new('ShaderNodeMix'); mix.data_type = 'RGBA'; mix.blend_type = 'MULTIPLY'
            mix.inputs['Factor'].default_value = 1.0
            nt.links.new(t.outputs['Color'], mix.inputs['A'])
            mix.inputs['B'].default_value = srgb2lin(hexc(tint))
            nt.links.new(mix.outputs['Result'], p.inputs['Base Color'])
        else:
            nt.links.new(t.outputs['Color'], p.inputs['Base Color'])
    else:
        p.inputs['Base Color'].default_value = srgb2lin(hexc(tint or '#888888'))
    _matcache[key] = m
    return m

GREY = False
def M(kind):
    """material for a material id"""
    if GREY:
        return tex_mat('g_' + kind, None, TINT.get(kind, '#888888'))
    return {
        'wattle_daub': lambda: tex_mat('daub', 'plaster'),
        'rubble_stone': lambda: tex_mat('rubble', 'stone'),
        'cut_stone': lambda: tex_mat('cut', 'cutstone'),
        'log': lambda: tex_mat('bark', 'bark'),
        'logend': lambda: tex_mat('logend', 'logend'),
        'plank': lambda: tex_mat('planks', 'planks'),
        'thatch': lambda: tex_mat('thatch', 'thatch'),
        'reed': lambda: tex_mat('reed', 'thatch', '#b8a68a'),
        'shingle': lambda: tex_mat('shingle', 'shingle'),
        'door': lambda: tex_mat('planksd', 'planks', '#9a8a7a'),
        'dirt': lambda: tex_mat('dirt', 'dirt'),
        'soil': lambda: tex_mat('soil', 'dirt', '#7a5a40'),
    }[kind]()

def retex_vmk(objs):
    """swap Quaternius textures for the Andersson set, PSX-downscale the rest."""
    for o in objs:
        for sl in getattr(o, 'material_slots', []):
            m = sl.material
            if not m or not m.use_nodes: continue
            for n in m.node_tree.nodes:
                if n.type == 'TEX_IMAGE' and n.image:
                    nm = n.image.name.lower()
                    if 'plaster_basecolor' in nm: n.image = load_img(IMG['plaster'])
                    elif 'unevenbrick_basecolor' in nm: n.image = load_img(IMG['stone'])
                    elif 'basecolor' in nm and n.image.get('_psx') is None:
                        im = n.image
                        if im.size[0] > TEX: im.scale(TEX, max(1, int(im.size[1] * TEX / im.size[0])))
                        grade_img(im); im['_psx'] = 1
                    n.interpolation = 'Closest'
                if n.type == 'BSDF_PRINCIPLED':
                    n.inputs['Roughness'].default_value = 0.95
                    for k in ('Specular IOR Level', 'Specular'):
                        if k in n.inputs: n.inputs[k].default_value = 0.1
                    for l in list(n.inputs['Normal'].links): m.node_tree.links.remove(l)
                    # drop roughness/orm maps
                    for l in list(n.inputs['Roughness'].links): m.node_tree.links.remove(l)
                    for k in ('Metallic',):
                        for l in list(n.inputs[k].links): m.node_tree.links.remove(l)
                        n.inputs[k].default_value = 0

# ---------------------------------------------------------------- mesh helpers
def mesh_obj(name, verts, faces, mats, face_mat=None, tile=2.0, uvmode='box'):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces); me.update()
    o = bpy.data.objects.new(name, me); bpy.context.scene.collection.objects.link(o)
    for m in mats: me.materials.append(m)
    if face_mat:
        for i, p in enumerate(me.polygons): p.material_index = face_mat(i, p)
    box_uv(o, tile)
    return o

def box_uv(o, tile, slope_axis=None):
    me = o.data
    bm = bmesh.new(); bm.from_mesh(me)
    uv = bm.loops.layers.uv.verify()
    for f in bm.faces:
        n = f.normal; a = max(range(3), key=lambda k: abs(n[k]))
        for l in f.loops:
            c = l.vert.co
            if a == 0: u, v = c.y, c.z
            elif a == 1: u, v = c.x, c.z
            else: u, v = c.x, c.y
            if slope_axis is not None and a == 2:
                # roofs: v runs down the slope
                u, v = (c.x, c.y) if slope_axis == 'x' else (c.y, c.x)
            l[uv].uv = (u / tile, v / tile)
    bm.to_mesh(me); bm.free()

def box(name, mn, mx, mat, tile=2.0):
    x0, y0, z0 = mn; x1, y1, z1 = mx
    v = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),(x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    f = [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]
    return mesh_obj(name, v, f, [mat], tile=tile)

def log_cyl(name, x0, x1, y, z, r, axis='x'):
    """a horizontal log along local x (or y) with end caps in log-end material."""
    seg = 8; vs = []; fs = []
    for e, xx in enumerate((x0, x1)):
        for k in range(seg):
            a = 2 * math.pi * k / seg
            dy, dz = r * math.cos(a), r * math.sin(a)
            vs.append((xx, y + dy, z + dz) if axis == 'x' else (y + dy, xx, z + dz))
    for k in range(seg):
        k2 = (k + 1) % seg
        fs.append((k, k2, seg + k2, seg + k))
    fs.append(tuple(range(seg - 1, -1, -1))); fs.append(tuple(range(seg, 2 * seg)))
    ncap = len(fs) - 2
    return mesh_obj(name, vs, fs, [M('log'), M('logend') if not GREY else M('log')],
                    face_mat=lambda i, p: 1 if i >= ncap else 0, tile=1.0)

def join(objs, name):
    objs = [o for o in objs if o]
    if not objs: return None
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if len(objs) > 1: bpy.ops.object.join()
    o = bpy.context.view_layer.objects.active; o.name = name
    return o

_vmk_cache = {}
def vmk(name):
    """import a Quaternius module once, return a fresh linked copy (meshes shared)."""
    if name not in _vmk_cache:
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=os.path.join(VMK, name + '.gltf'))
        new = [o for o in bpy.data.objects if o not in before]
        if not GREY: retex_vmk(new)
        for o in new:
            bpy.context.scene.collection.objects.unlink(o) if o.name in bpy.context.scene.collection.objects else None
            for c in o.users_collection: c.objects.unlink(o)
        _vmk_cache[name] = [o for o in new if o.type == 'MESH']
    out = []
    for o in _vmk_cache[name]:
        c = o.copy(); c.matrix_world = Matrix.Identity(4)
        bpy.context.scene.collection.objects.link(c); out.append(c)
    return out

def vmk_bbox(name):
    mn = Vector((1e9,)*3); mx = Vector((-1e9,)*3)
    for o in _vmk_cache[name] if name in _vmk_cache else []:
        for v in o.data.vertices:
            mn = Vector(map(min, mn, v.co)); mx = Vector(map(max, mx, v.co))
    return mn, mx

def place(objs, loc, rot_deg, scale=(1, 1, 1)):
    R = Matrix.Rotation(math.radians(rot_deg), 4, 'Z')
    T = Matrix.Translation(Vector(loc))
    Sx = Matrix.Diagonal(Vector(scale + (1,)))
    for o in objs: o.matrix_world = T @ R @ Sx @ o.matrix_world
    return objs

# ---------------------------------------------------------------- pieces (local frame)
# local frame: wall along X, centred at x=0, bottom z=0, exterior towards -Y (pivot edge_center_bottom)
VMK_WALL = {
    'wattle_daub': dict(wall='Wall_Plaster_Straight', door='Wall_Plaster_Door_Flat',
                        win='Wall_Plaster_Window_Wide_Flat', winsm='Wall_Plaster_Window_Thin_Round',
                        gate='Wall_Arch', corner='Corner_Exterior_Wood'),
    'rubble_stone': dict(wall='Wall_UnevenBrick_Straight', door='Wall_UnevenBrick_Door_Flat',
                         win='Wall_UnevenBrick_Window_Wide_Flat', winsm='Wall_UnevenBrick_Window_Thin_Round',
                         gate='Wall_Arch', corner='Corner_Exterior_Brick'),
}
VMK_WALL['cut_stone'] = VMK_WALL['rubble_stone']
INSERT = {'door': ('Door_1_Flat', -0.51, 0.0), 'win': ('Window_Wide_Flat1', 0, 0), 'winsm': ('Window_Thin_Round1', 0, 0)}
KIND = {'wall': 'wall', 'wall_door': 'door', 'wall_window': 'win', 'wall_window_small': 'winsm',
        'gate_wide': 'gate', 'wall_half': 'wall', 'wall_arch': 'gate'}

def piece_wall(kind, mat):
    k = KIND.get(kind, 'wall')
    if GREY:
        objs = [box('w', (-1, -0.1, 0), (1, 0.1, 3), M(mat))]
        if k == 'door': objs.append(box('d', (-0.5, -0.12, 0), (0.5, 0.12, 2.1), M('door')))
        if k == 'gate': objs.append(box('d', (-0.9, -0.12, 0), (0.9, 0.12, 2.6), M('door')))
        if k in ('win', 'winsm'): objs.append(box('d', (-0.4, -0.12, 1.1), (0.4, 0.12, 1.9), M('window')))
        return objs
    if mat in VMK_WALL:
        objs = vmk(VMK_WALL[mat][k])
        if k in INSERT:
            n, dx, dz = INSERT[k]
            objs += place(vmk(n), (dx, 0, dz), 0)
        return objs
    # log (and plank) walls: procedural stacked logs, openings cut out
    r = 0.15; objs = []
    hole = {'door': (-0.55, 0.55, 0, 2.2), 'gate': (-0.95, 0.95, 0, 2.6),
            'win': (-0.5, 0.5, 1.05, 1.85), 'winsm': (-0.3, 0.3, 1.2, 1.75)}.get(k)
    n = 10
    for i in range(n):
        z = r + i * (3.0 - r) / (n - 1) * 0.97
        x0, x1 = -1.12, 1.12
        if (i % 2): x0, x1 = -1.0, 1.0   # alternate courses: notched corner look
        if hole and hole[2] - r * 0.5 < z < hole[3] + r * 0.5:
            objs.append(log_cyl('l', x0, hole[0], 0.0, z, r))
            objs.append(log_cyl('l', hole[1], x1, 0.0, z, r))
        else:
            objs.append(log_cyl('l', x0, x1, 0.0, z, r))
    if k == 'door':
        objs += place(vmk('Door_1_Flat'), (-0.51, -0.02, 0), 0)
        objs.append(box('jamb', (-0.62, -0.17, 0), (-0.52, 0.17, 2.3), M('plank'), 1))
        objs.append(box('jamb', (0.52, -0.17, 0), (0.62, 0.17, 2.3), M('plank'), 1))
        objs.append(box('lintel', (-0.62, -0.17, 2.2), (0.62, 0.17, 2.36), M('plank'), 1))
    if k == 'gate':
        objs.append(box('gate', (-0.95, -0.03, 0), (0.95, 0.03, 2.6), M('door'), 1))
    if k in ('win', 'winsm'):
        objs += place(vmk('Window_Thin_Flat1' if k == 'winsm' else 'Window_Wide_Flat1'), (0, 0, 0.1 if k=='win' else 0.2), 0,
                      (0.62, 1, 0.55) if k == 'win' else (1, 1, 1))
    return objs

# ---------------------------------------------------------------- roof
def roof_part(part, pitch, E, mat, wall_mat, over=0.45, gover=0.35):
    x0, y0, w, h = [v * 2.0 for v in part['rect']]
    x1, y1 = x0 + w, y0 + h
    typ = part.get('type', 'gable'); along = part.get('along', 'x')
    t = math.tan(math.radians(pitch))
    th = 0.32 if mat in ('thatch', 'reed') else 0.08
    # work in a frame where the ridge runs along X
    if along == 'y':
        def tr(p): return (p[1], p[0], p[2])
        X0, X1, Y0, Y1 = y0, y1, x0, x1
    else:
        def tr(p): return p
        X0, X1, Y0, Y1 = x0, x1, y0, y1
    half = (Y1 - Y0) / 2; Yc = (Y0 + Y1) / 2; R = E + half * t
    zl = E - over * t
    objs = []
    if typ == 'lean_to':
        top = E + (Y1 - Y0) * t
        quads = [[(X0 - gover, Y0 - over, zl), (X1 + gover, Y0 - over, zl), (X1 + gover, Y1, top), (X0 - gover, Y1, top)]]
    elif typ == 'hip':
        hx0, hx1 = X0 + half, X1 - half
        if hx1 < hx0: hx0 = hx1 = (X0 + X1) / 2
        a, b = X0 - over, X1 + over; c, d = Y0 - over, Y1 + over
        quads = [[(a, c, zl), (b, c, zl), (hx1, Yc, R), (hx0, Yc, R)],
                 [(b, d, zl), (a, d, zl), (hx0, Yc, R), (hx1, Yc, R)],
                 [(a, d, zl), (a, c, zl), (hx0, Yc, R)],
                 [(b, c, zl), (b, d, zl), (hx1, Yc, R)]]
    else:
        a, b = X0 - gover, X1 + gover
        quads = [[(a, Y0 - over, zl), (b, Y0 - over, zl), (b, Yc, R), (a, Yc, R)],
                 [(b, Y1 + over, zl), (a, Y1 + over, zl), (a, Yc, R), (b, Yc, R)]]
    for q in quads:
        verts = [tr(p) for p in q]
        o = mesh_obj('roof', verts, [tuple(range(len(verts)))], [M(mat)], tile=2.0)
        # uv: along ridge = u, down slope = v
        me = o.data; uvl = me.uv_layers.active
        for poly in me.polygons:
            for li in poly.loop_indices:
                c = Vector(tr(q[me.loops[li].vertex_index]))
                along_c = c.x if along == 'x' else c.y
                uvl.data[li].uv = (along_c / 2.0, (R - c.z) / (t + 1e-6) / 1.6)
        sm = o.modifiers.new('s', 'SOLIDIFY'); sm.thickness = th; sm.offset = 1.0
        objs.append(o)
    # ridge cap
    if typ in ('gable', 'hip') and not GREY:
        if typ == 'gable': ra, rb = X0 - gover, X1 + gover
        else: ra, rb = X0 + half, X1 - half
        if rb > ra:
            if along == 'x':
                objs.append(log_cyl('ridge', ra, rb, Yc, R + th * 0.6, 0.13 if mat != 'shingle' else 0.09))
            else:
                objs.append(log_cyl('ridge', ra, rb, Yc, R + th * 0.6, 0.13 if mat != 'shingle' else 0.09, axis='y'))
    # gable triangles
    if typ == 'gable':
        for X in (X0, X1):
            tri = [(X, Y0, E), (X, Y1, E), (X, Yc, R - 0.05)]
            verts = [tr(p) for p in tri]
            wm = 'plank' if wall_mat == 'log' else wall_mat
            o = mesh_obj('gable', verts, [(0, 1, 2)], [M(wm)], tile=2.0)
            sm = o.modifiers.new('s', 'SOLIDIFY'); sm.thickness = 0.2; sm.offset = 0
            objs.append(o)
            if wall_mat in ('wattle_daub',) and not GREY:
                # timber framing on the daub gable
                for p0, p1 in (((X, Y0, E), (X, Yc, R)), ((X, Y1, E), (X, Yc, R)), ((X, Yc, E), (X, Yc, R))):
                    objs.append(beam(tr(p0), tr(p1), 0.12))
                objs.append(beam(tr((X, Y0, E + 0.06)), tr((X, Y1, E + 0.06)), 0.14))
    return objs, R

def beam(p0, p1, s):
    p0 = Vector(p0); p1 = Vector(p1); d = p1 - p0; L = d.length
    o = box('beam', (-s / 2, -s / 2, 0), (s / 2, s / 2, L), M('plank') if not GREY else M('log'), 1.0)
    q = d.to_track_quat('Z', 'Y')
    o.matrix_world = Matrix.Translation(p0) @ q.to_matrix().to_4x4()
    # nudge outward so the beam sits proud of the plaster
    return o

# ---------------------------------------------------------------- assemble
def edge_geom(key):
    a, b = key.split('|'); x1, y1 = map(int, a.split(',')); x2, y2 = map(int, b.split(','))
    if x1 != x2:   # cells side by side in x -> wall along Y at X=2*max
        X = 2 * max(x1, x2); Yc = 2 * y1 + 1
        return (X, Yc), 'y', ((x1, y1), (x2, y2)), (1 if x2 > x1 else -1, 0)
    Y = 2 * max(y1, y2); Xc = 2 * x1 + 1
    return (Xc, Y), 'x', ((x1, y1), (x2, y2)), (0, 1 if y2 > y1 else -1)

def assemble(bp):
    cells = set(map(tuple, bp['cells']))
    mats = bp['materials']; wm = mats['wall']; rm = bp['roof']['material']
    fd = bp['foundation']; drop = fd.get('drop_m', 0) or 0
    xs = [c[0] for c in cells]; ys = [c[1] for c in cells]
    objs = []
    # foundation
    if fd['type'] == 'foundation_stilt':
        base = max(0.9, drop + 0.6)
        pts = set()
        for (x, y) in cells:
            for dx in (0, 1):
                for dy in (0, 1): pts.add((2 * (x + dx), 2 * (y + dy)))
        for (px, py) in pts:
            if GREY: objs.append(box('stilt', (px - .14, py - .14, 0), (px + .14, py + .14, base), M('log')))
            else:
                o = log_cyl('stilt', 0, base, 0, 0, 0.14); o.rotation_euler = (0, -math.pi / 2, 0); o.location = (px, py, 0)
                objs.append(o)
                objs.append(box('pad', (px - .3, py - .3, base - 0.12), (px + .3, py + .3, base), M('rubble_stone'), 1))
    else:
        base = 0.25 + drop
        for (x, y) in cells:
            e = lambda dx, dy: 0.0 if (x + dx, y + dy) in cells else 0.12
            objs.append(box('plinth', (2 * x - e(-1, 0), 2 * y - e(0, -1), -0.3), (2 * x + 2 + e(1, 0), 2 * y + 2 + e(0, 1), base), M(mats['foundation']), 1.0))
    # floors
    for (x, y) in cells:
        if GREY or mats['floor'] not in ('plank', 'log'):
            objs.append(box('floor', (2 * x, 2 * y, base - 0.05), (2 * x + 2, 2 * y + 2, base + 0.02), M(mats['floor'])))
        else:
            objs += place(vmk('Floor_WoodDark' if mats['floor'] == 'plank' else 'Floor_WoodLight'), (2 * x + 1, 2 * y + 1, base), 0)
    # walls
    corner_dirs = {}
    for key, kind in bp['walls'].items():
        (cx, cy), axis, (c1, c2), d = edge_geom(key)
        in1, in2 = c1 in cells, c2 in cells
        if in1 and not in2: n = d               # normal points from c1 to the missing c2
        elif in2 and not in1: n = (-d[0], -d[1])
        else: n = None
        rot = {(0, -1): 0, (0, 1): 180, (1, 0): 90, (-1, 0): -90}.get(n, 0 if axis == 'x' else 90)
        objs += place(piece_wall(kind, wm), (cx, cy, base), rot)
        if n:
            # endpoints for corner posts
            if axis == 'x': ends = [(cx - 1, cy), (cx + 1, cy)]
            else: ends = [(cx, cy - 1), (cx, cy + 1)]
            for e in ends: corner_dirs.setdefault(e, set()).add((axis, n))
    if not GREY and wm in VMK_WALL:
        cn = VMK_WALL[wm]['corner']; vmk(cn); mn, mx = vmk_bbox(cn)
        ctr = (mn + mx) / 2
        for (ex, ey), ds in corner_dirs.items():
            if len({a for a, _ in ds}) < 2: continue
            nx = sum(n[0] for _, n in ds); ny = sum(n[1] for _, n in ds)
            o = place(vmk(cn), (ex - ctr.x + nx * 0.04, ey - ctr.y + ny * 0.04, base), 0, (1, 1, 1.0))
            objs += o
    if not GREY and wm == 'log':
        for (ex, ey), ds in corner_dirs.items():
            if len({a for a, _ in ds}) < 2: continue
            objs.append(box('cornerpost', (ex - .16, ey - .16, base), (ex + .16, ey + .16, base + 3.0), M('plank'), 1))
    # loft floor
    for (x, y) in map(tuple, bp.get('loft') or []):
        objs.append(box('loft', (2 * x, 2 * y, base + 3.0), (2 * x + 2, 2 * y + 2, base + 3.1), M('plank')))
    # roof
    E = base + 3.0; ridge = E
    for part in bp['roof']['parts']:
        o, R = roof_part(part, bp['roof'].get('pitch_deg', 45), E, rm, wm); objs += o; ridge = max(ridge, R)
    # chimney
    for f in bp.get('furniture', []):
        if not f.get('chimney'): continue
        rx, ry, rw, rh = f['rect']; cx = (rx + rw / 2) * 0.5; cy = (ry + rh / 2) * 0.5
        objs.append(box('chimney', (cx - .45, cy - .45, base), (cx + .45, cy + .45, ridge + 0.9), M('rubble_stone'), 1.0))
        if not GREY:
            objs.append(box('chimcap', (cx - .55, cy - .55, ridge + 0.8), (cx + .55, cy + .55, ridge + 0.95), M('rubble_stone'), 1.0))
    return objs

# ---------------------------------------------------------------- render
def render(out, res, camdir=(1.0, -1.25, 0.75), camf=2.0):
    scene = bpy.context.scene
    dg = bpy.context.evaluated_depsgraph_get()
    mn = Vector((1e9,)*3); mx = Vector((-1e9,)*3)
    for o in scene.objects:
        if o.type != 'MESH': continue
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c); mn = Vector(map(min, mn, w)); mx = Vector(map(max, mx, w))
    ctr = (mn + mx) / 2; size = (mx - mn).length
    bpy.ops.mesh.primitive_plane_add(size=size * 8, location=(ctr.x, ctr.y, 0.0))
    g = bpy.context.object; g.data.materials.append(tex_mat('ground', None, '#3a3628'))
    sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
    sun.data.energy = 3.5; sun.data.color = (1.0, 0.92, 0.8); sun.data.angle = math.radians(3)
    sun.rotation_euler = (math.radians(50), 0, math.radians(35)); scene.collection.objects.link(sun)
    world = bpy.data.worlds.new('w'); scene.world = world; world.use_nodes = True
    world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.42, 0.47, 0.55, 1)
    world.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.8
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam')); scene.collection.objects.link(cam); scene.camera = cam
    cam.data.lens = 50
    d = Vector(camdir).normalized(); cam.location = ctr + d * size * camf
    cam.rotation_euler = (ctr - cam.location).to_track_quat('-Z', 'Y').to_euler()
    scene.render.engine = 'CYCLES'; scene.cycles.device = 'CPU'; scene.cycles.samples = 24
    scene.cycles.use_denoising = True
    scene.render.resolution_x = res; scene.render.resolution_y = int(res * 0.62)
    scene.render.filepath = out; scene.view_settings.view_transform = 'Standard'
    bpy.ops.render.render(write_still=True)

