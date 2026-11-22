r"""Skyrim SE archives (BSA v105) for the dev-only asset pipeline (AL-11).

Lists and extracts files; LZ4 frame decompression in pure Python (no packages).
Everything taken from here is a placeholder to replace with our own art before
release (assets/altera_game/props/registry.json marks each one).

    python bsa.py list  "<Skyrim Data>/Skyrim - Meshes0.bsa" [substring]
    python bsa.py get   "<archive>" "meshes\clutter\barrel01.nif" out.nif
"""
import struct, sys, os

SEP = chr(92)   # the archives' path separator


def texture_key(path):
    """A NIF's texture path -> the archive key. Some meshes keep the author's
    absolute build path (skyrimhd/build/pc/data/textures/...): cut to
    the textures folder (AL-68: those trees and plants drew as grey cards)."""
    p = path.lower().replace('/', SEP).lstrip(SEP)
    i = p.find('textures' + SEP)
    if i > 0:
        p = p[i:]
    return p if p.startswith('textures' + SEP) else 'textures' + SEP + p


def _lz4_block(src, out, pos=0, end=None):
    end = len(src) if end is None else end
    while pos < end:
        token = src[pos]; pos += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[pos]; pos += 1; lit += b
                if b != 255: break
        out += src[pos:pos + lit]; pos += lit
        if pos >= end: break
        off = src[pos] | (src[pos + 1] << 8); pos += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[pos]; pos += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml): out.append(out[start + k])
    return out


def lz4_frame(data):
    assert struct.unpack_from('<I', data, 0)[0] == 0x184D2204, 'not an lz4 frame'
    flg = data[4]; bd = data[5]
    pos = 6 + (8 if flg & 0x08 else 0) + (4 if flg & 0x01 else 0) + 1
    out = bytearray()
    while True:
        size = struct.unpack_from('<I', data, pos)[0]; pos += 4
        if size == 0: break
        raw = size & 0x80000000; size &= 0x7FFFFFFF
        if raw: out += data[pos:pos + size]
        else: _lz4_block(data, out, pos, pos + size)
        pos += size
        if flg & 0x10: pos += 4
    return bytes(out)


class Bsa:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb')
        self.f = f
        hdr = f.read(36)
        magic, ver, off, flags, nfold, nfile, foldlen, filelen, ftype = struct.unpack('<4sIIIIIIII', hdr)
        assert magic == b'BSA\0' and ver in (104, 105), (magic, ver)
        self.compressed = bool(flags & 0x4)
        self.ver = ver   # 104: Oldrim and its mods (zlib), 105: SSE (LZ4 frames)
        self.embed = bool(flags & 0x100)
        folders = []
        for _ in range(nfold):
            if ver == 105:
                h, cnt, _pad, foff = struct.unpack('<QIIQ', f.read(24))
            else:
                h, cnt, foff = struct.unpack('<QII', f.read(16))
            folders.append(cnt)
        recs = []
        for cnt in folders:
            n = f.read(1)[0]
            name = f.read(n)[:-1].decode('cp1252')
            for _ in range(cnt):
                h, size, foff = struct.unpack('<QII', f.read(16))
                recs.append([name, size, foff])
        names = f.read(filelen).split(b'\0')
        self.files = {}
        for r, nm in zip(recs, names):
            p = (r[0] + SEP + nm.decode('cp1252')).lower()
            self.files[p] = (r[1], r[2])

    def read(self, path):
        size, off = self.files[path.lower()]
        comp = self.compressed != bool(size & 0x40000000)
        size &= 0x3FFFFFFF
        self.f.seek(off)
        if self.embed:
            n = self.f.read(1)[0]
            self.f.read(n)
            size -= n + 1
        if comp:
            orig = struct.unpack('<I', self.f.read(4))[0]
            if self.ver == 104:
                import zlib
                return zlib.decompress(self.f.read(size - 4))
            return lz4_frame(self.f.read(size - 4))
        return self.f.read(size)


class Library:
    """All archives of a Data folder, later ones winning (load order by name)."""
    def __init__(self, data_dir, kinds=('Meshes', 'Textures')):
        self.archives = []
        for n in sorted(os.listdir(data_dir)):
            if n.lower().endswith('.bsa') and any(k.lower() in n.lower() for k in kinds):
                self.archives.append(Bsa(os.path.join(data_dir, n)))
        self.index = {}
        for a in self.archives:
            for p in a.files: self.index[p] = a

    def read(self, path):
        p = path.lower().replace('/', SEP)
        a = self.index.get(p)
        return a.read(p) if a else None


if __name__ == '__main__':
    cmd = sys.argv[1]
    b = Bsa(sys.argv[2])
    if cmd == 'list':
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for p in sorted(b.files):
            if sub in p: print(p)
    elif cmd == 'get':
        open(sys.argv[4], 'wb').write(b.read(sys.argv[3]))
