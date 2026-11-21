"""ASKN v1 read/write (the format of src/game_view/skinned.cpp, see bake_people.py)."""
import struct
import numpy as np

STRIDE = 44
VDT = np.dtype([('pos', '<f4', 3), ('nrm', '<f4', 3), ('uv', '<f4', 2), ('j', 'u1', 4), ('w', 'u1', 4), ('k', '<u4')])


class Askin:
    def __init__(self):
        self.names, self.parents = [], []
        self.skeletons = []          # list of (nj, 2, 4, 4): local, invRest (row-major)
        self.looks = []              # (name, skeleton, first, count, height)
        self.verts = np.zeros(0, VDT)
        self.idx = np.zeros(0, np.uint32)
        self.atlas = np.zeros((1, 1, 4), np.uint8)   # rows top-down

    @staticmethod
    def read(path):
        b = open(path, 'rb').read()
        assert b[:4] == b'ASKN'
        ver, nj, ns, nl, nv, ni, aw, ah = struct.unpack_from('<8I', b, 4)
        p = 36
        a = Askin()
        for _ in range(nj):
            a.parents.append(struct.unpack_from('<i', b, p)[0])
            a.names.append(b[p + 4:p + 36].split(b'\0')[0].decode())
            p += 36
        for _ in range(ns):
            m = np.frombuffer(b, '<f4', nj * 32, p).reshape(nj, 2, 4, 4).astype(np.float64)
            a.skeletons.append(m)
            p += nj * 128
        for _ in range(nl):
            nm = b[p:p + 32].split(b'\0')[0].decode()
            s, fi, ic, h = struct.unpack_from('<3If', b, p + 32)
            a.looks.append([nm, s, fi, ic, h])
            p += 48
        a.verts = np.frombuffer(b, VDT, nv, p).copy()
        p += nv * STRIDE
        a.idx = np.frombuffer(b, '<u4', ni, p).copy()
        p += ni * 4
        a.atlas = np.frombuffer(b, np.uint8, aw * ah * 4, p).reshape(ah, aw, 4).copy()
        return a

    def rest_world(self, s=0):
        """World rest matrices (inverse of invRest), (nj, 4, 4)."""
        return np.linalg.inv(self.skeletons[s][:, 1])

    def write(self, path):
        nj = len(self.names)
        ah, aw = self.atlas.shape[:2]
        with open(path, 'wb') as f:
            f.write(b'ASKN' + struct.pack('<8I', 1, nj, len(self.skeletons), len(self.looks), len(self.verts),
                                          len(self.idx), aw, ah))
            for nm, pa in zip(self.names, self.parents):
                f.write(struct.pack('<i', pa) + nm.encode()[:31].ljust(32, b'\0'))
            for s in self.skeletons:
                f.write(s.astype('<f4').tobytes())
            for nm, s, fi, ic, h in self.looks:
                f.write(nm.encode()[:31].ljust(32, b'\0') + struct.pack('<3If', s, fi, ic, h))
            f.write(self.verts.astype(VDT).tobytes())
            f.write(self.idx.astype('<u4').tobytes())
            f.write(np.ascontiguousarray(self.atlas, np.uint8).tobytes())
