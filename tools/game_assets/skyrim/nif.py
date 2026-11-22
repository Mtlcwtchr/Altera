"""Skyrim SE NIF (20.2.0.7, user 12, stream 100) reader for static meshes, dev-only (AL-11).

Reads the node tree and every BSTriShape under it (positions, uvs, normals,
triangles, the diffuse texture of its BSLightingShaderProperty / the source
texture of a BSEffectShaderProperty, alpha testing), world-transformed into
the file's root space. Skinned, havok, particle and editor-marker shapes are
left out. Unknown blocks are skipped by the header's block sizes.
"""
import struct
import numpy as np


class _R:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.p)[0]
        self.p += 8
        return v

    def f(self, n=1):
        v = struct.unpack_from('<%df' % n, self.d, self.p)
        self.p += 4 * n
        return v if n > 1 else v[0]

    def sized(self):
        n = self.u32()
        s = self.d[self.p:self.p + n].decode('cp1252', 'replace')
        self.p += n
        return s

    def short(self):
        n = self.u8()
        s = self.d[self.p:self.p + n].rstrip(b'\0').decode('cp1252', 'replace')
        self.p += n
        return s


class Shape:
    def __init__(self):
        self.name = ''
        self.pos = None      # (n, 3) float32, root space, game units
        self.uv = None       # (n, 2)
        self.nrm = None      # (n, 3)
        self.tris = None     # (m, 3) uint32
        self.texture = ''    # diffuse, a textures\... path
        self.textures = []   # every slot of the texture set (6: the face tint of a FaceGen head)
        self.alpha_test = False
        self.alpha_blend = False
        self.weights = None  # (n, 4) float32, skinned shapes only
        self.bidx = None     # (n, 4) uint8 into .bones
        self.bones = []      # bone node names of the skin instance
        self.bind = []       # 4x4 bone -> shape space (the bind pose), per bone


class Nif:
    def __init__(self, data):
        r = _R(data)
        line_end = data.index(b'\n')
        self.header = data[:line_end].decode('ascii', 'replace')
        r.p = line_end + 1
        ver = r.u32()
        assert ver == 0x14020007, 'not a Skyrim SE nif: %x' % ver
        r.u8()                      # endian
        self.user = r.u32()
        nblocks = r.u32()
        self.bsver = r.u32()
        r.short(); r.short(); r.short()   # author, process, export
        if self.bsver >= 103:
            r.short()
        ntypes = r.u16()
        types = [r.sized() for _ in range(ntypes)]
        idx = [r.u16() for _ in range(nblocks)]
        sizes = [r.u32() for _ in range(nblocks)]
        nstr = r.u32()
        r.u32()
        self.strings = [r.sized() for _ in range(nstr)]
        ngroups = r.u32()
        r.p += 4 * ngroups
        self.types = [types[i] for i in idx]
        self.offsets = []
        p = r.p
        for s in sizes:
            self.offsets.append(p)
            p += s
        self.data = data
        self.blocks = {}

    def _str(self, i):
        return self.strings[i] if 0 <= i < len(self.strings) else ''

    def _netobj(self, r):
        name = self._str(r.i32())
        n = r.u32()
        r.p += 4 * n
        r.i32()   # controller
        return name

    def _avobj(self, r):
        name = self._netobj(r)
        r.u32()   # flags (bsver > 26)
        t = np.array(r.f(3), np.float64)
        rot = np.array(r.f(9), np.float64).reshape(3, 3)
        s = r.f()
        r.i32()   # collision
        return name, t, rot, s

    def node(self, i):
        r = _R(self.data, self.offsets[i])
        name, t, rot, s = self._avobj(r)
        n = r.u32()
        kids = [r.i32() for _ in range(n)]
        return name, t, rot, s, kids

    def texture_of(self, shader):
        self.last_textures = []
        if shader < 0:
            return '', False
        ty = self.types[shader]
        r = _R(self.data, self.offsets[shader])
        if ty == 'BSLightingShaderProperty':
            if self.bsver < 130:
                r.u32()   # skyrim shader type
            self._netobj(r)
            f1 = r.u32()
            r.u32()
            r.f(4)        # uv offset, scale
            ts = r.i32()
            if ts < 0 or self.types[ts] != 'BSShaderTextureSet':
                return '', False
            q = _R(self.data, self.offsets[ts])
            n = q.u32()
            tex = [q.sized() for _ in range(n)]
            self.last_textures = tex
            return (tex[0] if tex else ''), False
        if ty == 'BSEffectShaderProperty':
            self._netobj(r)
            r.u32(); r.u32(); r.f(4)
            return r.sized(), True
        return '', False

    def alpha_of(self, ref):
        if ref < 0 or self.types[ref] != 'NiAlphaProperty':
            return False, False
        r = _R(self.data, self.offsets[ref])
        self._netobj(r)
        flags = r.u16()
        return bool(flags & 0x200), bool(flags & 0x1)

    def tri_shape(self, i):
        r = _R(self.data, self.offsets[i])
        name, t, rot, s = self._avobj(r)
        r.f(4)                 # bounding sphere
        skin = r.i32()
        shader = r.i32()
        alpha = r.i32()
        desc = r.u64()
        ntri = r.u16() if self.bsver < 130 else r.u32()
        nv = r.u16()
        size = r.u32()
        sh = Shape()
        sh.name = name
        if size == 0 and skin >= 0:
            # skinned (trees sway on bones): the vertices live in the skin partition
            got = self.skin_vertices(skin)
            if got is None:
                return None, (t, rot, s)
            raw, desc, tris = got
            nv = len(raw)
        elif size == 0 or nv == 0:
            return None, (t, rot, s)
        else:
            tris = None
        vsize = (desc & 0xF) * 4
        flags = desc >> 44
        uv_off = ((desc >> 8) & 0xF) * 4
        n_off = ((desc >> 16) & 0xF) * 4
        full = bool(flags & 0x400) or self.bsver == 100
        if tris is None:
            raw = np.frombuffer(self.data, np.uint8, nv * vsize, r.p).reshape(nv, vsize)
            r.p += nv * vsize
        if full:
            pos = raw[:, 0:12].copy().view(np.float32).reshape(nv, 3)
        else:
            pos = raw[:, 0:6].copy().view(np.float16).reshape(nv, 3).astype(np.float32)
        if flags & 0x2:
            uv = raw[:, uv_off:uv_off + 4].copy().view(np.float16).reshape(nv, 2).astype(np.float32)
        else:
            uv = np.zeros((nv, 2), np.float32)
        if flags & 0x8:
            nb = raw[:, n_off:n_off + 3].astype(np.float32)
            nrm = nb / 127.5 - 1.0
        else:
            nrm = np.tile(np.array([0, 0, 1], np.float32), (nv, 1))
        if tris is None:
            tris = np.frombuffer(self.data, np.uint16, ntri * 3, r.p).reshape(ntri, 3).astype(np.uint32)
            r.p += ntri * 6
        if self.types[i] == 'BSDynamicTriShape':
            # heads: the morphed positions follow as full floats, the packed ones are stale
            if self.bsver == 100:
                r.u32()   # particle data size (0 on actors)
            dn = r.u32() // 16
            if dn == nv:
                pos = np.frombuffer(self.data, np.float32, nv * 4, r.p).reshape(nv, 4)[:, :3].copy()
        sh.pos, sh.uv, sh.nrm, sh.tris = pos, uv, nrm, tris
        if flags & 0x40 and skin >= 0:
            so = ((desc >> 28) & 0xF) * 4
            sh.weights = raw[:, so:so + 8].copy().view(np.float16).reshape(nv, 4).astype(np.float32)
            sh.bidx = raw[:, so + 8:so + 12].copy()
            sh.bones, sh.bind = self.skin_bones(skin)
        sh.texture, sh.alpha_blend = self.texture_of(shader)
        sh.textures = self.last_textures
        test, blend = self.alpha_of(alpha)
        sh.alpha_test = test
        sh.alpha_blend = sh.alpha_blend or (blend and not test)
        return sh, (t, rot, s)

    def skin_vertices(self, skin):
        if self.types[skin] not in ('NiSkinInstance', 'BSDismemberSkinInstance'):
            return None
        r = _R(self.data, self.offsets[skin])
        r.i32()
        part = r.i32()
        if part < 0 or self.types[part] != 'NiSkinPartition':
            return None
        r = _R(self.data, self.offsets[part])
        nparts = r.u32()
        dsize = r.u32()
        vsize = r.u32()
        desc = r.u64()
        if vsize == 0 or dsize == 0:
            return None
        nv = dsize // vsize
        raw = np.frombuffer(self.data, np.uint8, nv * vsize, r.p).reshape(nv, vsize)
        r.p += nv * vsize
        tris = []
        for _ in range(nparts):
            pnv, pnt, nb, ns, nw = (r.u16() for _ in range(5))
            r.p += 2 * nb
            vmap = None
            if r.u8():
                vmap = np.frombuffer(self.data, np.uint16, pnv, r.p).astype(np.uint32)
                r.p += 2 * pnv
            if r.u8():
                r.p += 4 * pnv * nw
            lens = [r.u16() for _ in range(ns)]
            if r.u8():
                if ns:
                    r.p += 2 * sum(lens)
                else:
                    r.p += 6 * pnt
            if r.u8():
                r.p += pnv * nw
            r.u8()         # lod level
            r.u8()         # global vb
            r.u64()        # vertex desc
            tc = np.frombuffer(self.data, np.uint16, pnt * 3, r.p).reshape(pnt, 3).astype(np.uint32)
            r.p += 6 * pnt
            tris.append(tc)
        return raw, desc, np.concatenate(tris) if tris else np.zeros((0, 3), np.uint32)

    def _transform(self, r):
        rot = np.array(r.f(9), np.float64).reshape(3, 3)
        t = np.array(r.f(3), np.float64)
        s = r.f()
        M = np.eye(4)
        M[:3, :3] = rot * s
        M[:3, 3] = t
        return M

    def skin_bones(self, skin):
        """Bone names and bone -> shape matrices (the inverse of NiSkinData's shape -> bone)."""
        r = _R(self.data, self.offsets[skin])
        data = r.i32()
        r.i32()           # partition
        r.i32()           # skeleton root
        n = r.u32()
        refs = [r.i32() for _ in range(n)]
        names = []
        for b in refs:
            q = _R(self.data, self.offsets[b])
            names.append(self._netobj(q) if b >= 0 else '')
        binds = [np.eye(4)] * n
        if data >= 0 and self.types[data] == 'NiSkinData':
            q = _R(self.data, self.offsets[data])
            self._transform(q)
            nb = q.u32()
            has_w = q.u8()
            binds = []
            for _ in range(nb):
                M = self._transform(q)
                q.f(4)
                if has_w:
                    nv = q.u16()
                    q.p += 6 * nv
                # tree wind bones (BSTreeNode) can carry a degenerate bind: statics don't need it
                binds.append(np.linalg.inv(M) if np.isfinite(M).all() and abs(np.linalg.det(M)) > 1e-12 else np.eye(4))
        return names, binds

    def skinned(self):
        """Every skinned shape of the file, vertices in shape space (no node transforms)."""
        out = []
        for i, ty in enumerate(self.types):
            if ty in ('BSTriShape', 'BSSubIndexTriShape', 'BSDynamicTriShape'):
                sh, _ = self.tri_shape(i)
                if sh is not None and sh.weights is not None:
                    out.append(sh)
        return out

    def old_shape(self, i):
        """Stream 83 (Oldrim, LE mods) NiTriShape / NiTriStrips: full floats in a NiTriShapeData,
        after CampfireLite's core/src/content/nif.cpp (read_geometry, read_geometry_data)."""
        r = _R(self.data, self.offsets[i])
        name, t, rot, s = self._avobj(r)
        data = r.i32()
        r.i32()                    # skin
        nm = r.u32()
        r.p += 8 * nm              # material names and extra data
        r.i32()                    # active material
        r.u8()                     # material needs update
        shader = r.i32()
        alpha = r.i32()
        if data < 0 or self.types[data] not in ('NiTriShapeData', 'NiTriStripsData'):
            return None, (t, rot, s)
        strips = self.types[data] == 'NiTriStripsData'
        q = _R(self.data, self.offsets[data])
        q.i32()                    # group id
        n = q.u16()
        q.u8(); q.u8()             # keep, compress flags
        if not q.u8() or n == 0:
            return None, (t, rot, s)
        pos = np.frombuffer(self.data, np.float32, n * 3, q.p).reshape(n, 3).copy()
        q.p += n * 12
        vflags = q.u16()
        q.u32()                    # material crc
        nrm = np.tile(np.array([0, 0, 1], np.float32), (n, 1))
        if q.u8():
            nrm = np.frombuffer(self.data, np.float32, n * 3, q.p).reshape(n, 3).copy()
            q.p += n * 12
            if vflags & 0x1000:
                q.p += n * 24
        q.p += 16                  # centre, radius
        if q.u8():
            q.p += n * 16          # colours
        uvsets = vflags & 0x3F
        uv = np.zeros((n, 2), np.float32)
        if uvsets > 0:
            uv = np.frombuffer(self.data, np.float32, n * 2, q.p).reshape(n, 2).copy()
            q.p += n * 8 * uvsets
        q.u16()                    # consistency
        q.i32()                    # additional data
        if not strips:
            nt = q.u16()
            q.u32()
            tris = np.zeros((0, 3), np.uint32)
            if q.u8():
                tris = np.frombuffer(self.data, np.uint16, nt * 3, q.p).reshape(nt, 3).astype(np.uint32)
        else:
            q.u16()
            ns = q.u16()
            lens = [q.u16() for _ in range(ns)]
            out = []
            if q.u8():
                for L in lens:
                    st = [q.u16() for _ in range(L)]
                    for k in range(2, L):
                        a, b, c = st[k - 2], st[k - 1], st[k]
                        if a == b or b == c or a == c:
                            continue
                        out.append((a, b, c) if k % 2 == 0 else (a, c, b))
            tris = np.array(out, np.uint32).reshape(-1, 3)
        sh = Shape()
        sh.name = name
        sh.pos, sh.uv, sh.nrm, sh.tris = pos, uv, nrm, tris
        sh.texture, sh.alpha_blend = self.texture_of(shader)
        sh.textures = self.last_textures
        test, blend = self.alpha_of(alpha)
        sh.alpha_test = test
        sh.alpha_blend = sh.alpha_blend or (blend and not test)
        return sh, (t, rot, s)

    def shapes(self):
        """Every static shape in root space."""
        out = []
        roots = [i for i, ty in enumerate(self.types) if ty in ('NiNode', 'BSFadeNode', 'BSLeafAnimNode', 'BSTreeNode',
                                                             'BSMultiBoundNode', 'BSOrderedNode', 'BSValueNode')][:1]
        if not roots:
            return out
        seen = set()

        def walk(i, M):
            if i < 0 or i in seen:
                return
            seen.add(i)
            ty = self.types[i]
            if ty in ('BSTriShape', 'BSMeshLODTriShape', 'BSSubIndexTriShape', 'NiTriShape', 'NiTriStrips'):
                sh, (t, rot, s) = self.old_shape(i) if ty.startswith('Ni') else self.tri_shape(i)
                if sh is None:
                    return
                if 'editormarker' in sh.name.lower() or sh.alpha_blend and not sh.texture:
                    return
                L = np.eye(4)
                L[:3, :3] = rot * s
                L[:3, 3] = t
                W = M @ L
                p = np.c_[sh.pos.astype(np.float64), np.ones(len(sh.pos))] @ W.T
                sh.pos = p[:, :3].astype(np.float32)
                n = sh.nrm.astype(np.float64) @ W[:3, :3].T
                n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
                sh.nrm = n.astype(np.float32)
                out.append(sh)
                return
            if ty in ('NiNode', 'BSFadeNode', 'BSLeafAnimNode', 'BSTreeNode', 'BSMultiBoundNode', 'BSOrderedNode',
                      'BSValueNode', 'BSDamageStage', 'BSBlastNode', 'NiSwitchNode', 'NiBillboardNode'):
                name, t, rot, s, kids = self.node(i)
                if 'editormarker' in name.lower() or name.lower().startswith('collision'):
                    return
                L = np.eye(4)
                L[:3, :3] = rot * s
                L[:3, 3] = t
                W = M @ L
                if ty == 'NiSwitchNode':
                    kids = kids[:1]
                for k in kids:
                    walk(k, W)

        walk(roots[0], np.eye(4))
        return out
