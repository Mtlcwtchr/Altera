"""Skyrim SE Havok packfiles (hk_2010.2.0-r1, 64-bit) for the dev-only people (AL-70).

Reads what the people need and nothing more: the skeleton (hkaSkeleton: bone names,
parents, reference pose) and animations (hkaSplineCompressedAnimation with its
hkaAnimationBinding: per track translation / rotation / scale, sampled per frame).
Pure Python + numpy. Bethesda content: read from the user's own install, never
committed, replaced before release.

    python hkx.py <file.hkx>          dump objects, skeleton or the first frames
"""
import math, struct, sys
import numpy as np


class Packfile:
    def __init__(self, data):
        self.d = d = data
        m0, m1 = struct.unpack_from('<2I', d, 0)
        assert (m0, m1) == (0x57E0E057, 0x10C0C010), 'not a Havok packfile'
        ver = struct.unpack_from('<i', d, 12)[0]
        ptr, little = d[16], d[17]
        assert ptr == 8 and little == 1 and ver == 8, 'only 64-bit little-endian hk_2010 packfiles'
        nsec = struct.unpack_from('<i', d, 20)[0]
        pred = struct.unpack_from('<h', d, 60)[0]
        pos = 64 + (struct.unpack_from('<h', d, 62)[0] if pred != -1 else 0)
        self.sec = {}
        for i in range(nsec):
            name = d[pos:pos + 20].split(b'\0')[0].decode()
            start, lf, gf, vf, ex, im, end = struct.unpack_from('<7I', d, pos + 20)
            self.sec[name] = (i, start, lf, gf, vf, ex, im, end)
            pos += 48
        _, cs, *_rest = self.sec['__classnames__']
        cend = cs + self.sec['__classnames__'][2]
        self.cls = {}
        p = cs
        while p + 5 < cend:
            if d[p + 4] != 9:
                break
            e = d.index(b'\0', p + 5)
            self.cls[p + 5 - cs] = d[p + 5:e].decode()
            p = e + 1
        self.di, s, lf, gf, vf, ex, im, end = self.sec['__data__']
        self.base = s
        self.local, self.glob, self.objects = {}, {}, []
        for o in range(s + lf, s + gf - 7, 8):
            a, b = struct.unpack_from('<2i', d, o)
            if a != -1:
                self.local[a] = b
        for o in range(s + gf, s + vf - 11, 12):
            a, sec, b = struct.unpack_from('<3i', d, o)
            if a != -1:
                self.glob[a] = b
        for o in range(s + vf, s + ex - 11, 12):
            a, sec, b = struct.unpack_from('<3i', d, o)
            if a != -1:
                self.objects.append((a, self.cls.get(b, '?')))

    # data-section relative readers
    def u(self, fmt, o):
        return struct.unpack_from('<' + fmt, self.d, self.base + o)

    def ptr(self, o):
        return self.local.get(o, self.glob.get(o))

    def arr(self, o):
        """hkArray at o -> (data offset or None, size)."""
        return self.ptr(o), self.u('i', o + 8)[0]

    def cstr(self, o):
        p = self.ptr(o)
        if p is None:
            return ''
        e = self.d.index(b'\0', self.base + p)
        return self.d[self.base + p:e].decode('latin-1')

    def of(self, cls):
        return [a for a, c in self.objects if c == cls]


class Skeleton:
    def __init__(self, pf, at=None):
        o = at if at is not None else pf.of('hkaSkeleton')[0]
        self.name = pf.cstr(o + 16)
        pp, n = pf.arr(o + 24)
        self.parents = list(pf.u('%dh' % n, pp)) if n else []
        bp, nb = pf.arr(o + 40)
        self.names = [pf.cstr(bp + 16 * i) for i in range(nb)]
        rp, nr = pf.arr(o + 56)
        ref = np.frombuffer(pf.d, '<f4', nr * 12, pf.base + rp).reshape(nr, 3, 4)
        self.t = ref[:, 0, :3].astype(np.float64)
        self.q = ref[:, 1, :].astype(np.float64)      # x y z w
        self.s = ref[:, 2, :3].astype(np.float64)


# ------------------------------------------------------------------ spline decompression
def _align(p, a):
    return (p + a - 1) // a * a


ROT_SIZE = {0: 4, 1: 5, 2: 6, 3: 3, 4: 2, 5: 16}
ROT_ALIGN = {0: 4, 1: 1, 2: 2, 3: 1, 4: 2, 5: 4}


def _q40(b):
    v = int.from_bytes(b, 'little')
    mask = (1 << 12) - 1
    half = mask >> 1
    frac = 0.000345436
    a = [((v >> (12 * i)) & mask) - half for i in range(3)]
    a = [x * frac for x in a]
    shift = (v >> 36) & 3
    neg = (v >> 38) & 1
    w = math.sqrt(max(0.0, 1.0 - a[0] * a[0] - a[1] * a[1] - a[2] * a[2]))
    if neg:
        w = -w
    a.insert(shift, w)
    return a


def _q48(b):
    x, y, z = struct.unpack('<3H', b)
    mask = (1 << 15) - 1
    half = mask >> 1
    frac = 0.000043161
    shift = ((y >> 14) & 2) | ((x >> 15) & 1)
    neg = (z >> 15) & 1
    a = [((c & mask) - half) * frac for c in (x, y, z)]
    w = math.sqrt(max(0.0, 1.0 - a[0] * a[0] - a[1] * a[1] - a[2] * a[2]))
    if neg:
        w = -w
    a.insert(shift, w)
    return a


def _q32(b):
    # POLAR32 (rare in Skyrim): kept for completeness
    c = struct.unpack('<I', b)[0]
    R_MASK = (1 << 10) - 1
    R_FRAC = 1.0 / R_MASK
    PHI_FRAC = math.pi / 4 / 511.0
    phi_theta = c & ((1 << 18) - 1)
    r = ((c >> 18) & R_MASK) * R_FRAC
    r = 1.0 - r * r
    phi = int(math.floor(math.sqrt(phi_theta)))
    theta = 0.0
    if phi > 0:
        theta = (math.pi / 4) * (phi_theta - phi * phi) / phi
        phi = PHI_FRAC * phi
    else:
        phi = 0.0
    m = math.sqrt(max(0.0, 1.0 - r * r))
    x = math.sin(phi) * math.cos(theta) * m
    y = math.sin(phi) * math.sin(theta) * m
    z = math.cos(phi) * m
    q = [x, y, z, r]
    for i, bit in enumerate((28, 29, 30, 31)):
        if c & (1 << bit):
            q[i] = -q[i]
    return q


def _rot(b, qt):
    if qt == 1:
        return _q40(b)
    if qt == 2:
        return _q48(b)
    if qt == 5:
        return list(struct.unpack('<4f', b))
    if qt == 0:
        return _q32(b)
    raise NotImplementedError('rotation quantization %d' % qt)


def _span(deg, t, n, knots):
    if t >= knots[n + 1]:
        return n
    lo, hi = deg, n + 1
    mid = (lo + hi) // 2
    while t < knots[mid] or t >= knots[mid + 1]:
        if t < knots[mid]:
            hi = mid
        else:
            lo = mid
        mid = (lo + hi) // 2
    return mid


def _eval(t, deg, knots, cps):
    """B-spline (Havok's: knots in frames, n = number of control points - 1)."""
    n = len(cps) - 1
    s = _span(deg, t, n, knots)
    N = [1.0] + [0.0] * deg
    left = [0.0] * (deg + 1)
    right = [0.0] * (deg + 1)
    for j in range(1, deg + 1):
        left[j] = t - knots[s + 1 - j]
        right[j] = knots[s + j] - t
        saved = 0.0
        for r in range(j):
            den = right[r + 1] + left[j - r]
            tmp = N[r] / den if den else 0.0
            N[r] = saved + right[r + 1] * tmp
            saved = left[j - r] * tmp
        N[j] = saved
    out = np.zeros(len(cps[0]))
    for i in range(deg + 1):
        out += N[i] * np.asarray(cps[s - deg + i])
    return out


class _Reader:
    def __init__(self, d, p):
        self.d, self.p = d, p

    def take(self, n):
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def f(self):
        return struct.unpack('<f', self.take(4))[0]

    def align(self, a):
        self.p = _align(self.p, a)


def _vec_track(r, qbits, types, default):
    """Translation or scale of one track in one block -> callable(frame) -> (3,)."""
    spline = [(types >> (4 + i)) & 1 for i in range(3)]
    static = [(types >> i) & 1 for i in range(3)]
    if any(spline):
        n = struct.unpack('<H', r.take(2))[0]
        deg = r.take(1)[0]
        knots = list(r.take(n + deg + 2))
        r.align(4)
        lo, hi, fixed = [0.0] * 3, [0.0] * 3, list(default)
        for i in range(3):
            if spline[i]:
                lo[i], hi[i] = r.f(), r.f()
            elif static[i]:
                fixed[i] = r.f()
        bits = 8 if (qbits & 3) == 0 else 16
        cps = []
        for _ in range(n + 1):
            c = list(fixed)
            for i in range(3):
                if spline[i]:
                    if bits == 8:
                        v = r.take(1)[0] / 255.0
                    else:
                        v = struct.unpack('<H', r.take(2))[0] / 65535.0
                    c[i] = lo[i] + v * (hi[i] - lo[i])
            cps.append(c)
        r.align(4)
        return lambda t: _eval(t, deg, knots, cps)
    v = list(default)
    for i in range(3):
        if static[i]:
            v[i] = r.f()
    r.align(4)
    v = np.array(v)
    return lambda t: v


def _rot_track(r, qt, types):
    sz, al = ROT_SIZE[qt], ROT_ALIGN[qt]
    if types & 0xF0:
        n = struct.unpack('<H', r.take(2))[0]
        deg = r.take(1)[0]
        knots = list(r.take(n + deg + 2))
        r.align(al)
        cps = [_rot(r.take(sz), qt) for _ in range(n + 1)]
        # keep neighbours on one hemisphere so the blend runs the short way
        for i in range(1, len(cps)):
            if np.dot(cps[i], cps[i - 1]) < 0:
                cps[i] = [-x for x in cps[i]]
        r.align(4)

        def f(t):
            q = _eval(t, deg, knots, cps)
            return q / max(np.linalg.norm(q), 1e-12)
        return f
    if types & 0x0F:
        r.align(al)
        q = np.array(_rot(r.take(sz), qt))
        r.align(4)
        return lambda t: q
    r.align(4)
    q = np.array([0.0, 0.0, 0.0, 1.0])
    return lambda t: q


class Animation:
    """Sampled clip: t (frames, tracks, 3), q (frames, tracks, 4) xyzw, s; track -> bone."""

    def __init__(self, pf, at=None):
        o = at if at is not None else pf.of('hkaSplineCompressedAnimation')[0]
        self.duration = pf.u('f', o + 20)[0]
        self.ntracks = pf.u('i', o + 24)[0]
        (self.nframes, self.nblocks, self.fpb, self.mqsize, self.block_dur, self.block_inv,
         self.frame_dur) = pf.u('4i3f', o + 56)
        bo, nbo = pf.arr(o + 88)
        dp, nd = pf.arr(o + 152)
        self.block_offsets = list(pf.u('%dI' % nbo, bo))
        data = pf.d[pf.base + dp:pf.base + dp + nd]
        b = pf.of('hkaAnimationBinding')
        self.bones = list(range(self.ntracks))
        self.skeleton_name = ''
        if b:
            bb = b[0]
            self.skeleton_name = pf.cstr(bb + 16)
            tp, nt = pf.arr(bb + 32)
            if nt:
                self.bones = list(pf.u('%dh' % nt, tp))
        self.annotations = []
        ap, na = pf.arr(o + 40)
        for i in range(na):     # hkaAnnotationTrack: name ptr, hkArray<hkaAnnotation{float, ptr}>
            tp, nt = pf.arr(ap + 24 * i + 8)
            for k in range(nt):
                tm = pf.u('f', tp + 16 * k)[0]
                txt = pf.cstr(tp + 16 * k + 8)
                if txt:
                    self.annotations.append((tm, txt))
        self._blocks = []
        for bi in range(self.nblocks):
            r = _Reader(data, self.block_offsets[bi])
            masks = [tuple(r.take(4)) for _ in range(self.ntracks)]
            r.p = self.block_offsets[bi] + self.mqsize    # float track masks (1 byte each) sit in it too
            tracks = []
            for qtz, pt, rt, st in masks:
                fp = _vec_track(r, qtz & 3, pt, (0.0, 0.0, 0.0))
                fr = _rot_track(r, (qtz >> 2) & 0xF, rt)
                fs = _vec_track(r, (qtz >> 6) & 3, st, (1.0, 1.0, 1.0))
                tracks.append((fp, fr, fs))
            self._blocks.append(tracks)

    def sample(self, frame):
        """Frame (float, 0..nframes-1) -> t (tracks,3), q (tracks,4)."""
        frame = min(max(frame, 0.0), self.nframes - 1)
        per = self.fpb - 1
        bi = min(int(frame // per), self.nblocks - 1)
        lt = frame - bi * per
        T = np.zeros((self.ntracks, 3))
        Q = np.zeros((self.ntracks, 4))
        for i, (fp, fr, fs) in enumerate(self._blocks[bi]):
            T[i] = fp(lt)
            Q[i] = fr(lt)
        return T, Q

    @property
    def fps(self):
        return 1.0 / self.frame_dur if self.frame_dur else 30.0


def load(path_or_bytes):
    d = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, 'rb').read()
    return Packfile(bytes(d))


if __name__ == '__main__':
    pf = load(sys.argv[1])
    for a, c in pf.objects:
        print('%6d %s' % (a, c))
    if pf.of('hkaSkeleton'):
        sk = Skeleton(pf)
        print('skeleton', sk.name, len(sk.names))
        for i, n in enumerate(sk.names[:8]):
            print(i, n, sk.parents[i], np.round(sk.t[i], 2), np.round(sk.q[i], 3))
    if pf.of('hkaSplineCompressedAnimation'):
        an = Animation(pf)
        print('anim tracks', an.ntracks, 'frames', an.nframes, 'blocks', an.nblocks, 'fpb', an.fpb,
              'dur', an.duration, 'fps', an.fps, 'skel', an.skeleton_name, 'bones', an.bones[:6])
        print('annotations', an.annotations[:10])
        T, Q = an.sample(0)
        for i in range(6):
            print(i, np.round(T[i], 2), np.round(Q[i], 3))
