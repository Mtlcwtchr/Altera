// AL-95: layered pawns, 20x20 (gdd/ui/client_2d/CLIENT_2D.md §3), a port of the
// design's generator (gdd/ui/client_2d/mockups/src/pawns.py). A pawn is a body
// "bob" without arms or legs, a round head and layers over it: cloak behind ->
// body -> clothes (shirt, pants, robe, mail, belt, apron, tabard) -> hand dots ->
// tool -> head -> beard -> hair -> headwear. Sides S, E, N are drawn; W mirrors E.
// spec: {body: norm|thin|fat|fem, skin: light|mid|dark, child, hair: [kind, colour],
//        beard, wear: [[kind, colour]...], cloak: colour, hat: [kind, colour], tool}
'use strict';
const Pawns = (() => {
  const N = 20, OUT = '#1a1410';
  const SKIN = { light: '#eab48a', mid: '#c88a5c', dark: '#8a5a3a' };
  const key = (x, y) => y * N + x;
  function ell(cx, cy, rx, ry) {
    const s = new Set();
    for (let x = 0; x < N; x++) for (let y = 0; y < N; y++) {
      const a = (x + 0.5 - cx) / rx, b = (y + 0.5 - cy) / ry;
      if (a * a + b * b <= 1) s.add(key(x, y));
    }
    return s;
  }
  const xs = k => k % N, ys = k => Math.floor(k / N);
  const filter = (s, f) => { const o = new Set(); for (const k of s) if (f(xs(k), ys(k))) o.add(k); return o; };
  const union = (...ss) => { const o = new Set(); for (const s of ss) for (const k of s) o.add(k); return o; };
  const minus = (a, b) => { const o = new Set(a); for (const k of b) o.delete(k); return o; };
  const hx = c => [1, 3, 5].map(i => parseInt(c.substr(i, 2), 16));
  const tohex = t => '#' + t.map(v => Math.max(0, Math.min(255, Math.trunc(v))).toString(16).padStart(2, '0')).join('');
  const mul = (c, k) => tohex(hx(c).map(v => v * k));
  const mix = (c, d, k) => { const a = hx(c), b = hx(d); return tohex(a.map((v, i) => v * (1 - k) + b[i] * k)); };

  class Canvas {
    constructor() { this.g = new Array(N * N).fill(null); }
    layer(mask, col, pattern, outline = true) {
      const m = filter(mask, (x, y) => x >= 0 && x < N && y >= 0 && y < N);
      if (outline) {
        const ring = new Set();
        for (const k of m) {
          const x = xs(k), y = ys(k);
          for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
            const qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
            const q = key(qx, qy);
            if (!m.has(q)) ring.add(q);
          }
        }
        for (const q of ring) { const u = this.g[q]; this.g[q] = (u === null || u === OUT) ? OUT : mul(u, 0.55); }
      }
      for (const k of m) {
        const x = xs(k), y = ys(k);
        let c = col;
        if (pattern) c = pattern(x, y, col) || col;
        if (!m.has(key(x + 1, y + 1)) || x + 1 >= N || y + 1 >= N) c = mul(c, 0.78);
        else if (!m.has(key(x - 1, y - 1)) || x === 0 || y === 0) c = mix(c, '#ffffff', 0.18);
        this.g[k] = c;
      }
    }
    px(x, y, c) { if (x >= 0 && x < N && y >= 0 && y < N) this.g[key(x, y)] = c; }
  }

  // --- LR-139: the parts of a pawn as data (content/sprites_2d/pawns2d.json; pixels2d.cpp reads the same) ---
  let PARTS = {};
  const VARS = ['x', 'y', 'hcx', 'hcy', 'hrx', 'hry', 'top', 'fx', 'fy', 'ey', 'hl', 'hrr', 'eb', 'nx', 'bcx', 'bcy', 'brx', 'bry', 'bx', 'by', 'step', 'swing', 'child', 'tx', 'ty', 'hy', 'sw', 'footy'];
  const exprCache = new Map();
  function compiled(t) {
    let f = exprCache.get(t);
    if (f) return f;
    // the C++ side's little language: numbers, the variables, + - * / % comparisons && || ! abs min max trunc floor
    const ok = /^[0-9a-z_+\-*/%().<>=!&|, ]*$/.test(t) && (t.match(/[a-z_]+/g) || []).every(w => VARS.includes(w) || ['abs', 'min', 'max', 'trunc', 'floor'].includes(w));
    if (!ok) { console.warn('pawns2d.json: bad expression', t); f = () => 0; }
    else {
      // (the data takes % of x, y and small sums only, never negative: JS % matches the C++ side)
      const g = new Function(...VARS, 'abs', 'min', 'max', 'trunc', 'floor', 'return (' + t + ');');
      f = v => +g(...VARS.map(n => v[n]), Math.abs, Math.min, Math.max, Math.trunc, Math.floor);
    }
    exprCache.set(t, f);
    return f;
  }
  const num = (j, V) => (typeof j === 'number' ? j : typeof j === 'string' ? compiled(j)(V) : 0);
  function vars(d, hcx, hcy, hrx, hry, bcx, bcy, brx, bry, pose, child) {
    const T = Math.trunc;
    return { side: d, x: 0, y: 0, hcx, hcy, hrx, hry, top: T(hcy - hry), fx: T(hcx), fy: T(hcy + 0.3), ey: T(hcy), hl: T(hcx - hrx), hrr: T(hcx + hrx),
      eb: T(hcx - 0.6), nx: T(hcx + hrx * 0.95), bcx, bcy, brx, bry, bx: T(bcx), by: T(bcy), step: pose.step ?? -1, swing: pose.swing || 0, child: child ? 1 : 0,
      // the hands swing in turn walking; at work the tool hand goes up and down; the tool sits by it
      hy: T(bcy + 1) - 2 * (pose.swing || 0), sw: pose.step === 1 ? 1 : pose.step === 3 ? -1 : 0,
      tx: d === 'N' ? T(bcx - brx) - 2 : T(bcx + brx) + (d !== 'E' ? 1 : 0), ty: T(bcy + 1) - 2 * (pose.swing || 0), footy: Math.min(N - 1, T(bcy + bry - 0.2)) };
  }
  function colours(spec, sk, blink) {
    const C = Object.assign({}, PARTS.colours || {});
    C.skin = sk;
    C.hair = (spec.hair || [])[1] || '#5a3a20';
    C.eye = blink ? mul(sk, 0.6) : (spec.eyes || C.outline || OUT);
    if (spec.hat) C.hat = spec.hat[1];
    return C;
  }
  function colourOf(sp, C, own) {
    const t = (sp || '').replace(/ /g, '');
    if (!t) return own;
    if (t[0] === '#') return t;
    const m = /^mix\(([^,]+),([^,]+),([\d.]+)\)$/.exec(t);
    if (m) return mix(colourOf(m[1], C, own), colourOf(m[2], C, own), +m[3]);
    const st = t.indexOf('*');
    if (st >= 0) return mul(colourOf(t.slice(0, st), C, own), +t.slice(st + 1));
    if (t === 'c') return own;
    return C[t] || '#ff00ff';
  }
  const onSide = (j, d) => (j.sides || 'SEN').includes(d);
  function shapeMask(sh, d, V, head, body) {
    let m = new Set();
    if (typeof sh === 'string') {
      const def = sh[0] === '@' && (PARTS.shapes || {})[sh.slice(1)];
      if (def) for (const x of Array.isArray(def) ? def : [def]) m = union(m, shapeMask(x, d, V, head, body));
      return m;
    }
    if (!sh || !onSide(sh, d)) return m;
    if (sh.when !== undefined && !num(sh.when, V)) return m;
    if (sh.ell) m = ell(num(sh.ell[0], V), num(sh.ell[1], V), num(sh.ell[2], V), num(sh.ell[3], V));
    else if (sh.head) m = head;
    else if (sh.body) m = body;
    else if (sh.rect) { const [x0, y0, x1, y1] = sh.rect.map(v => Math.floor(num(v, V))); for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (x >= 0 && y >= 0 && x < N && y < N) m.add(key(x, y)); }
    else if (sh.col) { const [x, y0, y1] = sh.col.map(v => Math.floor(num(v, V))); for (let y = y0; y <= y1; y++) if (x >= 0 && y >= 0 && x < N && y < N) m.add(key(x, y)); }
    else if (sh.px) for (const [a, b] of sh.px) { const x = Math.floor(num(a, V)), y = Math.floor(num(b, V)); if (x >= 0 && y >= 0 && x < N && y < N) m.add(key(x, y)); }
    if (sh.where) { const w = compiled(sh.where); m = filter(m, (x, y) => { V.x = x; V.y = y; return w(V); }); }
    if (sh.minus) m = minus(m, shapeMask(sh.minus, d, V, head, body));
    return m;
  }
  function layerPatterned(c, m, col, pattern, V, C, outline = true) {
    if (!pattern || !pattern.if) { c.layer(m, col, null, outline); return; }
    const w = compiled(pattern.if), cs = pattern.colour || 'c*0.78';
    c.layer(m, col, (x, y, cc) => { V.x = x; V.y = y; return w(V) ? colourOf(cs, C, cc) : null; }, outline);
  }
  function drawOps(c, ops, d, V, head, body, C) {
    for (const op of ops || []) {
      if (!op || !onSide(op, d)) continue;
      let m;
      if (op.shapes) {
        m = new Set();
        for (const sh of op.shapes) m = union(m, shapeMask(sh, d, V, head, body));
        if (op.where) { const w = compiled(op.where); m = filter(m, (x, y) => { V.x = x; V.y = y; return w(V); }); }
      } else m = shapeMask(op, d, V, head, body);
      if (!m.size) continue;
      const col = colourOf(op.colour || 'skin', C, null);
      if (op.flat) { for (const k of m) c.g[k] = col; continue; }
      layerPatterned(c, m, col, op.pattern, V, C, op.outline !== false);
    }
  }
  const skinPattern = (spec, where) => (spec.skin_pattern && ((PARTS.skin_patterns || {})[spec.skin_pattern] || {})[where]) || null;
  function partOps(group, name, slot) {
    const e = name && (PARTS[group] || {})[name];
    if (!e) return [];
    return slot ? (Array.isArray(e) ? [] : e[slot] || []) : e;
  }
  function drawHead(c, spec, d, pose, V, body, sk) {
    const C = colours(spec, sk, pose.blink);
    const head = ell(V.hcx, V.hcy, d !== 'E' ? V.hrx : V.hrx * 0.95, V.hry);
    let ears = new Set();
    for (const op of partOps('ears', spec.ears)) if (op && onSide(op, d)) ears = union(ears, shapeMask(op, d, V, new Set(), body));
    if (ears.size) c.layer(ears, sk);
    layerPatterned(c, head, sk, skinPattern(spec, 'head'), V, C);
    drawOps(c, PARTS.face, d, V, head, body, C);
    drawOps(c, partOps('brows', spec.brow), d, V, head, body, C);
    drawOps(c, partOps('noses', spec.nose), d, V, head, body, C);
    if (spec.beard) drawOps(c, partOps('beards', spec.beard_style || (spec.long_beard ? 'long' : 'full')), d, V, head, body, C);
    drawOps(c, partOps('tusks', spec.tusks), d, V, head, body, C);
    const hair = (spec.hair || [])[0] || 'short';
    drawOps(c, partOps('hair', hair, 'front'), d, V, head, body, C);
    if (ears.size && !(PARTS.bald || []).includes(hair)) c.layer(minus(ears, head), sk, null, false);   // ears through the hair
    if (spec.hat) drawOps(c, partOps('hats', spec.hat[0]), d, V, head, body, C);
  }

  // LR-139: pose {bob, step, swing, dip, blink} (pixels2d.cpp pawnPose); a number is the bob alone
  function pawn(spec, d, pose, child) {
    if (typeof pose === 'number') pose = { bob: pose };
    const bob = pose.bob || 0, step = pose.step === undefined ? -1 : pose.step;
    const c = new Canvas(), sk = (spec.skin || '').startsWith('#') ? spec.skin : ((PARTS.skins || {})[spec.skin || 'light'] || (PARTS.skins || {}).light || SKIN.light);
    // the frame of the pawn (pawns2d.json frame, bodies, builds, heads)
    const fr = ((PARTS.frame || {})[child ? 'child' : 'adult']) || {};
    let hcx = fr.hcx ?? 10, hcy = fr.hcy ?? (child ? 9.5 : 6.6), hr = fr.hr ?? (child ? 3.7 : 4.3);
    let bcx = fr.bcx ?? 10, bcy = fr.bcy ?? (child ? 15.6 : 14), brx = fr.brx ?? (child ? 3.8 : 5), bry = fr.bry ?? (child ? 3.9 : 5.2), hcxE = 0;
    if (!child) {
      const bd = (PARTS.bodies || {})[spec.body || 'norm'];
      if (bd) [brx, bry] = bd;
      const b = spec.build && (PARTS.builds || {})[spec.build];   // LR-139: the kind's build
      if (b) { brx *= b.brx ?? 1; bry *= b.bry ?? 1; bcy += b.bcy || 0; hcy += b.hcy || 0; hcxE = b.hcx_e || 0; }
    }
    let hrx = hr, hry = hr;   // LR-139: the shape of the head
    const hs = spec.head && (PARTS.heads || {})[spec.head];
    if (hs) { hrx = hr * hs[0]; hry = hr * hs[1]; }
    if (d === 'E') { brx *= 0.82; hcx += 0.7 + hcxE; }
    hcy -= bob; bcy -= bob;
    hcy += pose.dip || 0;
    const body = ell(bcx, bcy, brx, bry);
    const V = vars(d, hcx, hcy, hrx, hry, bcx, bcy, brx, bry, pose, child);
    const C = colours(spec, sk, false);
    if (spec.cloak) C.cloak = spec.cloak;
    drawOps(c, partOps('hair', (spec.hair || [])[0], 'back'), d, V, new Set(), body, C);   // the back of the hair, behind the body
    if (spec.cloak) drawOps(c, partOps('cloak', 'back'), d, V, new Set(), body, C);
    layerPatterned(c, body, sk, skinPattern(spec, 'body'), V, C);
    // feet (bare, or in the boots' colour), the clothes layer by layer, the cloak's front, the hands, the tool
    C.feet = colourOf((PARTS.colours || {}).feet || 'skin*0.8', C, sk);
    for (const [kind, col] of spec.wear || []) if (kind === 'boots') C.feet = col;
    drawOps(c, PARTS.feet, d, V, new Set(), body, C);
    for (const [kind, col] of spec.wear || []) { C.wear = col; drawOps(c, partOps('wear', kind), d, V, new Set(), body, C); }
    if (spec.cloak) drawOps(c, partOps('cloak', 'front'), d, V, new Set(), body, C);
    drawOps(c, PARTS.hands, d, V, new Set(), body, C);
    drawOps(c, partOps('tools', spec.tool), d, V, new Set(), body, C);
    drawHead(c, spec, d, pose, V, body, sk);
    return c.g;
  }

  // A pawn as a 20x20 canvas: side S | E | N | W, bob 0 | 1. Cached by spec.
  const cache = new Map();
  function sprite(spec, d, bob) {
    const k = JSON.stringify(spec) + d + JSON.stringify(bob);
    let cv = cache.get(k);
    if (cv) return cv;
    const g = pawn(spec, d === 'W' ? 'E' : d, bob, !!spec.child);
    cv = document.createElement('canvas');
    cv.width = cv.height = N;
    const ctx = cv.getContext('2d');
    for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
      const col = g[key(d === 'W' ? N - 1 - x : x, y)];
      if (col) { ctx.fillStyle = col; ctx.fillRect(x, y, 1, 1); }
    }
    if (cache.size > 4000) cache.clear();
    cache.set(k, cv);
    return cv;
  }
  // LR-139: the frame now: walking by metres walked, working by the clock, idle a breath and a blink
  function hash(n, salt) {
    let h = (Math.imul(n >>> 0, 2654435761) + Math.imul(salt, 40503)) >>> 0;
    h ^= h >>> 15; h = Math.imul(h, 2246822519) >>> 0; h ^= h >>> 13;
    return h >>> 0;
  }
  function pose(moving, walked, working, ms, id) {
    const phase = (hash(Math.max(0, id | 0), 7) % 1000) / 1000;
    if (moving) { const st = Math.floor(walked * 3) & 3; return { bob: st & 1, step: st }; }
    const s = ms / 1000 + phase * 4;
    if (working) { const f = Math.floor(s / 0.3) & 1; return { bob: f, swing: f }; }
    return { dip: (s % 2.6) < 0.9 ? 1 : 0, blink: ((s + phase * 3) % 3.7) < 0.14 };
  }
  function setParts(j) { PARTS = j || {}; exprCache.clear(); cache.clear(); }
  return { sprite, pose, setParts, SIZE: N };
})();
