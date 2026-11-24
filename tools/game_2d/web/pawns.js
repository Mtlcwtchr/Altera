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
  const BODY = { norm: [5, 5.2], thin: [4.2, 5.2], fat: [6.4, 5.4], fem: [4.6, 5.2] };
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

  function pawn(spec, d, bob, child) {
    const c = new Canvas(), sk = SKIN[spec.skin || 'light'] || SKIN.light;
    let hcx, hcy, hr, bcx, bcy, brx, bry;
    if (child) { hcx = 10; hcy = 9.5; hr = 3.7; bcx = 10; bcy = 15.6; brx = 3.8; bry = 3.9; }
    else { hcx = 10; hcy = 6.6; hr = 4.3; bcx = 10; bcy = 14; [brx, bry] = BODY[spec.body || 'norm'] || BODY.norm; }
    if (d === 'E') { brx *= 0.82; hcx += 0.7; }
    hcy -= bob; bcy -= bob;
    const body = ell(bcx, bcy, brx, bry);
    const cloak = spec.cloak;
    if (cloak && (d === 'S' || d === 'E')) c.layer(ell(bcx - (d === 'E' ? 0.6 : 0), bcy - 0.3, brx + 1.2, bry + 0.6), cloak);
    c.layer(body, sk);
    const robeMask = () => union(body, ell(bcx, bcy + 1.5, brx + 0.8, bry - 0.4));
    for (const [kind, col] of spec.wear || []) {
      if (kind === 'shirt') c.layer(body, col);
      if (kind === 'pants') c.layer(filter(body, (x, y) => y >= bcy + 2.2), col);
      if (kind === 'robe') c.layer(robeMask(), col);
      if (kind === 'mail') c.layer(body, col, (x, y, cc) => ((x + y) % 2 ? mul(cc, 0.8) : null));
      if (kind === 'belt') c.layer(filter(robeMask(), (x, y) => Math.trunc(bcy) <= y && y <= Math.trunc(bcy)), col, null, false);
      if (kind === 'apron' && d !== 'N') {
        const fx = d === 'S' ? x => Math.abs(x + 0.5 - bcx) <= 2.6 : x => x + 0.5 >= bcx + brx - 2.4;
        c.layer(filter(body, (x, y) => fx(x) && y >= bcy - 2), col);
      }
      if (kind === 'tabard' && d !== 'N') {
        const fx = d === 'S' ? x => Math.abs(x + 0.5 - bcx) <= 2.2 : x => x + 0.5 >= bcx + brx - 2;
        c.layer(filter(body, x => fx(x)), col);
      }
    }
    if (cloak && d === 'N') c.layer(ell(bcx, bcy - 0.3, brx + 1.2, bry + 0.6), cloak);
    if (cloak && d === 'S') { c.px(Math.trunc(bcx) - 1, Math.trunc(bcy - bry) + 1, '#e8c040'); c.px(Math.trunc(bcx), Math.trunc(bcy - bry) + 1, '#e8c040'); }
    // hands
    const hy = Math.trunc(bcy + 1);
    if (d === 'S' || d === 'N') for (const hx_ of [Math.trunc(bcx - brx) - 1, Math.trunc(bcx + brx)]) c.layer(new Set([key(hx_, hy)]), sk);
    else c.layer(new Set([key(Math.trunc(bcx + brx) - 1, hy)]), sk);
    // tool
    const tool = spec.tool;
    let tx = Math.trunc(bcx + brx) + (d !== 'E' ? 1 : 0); const ty = hy;
    if (d === 'N') tx = Math.trunc(bcx - brx) - 2;
    const S = pts => new Set(pts.map(([x, y]) => key(x, y)));
    if (tool === 'spear') {
      const pts = []; for (let y = 0; y < ty + 4; y++) pts.push([tx, y]);
      c.layer(S(pts), '#7a4a28'); c.layer(S([[tx, 0], [tx, 1]]), '#c8ccd0', null, false);
    }
    if (tool === 'hammer') {
      c.layer(S([[tx, ty - 1], [tx, ty], [tx, ty + 1]]), '#7a4a28');
      c.layer(S([[tx - 1, ty - 3], [tx, ty - 3], [tx + 1, ty - 3], [tx - 1, ty - 2], [tx, ty - 2], [tx + 1, ty - 2]]), '#8a8e94');
    }
    if (tool === 'staff') {
      const pts = []; for (let y = Math.trunc(hcy) - 2; y < N - 1; y++) pts.push([tx, y]);
      c.layer(S(pts), '#6a4a28'); c.layer(S([[tx, Math.trunc(hcy) - 3]]), '#c8a848');
    }
    if (tool === 'bread') c.layer(S([[tx - 1, ty - 1], [tx, ty - 1], [tx + 1, ty - 1], [tx - 1, ty], [tx, ty], [tx + 1, ty]]), '#d89848');
    if (tool === 'sickle') { c.layer(S([[tx, ty], [tx, ty + 1]]), '#7a4a28'); c.layer(S([[tx, ty - 1], [tx + 1, ty - 2], [tx + 1, ty - 3], [tx, ty - 4]]), '#c8ccd0'); }
    // head
    const head = ell(hcx, hcy, d !== 'E' ? hr : hr * 0.95, hr);
    const [hair, hc] = spec.hair || ['short', '#5a3a20'];
    if (hair === 'long' && d !== 'N')
      c.layer(filter(ell(hcx - (d === 'E' ? 0.5 : 0), hcy + 1.4, hr + 0.9, hr + 1.6), (x, y) => y >= hcy - 1 && (d === 'E' || Math.abs(x + 0.5 - hcx) >= hr - 1.2)), hc);
    c.layer(head, sk);
    const top = Math.trunc(hcy - hr), faceY = Math.trunc(hcy + 0.3);
    if (d === 'S') { c.px(Math.trunc(hcx) - 2, faceY, OUT); c.px(Math.trunc(hcx) + 1, faceY, OUT); }
    else if (d === 'E') c.px(Math.trunc(hcx) + 2, faceY, OUT);
    if (spec.beard && d !== 'N') c.layer(filter(head, (x, y) => y >= hcy + 1.2 && (d === 'S' || x + 0.5 >= hcx - 0.5)), hc, null, false);
    if (['short', 'long', 'bun', 'braids'].includes(hair)) {
      let hm;
      if (d === 'S') hm = filter(head, (x, y) => y + 0.5 <= hcy - 1.1);
      else if (d === 'E') hm = filter(head, (x, y) => y + 0.5 <= hcy - 1.1 || (x + 0.5 <= hcx - 1.2 && y + 0.5 <= hcy + 2));
      else hm = union(filter(head, (x, y) => y + 0.5 <= hcy + 2.6), hair !== 'short' ? head : new Set());
      if (hair === 'long' && d === 'N') hm = union(hm, ell(hcx, hcy + 1.4, hr + 0.9, hr + 1.6));
      if (hair === 'bun') hm = union(hm, ell(hcx, top - 0.2, 1.8, 1.6));
      if (hair === 'braids' && d !== 'E') hm = union(hm, ell(hcx - hr - 0.6, hcy + 1.5, 1.2, 1.6), ell(hcx + hr + 0.6, hcy + 1.5, 1.2, 1.6));
      c.layer(hm, hc);
    }
    const [hat, hcol] = spec.hat || [null, null];
    if (hat === 'hood') {
      let m = union(ell(hcx, hcy, hr + 1, hr + 0.9), ell(hcx, hcy + 3.5, hr + 1.2, 2.2));
      if (d === 'S') m = minus(m, filter(head, (x, y) => y + 0.5 >= hcy - 1.3 && Math.abs(x + 0.5 - hcx) <= 2.8));
      if (d === 'E') m = minus(m, filter(head, (x, y) => y + 0.5 >= hcy - 1.3 && x + 0.5 >= hcx + 0.2));
      c.layer(m, hcol);
    }
    if (hat === 'helmet') {
      c.layer(filter(ell(hcx, hcy, hr + 0.6, hr + 0.6), (x, y) => y + 0.5 <= hcy - 0.6), hcol);
      if (d === 'S') c.layer(new Set([key(Math.trunc(hcx), Math.trunc(hcy))]), hcol, null, false);
    }
    if (hat === 'straw') {
      c.layer(ell(hcx, top + 2, hr + 2.6, 1.8), hcol, (x, y, cc) => ((x + y) % 3 === 0 ? mul(cc, 0.85) : null));
      c.layer(ell(hcx, top + 0.6, hr - 1.2, 1.7), hcol);
    }
    if (hat === 'kerchief') c.layer(filter(ell(hcx, hcy, hr + 0.4, hr + 0.4), (x, y) => y + 0.5 <= hcy - 0.8 || d === 'N'), hcol);
    if (hat === 'cap') c.layer(filter(ell(hcx, hcy - 0.6, hr + 0.3, hr), (x, y) => y + 0.5 <= hcy - 1.4), hcol);
    return c.g;
  }

  // A pawn as a 20x20 canvas: side S | E | N | W, bob 0 | 1. Cached by spec.
  const cache = new Map();
  function sprite(spec, d, bob) {
    const k = JSON.stringify(spec) + d + bob;
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
  return { sprite, SIZE: N };
})();
