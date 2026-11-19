"""Bake the game's people: Quaternius base heads + fantasy outfits + hair on the UAL rig,
and the retargeted UAL clips the sim's states and interactions play.

blender -b --factory-startup -P bake_people.py -- --src D:/AlteraGame/_asset_src/chr --repo D:/AlteraGame/altera
        [--clips-only | --mesh-only] [--max-clips N]

Writes
  assets/altera_game/people/people.askin      meshes of every look, skeletons, one texture atlas
  assets/altera_game/people/portrait_<look>.png, portrait_<look>_id.png   head shots and their class masks
  content/animations/humanoid_ual/baked/people.aanim   clips as per-joint basis (git-ignored, clip licences)
  content/animations/humanoid_ual/baked/people_anim.json   clip names, interaction/state -> clip

Formats (little endian), read by src/game_view/skinned.cpp:
  ASKN v1: u32 joints, skeletons, looks, vertices, indices, atlasW, atlasH
           joints x {i32 parent, char name[32]}
           skeletons x joints x {f32 local[16], f32 invRest[16]}   row-major, Blender armature space (z up, -y forward)
           looks x {char name[32], u32 skeleton, u32 firstIndex, u32 indexCount, f32 height}
           vertices x {f32 pos[3], f32 nrm[3], f32 uv[2], u8 joints[4], u8 weights[4], u32 klass}  (44 bytes)
           indices x u32;  atlas RGBA8, rows top-down
  AANM v1: u32 joints, clips; f32 fps
           clips x {char name[64], u32 frames, u32 loop, u32 first}
           frames x {joints x {f32 q[4] (x y z w), f32 t[3]}, f32 anchor[2]}
klass: 0 plain, 1 skin, 2 hair, 3 cloth (tinted per person at run time)."""
import bpy, sys, os, json, struct, math
import numpy as np
from mathutils import Matrix, Vector

a = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
def opt(k, d=None):
    return a[a.index(k) + 1] if k in a else d
SRC = opt('--src', 'D:/AlteraGame/_asset_src/chr')
REPO = opt('--repo', 'D:/AlteraGame/altera')
MAXC = int(opt('--max-clips', '100000'))
DO_MESH = '--clips-only' not in a
DO_CLIPS = '--mesh-only' not in a

OUT_G = SRC + '/OF/Modular Character Outfits - Fantasy[Standard]/Exports/glTF (Godot-Unreal)/Outfits/'
UB = SRC + '/UB/Universal Base Characters[Standard]/Base Characters/Godot - UE/'
HAIR = SRC + '/UB/Universal Base Characters[Standard]/Hairstyles/Rigged to Head Bone/glTF (Godot -Unreal)/'
ANIM = REPO + '/content/animations/humanoid_ual'
OUT_MESH = REPO + '/assets/altera_game/people'
OUT_ANIM = ANIM + '/baked'
os.makedirs(OUT_MESH, exist_ok=True)
os.makedirs(OUT_ANIM, exist_ok=True)

LOOKS = [
    # name, outfit, hair, beard
    ('m_peasant_parted', 'Male_Peasant', 'Hair_SimpleParted', False),
    ('m_peasant_beard', 'Male_Peasant', 'Hair_Buzzed', True),
    ('m_peasant_long', 'Male_Peasant', 'Hair_Long', True),
    ('m_ranger', 'Male_Ranger', 'Hair_SimpleParted', True),
    ('f_peasant_long', 'Female_Peasant', 'Hair_Long', False),
    ('f_peasant_buns', 'Female_Peasant', 'Hair_Buns', False),
    ('f_peasant_parted', 'Female_Peasant', 'Hair_SimpleParted', False),
    ('f_ranger', 'Female_Ranger', 'Hair_BuzzedFemale', False),
]

# atlas cells: image name prefix -> (x, y, size) in pixels, origin bottom-left (Blender pixels)
ATLAS_W = ATLAS_H = 512
CELLS = {
    'T_Peasant_BaseColor': (0, 256, 256), 'T_Ranger_BaseColor': (256, 256, 256),
    'T_Regular_Male': (0, 128, 128), 'T_Regular_Female': (128, 128, 128),
    'T_Superhero_Male': (256, 128, 128), 'T_Superhero_Female': (384, 128, 128),
    'T_Hair_1': (0, 0, 128), 'T_Hair_2': (128, 0, 128), 'T_Eye': (256, 0, 128),
}

def klass_of(mat_name):
    n = mat_name.lower()
    if 'hair' in n: return 2
    if 'regular' in n or 'superhero' in n: return 1
    if 'peasant' in n or 'ranger' in n: return 3
    return 0

def imp(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in list(new):
        if o.type == 'MESH' and o.name.startswith('Icosphere'):
            bpy.data.objects.remove(o)
            new.remove(o)
    return new

def clear():
    for o in list(bpy.data.objects): bpy.data.objects.remove(o)
    for coll in (bpy.data.meshes, bpy.data.armatures, bpy.data.actions, bpy.data.materials, bpy.data.images):
        for x in list(coll):
            if x.users == 0: coll.remove(x)

def base_image(mat):
    if not mat or not mat.use_nodes: return None
    bsdf = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf and bsdf.inputs['Base Color'].links:
        n = bsdf.inputs['Base Color'].links[0].from_node
        while n and n.type != 'TEX_IMAGE' and n.inputs:
            ins = [i for i in n.inputs if i.links]
            n = ins[0].links[0].from_node if ins else None
        if n is not None and n.type == 'TEX_IMAGE' and n.image: return n.image
    for n in mat.node_tree.nodes:
        if n.type == 'TEX_IMAGE' and n.image and 'basecolor' in n.image.name.lower(): return n.image
    for n in mat.node_tree.nodes:
        if n.type == 'TEX_IMAGE' and n.image and n.image.size[0] > 0 and 'normal' not in n.image.name.lower(): return n.image
    return None

def cell_for(img_name):
    for k, c in CELLS.items():
        if img_name.startswith(k): return k, c
    return None, None

def keep_head(o):
    import bmesh
    gi = {g.index: g.name for g in o.vertex_groups}
    bm = bmesh.new(); bm.from_mesh(o.data)
    dl = bm.verts.layers.deform.active
    kill = []
    for v in bm.verts:
        w = v[dl]
        h = sum(wt for g, wt in w.items() if gi.get(g) == 'Head')
        n = sum(wt for g, wt in w.items() if gi.get(g) == 'neck_01')
        if h + n * 0.7 < 0.5: kill.append(v)
    bmesh.ops.delete(bm, geom=kill, context='VERTS')
    bm.to_mesh(o.data); bm.free()

def mat_np(m):
    return np.array([[m[r][c] for c in range(4)] for r in range(4)], np.float64)

# ------------------------------------------------------------------ meshes
atlas = np.zeros((ATLAS_H, ATLAS_W, 4), np.float32)
atlas[..., 3] = 1
filled = set()

def fill_cell(key, cell, img):
    if key in filled: return
    x, y, s = cell
    im = img.copy()
    im.scale(s, s)
    px = np.array(im.pixels[:], np.float32).reshape(s, s, 4)
    lum = (px[..., :3] @ np.array([0.3, 0.59, 0.11], np.float32))[..., None]
    px[..., :3] = px[..., :3] * 0.85 + lum * 0.15
    atlas[y:y + s, x:x + s] = px
    bpy.data.images.remove(im)
    filled.add(key)

joint_names, joint_parent = None, None
skeletons = {}   # 'm'/'f' -> list of (local, invRest)
looks_out = []
verts_out = bytearray()
idx_out = []
nverts = 0

def skeleton_of(arm):
    bones = arm.data.bones
    out = []
    for nm in joint_names:
        b = bones[nm]
        R = mat_np(b.matrix_local)
        if b.parent:
            L = np.linalg.inv(mat_np(b.parent.matrix_local)) @ R
        else:
            L = R
        out.append((L, np.linalg.inv(R)))
    return out

def bake_look(name, outfit, hair, beard):
    global joint_names, joint_parent, nverts
    clear()
    male = outfit.startswith('Male')
    objs = imp(OUT_G + outfit + '.gltf')
    arm = next(o for o in objs if o.type == 'ARMATURE')
    if joint_names is None:
        joint_names = [b.name for b in arm.data.bones]
        joint_parent = [joint_names.index(b.parent.name) if b.parent else -1 for b in arm.data.bones]
    sk = 'm' if male else 'f'
    if sk not in skeletons: skeletons[sk] = skeleton_of(arm)
    meshes = [o for o in objs if o.type == 'MESH']
    base = imp(UB + ('Superhero_Male_FullBody.gltf' if male else 'Superhero_Female_FullBody.gltf'))
    for o in base:
        if o.type == 'MESH':
            if o.name.lower().startswith('superhero'): keep_head(o)
            if o.name.lower().startswith('eyebrows') or o.type == 'MESH': meshes.append(o)
    parts = [hair] + (['Hair_Beard'] if beard else [])
    for hp in parts:
        meshes += [o for o in imp(HAIR + hp + '.gltf') if o.type == 'MESH']
    first = len(idx_out)
    zmin, zmax = 1e9, -1e9
    for o in meshes:
        me = o.data
        me.calc_loop_triangles()
        gi = {g.index: g.name for g in o.vertex_groups}
        jidx = {gidx: joint_names.index(nm) for gidx, nm in gi.items() if nm in joint_names}
        uvl = me.uv_layers.active
        cn = me.corner_normals
        w2a = o.matrix_world  # armatures are at identity: object space = armature space
        nm3 = w2a.to_3x3()
        mats = []
        for sl in o.material_slots:
            img = base_image(sl.material)
            key, cell = cell_for(img.name) if img else (None, None)
            if key: fill_cell(key, cell, img)
            mats.append((cell, klass_of(sl.material.name if sl.material else '')))
        vw = []
        for v in me.vertices:
            ws = sorted(((g.weight, jidx[g.group]) for g in v.groups if g.group in jidx and g.weight > 1e-4), reverse=True)[:4]
            tot = sum(w for w, _ in ws) or 1.0
            q = [int(round(w / tot * 255)) for w, _ in ws]
            if ws:
                q[0] += 255 - sum(q)
            js = [j for _, j in ws]
            while len(q) < 4: q.append(0); js.append(js[0] if js else 0)
            vw.append((js, q))
        cache = {}
        for t in me.loop_triangles:
            cell, kl = mats[t.material_index] if t.material_index < len(mats) else (None, 0)
            tri = []
            for li, vi in zip(t.loops, t.vertices):
                p = w2a @ me.vertices[vi].co
                n = (nm3 @ Vector(cn[li].vector)).normalized()
                u, v = (uvl.data[li].uv if uvl else (0.5, 0.5))
                if cell:
                    x, y, s = cell
                    e = 0.5 / s
                    u = min(max(u - math.floor(u) if (u < 0 or u > 1) else u, e), 1 - e)
                    v = min(max(v - math.floor(v) if (v < 0 or v > 1) else v, e), 1 - e)
                    au, av = (x + u * s) / ATLAS_W, (y + v * s) / ATLAS_H
                else:
                    au, av = 1.0 - 4.0 / ATLAS_W, 4.0 / ATLAS_H   # a plain corner cell
                key = (vi, round(au, 5), round(av, 5), round(n.x, 3), round(n.y, 3), round(n.z, 3), kl)
                k = cache.get(key)
                if k is None:
                    js, q = vw[vi]
                    verts_out.extend(struct.pack('<8f4B4BI', p.x, p.y, p.z, n.x, n.y, n.z, au, 1.0 - av, *js, *q, kl))
                    k = nverts
                    nverts += 1
                    cache[key] = k
                    zmin, zmax = min(zmin, p.z), max(zmax, p.z)
                tri.append(k)
            idx_out.extend(tri)
    looks_out.append((name, 0 if male else 1, first, len(idx_out) - first, zmax - zmin))
    print('LOOK', name, 'tris', (len(idx_out) - first) // 3, 'height', round(zmax - zmin, 3), 'verts', nverts)
    portrait(name, arm, meshes)

def portrait(name, arm, meshes):
    sc = bpy.context.scene
    head = arm.matrix_world @ arm.data.bones['Head'].head_local
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
    sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = 'ORTHO'
    cam.data.ortho_scale = 0.42
    target = head + Vector((0, 0, 0.09))
    cam.location = target + Vector((0.22, -1.2, 0.05))
    cam.rotation_euler = (target - cam.location).to_track_quat('-Z', 'Y').to_euler()
    sc.render.resolution_x = sc.render.resolution_y = 128
    sc.render.film_transparent = True
    sc.view_settings.view_transform = 'Standard'
    try: sc.render.engine = 'BLENDER_EEVEE'
    except TypeError:
        try: sc.render.engine = 'BLENDER_EEVEE_NEXT'
        except TypeError: pass
    sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
    sun.data.energy = 3.0
    sun.rotation_euler = (math.radians(60), 0, math.radians(-35))
    sc.collection.objects.link(sun)
    w = bpy.data.worlds.new('w'); sc.world = w; w.use_nodes = True
    bg = next(n for n in w.node_tree.nodes if n.type == 'BACKGROUND')
    bg.inputs['Color'].default_value = (0.45, 0.45, 0.5, 1); bg.inputs['Strength'].default_value = 0.8
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.render.filepath = OUT_MESH + '/portrait_' + name + '.png'
    bpy.ops.render.render(write_still=True)
    # class mask: flat emission red = skin, green = hair, blue = cloth
    flat = {}
    for o in meshes:
        for sl in o.material_slots:
            kl = klass_of(sl.material.name if sl.material else '')
            if kl not in flat:
                m = bpy.data.materials.new('id%d' % kl); m.use_nodes = True
                nt = m.node_tree
                for n in list(nt.nodes): nt.nodes.remove(n)
                out = nt.nodes.new('ShaderNodeOutputMaterial'); em = nt.nodes.new('ShaderNodeEmission')
                em.inputs['Color'].default_value = [(0, 0, 0, 1), (1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1)][kl]
                nt.links.new(em.outputs[0], out.inputs['Surface'])
                flat[kl] = m
            sl.material = flat[kl]
    bg.inputs['Strength'].default_value = 0
    sc.render.filepath = OUT_MESH + '/portrait_' + name + '_id.png'
    bpy.ops.render.render(write_still=True)

if DO_MESH:
    for L in LOOKS: bake_look(*L)
    with open(OUT_MESH + '/people.askin', 'wb') as f:
        nj = len(joint_names)
        f.write(b'ASKN' + struct.pack('<8I', 1, nj, 2, len(looks_out), nverts, len(idx_out), ATLAS_W, ATLAS_H))
        for nm, p in zip(joint_names, joint_parent):
            f.write(struct.pack('<i', p) + nm.encode()[:31].ljust(32, b'\0'))
        for sk in ('m', 'f'):
            for L, I in skeletons[sk]:
                f.write(struct.pack('<16f', *L.astype(np.float32).ravel()))
                f.write(struct.pack('<16f', *I.astype(np.float32).ravel()))
        for nm, s, fi, ic, h in looks_out:
            f.write(nm.encode()[:31].ljust(32, b'\0') + struct.pack('<3If', s, fi, ic, h))
        f.write(bytes(verts_out))
        f.write(np.array(idx_out, np.uint32).tobytes())
        rgba = (np.clip(atlas, 0, 1) * 255 + 0.5).astype(np.uint8)[::-1]   # top-down rows
        f.write(rgba.tobytes())
    with open(OUT_MESH + '/people_joints.json', 'w') as f:
        json.dump({'joints': joint_names, 'parents': joint_parent}, f)
    print('WROTE people.askin', nverts, 'verts', len(idx_out) // 3, 'tris')

# ------------------------------------------------------------------ clips
FPS = 15.0
WINDOW_S = 5.0
STATE_CLIPS = {
    'idle': ['ual1__idle_loop'], 'walk': ['ual1__walk_loop'], 'run': ['ual1__jog_fwd_loop'],
    'sprint': ['ual1__sprint_loop'], 'sit': ['ual1__sitting_idle_loop'], 'talk': ['ual1__idle_talking_loop'],
    'sleep': ['mx_single__sleeping_idle', 'mx_single__laying_sleeping'], 'pray': ['mx_single__praying'],
    'fight': ['ual1__sword_idle'], 'attack': ['ual1__sword_attack', 'ual1__punch_jab'], 'pain': ['ual1__hit_chest'],
    'death': ['ual1__death01'], 'down': ['ual1__death01'], 'carry': ['ual2__walk_carry_loop'],
    'cast': ['ual1__spell_simple_idle_loop'], 'eat': ['ual2__consume'], 'work': ['ual1__interact'],
    'cold': ['cmu__80_41_freezing'], 'crouch': ['ual1__crouch_idle_loop'], 'swim': ['ual1__swim_fwd_loop'],
}
HOLD = {'ual1__death01'}

if DO_CLIPS:
    man = json.load(open(ANIM + '/manifest.json', encoding='utf-8'))
    clips = man['clips']
    if joint_names is None:
        jj = json.load(open(OUT_MESH + '/people_joints.json'))
        joint_names, joint_parent = jj['joints'], jj['parents']
    want, inter, state = [], {}, {}
    def pick(cands):
        for c in cands:
            if c in clips and os.path.exists(ANIM + '/' + clips[c]['file']):
                if c not in want: want.append(c)
                return c
        return None
    for st, cands in STATE_CLIPS.items():
        c = pick(cands)
        if c: state[st] = c
    for k, v in man['keys']['interaction'].items():
        c = pick(v.get('clips', []))
        if c: inter[k] = c
    want = want[:MAXC]
    nj = len(joint_names)
    frames_blob = bytearray()
    table = []
    total = 0
    sc = bpy.context.scene
    for ci, cname in enumerate(want):
        clear()
        info = clips[cname]
        objs = imp(ANIM + '/' + info['file'])
        arm = next(o for o in objs if o.type == 'ARMATURE')
        act = arm.animation_data.action if arm.animation_data else None
        if act is None and bpy.data.actions:
            act = bpy.data.actions[0]
            arm.animation_data_create().action = act
        f0, f1 = act.frame_range
        src_fps = float(info.get('fps', 30))
        dur = (f1 - f0) / src_fps
        loop = bool(info.get('loop')) or cname.endswith('_loop')
        t0, t1 = 0.0, dur
        if dur > WINDOW_S and not loop:
            t0 = (dur - WINDOW_S) * 0.5; t1 = t0 + WINDOW_S
        elif dur > 8.0:
            t1 = 8.0
        n = max(2, int(round((t1 - t0) * FPS)) + (0 if loop else 1))
        rest = {b.name: mat_np(b.matrix_local) for b in arm.data.bones}
        pel = []
        rows = []
        for k in range(n):
            fr = f0 + (t0 + k / FPS) * src_fps
            sc.frame_set(int(math.floor(fr)), subframe=fr - math.floor(fr))
            row = bytearray()
            for nm in joint_names:
                pb = arm.pose.bones.get(nm)
                if pb is None:
                    row.extend(struct.pack('<7f', 0, 0, 0, 1, 0, 0, 0)); continue
                loc, q, _ = pb.matrix_basis.decompose()
                row.extend(struct.pack('<7f', q.x, q.y, q.z, q.w, loc.x, loc.y, loc.z))
            pm = arm.pose.bones['pelvis'].matrix   # armature space
            pel.append((pm.translation.x, pm.translation.y))
            rows.append(row)
        pel = np.array(pel)
        for k, row in enumerate(rows):
            if loop:
                d = (pel[-1] - pel[0]) * (k / max(1, n)) if n > 1 else np.zeros(2)
                anc = pel[0] + d
            else:
                anc = pel[0]
            frames_blob.extend(row)
            frames_blob.extend(struct.pack('<2f', float(anc[0]), float(anc[1])))
        table.append((cname, n, 2 if cname in HOLD else (1 if loop else 0), total))
        total += n
        print('CLIP %d/%d %s frames %d loop %s dur %.2f' % (ci + 1, len(want), cname, n, loop, dur), flush=True)
    with open(OUT_ANIM + '/people.aanim', 'wb') as f:
        f.write(b'AANM' + struct.pack('<3If', 1, nj, len(table), FPS))
        for cname, n, loop, first in table:
            f.write(cname.encode()[:63].ljust(64, b'\0') + struct.pack('<3I', n, loop, first))
        f.write(bytes(frames_blob))
    with open(OUT_ANIM + '/people_anim.json', 'w', encoding='utf-8') as f:
        json.dump({'clips': [t[0] for t in table], 'state': state, 'interaction': inter}, f, indent=1)
    print('WROTE people.aanim', len(table), 'clips', total, 'frames', len(frames_blob) // 1024, 'KB')
