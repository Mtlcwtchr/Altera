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

  // LR-139: pose {bob, step, swing, dip, blink} (pixels2d.cpp pawnPose); a number is the bob alone
  function pawn(spec, d, pose, child) {
    if (typeof pose === 'number') pose = { bob: pose };
    const bob = pose.bob || 0, step = pose.step === undefined ? -1 : pose.step;
    const c = new Canvas(), sk = (spec.skin || '').startsWith('#') ? spec.skin : (SKIN[spec.skin || 'light'] || SKIN.light);
    let hcx, hcy, hr, bcx, bcy, brx, bry;
    if (child) { hcx = 10; hcy = 9.5; hr = 3.7; bcx = 10; bcy = 15.6; brx = 3.8; bry = 3.9; }
    else {
      hcx = 10; hcy = 6.6; hr = 4.3; bcx = 10; bcy = 14; [brx, bry] = BODY[spec.body || 'norm'] || BODY.norm;
      const b = spec.build;   // LR-139: the kind's build
      if (b === 'stocky') { brx *= 1.2; bry *= 0.8; bcy += 1.2; hcy += 2.4; }
      else if (b === 'slim') { brx *= 0.86; bry *= 1.06; hcy -= 0.4; }
      else if (b === 'broad') { brx *= 1.14; hcy += 0.3; }
      else if (b === 'hulking') { brx *= 1.24; bry *= 1.02; hcy += 1.4; }
      else if (b === 'wiry') { brx *= 0.84; bry *= 0.9; bcy += 0.5; hcy += 0.6; }
    }
    let hrx = hr, hry = hr;   // LR-139: the shape of the head
    if (spec.head === 'wide') { hrx = hr * 1.14; hry = hr * 0.92; }
    else if (spec.head === 'long') { hrx = hr * 0.9; hry = hr * 1.08; }
    else if (spec.head === 'small') hrx = hry = hr * 0.8;
    else if (spec.head === 'big') hrx = hry = hr * 1.12;
    if (d === 'E') { brx *= 0.82; hcx += 0.7; }
    if (d === 'E' && spec.build === 'hulking') hcx += 0.8;
    hcy -= bob; bcy -= bob;
    hcy += pose.dip || 0;
    const body = ell(bcx, bcy, brx, bry);
    const warts = spec.skin_pattern === 'warts';
    const P0 = pts => new Set(pts.filter(([x, y]) => x >= 0 && y >= 0 && x < N && y < N).map(([x, y]) => key(x, y)));
    const cloak = spec.cloak;
    if (cloak && (d === 'S' || d === 'E')) c.layer(ell(bcx - (d === 'E' ? 0.6 : 0), bcy - 0.3, brx + 1.2, bry + 0.6), cloak);
    c.layer(body, sk, warts ? (x, y, cc) => ((x * 7 + y * 3) % 11 === 0 ? mul(cc, 0.78) : null) : null);
    { // LR-139: feet under the body, bare or shod; walking they step in turn
      let fc = mul(sk, 0.8);
      for (const [kind, col] of spec.wear || []) if (kind === 'boots') fc = col;
      const fy = Math.min(N - 1, Math.trunc(bcy + bry - 0.2)), bx = Math.trunc(bcx);
      if (d === 'E') {
        if (step === 1) c.layer(P0([[bx - 2, fy], [bx + 2, fy]]), fc, null, false);
        else if (step === 3) c.layer(P0([[bx, fy]]), fc, null, false);
        else c.layer(P0([[bx - 1, fy], [bx + 1, fy]]), fc, null, false);
      } else {
        if (step !== 1) c.layer(P0([[bx - 2, fy]]), fc, null, false);
        if (step !== 3) c.layer(P0([[bx + 1, fy]]), fc, null, false);
      }
    }
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
      // AL-109: boots at the bottom of the body, a satchel on the hip, a pack on the back
      if (kind === 'boots') c.layer(filter(body, (x, y) => y + 0.5 >= bcy + bry - 1.6), col, null, false);
      const bx = Math.trunc(bcx), by = Math.trunc(bcy), P = pts => new Set(pts.map(([x, y]) => key(x, y)));
      if (kind === 'bag') {
        if (d === 'S') {
          c.layer(P([[bx - 2, by - 4], [bx - 1, by - 3], [bx, by - 2], [bx + 1, by - 1]]), mul(col, 0.7), null, false);
          c.layer(P([[bx + 2, by], [bx + 3, by], [bx + 2, by + 1], [bx + 3, by + 1]]), col);
        } else if (d === 'E') c.layer(P([[bx - 1, by], [bx, by], [bx - 1, by + 1], [bx, by + 1]]), col);
        else c.layer(P([[bx - 3, by], [bx - 2, by], [bx - 3, by + 1], [bx - 2, by + 1]]), col);
      }
      if (kind === 'pack') {
        const m = [];
        if (d === 'N') { for (let y = by - 3; y <= by + 1; y++) for (let x = bx - 2; x <= bx + 1; x++) m.push([x, y]); c.layer(P(m), col); }
        else if (d === 'E') { const x0 = Math.trunc(bcx - brx); for (let y = by - 3; y <= by + 1; y++) m.push([x0, y], [x0 + 1, y]); c.layer(P(m), col); }
        else c.layer(P([[bx - 2, by - 4], [bx - 2, by - 3], [bx + 1, by - 4], [bx + 1, by - 3]]), mul(col, 0.7), null, false);
      }
    }
    if (cloak && d === 'N') c.layer(ell(bcx, bcy - 0.3, brx + 1.2, bry + 0.6), cloak);
    if (cloak && d === 'S') { c.px(Math.trunc(bcx) - 1, Math.trunc(bcy - bry) + 1, '#e8c040'); c.px(Math.trunc(bcx), Math.trunc(bcy - bry) + 1, '#e8c040'); }
    // hands
    const hy = Math.trunc(bcy + 1) - 2 * (pose.swing || 0), sw = step === 1 ? 1 : step === 3 ? -1 : 0;
    if (d === 'S' || d === 'N') {
      c.layer(P0([[Math.trunc(bcx - brx) - 1, Math.trunc(bcy + 1) + sw]]), sk);
      c.layer(P0([[Math.trunc(bcx + brx), hy - sw]]), sk);
    } else c.layer(P0([[Math.trunc(bcx + brx) - 1 + sw, hy]]), sk);
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
    // AL-109: the tool of the act in hand (pixels2d.cpp draws the same)
    const WOOD = '#7a4a28', STEEL = '#c8ccd0', IRON = '#8a8e94';
    const stick = (y0, y1, col) => { const p = []; for (let y = y0; y <= y1; y++) p.push([tx, y]); c.layer(S(p), col); };
    if (tool === 'axe') { stick(ty - 2, ty + 1, WOOD); c.layer(S([[tx + 1, ty - 3], [tx + 1, ty - 2], [tx + 1, ty - 1], [tx + 2, ty - 2]]), STEEL); }
    if (tool === 'knife') { c.layer(S([[tx, ty]]), WOOD); c.layer(S([[tx, ty - 1], [tx, ty - 2]]), STEEL, null, false); }
    if (tool === 'sword') { stick(ty - 7, ty - 1, STEEL); c.layer(S([[tx - 1, ty], [tx, ty], [tx + 1, ty]]), '#b89040', null, false); c.layer(S([[tx, ty + 1]]), WOOD, null, false); }
    if (tool === 'bow') {
      c.layer(S([[tx, ty - 4], [tx + 1, ty - 3], [tx + 1, ty - 2], [tx + 1, ty - 1], [tx + 1, ty], [tx + 1, ty + 1], [tx, ty + 2]]), WOOD);
      const p = []; for (let y = ty - 3; y <= ty + 1; y++) p.push([tx, y]); c.layer(S(p), '#e8e0c8', null, false);
    }
    if (tool === 'stick') stick(ty - 3, ty + 3, WOOD);
    if (tool === 'pick') { stick(ty - 3, ty + 1, WOOD); c.layer(S([[tx - 1, ty - 3], [tx, ty - 4], [tx + 1, ty - 3]]), IRON); }
    if (tool === 'spade') { stick(ty - 4, ty + 1, WOOD); c.layer(S([[tx - 1, ty + 2], [tx, ty + 2], [tx + 1, ty + 2], [tx - 1, ty + 3], [tx, ty + 3], [tx + 1, ty + 3]]), IRON); }
    if (tool === 'fork') { stick(ty - 4, ty + 3, WOOD); c.layer(S([[tx - 1, ty - 5], [tx + 1, ty - 5], [tx - 1, ty - 6], [tx + 1, ty - 6], [tx, ty - 5]]), IRON, null, false); }
    if (tool === 'rod') {
      c.layer(S([[tx, ty + 1], [tx, ty], [tx, ty - 1], [tx, ty - 2], [tx + 1, ty - 3], [tx + 1, ty - 4], [tx + 1, ty - 5], [tx + 2, ty - 6], [tx + 2, ty - 7]]), WOOD, null, false);
      c.layer(S([[tx + 3, ty - 6], [tx + 3, ty - 5], [tx + 3, ty - 4], [tx + 3, ty - 3]]), '#d8e0e8', null, false);
    }
    if (tool === 'needle') c.layer(S([[tx, ty - 1], [tx, ty - 2]]), STEEL, null, false);
    if (tool === 'spindle') { stick(ty - 2, ty + 1, WOOD); c.layer(S([[tx - 1, ty + 1], [tx + 1, ty + 1]]), '#a06a38', null, false); c.layer(S([[tx, ty - 3]]), '#e8e0d0', null, false); }
    if (tool === 'saw') { c.layer(S([[tx, ty]]), WOOD); c.layer(S([[tx, ty - 1], [tx, ty - 2], [tx, ty - 3], [tx, ty - 4], [tx + 1, ty - 2], [tx + 1, ty - 4]]), STEEL); }
    if (tool === 'torch') { stick(ty - 1, ty + 1, WOOD); c.layer(S([[tx, ty - 2]]), '#e87828', null, false); c.layer(S([[tx, ty - 3]]), '#f8d848', null, false); }
    if (tool === 'basket') c.layer(S([[tx - 1, ty], [tx, ty], [tx + 1, ty], [tx - 1, ty + 1], [tx, ty + 1], [tx + 1, ty + 1]]), '#c8a050', (x, y, cc) => ((x + y) % 2 ? mul(cc, 0.8) : null));
    if (tool === 'bucket') { c.layer(S([[tx - 1, ty], [tx, ty], [tx + 1, ty], [tx - 1, ty + 1], [tx, ty + 1], [tx + 1, ty + 1]]), WOOD); c.layer(S([[tx - 1, ty], [tx, ty], [tx + 1, ty]]), IRON, null, false); }
    // LR-139: the kind's ears, under the head so its outline shades them (and again over the hair)
    let ears = new Set();
    {
      const ey = Math.trunc(hcy), hl = Math.trunc(hcx - hrx), hrr = Math.trunc(hcx + hrx), e = spec.ears;
      let ear = [];
      if (d === 'S' || d === 'N') {
        if (e === 'pointed') ear = [[hl - 1, ey], [hl - 1, ey - 1], [hl - 2, ey - 2], [hl - 3, ey - 3], [hrr, ey], [hrr, ey - 1], [hrr + 1, ey - 2], [hrr + 2, ey - 3]];
        if (e === 'half') ear = [[hl - 1, ey], [hl - 1, ey - 1], [hrr, ey], [hrr, ey - 1]];
        if (e === 'long') ear = [[hl - 1, ey], [hl - 2, ey], [hl - 3, ey - 1], [hrr, ey], [hrr + 1, ey], [hrr + 2, ey - 1]];
        if (e === 'big') ear = [[hl - 1, ey], [hl - 1, ey + 1], [hrr, ey], [hrr, ey + 1]];
        ears = P0(ear);
      } else {
        const bx = Math.trunc(hcx - 0.6);
        if (e === 'pointed') ear = [[bx - 1, ey - 1], [bx - 2, ey - 2], [bx - 3, ey - 3]];
        if (e === 'half') ear = [[bx - 1, ey - 1], [bx - 2, ey - 2]];
        if (e === 'long') ear = [[bx - 1, ey], [bx - 2, ey - 1], [bx - 3, ey - 1], [bx - 4, ey - 2]];
        if (e === 'big') ear = [[bx - 1, ey], [bx - 1, ey + 1], [bx - 2, ey]];
        if (Math.trunc(hrx + 0.5) >= 4 && (e === 'pointed' || e === 'half')) ear.push([bx - 4 + (e === 'half' ? 1 : 0), ey - 3 + (e === 'half' ? 1 : 0)]);
        ears = minus(P0(ear), ell(hcx, hcy, hrx * 0.95, hry));
      }
      if (ears.size) c.layer(ears, sk);
    }
    // head
    const head = ell(hcx, hcy, d !== 'E' ? hrx : hrx * 0.95, hry);
    const [hair, hc] = spec.hair || ['short', '#5a3a20'];
    if (hair === 'long' && d !== 'N')
      c.layer(filter(ell(hcx - (d === 'E' ? 0.5 : 0), hcy + 1.4, hrx + 0.9, hry + 1.6), (x, y) => y >= hcy - 1 && (d === 'E' || Math.abs(x + 0.5 - hcx) >= hrx - 1.2)), hc);
    c.layer(head, sk, warts ? (x, y, cc) => ((x * 5 + y * 7) % 9 === 0 ? mul(cc, 0.78) : null) : null);
    const top = Math.trunc(hcy - hry), faceY = Math.trunc(hcy + 0.3), fx = Math.trunc(hcx);
    // LR-139: the face of the kind: eyes (their colour, a blink), a heavy brow, a nose, tusks
    const eye = pose.blink ? mul(sk, 0.6) : (spec.eyes || OUT), dark = mul(sk, 0.62), ivory = '#f0e8d0';
    if (d === 'S') {
      c.px(fx - 2, faceY, eye); c.px(fx + 1, faceY, eye);
      if (spec.brow === 'heavy') c.layer(P0([[fx - 2, faceY - 1], [fx - 1, faceY - 1], [fx, faceY - 1], [fx + 1, faceY - 1]]), dark, null, false);
      if (spec.nose === 'big' || spec.nose === 'long') c.layer(P0([[fx - 1, faceY + 1], [fx, faceY + 1]]), mul(sk, 0.8), null, false);
      if (spec.nose === 'long') c.px(fx, faceY + 2, mul(sk, 0.7));
      if (spec.tusks === 'yes') c.layer(P0([[fx - 2, faceY + 2], [fx + 1, faceY + 2], [fx - 2, faceY + 1], [fx + 1, faceY + 1]]), ivory, null, false);
      else if (spec.tusks === 'small') c.layer(P0([[fx - 2, faceY + 2], [fx + 1, faceY + 2]]), ivory, null, false);
    } else if (d === 'E') {
      c.px(fx + 2, faceY, eye);
      if (spec.brow === 'heavy') c.layer(P0([[fx + 1, faceY - 1], [fx + 2, faceY - 1], [fx + 3, faceY - 1]]), dark, null, false);
      const nx = Math.trunc(hcx + hrx * 0.95);
      if (spec.nose === 'big') c.layer(P0([[nx, faceY + 1]]), sk);
      if (spec.nose === 'long') c.layer(P0([[nx, faceY + 1], [nx + 1, faceY + 1], [nx + 1, faceY + 2]]), sk);
      if (spec.tusks) c.layer(P0([[fx + 2, faceY + 2], [fx + 2, faceY + (spec.tusks === 'yes' ? 1 : 2)]]), ivory, null, false);
    }
    if (spec.beard && d !== 'N') {
      let bm = filter(head, (x, y) => y >= hcy + 1.2 && (d === 'S' || x + 0.5 >= hcx - 0.5));
      if (spec.long_beard) bm = union(bm, ell(hcx + (d === 'E' ? 1.2 : 0), hcy + hry + 1.2, d === 'E' ? 1.8 : 2.6, 2.6));
      c.layer(bm, hc, null, !!spec.long_beard);
      if (spec.tusks === 'yes' && d === 'S') c.layer(P0([[fx - 2, faceY + 1], [fx + 1, faceY + 1]]), ivory, null, false);
    }
    if (hair === 'tuft') c.layer(P0([[fx - 1, top], [fx, top - 1], [fx, top], [fx + 1, top]]), hc);
    if (['short', 'long', 'bun', 'braids'].includes(hair)) {
      let hm;
      if (d === 'S') hm = filter(head, (x, y) => y + 0.5 <= hcy - 1.1);
      else if (d === 'E') hm = filter(head, (x, y) => y + 0.5 <= hcy - 1.1 || (x + 0.5 <= hcx - 1.2 && y + 0.5 <= hcy + 2));
      else hm = union(filter(head, (x, y) => y + 0.5 <= hcy + 2.6), hair !== 'short' ? head : new Set());
      if (hair === 'long' && d === 'N') hm = union(hm, ell(hcx, hcy + 1.4, hrx + 0.9, hry + 1.6));
      if (hair === 'bun') hm = union(hm, ell(hcx, top - 0.2, 1.8, 1.6));
      if (hair === 'braids' && d !== 'E') hm = union(hm, ell(hcx - hrx - 0.6, hcy + 1.5, 1.2, 1.6), ell(hcx + hrx + 0.6, hcy + 1.5, 1.2, 1.6));
      c.layer(hm, hc);
    }
    if (ears.size && hair !== 'none') c.layer(minus(ears, head), sk, null, false);   // ears through the hair
    const [hat, hcol] = spec.hat || [null, null];
    if (hat === 'hood') {
      let m = union(ell(hcx, hcy, hrx + 1, hry + 0.9), ell(hcx, hcy + 3.5, hrx + 1.2, 2.2));
      if (d === 'S') m = minus(m, filter(head, (x, y) => y + 0.5 >= hcy - 1.3 && Math.abs(x + 0.5 - hcx) <= 2.8));
      if (d === 'E') m = minus(m, filter(head, (x, y) => y + 0.5 >= hcy - 1.3 && x + 0.5 >= hcx + 0.2));
      c.layer(m, hcol);
    }
    if (hat === 'helmet') {
      c.layer(filter(ell(hcx, hcy, hrx + 0.6, hry + 0.6), (x, y) => y + 0.5 <= hcy - 0.6), hcol);
      if (d === 'S') c.layer(new Set([key(Math.trunc(hcx), Math.trunc(hcy))]), hcol, null, false);
    }
    if (hat === 'straw') {
      c.layer(ell(hcx, top + 2, hrx + 2.6, 1.8), hcol, (x, y, cc) => ((x + y) % 3 === 0 ? mul(cc, 0.85) : null));
      c.layer(ell(hcx, top + 0.6, hrx - 1.2, 1.7), hcol);
    }
    if (hat === 'kerchief') c.layer(filter(ell(hcx, hcy, hrx + 0.4, hry + 0.4), (x, y) => y + 0.5 <= hcy - 0.8 || d === 'N'), hcol);
    if (hat === 'cap') c.layer(filter(ell(hcx, hcy - 0.6, hrx + 0.3, hry), (x, y) => y + 0.5 <= hcy - 1.4), hcol);
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
  return { sprite, pose, SIZE: N };
})();
