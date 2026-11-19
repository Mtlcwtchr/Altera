"""Bake the sim's beasts: Quaternius Ultimate Animated Animals (glTF) and Farm Animals
Animated (the .blend files: they carry Walk/Run/Death, the FBX only Idle), plus procedural hens
and fish (proc_beasts.py), each species its own set (mesh, skeleton, its own clips), in the formats
of bake_people.py (ASKN v1 / AANM v1, see there), flat-coloured materials as cells of a
small atlas.

blender -b --factory-startup -P bake_animals.py -- --src D:/AlteraGame/_asset_src/animals --repo D:/AlteraGame/altera

Writes assets/altera_game/beasts/<species>.askin, .aanim, _anim.json (Quaternius, CC0)."""
import bpy, sys, os, json, struct, math
import numpy as np
from mathutils import Vector, Matrix

a = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
def opt(k, d=None):
    return a[a.index(k) + 1] if k in a else d
SRC = opt('--src', 'D:/AlteraGame/_asset_src/animals')
REPO = opt('--repo', 'D:/AlteraGame/altera')
OUT = REPO + '/assets/altera_game/beasts'
os.makedirs(OUT, exist_ok=True)
UAA = SRC + '/UAA/Ultimate Animated Animals - July 2021/glTF/'
FAA = SRC + '/FAA/Blends/'
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc_beasts

STD = {'idle': ['Idle'], 'idle2': ['Idle_2', 'Idle_Headlow', 'Idle_2_HeadLow', 'WalkSlow'], 'walk': ['Walk'],
       'run': ['Gallop', 'Run'], 'eat': ['Eating'], 'death': ['Death'], 'attack': ['Attack', 'Attack_Headbutt', 'Attack_Kick'],
       'pain': ['Idle_HitReact1']}
# species: file, body length (m), tint multiplier
SPECIES = {
    'deer': (UAA + 'Deer.gltf', 1.7, (1, 1, 1)),
    'wolf': (UAA + 'Wolf.gltf', 1.35, (1, 1, 1)),
    'hare': (UAA + 'Fox.gltf', 0.55, (0.72, 0.62, 0.55)),
    'boar': (FAA + 'Pig.blend', 1.4, (0.42, 0.33, 0.28)),
    'pig': (FAA + 'Pig.blend', 1.25, (1, 1, 1)),
    'goats': (FAA + 'Sheep.blend', 1.1, (0.95, 0.9, 0.82)),
    'hens': ('proc:hen', 0.42, (1, 1, 1)),
    'fish_shoal': ('proc:fish', 0.34, (1, 1, 1)),
}
FPS = 15.0
HOLD = {'Death'}

def mat_np(m):
    return np.array([[m[r][c] for c in range(4)] for r in range(4)], np.float64)

def gltf_colours(path):
    if not path.endswith('.gltf'): return {}
    j = json.load(open(path, encoding='utf-8'))
    out = {}
    for m in j.get('materials', []):
        c = m.get('pbrMetallicRoughness', {}).get('baseColorFactor')
        if c: out[m['name']] = c[:3]
    return out

def lin2srgb(x):
    return 12.92 * x if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055

def bake(name, path, length, tint):
    if path.startswith('proc:'): proc_beasts.build(path[5:])
    elif path.endswith('.blend'): bpy.ops.wm.open_mainfile(filepath=path)
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if path.endswith('.fbx'): bpy.ops.import_scene.fbx(filepath=path)
        else: bpy.ops.import_scene.gltf(filepath=path)
    for o in list(bpy.data.objects):
        if o.type == 'MESH' and o.name.startswith('Icosphere'): bpy.data.objects.remove(o)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    A = arm.matrix_world.inverted()
    names = [b.name for b in arm.data.bones]
    parent = [names.index(b.parent.name) if b.parent else -1 for b in arm.data.bones]
    # size from the rest mesh
    ys = [(A @ o.matrix_world @ v.co).y for o in meshes for v in o.data.vertices]
    zs = [(A @ o.matrix_world @ v.co).z for o in meshes for v in o.data.vertices]
    S = length / (max(ys) - min(ys))
    zmin = min(zs)
    cols = gltf_colours(path)
    # atlas: one 16 px cell a material
    cells, atlas = {}, np.zeros((128, 128, 4), np.uint8)
    def cell(m):
        key = m.name if m else '-'
        if key not in cells:
            k = len(cells)
            c = cols.get(key)
            if c is None:
                b = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None) if m and m.node_tree else None
                c = list(b.inputs['Base Color'].default_value[:3]) if b else [0.6, 0.6, 0.6]
            rgb = [min(1.0, lin2srgb(c[i]) * tint[i]) for i in range(3)]
            x, y = (k % 8) * 16, (k // 8) * 16
            atlas[y:y + 16, x:x + 16, :3] = (np.array(rgb) * 255 + 0.5).astype(np.uint8)
            atlas[y:y + 16, x:x + 16, 3] = 255
            cells[key] = ((x + 8) / 128.0, (y + 8) / 128.0)
        return cells[key]
    verts, idx, nv = bytearray(), [], 0
    for o in meshes:
        me = o.data
        me.calc_loop_triangles()
        M = A @ o.matrix_world
        N = M.to_3x3().inverted().transposed()
        gi = {g.index: g.name for g in o.vertex_groups}
        jidx = {g: names.index(n) for g, n in gi.items() if n in names}
        # a mesh parented to a bone without groups follows that bone
        bone_parent = names.index(o.parent_bone) if o.parent_type == 'BONE' and o.parent_bone in names else 0
        vw = []
        for v in me.vertices:
            ws = sorted(((g.weight, jidx[g.group]) for g in v.groups if g.group in jidx and g.weight > 1e-4), reverse=True)[:4]
            if not ws: ws = [(1.0, bone_parent)]
            tot = sum(w for w, _ in ws)
            q = [int(round(w / tot * 255)) for w, _ in ws]
            q[0] += 255 - sum(q)
            js = [j for _, j in ws]
            while len(q) < 4: q.append(0); js.append(js[0])
            vw.append((js, q))
        cn = me.corner_normals
        mats = [cell(sl.material) for sl in o.material_slots] or [cell(None)]
        cache = {}
        for t in me.loop_triangles:
            uv = mats[min(t.material_index, len(mats) - 1)]
            for li, vi in zip(t.loops, t.vertices):
                p = M @ me.vertices[vi].co
                n = (N @ Vector(cn[li].vector)).normalized()
                key = (vi, round(n.x, 3), round(n.y, 3), round(n.z, 3), uv)
                k = cache.get(key)
                if k is None:
                    js, q = vw[vi]
                    verts.extend(struct.pack('<8f4B4BI', p.x * S, p.y * S, (p.z - zmin) * S, n.x, n.y, n.z, uv[0], uv[1], *js, *q, 0))
                    k = cache[key] = nv
                    nv += 1
                idx.append(k)
    def scaled(m):
        m = m.copy(); m[:3, 3] *= S; return m
    bones = []
    for b in arm.data.bones:
        R = mat_np(b.matrix_local)
        R[2, 3] -= zmin
        L = R if not b.parent else np.linalg.inv(mat_np(b.parent.matrix_local) - np.array([[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, zmin], [0, 0, 0, 0]])) @ R
        bones.append((scaled(L), np.linalg.inv(scaled(R))))
    with open(OUT + '/' + name + '.askin', 'wb') as f:
        f.write(b'ASKN' + struct.pack('<8I', 1, len(names), 1, 1, nv, len(idx), 128, 128))
        for nm, p in zip(names, parent): f.write(struct.pack('<i', p) + nm.encode()[:31].ljust(32, b'\0'))
        for L, I in bones:
            f.write(struct.pack('<16f', *L.astype(np.float32).ravel()) + struct.pack('<16f', *I.astype(np.float32).ravel()))
        f.write(name.encode().ljust(32, b'\0') + struct.pack('<3If', 0, 0, len(idx), (max(zs) - zmin) * S))
        f.write(bytes(verts)); f.write(np.array(idx, np.uint32).tobytes()); f.write(atlas.tobytes())
    # clips
    acts = {}
    for ac in bpy.data.actions:
        short = ac.name.split('|')[-1]
        acts[short] = ac
    sc = bpy.context.scene
    ad = arm.animation_data or arm.animation_data_create()
    body = next((names.index(n) for n in ('Body', 'Hips', 'Pelvis', 'Spine', 'Root') if n in names), 0)
    table, blob, total, state = [], bytearray(), 0, {}
    for st, cands in STD.items():
        c = next((x for x in cands if x in acts), None)
        if c: state[st] = c
    for cname in sorted(set(state.values())):
        ac = acts[cname]
        ad.action = ac
        if hasattr(ad, 'action_slot') and ac.slots: ad.action_slot = ac.slots[0]
        f0, f1 = ac.frame_range
        fps = sc.render.fps / sc.render.fps_base if path.endswith('.blend') else 24.0
        dur = (f1 - f0) / fps
        loop = cname not in HOLD and 'Attack' not in cname and 'HitReact' not in cname
        n = max(2, int(round(dur * FPS)) + (0 if loop else 1))
        rows, pel = [], []
        for k in range(n):
            fr = f0 + (k / FPS) * fps
            sc.frame_set(int(math.floor(fr)), subframe=fr - math.floor(fr))
            row = bytearray()
            for nm in names:
                pb = arm.pose.bones[nm]
                # The visual pose (constraints solved: the farm pack's legs are IK):
                # basis = restRel^-1 @ parentPose^-1 @ pose, what the runtime composes back.
                bn = pb.bone
                rest = bn.matrix_local if not bn.parent else bn.parent.matrix_local.inverted() @ bn.matrix_local
                par = pb.parent.matrix if pb.parent else Matrix.Identity(4)
                loc, q, _ = (rest.inverted() @ par.inverted() @ pb.matrix).decompose()
                row.extend(struct.pack('<7f', q.x, q.y, q.z, q.w, loc.x * S, loc.y * S, loc.z * S))
            t = arm.pose.bones[names[body]].matrix.translation
            pel.append((t.x * S, t.y * S))
            rows.append(row)
        pel = np.array(pel)
        for k, row in enumerate(rows):
            anc = pel[0] + ((pel[-1] - pel[0]) * (k / n) if loop else 0)
            blob.extend(row); blob.extend(struct.pack('<2f', float(anc[0]), float(anc[1])))
        table.append((cname, n, 2 if cname in HOLD else (1 if loop else 0), total))
        total += n
    with open(OUT + '/' + name + '.aanim', 'wb') as f:
        f.write(b'AANM' + struct.pack('<3If', 1, len(names), len(table), FPS))
        for cname, n, lp, first in table:
            f.write(cname.encode()[:63].ljust(64, b'\0') + struct.pack('<3I', n, lp, first))
        f.write(bytes(blob))
    json.dump({'clips': [t[0] for t in table], 'state': state, 'interaction': {}}, open(OUT + '/' + name + '_anim.json', 'w'), indent=1)
    print('BEAST', name, 'verts', nv, 'tris', len(idx) // 3, 'scale', round(S, 4), 'clips', [t[0] for t in table])

only = opt('--only')
for nm, (p, L, t) in SPECIES.items():
    if only and nm != only: continue
    bake(nm, p, L, t)
