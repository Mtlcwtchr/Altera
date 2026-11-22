"""AL-70: dev-only Skyrim people on Skyrim's OWN skeleton, skinning and animations.

    python people_native.py [--data <Skyrim SE Data>] [--repo D:/AlteraGame/altera] [--weight 0.4]

Why: AL-11 (actors.py) bent the Skyrim meshes onto our UAL rig. Every Skyrim bone was
turned to lie along the UAL bone; the pelvis bone of Skyrim points almost backwards
(its child, Spine, sits 5 cm above and 7 cm behind), so the whole pelvis was turned by
70 degrees: belly out, hollow back. It also took the heaviest body (_1 meshes).

Now the mesh keeps its own bind: the 99-bone skeleton.hkx (male / female reference
pose) is the rig, the NIF skin weights stay on their own bones, and the clips are
Skyrim's (male/ and female/ locomotion, idles, crafting, furniture-free work), read by
hkx.py. A sim state or interaction without a Skyrim analogue plays its UAL clip
retargeted onto the Skyrim skeleton (world rotation deltas; only the limbs are aimed
from the T-pose onto Skyrim's A-pose, the spine keeps its own frames).

Output (git-ignored, Bethesda content is never committed; replace before prod):
  assets/altera_game/people/people_sky.askin        meshes, two skeletons (m, f), atlas
  assets/altera_game/people/people_sky.aanim        clips (sky__* Skyrim, ual__* retargeted; @f = female take)
  assets/altera_game/people/people_sky_anim.json    state / interaction -> clip
  assets/altera_game/props/registry_people.json     what was used, replace_before_prod
"""
import argparse, json, math, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hkx
from bsa import Bsa, SEP
from nif import Nif
from askin import Askin, VDT
import actors as A

FPS = 15.0
MAX_S = 12.0
FLIP = A.FLIP

# Skyrim clips (Skyrim - Animations.bsa, meshes/actors/character/animations/...). A name
# that exists under male/ and female/ is taken from both (the female take is "<clip>@f").
SKY = {
    'idle': 'mt_idle', 'walk': 'mt_walkforward', 'run': 'mt_runforward', 'sprint': 'mt_sprintforward',
    'sit': 'idlesit_crosslegged', 'talk': 'dialogueneutralexpressivea', 'sleep': 'bedroll_frontsleep',
    'pray': 'idlepraycrouched_loop',   # death / down: Skyrim hands those to the ragdoll, the UAL fall stays
    'eat': 'animobjecteatingidlebase', 'cold': 'idlewarmhands_crouched', 'crouch': 'idlekneelidle',
    'work': 'idleexamine',
}
# interaction -> Skyrim clip (where Skyrim has the same act); the rest keep their UAL clip, retargeted
SKY_ACT = {
    'eat_meal': 'animobjecteatingidlebase', 'eat_berries': 'animobjecteatingidlebase',
    'eat_roots': 'animobjecteatingidlebase', 'drink_water': 'idledrinkfromfountain',
    'sleep_in_bed': 'bedroll_frontsleep', 'sleep_by_fire': 'bedroll_frontsleep', 'sleep_rough': 'bedroll_frontsleep',
    'rest': 'idlesit_crosslegged', 'shelter_in_house': 'idlesit_crosslegged',
    'warm_at_hearth': 'idlewarmhands_crouched', 'warm_at_campfire': 'idlewarmhands_crouched',
    'warm_at_village_fire': 'idlewarmhands_crouched',
    'take_meal': 'mt_activatepickup', 'steal_meal': 'mt_activatepickup',
    'take_roots': 'mt_activatepickuplow', 'take_firewood': 'mt_activatepickuplow',
    'gather_firewood': 'mt_activatepickuplow', 'gather_branches': 'mt_activatepickuplow',
    'claim_object': 'mt_activatepickup', 'gather_stones': 'mt_activatepickuplow',
    'split_log': 'animobjectchoploop', 'knap_blade': 'blacksmith_hammering', 'make_axe': 'blacksmith_hammering',
    'build_hut': 'aohammerwallloop', 'make_loom': 'aohammerloop',
    'cook_meal': 'craftingcookingpotstir', 'cook_mushrooms': 'craftingcookingpotstir',
    'brew_remedy': 'craftingcookingpotstir', 'cook_meal_campfire': 'craftingcookingpotstir',
    'cook_mushrooms_campfire': 'craftingcookingpotstir', 'cook_meat': 'craftingcookingpotstir',
    'dig_roots': 'idlepickaxe_floorloop', 'dig_clay': 'idlepickaxe_floorloop', 'bury_dead': 'idlepickaxe_floorloop',
    'ret_fiber': 'craftingtanningidlea', 'weave_cloth': 'craftingtanningidlea',
    'chat': 'dialogueneutralexpressivea', 'start_dialog': 'dialogueneutralexpressivea',
    'gossip': 'dialogueneutralsubtlea', 'reveal_secret': 'dialogueneutralsubtlea',
    'argue': 'dialogueangrya', 'insult': 'dialogueangrya', 'extort': 'dialogueangrya', 'divorce': 'dialogueangrya',
    'pray_at_shrine': 'idlepraycrouched_loop', 'pray_alone': 'idlepraycrouched_loop',
    'make_offering': 'idleblessingkneel_loop', 'swear_fealty': 'idlekneelidle', 'surrender': 'idlekneelidle',
    'propose_marriage': 'idlekneelidle', 'observe_work': 'mt_idle_a_arms_crossed', 'examine': 'idleexamine',
    'compliment': 'spectatorclap', 'vote': 'npc_applaud2', 'greet': 'special_waveover',
    'wander': 'mt_walkforward', 'travel_to': 'mt_walkforward', 'flee': 'mt_sprintforward',
    'treat_wound': 'idlekneelidle', 'tend_sick': 'idlekneelidle',
}
HOLD = {'deathanimationa'}

ANIM_DIR = 'meshes' + SEP + 'actors' + SEP + 'character' + SEP + 'animations' + SEP


# ------------------------------------------------------------------ math
def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def matq(m):
    t = np.trace(m)
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


class Rig:
    """The Skyrim skeleton in OUR space: world = (FLIP * sc) @ skyrim world (metres, -y forward).
    Locals stay Skyrim's (translations in Skyrim units); only the root carries FLIP and sc."""

    def __init__(self, sk, sc):
        self.names, self.parents, self.sc = sk.names, sk.parents, sc
        self.n = len(sk.names)
        self.rt, self.rq = sk.t.copy(), sk.q.copy()
        self.index = {nm: i for i, nm in enumerate(self.names)}
        self.rest_R, self.rest_p = self.fk(self.rt, [qmat(q) for q in self.rq])

    def fk(self, T, Rs):
        """locals (sky units) -> world rotations (ours) and positions (metres)."""
        R = [None] * self.n
        P = [None] * self.n
        for j in range(self.n):
            pa = self.parents[j]
            if pa < 0:
                R[j] = FLIP @ Rs[j]
                P[j] = FLIP @ T[j] * self.sc
            else:
                R[j] = R[pa] @ Rs[j]
                P[j] = P[pa] + R[pa] @ T[j] * self.sc
        return R, P

    def matrices(self):
        """(nj, 2, 4, 4) local, invRest for the ASKN skeleton block."""
        out = np.zeros((self.n, 2, 4, 4))
        for j in range(self.n):
            L = np.eye(4)
            L[:3, :3] = qmat(self.rq[j])
            L[:3, 3] = self.rt[j]
            if self.parents[j] < 0:
                F = np.eye(4)
                F[:3, :3] = FLIP * self.sc
                L = F @ L
            W = np.eye(4)
            W[:3, :3] = self.rest_R[j] * self.sc     # the chain carries the root's scale
            W[:3, 3] = self.rest_p[j]
            out[j, 0] = L
            out[j, 1] = np.linalg.inv(W)
        return out

    def basis_row(self, T, Rs):
        """Animated locals -> per joint basis (x y z w, t) = restLocal^-1 @ animLocal."""
        row = np.zeros((self.n, 7))
        for j in range(self.n):
            Rr = qmat(self.rq[j])
            qb = matq(Rr.T @ Rs[j])
            tb = Rr.T @ (T[j] - self.rt[j])
            row[j, :4], row[j, 4:] = qb, tb
        return row


# ------------------------------------------------------------------ clips
def resample(n_src_frames, fps_src):
    dur = min((n_src_frames - 1) / fps_src, MAX_S)
    n = max(2, int(round(dur * FPS)))
    return [k / FPS * fps_src for k in range(n)], n


def sky_clip(rig, data):
    an = hkx.Animation(hkx.load(data))
    frames, n = resample(an.nframes, an.fps)
    rows, pel = [], []
    pi = rig.index['NPC Pelvis [Pelv]']
    for f in frames:
        Tt, Qt = an.sample(f)
        T = rig.rt.copy()
        Q = [qmat(q) for q in rig.rq]
        for ti, b in enumerate(an.bones):
            if 0 <= b < rig.n:
                T[b] = Tt[ti]
                Q[b] = qmat(Qt[ti] / max(np.linalg.norm(Qt[ti]), 1e-12))
        rows.append(rig.basis_row(T, Q))
        _, P = rig.fk(T, Q)
        pel.append(P[pi][:2])
    return rows, np.array(pel)


def anchors(pel, loop):
    n = len(pel)
    if loop and n > 1:
        return [pel[0] + (pel[-1] - pel[0]) * (k / n) for k in range(n)]
    return [pel[0]] * n


# UAL -> Skyrim retarget of the clips Skyrim has no analogue for
LIMB = ('clavicle',)   # clavicles keep their frames (aiming them raises the shoulders)
AIMED = ('upperarm', 'lowerarm', 'hand', 'thigh', 'calf', 'foot', 'thumb', 'index', 'middle', 'ring', 'pinky')


class Retarget:
    def __init__(self, rig, ual, ual_si):
        self.rig, self.ual = rig, ual
        self.uj = {n: i for i, n in enumerate(ual.names)}
        sk = ual.skeletons[ual_si]
        self.uL = sk[:, 0]
        RW = np.linalg.inv(sk[:, 1])
        self.uR0, self.uP0 = RW[:, :3, :3], RW[:, :3, 3]
        # orthonormalise (Blender matrices may carry tiny scale)
        self.uR0 = np.array([self._ortho(m) for m in self.uR0])
        self.map = {}
        for nm, j in rig.index.items():
            if nm in A.HUMAN and 'Twist' not in nm and A.HUMAN[nm] in self.uj:
                self.map[j] = self.uj[A.HUMAN[nm]]
        # rest alignment: Skyrim limb bone -> the direction of the UAL bone (A- to T-pose)
        self.C = {}
        sky_of = {A.HUMAN[nm]: rig.index[nm] for nm in rig.index if nm in A.HUMAN and 'Twist' not in nm}
        for j, u in self.map.items():
            un = ual.names[u]
            self.C[j] = np.eye(3)
            if un.split('_')[0] in AIMED and un in A.AIM and A.AIM[un] in sky_of and A.AIM[un] in self.uj:
                ds = rig.rest_p[sky_of[A.AIM[un]]] - rig.rest_p[j]
                du = self.uP0[self.uj[A.AIM[un]]] - self.uP0[u]
                if np.linalg.norm(ds) > 1e-4 and np.linalg.norm(du) > 1e-4:
                    self.C[j] = A.rot_between(ds, du)
        self.com = rig.index['NPC COM [COM ]']
        self.upel = self.uj['pelvis']
        self.ratio = rig.rest_p[self.com][2] / max(self.uP0[self.upel][2], 1e-6)

    @staticmethod
    def _ortho(m):
        u, _, vt = np.linalg.svd(m)
        return u @ vt

    def ual_world(self, row):
        n = len(self.ual.names)
        W = [None] * n
        for j in range(n):
            q, t = row[j, :4], row[j, 4:7]
            B = np.eye(4)
            B[:3, :3] = qmat(q / max(np.linalg.norm(q), 1e-12))
            B[:3, 3] = t
            L = self.uL[j] @ B
            pa = self.ual.parents[j]
            W[j] = W[pa] @ L if pa >= 0 else L
        return W

    def frame(self, urow):
        rig = self.rig
        UW = self.ual_world(urow)
        R = [None] * rig.n
        P = [None] * rig.n
        Rl = [None] * rig.n
        T = rig.rt.copy()
        for j in range(rig.n):
            pa = rig.parents[j]
            if j in self.map:
                u = self.map[j]
                D = self._ortho(UW[u][:3, :3]) @ self.uR0[u].T
                R[j] = D @ self.C[j] @ rig.rest_R[j]
            else:
                R[j] = (R[pa] if pa >= 0 else FLIP) @ qmat(rig.rq[j])
            if pa < 0:
                Rl[j] = FLIP.T @ R[j]
                P[j] = rig.rest_p[j]
            else:
                Rl[j] = R[pa].T @ R[j]
                if j == self.com:
                    P[j] = rig.rest_p[j] + (UW[self.upel][:3, 3] - self.uP0[self.upel]) * self.ratio
                    T[j] = R[pa].T @ (P[j] - P[pa]) / rig.sc
                else:
                    P[j] = P[pa] + R[pa] @ rig.rt[j] * rig.sc
        return rig.basis_row(T, Rl), P[rig.index['NPC Pelvis [Pelv]']][:2]


def read_aanim(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'AANM'
    _, nj, nc, fps = struct.unpack_from('<3If', b, 4)
    p = 20
    clips = []
    for _ in range(nc):
        nm = b[p:p + 64].split(b'\0')[0].decode()
        n, loop, first = struct.unpack_from('<3I', b, p + 64)
        clips.append((nm, n, loop, first))
        p += 76
    row = nj * 7 + 2
    total = max(c[3] + c[1] for c in clips)
    fr = np.frombuffer(b, '<f4', total * row, p).reshape(total, row).astype(np.float64)
    return clips, fr, nj, fps


# ------------------------------------------------------------------ meshes
def read_part(lib, path, weight, blended=False):
    """Skinned shapes of a part; a _1 (heavy) mesh is blended with its _0 (thin) twin."""
    shapes = A.nif_parts(lib, path, blended)
    if path.endswith('_1.nif'):
        thin = A.nif_parts(lib, path[:-6] + '_0.nif')
        if len(thin) == len(shapes):
            for s1, s0 in zip(shapes, thin):
                if s1.pos.shape == s0.pos.shape:
                    s1.pos = s0.pos + (s1.pos - s0.pos) * weight
    return shapes


FACEGEN = 'actors/character/facegendata/facegeom/'


def shape_kind(sh, kind):
    """What a shape is, by its texture: the skin under the clothes is skin, not cloth (AL-71:
    the necks and arms took the family's cloth colour), brows and beards are hair."""
    t = sh.texture.lower().replace(SEP, '/')
    if '/hair/' in t or '/beards/' in t or 'brows/' in t:
        return 'hair'
    if '/eyes/' in t or '/mouth/' in t:
        return 'plain'
    if 'actors/character/male/' in t or 'actors/character/female/' in t:
        return 'skin'
    return kind


def expand_looks(cfg):
    """AL-71: every outfit with several FaceGen heads (head, eyes, brows, hair, beard of a
    named Skyrim NPC, morphed and tinted by the game's own FaceGen bake): more faces."""
    faces = cfg.get('faces')
    if not faces:
        return cfg['looks']
    out = []
    for sex in 'mf':
        outs = [L for L in cfg['looks'] if L['look'][0] == sex]
        fs = faces[sex]
        for i, L in enumerate(outs):
            for k, face in enumerate(fs[i::len(outs)]):
                parts = [p for p in L['parts'] if p.get('kind') != 'hair'
                         and not p['nif'].endswith(('head.nif', 'eyesmale.nif', 'eyesfemale.nif'))]
                parts.append({'nif': FACEGEN + face['id'] + '.nif', 'kind': 'face'})
                out.append({'look': L['look'] + '_' + face['name'].lower(), 'parts': parts, 'face': face['name']})
    return out


def bake_meshes(lib, cfg, rigs, nif_skel, atlas, weight, report):
    verts, idx, looks = [], [], []
    nv = 0
    for L in expand_looks(cfg):
        sex = L['look'][0]
        si = 0 if sex == 'm' else 1
        rig, nsk = rigs[si], nif_skel[si]

        def bone_of(name):
            b = name
            while b is not None and b not in rig.index:
                b = nsk.get(b, (None, None))[1]
            return rig.index.get(b, rig.index['NPC Pelvis [Pelv]'])

        first = len(idx)
        for part in L['parts']:
            kind = part.get('kind', 'plain')
            for sh in read_part(lib, part['nif'], weight, kind == 'face'):
                if not any(b in nsk for b in sh.bones):
                    print('  no skeleton bone in', part['nif'])
                    continue
                w = sh.weights.astype(np.float64)
                w /= np.maximum(w.sum(1, keepdims=True), 1e-9)
                bj = [bone_of(b) for b in sh.bones]
                # shape -> bone space by the skin (Skyrim units); an extra bone (not in the
                # rig) goes on through its NIF node into the space of the rig bone it rides on
                Mb = []
                for i, b in enumerate(sh.bones):
                    M = np.linalg.inv(sh.bind[i])
                    anc = rig.names[bj[i]]
                    if b != anc and b in nsk and anc in nsk:
                        M = np.linalg.inv(nsk[anc][0]) @ nsk[b][0] @ M
                    Mb.append(M)
                h = np.c_[sh.pos.astype(np.float64), np.ones(len(sh.pos))]
                p = np.zeros((len(h), 3))
                n = np.zeros((len(h), 3))
                jw = np.zeros((len(h), rig.n))
                for c in range(4):
                    for bi in np.unique(sh.bidx[:, c]):
                        sel = (sh.bidx[:, c] == bi) & (w[:, c] > 0)
                        if not sel.any() or bi >= len(Mb):
                            continue
                        j = bj[bi]
                        R, P0 = rig.rest_R[j], rig.rest_p[j]
                        q = (h[sel] @ Mb[bi].T)[:, :3]
                        p[sel] += w[sel, c, None] * ((q @ R.T) * rig.sc + P0)
                        nb = sh.nrm[sel].astype(np.float64) @ Mb[bi][:3, :3].T
                        n[sel] += w[sel, c, None] * (nb @ R.T)
                        jw[sel, j] += w[sel, c]
                n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
                top = np.argsort(-jw, axis=1)[:, :4]
                tw = np.take_along_axis(jw, top, 1)
                tw /= np.maximum(tw.sum(1, keepdims=True), 1e-9)
                sk = shape_kind(sh, 'plain' if kind == 'face' else kind)
                tint = sh.textures[6] if sk == 'skin' and len(sh.textures) > 6 else ''
                eyes = '/eyes/' in sh.texture.lower().replace(SEP, '/')
                cut = sh.alpha_test or sh.alpha_blend   # eyes too: the black round the iris is clear
                gain = 1.0 if sh.alpha_test or eyes else 1.7   # blended brows/beards kept thick
                cell = atlas.add(lib, sh.texture, sk if sk in ('hair', 'skin') else 'plain', cut,
                                 tint=tint, big=bool(tint), alpha_gain=gain)
                v = np.zeros(len(p), VDT)
                v['pos'], v['nrm'], v['uv'], v['j'] = p, n, sh.uv, top
                wq = np.round(tw * 255).astype(np.int32)
                wq[:, 0] += 255 - wq.sum(1)
                v['w'] = np.clip(wq, 0, 255)
                v['k'] = {'skin': 1, 'hair': 2, 'cloth': 3}.get(sk, 0)
                verts.append((v, cell))
                idx.append(sh.tris.reshape(-1).astype(np.uint32) + nv)
                nv += len(p)
        looks.append((L['look'], si, first, len(idx) - first))
        report.append(dict(L, weight=weight, replace_before_prod=True))
    return verts, idx, looks


def find_anim(anims, name, sex):
    """Archive key of a Skyrim clip: the sex folder first, then the shared folder."""
    for sub in (('male' if sex == 'm' else 'female') + SEP, ''):
        k = ANIM_DIR + sub + name + '.hkx'
        if k in anims.files:
            return k
    return None


def write_aanim(path, nj, table, blob):
    with open(path, 'wb') as f:
        f.write(b'AANM' + struct.pack('<3If', 1, nj, len(table), FPS))
        for cname, n, loop, first in table:
            f.write(cname.encode()[:63].ljust(64, b'\0') + struct.pack('<3I', n, loop, first))
        f.write(b''.join(blob))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='D:/Steam/steamapps/common/Skyrim Special Edition/Data')
    ap.add_argument('--repo', default='D:/AlteraGame/altera')
    ap.add_argument('--weight', type=float, default=0.4, help='Skyrim NPC weight: 0 thin, 1 heavy')
    ap.add_argument('--no-clips', action='store_true')
    ap.add_argument('--only-clips', default='', help='comma list of clip names (debug)')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    reg = json.load(open(os.path.join(here, 'registry.json'), encoding='utf-8'))
    cfg = reg['actors']
    lib = A.Lib(a.data, cfg.get('extra_archives', []))
    anims = Bsa(os.path.join(a.data, 'Skyrim - Animations.bsa'))
    pc = cfg['people']
    hk = {'m': 'meshes/actors/character/character assets/skeleton.hkx',
          'f': 'meshes/actors/character/character assets female/skeleton_female.hkx'}
    sks = [hkx.Skeleton(hkx.load(anims.read(hk[s].replace('/', SEP)))) for s in 'mf']
    assert sks[0].names == sks[1].names and sks[0].parents == sks[1].parents, 'male and female rigs differ'
    nif_skel = [A.skeleton_world(lib, pc['skeleton_m']), A.skeleton_world(lib, pc['skeleton_f'])]
    # one scale for both: the male head joint at the height the UAL people had (1.6 m)
    head = sks[0].names.index('NPC Head [Head]')
    tmp = Rig(sks[0], A.UNIT)
    sc = A.UNIT * 1.6 / tmp.rest_p[head][2]
    rigs = [Rig(s, sc) for s in sks]
    # how far the hkx reference pose is from the NIF skeleton (the meshes' bind)
    for si, (rig, nsk) in enumerate(zip(rigs, nif_skel)):
        d = [np.linalg.norm(FLIP @ nsk[nm][0][:3, 3] * sc - rig.rest_p[j]) for nm, j in rig.index.items() if nm in nsk]
        print('rig %s: %d bones, hkx vs nif rest max %.4f m' % ('mf'[si], rig.n, max(d)))
    report = []
    atlas = A.Atlas(cell=256)
    verts, idx, looks = bake_meshes(lib, pc, rigs, nif_skel, atlas, a.weight, report)

    class Base:
        pass
    base = Base()
    base.names = [nm[:31] for nm in rigs[0].names]
    base.parents = rigs[0].parents
    base.skeletons = [r.matrices() for r in rigs]
    out = A.write_people(a.repo, base, verts, idx, looks, atlas)
    pdir = os.path.join(a.repo, 'assets/altera_game/people')
    # write_people names its file people_sky.askin: that is the one the game loads
    if a.no_clips:
        return
    # ---- clips
    ual = Askin.read(os.path.join(a.repo, 'assets/altera_game/people/people.askin'))
    ujs = json.load(open(os.path.join(a.repo, 'content/animations/humanoid_ual/baked/people_anim.json'), encoding='utf-8'))
    uclips, uframes, unj, ufps = read_aanim(os.path.join(a.repo, 'content/animations/humanoid_ual/baked/people.aanim'))
    ucl = {c[0]: c for c in uclips}
    rt = Retarget(rigs[0], ual, 0)
    table, blob, total = [], [], 0
    made = {}
    nj = rigs[0].n
    only = set(x for x in a.only_clips.split(',') if x)

    def emit(name, rows, anc, loop):
        nonlocal total
        for r, an in zip(rows, anc):
            blob.append(r.astype('<f4').tobytes() + np.asarray(an, '<f4').tobytes())
        table.append((name, len(rows), loop, total))
        total += len(rows)

    def sky(name):
        """'sky__<name>' (+ '@f' female take when the female folder has its own)."""
        cn = 'sky__' + name
        if cn in made:
            return made[cn]
        km = find_anim(anims, name, 'm')
        if km is None:
            print('  no Skyrim clip', name)
            made[cn] = None
            return None
        loop = 2 if name in HOLD else 1
        rows, pel = sky_clip(rigs[0], anims.read(km))
        emit(cn, rows, anchors(pel, loop == 1), loop)
        kf = find_anim(anims, name, 'f')
        if kf and kf != km:
            rows, pel = sky_clip(rigs[1], anims.read(kf))
            emit(cn + '@f', rows, anchors(pel, loop == 1), loop)
        print('CLIP', cn, len(rows), 'frames', '(+f)' if kf and kf != km else '', flush=True)
        made[cn] = cn
        return cn

    def ret(uname):
        cn = 'ual__' + uname
        if cn in made:
            return made[cn]
        if uname not in ucl:
            made[cn] = None
            return None
        _, n, loop, first = ucl[uname]
        rows, pel = [], []
        for k in range(n):
            r, pp = rt.frame(uframes[first + k, :unj * 7].reshape(unj, 7))
            rows.append(r)
            pel.append(pp)
        emit(cn, rows, anchors(np.array(pel), loop == 1), loop)
        print('CLIP', cn, n, 'frames (retargeted)', flush=True)
        made[cn] = cn
        return cn

    state, inter = {}, {}
    for st, uc in ujs['state'].items():
        if only and st not in only:
            continue
        c = sky(SKY[st]) if st in SKY else None
        state[st] = c or ret(uc)
    for it, uc in ujs['interaction'].items():
        if only and it not in only:
            continue
        c = sky(SKY_ACT[it]) if it in SKY_ACT else None
        inter[it] = c or ret(uc)
    state = {k: v for k, v in state.items() if v}
    inter = {k: v for k, v in inter.items() if v}
    write_aanim(os.path.join(pdir, 'people_sky.aanim'), nj, table, blob)
    json.dump({'clips': [t[0] for t in table], 'state': state, 'interaction': inter},
              open(os.path.join(pdir, 'people_sky_anim.json'), 'w', encoding='utf-8'), indent=1)
    print('wrote people_sky.aanim', len(table), 'clips', total, 'frames')
    rdir = os.path.join(a.repo, 'assets/altera_game/props')
    json.dump({'source': reg['source'], 'replace_before_prod': True,
               'note': 'AL-70: Skyrim people on the Skyrim skeleton.hkx with Skyrim clips (sky__*) and UAL clips retargeted (ual__*)',
               'looks': report, 'clips': [t[0] for t in table]},
              open(os.path.join(rdir, 'registry_people.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
