"""AL-11: dev-only Skyrim people and beasts on OUR rigs, so the baked clips play on them.

    python actors.py [--data <Skyrim SE Data>] [--repo D:/AlteraGame/altera] [--only people|hens|...]

The game's clips (people.aanim, beasts/<sp>.aanim) are made for our skeletons, so a
Skyrim body cannot bring its own rig. Each stand-in is moved onto the rig of the
committed bake instead:

  people   the parts of a look (clothes, hands, feet, head, hair, beard, eyes) keep
           their Skyrim skin weights; Skyrim bones map to UAL joints by name, and
           every joint's Skyrim bone is turned to lie along the UAL bone (Skyrim binds
           in an A-pose, UAL in a T-pose), the mesh following by its own weights.
  beasts   a Skyrim creature has another skeleton, so its mesh is stretched onto the
           bounding box of our bake and takes the weights of the nearest vertices of
           our mesh (a data transfer).

Output (git-ignored, Bethesda content is never committed), loaded by the game in
place of the committed bake when present:
  assets/altera_game/people/people_sky.askin
  assets/altera_game/beasts/<species>_sky.askin
plus preview_actors.png and the registry entries (replace_before_prod) in
assets/altera_game/props/registry_actors.json.
"""
import argparse, io, json, os, sys
import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bsa import Library, Bsa, SEP, texture_key
from nif import Nif
from askin import Askin, VDT

UNIT = 0.0142875
CELL = 256                       # atlas cell of one texture
FLIP = np.diag([-1.0, -1.0, 1.0])  # Skyrim faces +y with its left at -x; our rigs face -y, left at +x

# Skyrim bone -> UAL joint. Twist bones ride on their limb; unlisted bones climb to a listed parent.
HUMAN = {
    'NPC Pelvis [Pelv]': 'pelvis', 'NPC Spine [Spn0]': 'spine_01', 'NPC Spine1 [Spn1]': 'spine_02',
    'NPC Spine2 [Spn2]': 'spine_03', 'NPC Neck [Neck]': 'neck_01', 'NPC Head [Head]': 'Head',
    'NPC Root [Root]': 'root', 'NPC COM [COM ]': 'pelvis',
}
for S, s in (('L', 'l'), ('R', 'r')):
    HUMAN.update({
        'NPC %s Clavicle [%sClv]' % (S, S): 'clavicle_' + s, 'NPC %s UpperArm [%sUar]' % (S, S): 'upperarm_' + s,
        'NPC %s UpperarmTwist1 [%sUt1]' % (S, S): 'upperarm_' + s,
        'NPC %s UpperarmTwist2 [%sUt2]' % (S, S): 'upperarm_' + s,
        'NPC %s Forearm [%sLar]' % (S, S): 'lowerarm_' + s, 'NPC %s ForearmTwist1 [%sLt1]' % (S, S): 'lowerarm_' + s,
        'NPC %s ForearmTwist2 [%sLt2]' % (S, S): 'lowerarm_' + s, 'NPC %s Hand [%sHnd]' % (S, S): 'hand_' + s,
        'NPC %s Thigh [%sThg]' % (S, S): 'thigh_' + s, 'NPC %s Calf [%sClf]' % (S, S): 'calf_' + s,
        'NPC %s Foot [%sft ]' % (S, S): 'foot_' + s, 'NPC %s Toe0 [%sToe]' % (S, S): 'ball_' + s,
    })
    for f, fin in enumerate(('thumb', 'index', 'middle', 'ring', 'pinky')):
        for k in range(3):
            HUMAN['NPC %s Finger%d%d [%sF%d%d]' % (S, f, k, S, f, k)] = '%s_%02d_%s' % (fin, k + 1, s)
# Which joint a joint's bone points at (the direction that is turned from A- to T-pose).
AIM = {'pelvis': 'spine_01', 'spine_01': 'spine_02', 'spine_02': 'spine_03', 'spine_03': 'neck_01',
       'neck_01': 'Head'}
for s in 'lr':
    AIM.update({'clavicle_' + s: 'upperarm_' + s, 'upperarm_' + s: 'lowerarm_' + s, 'lowerarm_' + s: 'hand_' + s,
                'hand_' + s: 'middle_01_' + s, 'thigh_' + s: 'calf_' + s, 'calf_' + s: 'foot_' + s,
                'foot_' + s: 'ball_' + s})
    for fin in ('thumb', 'index', 'middle', 'ring', 'pinky'):
        AIM['%s_01_%s' % (fin, s)] = '%s_02_%s' % (fin, s)
        AIM['%s_02_%s' % (fin, s)] = '%s_03_%s' % (fin, s)


class Lib:
    def __init__(self, data, extra=()):
        self.base = Library(data)
        self.extra = [Bsa(p) for p in extra if os.path.exists(p)]

    def read(self, path):
        p = path.lower().replace('/', SEP)
        for a in reversed(self.extra):
            if p in a.files:
                return a.read(p)
        return self.base.read(p)


def skeleton_world(lib, path):
    """name -> (4x4 world, parent name) of a skeleton.nif."""
    n = Nif(lib.read('meshes' + SEP + path.replace('/', SEP)))
    out = {}

    def walk(i, M, parent):
        if i < 0 or not n.types[i].endswith('Node'):
            return
        name, t, rot, s, kids = n.node(i)
        L = np.eye(4)
        L[:3, :3] = rot * s
        L[:3, 3] = t
        W = M @ L
        out[name] = (W, parent)
        for k in kids:
            walk(k, W, name)

    root = next(i for i, ty in enumerate(n.types) if ty.endswith('Node'))
    walk(root, np.eye(4), None)
    return out


class Atlas:
    def __init__(self):
        self.cells, self.index = [], {}

    def add(self, lib, tex, kind, cutout=False):
        key = (tex.lower(), kind, cutout)
        if key in self.index:
            return self.index[key]
        p = texture_key(tex)
        data = lib.read(p)
        img = None
        if data:
            try:
                img = Image.open(io.BytesIO(data))
                img.load()
                img = img.convert('RGBA').resize((CELL, CELL), Image.LANCZOS)
            except Exception as e:
                print('  texture', tex, 'unreadable:', e)
        if img is None:
            img = Image.new('RGBA', (CELL, CELL), (150, 140, 130, 255))
        a = np.asarray(img, np.float32) / 255.0
        if kind == 'hair':
            # tinted per person (klass 2): a light grey with the strands' detail
            g = a[:, :, :3].mean(2, keepdims=True)
            m = max(float(g[a[:, :, 3] > 0.5].mean()) if (a[:, :, 3] > 0.5).any() else 0.5, 0.05)
            a[:, :, :3] = np.clip(g / m * 0.85, 0, 1)
        if not cutout:
            a[:, :, 3] = 1.0   # Skyrim keeps gloss in the alpha of opaque textures
        self.index[key] = len(self.cells)
        self.cells.append((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8))
        return self.index[key]

    def image(self):
        n = max(1, len(self.cells))
        side = 1
        while side * side < n:
            side *= 2
        img = np.zeros((side * CELL, side * CELL, 4), np.uint8)
        for i, c in enumerate(self.cells):
            y, x = divmod(i, side)
            img[y * CELL:(y + 1) * CELL, x * CELL:(x + 1) * CELL] = c
        return img, side

    def remap(self, uv, cell, side):
        y, x = divmod(cell, side)
        h = 0.5 / CELL
        u = np.clip(uv, h, 1 - h)
        return np.c_[(x + u[:, 0]) / side, (y + u[:, 1]) / side].astype(np.float32)


def rot_between(a, b):
    a = a / max(np.linalg.norm(a), 1e-9)
    b = b / max(np.linalg.norm(b), 1e-9)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c < -0.9999:
        ax = np.cross(a, [1, 0, 0])
        if np.linalg.norm(ax) < 1e-6:
            ax = np.cross(a, [0, 1, 0])
        ax /= np.linalg.norm(ax)
        return 2 * np.outer(ax, ax) - np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + c)


def nif_parts(lib, path):
    d = lib.read('meshes' + SEP + path.replace('/', SEP))
    if d is None:
        print('  MISSING', path)
        return []
    return [s for s in Nif(d).skinned() if not s.alpha_blend or s.alpha_test]


# ------------------------------------------------------------------ people
def bake_people(lib, cfg, repo, atlas, report):
    base = Askin.read(os.path.join(repo, 'assets/altera_game/people/people.askin'))
    jindex = {n: i for i, n in enumerate(base.names)}
    skel = {'m': skeleton_world(lib, cfg['skeleton_m']), 'f': skeleton_world(lib, cfg['skeleton_f'])}
    verts, idx, looks = [], [], []
    nv = 0
    for L in cfg['looks']:
        sex = L['look'][0]
        sk = skel[sex]
        si = 0 if sex == 'm' else 1
        RW = base.rest_world(si)
        Pt = RW[:, :3, 3]

        def mapped(bone):
            b = bone
            while b is not None and b not in HUMAN:
                b = sk.get(b, (None, None))[1]
            return HUMAN.get(b, 'pelvis')

        # Skyrim joint positions (skeleton space, flipped, metres), scaled to the UAL height of the head
        Ps = {}
        for b, (W, _) in sk.items():
            if b in HUMAN and HUMAN[b] not in Ps and 'Twist' not in b:
                Ps[HUMAN[b]] = FLIP @ W[:3, 3] * UNIT
        k = Pt[jindex['Head'], 2] / Ps['Head'][2]
        for j in Ps:
            Ps[j] = Ps[j] * k
        # per joint: turn the Skyrim bone onto the UAL bone, then move its start onto the UAL joint
        R, T = {}, {}
        order = sorted(Ps, key=lambda j: len(chain(base, jindex[j])))
        for j in order:
            ji = jindex[j]
            if j in AIM and AIM[j] in Ps:
                a = jindex[AIM[j]]
                R[j] = rot_between(Ps[AIM[j]] - Ps[j], Pt[a] - Pt[ji])
            else:
                p = base.parents[ji]
                while p >= 0 and base.names[p] not in R:
                    p = base.parents[p]
                R[j] = R[base.names[p]] if p >= 0 else np.eye(3)
            T[j] = Pt[ji] - R[j] @ Ps[j]
        first = len(idx)
        for part in L['parts']:
            kind = part.get('kind', 'plain')
            for sh in nif_parts(lib, part['nif']):
                b0 = next((i for i, b in enumerate(sh.bones) if b in sk), None)
                if b0 is None:
                    print('  no skeleton bone in', part['nif'])
                    continue
                w = sh.weights.astype(np.float64)
                w /= np.maximum(w.sum(1, keepdims=True), 1e-9)
                # shape -> skeleton space by the skin itself (bone world after shape -> bone)
                M0 = sk[sh.bones[b0]][0] @ np.linalg.inv(sh.bind[b0])
                Mb = [sk[b][0] @ np.linalg.inv(sh.bind[i]) if b in sk else M0 for i, b in enumerate(sh.bones)]
                h = np.c_[sh.pos.astype(np.float64), np.ones(len(sh.pos))]
                p = np.zeros((len(h), 3))
                n = np.zeros((len(h), 3))
                for c in range(4):
                    for bi in np.unique(sh.bidx[:, c]):
                        sel = (sh.bidx[:, c] == bi) & (w[:, c] > 0)
                        if sel.any():
                            Mx = Mb[bi] if bi < len(Mb) else M0
                            p[sel] += w[sel, c, None] * (h[sel] @ Mx.T)[:, :3]
                            n[sel] += w[sel, c, None] * (sh.nrm[sel].astype(np.float64) @ Mx[:3, :3].T)
                p = p @ FLIP.T * UNIT * k
                n = n @ FLIP.T
                bj = [mapped(b) for b in sh.bones]
                q = np.zeros_like(p)
                qn = np.zeros_like(p)
                jw = np.zeros((len(p), len(base.names)))
                for c in range(4):
                    for bi in np.unique(sh.bidx[:, c]):
                        sel = (sh.bidx[:, c] == bi) & (w[:, c] > 0)
                        if not sel.any():
                            continue
                        j = bj[bi]
                        jj = j if j in R else 'pelvis'
                        q[sel] += w[sel, c, None] * (p[sel] @ R[jj].T + T[jj])
                        qn[sel] += w[sel, c, None] * (n[sel] @ R[jj].T)
                        jw[sel, jindex[j]] += w[sel, c]
                qn /= np.maximum(np.linalg.norm(qn, axis=1, keepdims=True), 1e-9)
                top = np.argsort(-jw, axis=1)[:, :4]
                tw = np.take_along_axis(jw, top, 1)
                tw /= np.maximum(tw.sum(1, keepdims=True), 1e-9)
                tex_kind = 'hair' if kind == 'hair' else 'plain'
                cell = atlas.add(lib, sh.texture, tex_kind, sh.alpha_test)
                v = np.zeros(len(p), VDT)
                v['pos'], v['nrm'] = q, qn
                v['uv'] = sh.uv            # remapped once the atlas is final
                v['j'] = top
                wq = np.round(tw * 255).astype(np.int32)
                wq[:, 0] += 255 - wq.sum(1)
                v['w'] = np.clip(wq, 0, 255)
                v['k'] = {'skin': 1, 'hair': 2, 'cloth': 3}.get(kind, 0)
                verts.append((v, cell))
                idx.append(sh.tris.reshape(-1).astype(np.uint32) + nv)
                nv += len(p)
        looks.append((L['look'], si, first, len(idx) - first))
        report.append(dict(L, replace_before_prod=True))
    return base, verts, idx, looks


def chain(a, j):
    out = []
    while j >= 0:
        out.append(j)
        j = a.parents[j]
    return out


def write_people(repo, base, verts, idx, looks, atlas):
    img, side = atlas.image()
    out = Askin()
    out.names, out.parents, out.skeletons = base.names, base.parents, base.skeletons
    vs = []
    for v, cell in verts:
        v = v.copy()
        v['uv'] = atlas.remap(v['uv'], cell, side)
        vs.append(v)
    out.verts = np.concatenate(vs)
    ilist, starts = [], []
    total = 0
    for nm, si, first, count in looks:
        part = np.concatenate(idx[first:first + count]) if count else np.zeros(0, np.uint32)
        starts.append((nm, si, total, len(part)))
        ilist.append(part)
        total += len(part)
    out.idx = np.concatenate(ilist)
    for nm, si, fi, ic in starts:
        sel = np.unique(out.idx[fi:fi + ic])
        h = float(out.verts['pos'][sel, 2].max()) if len(sel) else 1.8
        out.looks.append([nm, si, fi, ic, h])
    out.atlas = img
    path = os.path.join(repo, 'assets/altera_game/people/people_sky.askin')
    out.write(path)
    print('wrote', path, len(out.verts), 'verts', len(out.idx) // 3, 'tris, atlas', img.shape[:2],
          'MB', round(os.path.getsize(path) / 1e6, 1))
    return out


# ------------------------------------------------------------------ beasts
def bake_beast(lib, e, repo, report):
    sp = e['species']
    base = Askin.read(os.path.join(repo, 'assets/altera_game/beasts', sp + '.askin'))
    parts = []
    for p in e['nif'] if isinstance(e['nif'], list) else [e['nif']]:
        got = nif_parts(lib, p)
        if not got:   # a critter without a skin: its static shapes, our weights carry it
            d = lib.read('meshes' + SEP + p.replace('/', SEP))
            got = [s for s in Nif(d).shapes() if not s.alpha_blend] if d else []
        parts += got
    if not parts:
        report.append(dict(e, ok=False))
        return None
    atlas = Atlas()
    P, N, UV, tris, cells = [], [], [], [], []
    nv = 0
    ref = parts[0]
    for sh in parts:
        # parts of one creature share its bones: bring each shape into the first one's space
        M = np.eye(4)
        if sh is not ref and sh.bones and ref.bones:
            common = [b for b in sh.bones if b in ref.bones]
            if common:
                b = common[0]
                M = ref.bind[ref.bones.index(b)] @ np.linalg.inv(sh.bind[sh.bones.index(b)])
        p = (np.c_[sh.pos.astype(np.float64), np.ones(len(sh.pos))] @ M.T)[:, :3] @ FLIP.T
        n = sh.nrm.astype(np.float64) @ (FLIP @ M[:3, :3]).T
        P.append(p)
        N.append(n)
        UV.append((sh.uv, atlas.add(lib, sh.texture, 'plain', sh.alpha_test)))
        tris.append(sh.tris.reshape(-1).astype(np.uint32) + nv)
        nv += len(p)
    P = np.concatenate(P)
    N = np.concatenate(N)
    tv = base.verts
    # stretch onto the box of our mesh (per axis: our clips move our proportions)
    lo, hi = P.min(0), P.max(0)
    tlo, thi = tv['pos'].min(0).astype(np.float64), tv['pos'].max(0).astype(np.float64)
    if e.get('uniform'):
        s = (thi - tlo) / np.maximum(hi - lo, 1e-9)
        s = np.full(3, np.median(s))
    else:
        s = (thi - tlo) / np.maximum(hi - lo, 1e-9)
    Q = (P - (lo + hi) * 0.5) * s + (tlo + thi) * 0.5
    Q[:, 2] += tlo[2] - Q[:, 2].min()
    Nq = N / s
    Nq /= np.maximum(np.linalg.norm(Nq, axis=1, keepdims=True), 1e-9)
    # weights of the nearest vertices of our mesh, blended by distance
    tree = cKDTree(tv['pos'].astype(np.float64))
    d, nb = tree.query(Q, k=6)
    jw = np.zeros((len(Q), len(base.names)))
    inv = 1.0 / np.maximum(d, 1e-4) ** 2
    for c in range(6):
        src = tv[nb[:, c]]
        for s4 in range(4):
            np.add.at(jw, (np.arange(len(Q)), src['j'][:, s4]), inv[:, c] * src['w'][:, s4] / 255.0)
    top = np.argsort(-jw, axis=1)[:, :4]
    tw = np.take_along_axis(jw, top, 1)
    tw /= np.maximum(tw.sum(1, keepdims=True), 1e-9)
    img, side = atlas.image()
    v = np.zeros(len(Q), VDT)
    v['pos'], v['nrm'] = Q, Nq
    o = 0
    uvs = []
    for (uv, cell) in UV:
        uvs.append(atlas.remap(uv, cell, side))
    v['uv'] = np.concatenate(uvs)
    v['j'] = top
    wq = np.round(tw * 255).astype(np.int32)
    wq[:, 0] += 255 - wq.sum(1)
    v['w'] = np.clip(wq, 0, 255)
    out = Askin()
    out.names, out.parents, out.skeletons = base.names, base.parents, base.skeletons
    out.verts = v
    out.idx = np.concatenate(tris)
    out.looks = [[base.looks[0][0], 0, 0, len(out.idx), float(Q[:, 2].max())]]
    out.atlas = img
    path = os.path.join(repo, 'assets/altera_game/beasts', sp + '_sky.askin')
    out.write(path)
    print('%-10s %-55s verts %6d tris %6d size %s' % (sp, str(e['nif'])[:55], len(Q), len(out.idx) // 3,
                                                      (Q.max(0) - Q.min(0)).round(2)))
    report.append(dict(e, ok=True, replace_before_prod=True))
    return out


# ------------------------------------------------------------------ preview
def preview(sets, path):
    """Rest-pose front and side views, flat shaded with the atlas colour, one row per set."""
    rows = []
    for name, a in sets:
        for li, L in enumerate(a.looks):
            ids = a.idx[L[2]:L[2] + L[3]].reshape(-1, 3)
            P = a.verts['pos'].astype(np.float64)
            uv = a.verts['uv']
            ah, aw = a.atlas.shape[:2]
            col = a.atlas[np.clip((uv[:, 1] * ah).astype(int), 0, ah - 1), np.clip((uv[:, 0] * aw).astype(int), 0, aw - 1)]
            tiles = []
            for view in ((0, 2, 1), (1, 2, 0)):
                S = 160
                img = np.full((S, S, 3), 235, np.uint8)
                zb = np.full((S, S), -1e9)
                x, y, z = P[:, view[0]], P[:, view[1]], -P[:, view[2]] if view[0] == 0 else P[:, view[2]]
                h = max(P[:, 2].max(), (P[:, view[0]].max() - P[:, view[0]].min()))
                sc = (S - 8) / max(h, 1e-6)
                cx = (P[:, view[0]].max() + P[:, view[0]].min()) / 2
                for t in ids[::1]:
                    px = ((x[t] - cx) * sc + S / 2).astype(int)
                    py = (S - 4 - y[t] * sc).astype(int)
                    x0, x1 = max(px.min(), 0), min(px.max(), S - 1)
                    y0, y1 = max(py.min(), 0), min(py.max(), S - 1)
                    if x1 < x0 or y1 < y0:
                        continue
                    zz = z[t].mean()
                    if col[t[0], 3] < 128:
                        continue
                    sub = zb[y0:y1 + 1, x0:x1 + 1]
                    m = sub < zz
                    sub[m] = zz
                    img[y0:y1 + 1, x0:x1 + 1][m] = col[t[0], :3]
                tiles.append(img)
            rows.append(np.concatenate(tiles, 1))
    if not rows:
        return
    per = 6
    grid = []
    for i in range(0, len(rows), per):
        r = rows[i:i + per]
        while len(r) < per:
            r.append(np.full_like(rows[0], 255))
        grid.append(np.concatenate(r, 1))
    Image.fromarray(np.concatenate(grid, 0)).save(path)
    print('preview', path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    ap.add_argument('--only', default='')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    reg = json.load(open(os.path.join(here, 'registry.json'), encoding='utf-8'))
    cfg = reg['actors']
    lib = Lib(a.data, cfg.get('extra_archives', []))
    report = {'people': [], 'beasts': []}
    sets = []
    if a.only in ('', 'people'):
        atlas = Atlas()
        base, verts, idx, looks = bake_people(lib, cfg['people'], a.repo, atlas, report['people'])
        sets.append(('people', write_people(a.repo, base, verts, idx, looks, atlas)))
    for e in cfg['beasts']:
        if a.only in ('', e['species']):
            o = bake_beast(lib, e, a.repo, report['beasts'])
            if o is not None:
                sets.append((e['species'], o))
    out_dir = os.path.join(a.repo, 'assets/altera_game/props')
    os.makedirs(out_dir, exist_ok=True)
    preview(sets, os.path.join(out_dir, 'preview_actors.png'))
    json.dump({'source': reg['source'], 'replace_before_prod': True, 'actors': report},
              open(os.path.join(out_dir, 'registry_actors.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
