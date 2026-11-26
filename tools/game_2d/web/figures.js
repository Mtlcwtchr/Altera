// LR-165: the 48x64 people (centaurs 76x76) of the mythic world, drawn from data
// (content/sprites_2d/figures2d.json): smooth shapes in figure space (ellipses, capsules,
// polygons, a torso profile, mirrors), three tones lit from the top left, a coloured
// outline. Bodies by people, dress by culture, hair and beards by sets, tools by hand.
// native/figure2d.cpp is a line-by-line port: keep the two pixel-equal
// (tools/game_2d/figure_sheet.js --check compares them).
// spec: {people, outfit, fem, child, hide, v, hair, beard, tool, kit, skin, hairColour}
'use strict';
const Figures = (() => {
  let D = null;
  const sprites = new Map();
  function setData(d) { D = d && d.peoples ? d : null; sprites.clear(); exprs.clear(); }
  const data = () => D;

  // --- numbers -----------------------------------------------------------------------
  const fl = Math.floor;
  const clamp = v => Math.max(0, Math.min(255, fl(v + 0.5)));
  // an expression: numbers, names, + - * / %, < <= > >= == !=, && || !, abs min max trunc floor sel
  const exprs = new Map();
  function compile(t) {
    let i = 0, bad = false;
    const ws = () => { while (t[i] === ' ') i++; };
    const eat = s => { ws(); if (t.startsWith(s, i)) { i += s.length; return true; } return false; };
    const FN = { abs: [1, a => Math.abs(a)], min: [2, (a, b) => Math.min(a, b)], max: [2, (a, b) => Math.max(a, b)],
      trunc: [1, a => Math.trunc(a)], floor: [1, a => fl(a)], sel: [3, (c, a, b) => c !== 0 ? a : b] };
    const bin = (a, b, f) => V => f(a(V), b(V));
    const orE = () => { let a = andE(); while (eat('||')) { const l = a, r = andE(); a = V => (l(V) !== 0 || r(V) !== 0) ? 1 : 0; } return a; };
    const andE = () => { let a = eqE(); while (eat('&&')) { const l = a, r = eqE(); a = V => (l(V) !== 0 && r(V) !== 0) ? 1 : 0; } return a; };
    const eqE = () => { let a = relE(); for (;;) { if (eat('==')) a = bin(a, relE(), (x, y) => x === y ? 1 : 0); else if (eat('!=')) a = bin(a, relE(), (x, y) => x !== y ? 1 : 0); else return a; } };
    const relE = () => { let a = addE(); for (;;) {
      if (eat('<=')) a = bin(a, addE(), (x, y) => x <= y ? 1 : 0); else if (eat('>=')) a = bin(a, addE(), (x, y) => x >= y ? 1 : 0);
      else if (eat('<')) a = bin(a, addE(), (x, y) => x < y ? 1 : 0); else if (eat('>')) a = bin(a, addE(), (x, y) => x > y ? 1 : 0); else return a; } };
    const addE = () => { let a = mulE(); for (;;) { if (eat('+')) a = bin(a, mulE(), (x, y) => x + y); else if (eat('-')) a = bin(a, mulE(), (x, y) => x - y); else return a; } };
    const mulE = () => { let a = unE(); for (;;) {
      if (eat('*')) a = bin(a, unE(), (x, y) => x * y); else if (eat('/')) a = bin(a, unE(), (x, y) => y === 0 ? 0 : x / y);
      else if (eat('%')) a = bin(a, unE(), (x, y) => { const b = Math.trunc(y); if (!b) return 0; const r = Math.trunc(x) % b; return r < 0 ? r + Math.abs(b) : r; });
      else return a; } };
    const unE = () => { if (eat('-')) { const a = unE(); return V => -a(V); } if (eat('!')) { const a = unE(); return V => a(V) === 0 ? 1 : 0; } return prim(); };
    const prim = () => {
      ws();
      if (eat('(')) { const e = orE(); if (!eat(')')) bad = true; return e; }
      let j = i;
      if (/[0-9.]/.test(t[i] || '')) { while (/[0-9.]/.test(t[j] || '')) j++; const n = parseFloat(t.slice(i, j)); i = j; return () => n; }
      while (/[A-Za-z0-9_]/.test(t[j] || '')) j++;
      const id = t.slice(i, j); i = j;
      if (!id) { bad = true; return () => 0; }
      if (FN[id]) {
        const [n, f] = FN[id], a = [];
        if (!eat('(')) bad = true;
        for (let k = 0; k < n; k++) { if (k && !eat(',')) bad = true; a.push(orE()); }
        if (!eat(')')) bad = true;
        return n === 1 ? V => f(a[0](V)) : n === 2 ? V => f(a[0](V), a[1](V)) : V => f(a[0](V), a[1](V), a[2](V));
      }
      return V => (V[id] === undefined ? 0 : V[id]);
    };
    const e = orE(); ws();
    if (bad || i !== t.length) { if (typeof console !== 'undefined') console.warn('figures2d.json: bad expression', t); return () => 0; }
    return e;
  }
  const expr = t => { let e = exprs.get(t); if (!e) { e = compile(t); exprs.set(t, e); } return e; };
  const num = (j, V) => typeof j === 'number' ? j : typeof j === 'string' ? expr(j)(V) : 0;

  // --- colours: '#hex', a name, 'a*k' (k < 1 darker, k > 1 toward white), mix(a,b,k), tint(a,r,g,b)
  const hex = h => { const n = parseInt(h.slice(1, 7), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255]; };
  const sh = (c, f) => c.map(v => clamp(f < 1 ? v * f : v + (255 - v) * (f - 1)));
  const warm = c => [clamp(c[0] * 1.04), clamp(c[1]), clamp(c[2] * 0.94)];
  const cool = c => [clamp(c[0] * 0.95), clamp(c[1] * 0.97), clamp(c[2] * 1.07)];
  function args(s) {   // the top-level comma parts of "a,b(c,d),e"
    const out = []; let d = 0, st = 0;
    for (let i = 0; i < s.length; i++) { const ch = s[i]; if (ch === '(') d++; else if (ch === ')') d--; else if (ch === ',' && !d) { out.push(s.slice(st, i).trim()); st = i + 1; } }
    out.push(s.slice(st).trim());
    return out;
  }
  function colour(s, C, V) {
    s = String(s).trim();
    if (s[0] === '#') return hex(s);
    if (s.startsWith('mix(') && s.endsWith(')')) { const a = args(s.slice(4, -1)); const x = colour(a[0], C, V), y = colour(a[1], C, V), k = num(a[2], V);
      return x.map((v, i) => clamp(v * (1 - k) + y[i] * k)); }
    if (s.startsWith('tint(') && s.endsWith(')')) { const a = args(s.slice(5, -1)); const x = colour(a[0], C, V);
      return [clamp(x[0] * num(a[1], V)), clamp(x[1] * num(a[2], V)), clamp(x[2] * num(a[3], V))]; }
    let d = 0, star = -1;
    for (let i = 0; i < s.length; i++) { if (s[i] === '(') d++; else if (s[i] === ')') d--; else if (s[i] === '*' && !d) star = i; }
    if (star > 0) return sh(colour(s.slice(0, star), C, V), num(s.slice(star + 1).trim(), V));
    if (C[s]) return C[s];
    return [255, 0, 255];
  }

  // --- shapes in figure space -----------------------------------------------------------
  // A compiled shape: {t(x, y, V): inside?, b: [x0, y0, x1, y1] bounds or null (unbounded)}
  const INF = null;
  const uni = (a, b) => (a && b) ? [Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.max(a[2], b[2]), Math.max(a[3], b[3])] : INF;
  function shape(j, V) {
    if (typeof j === 'string') {
      if (j[0] === '@') return shape((D.shapes || {})[j.slice(1)] || {}, V);
      return { t: () => false, b: [0, 0, 0, 0] };
    }
    if (Array.isArray(j)) return shape({ or: j }, V);
    if (j.ell) { const [cx, cy, rx, ry] = j.ell.map(n => num(n, V));
      return { t: (x, y) => { const a = (x - cx) / rx, b = (y - cy) / ry; return a * a + b * b <= 1; }, b: [cx - rx, cy - ry, cx + rx, cy + ry] }; }
    if (j.cap) { const [x0, y0, x1, y1, r0, r1] = j.cap.map(n => num(n, V));
      const dx = x1 - x0, dy = y1 - y0, L = dx * dx + dy * dy, rm = Math.max(r0, r1);
      return { t: (x, y) => { let t = L ? ((x - x0) * dx + (y - y0) * dy) / L : 0; t = Math.max(0, Math.min(1, t));
        const px = x0 + dx * t, py = y0 + dy * t, r = r0 + (r1 - r0) * t; return (x - px) * (x - px) + (y - py) * (y - py) <= r * r; },
        b: [Math.min(x0, x1) - rm, Math.min(y0, y1) - rm, Math.max(x0, x1) + rm, Math.max(y0, y1) + rm] }; }
    if (j.poly) return polyShape(j.poly.map(p => [num(p[0], V), num(p[1], V)]));
    if (j.rect) { const [x0, y0, x1, y1] = j.rect.map(n => num(n, V)); return { t: (x, y) => x >= x0 && x <= x1 && y >= y0 && y <= y1, b: [x0, y0, x1, y1] }; }
    if (j.torso) {
      const yb = num(j.torso[0], V), flare = num(j.torso[1], V), T = D.torso || {};
      const R = (V.fem ? T.f : T.m) || [[34, 22], [32, 46]];
      const pts = R.filter(p => p[1] <= yb).map(p => [p[0], p[1]]);
      const last = pts[pts.length - 1] || [32, 22];
      pts.push(yb > (T.hem_y || 46) ? [last[0] + flare, yb] : [last[0] - 0.8, yb]);
      const mx = D.mirror_x || 24;
      return polyShape(pts.concat(pts.map(([x, y]) => [2 * mx - x, y]).reverse()));
    }
    if (j.or) { const s = j.or.map(k => shape(k, V)); let b = s.length ? s[0].b : [0, 0, 0, 0]; for (const k of s) b = uni(b, k.b);
      return { t: (x, y, W) => { for (const k of s) if (k.t(x, y, W)) return true; return false; }, b }; }
    if (j.and) { const s = j.and.map(k => shape(k, V)); let b = INF;
      for (const k of s) if (k.b) b = b ? [Math.max(b[0], k.b[0]), Math.max(b[1], k.b[1]), Math.min(b[2], k.b[2]), Math.min(b[3], k.b[3])] : k.b;
      return { t: (x, y, W) => { for (const k of s) if (!k.t(x, y, W)) return false; return true; }, b }; }
    if (j.sub) { const a = shape(j.sub[0], V), c = shape(j.sub[1], V); return { t: (x, y, W) => a.t(x, y, W) && !c.t(x, y, W), b: a.b }; }
    if (j.mir || j.both) {
      const a = shape(j.mir || j.both, V), m2 = 2 * (D.mirror_x || 24);
      const mb = a.b ? [m2 - a.b[2], a.b[1], m2 - a.b[0], a.b[3]] : INF;
      if (j.mir) return { t: (x, y, W) => a.t(m2 - x, y, W), b: mb };
      return { t: (x, y, W) => a.t(x, y, W) || a.t(m2 - x, y, W), b: uni(a.b, mb) };
    }
    if (j.use) {   // a named shape with some numbers set: {use: name, with: {var: expr}}
      const W = Object.assign({}, V);
      for (const [k, e] of Object.entries(j.with || {})) W[k] = num(e, V);
      return shape((D.shapes || {})[j.use] || {}, W);
    }
    if (j.where !== undefined) {   // a per-pixel test of x, y (figure space)
      const e = expr(String(j.where));
      return { t: (x, y, W) => { W.x = x; W.y = y; return e(W) !== 0; }, b: INF };
    }
    return { t: () => false, b: [0, 0, 0, 0] };
  }
  function polyShape(pts) {
    let b = [1e9, 1e9, -1e9, -1e9];
    for (const [x, y] of pts) b = [Math.min(b[0], x), Math.min(b[1], y), Math.max(b[2], x), Math.max(b[3], y)];
    return { t: (x, y) => { let c = false;
      for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) { const xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
        if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) c = !c; }
      return c; }, b: pts.length ? b : [0, 0, 0, 0] };
  }

  // --- the scene: figure space -> canvas, the body stretched by build, the head only in width
  function scene(cv, t) {
    const G = D.ground || 66, HY = D.neck || 22, mx = D.mirror_x || 24;
    const sc = { w: cv.w, h: cv.h, ox: cv.ox, oy: cv.oy, dy: 0, sx: t.sx, sy: t.sy, hx: t.hx, G, HY, mx, f22: G - (G - HY) * t.sy, px: new Array(cv.w * cv.h).fill(null) };
    return sc;
  }
  function inv(sc, lx, ly) {
    if (ly >= sc.f22) return [sc.mx + (lx - sc.mx) / sc.sx, sc.G - (sc.G - ly) / sc.sy];
    return [sc.mx + (lx - sc.mx) / sc.hx, ly - (sc.f22 - sc.HY)];
  }
  function fwd(sc, x, y) {
    if (y >= sc.HY) return [sc.mx + (x - sc.mx) * sc.sx, sc.G - (sc.G - y) * sc.sy];
    return [sc.mx + (x - sc.mx) * sc.hx, y + (sc.f22 - sc.HY)];
  }
  function paint(sc, S, base, mode, pat, C, V) {
    const { w, h } = sc, oy = sc.oy + sc.dy;
    let x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (S.b) {   // only the pixels the shape can reach
      const m = sc.mx, xs = [m + (S.b[0] - m) * sc.sx, m + (S.b[2] - m) * sc.sx, m + (S.b[0] - m) * sc.hx, m + (S.b[2] - m) * sc.hx];
      const ya = fwd(sc, 0, S.b[1])[1], yb = fwd(sc, 0, S.b[3])[1];
      x0 = Math.max(0, fl(Math.min(xs[0], xs[1], xs[2], xs[3]) + sc.ox) - 2); x1 = Math.min(w - 1, fl(Math.max(xs[0], xs[1], xs[2], xs[3]) + sc.ox) + 2);
      y0 = Math.max(0, fl(ya + oy) - 2); y1 = Math.min(h - 1, fl(yb + oy) + 2);
      if (x0 > x1 || y0 > y1) return;
    }
    const m = new Uint8Array(w * h);
    let any = false;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const [px, py] = inv(sc, x + 0.5 - sc.ox, y + 0.5 - oy);
      if (S.t(px, py, V)) { m[y * w + x] = 1; any = true; }
    }
    if (!any) return;
    const at = (x, y) => (x < 0 || y < 0 || x >= w || y >= h) ? 0 : m[y * w + x];
    const sd = D.shade || {}, Lc = warm(sh(base, sd.light || 1.32)), Sc = cool(sh(base, sd.shadow || 0.76)), Dc = cool(sh(base, sd.dark || 0.5));
    const rim = mode.rim1 ? 1 : 2;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      if (!m[y * w + x]) continue;
      let c = base;
      if (!at(x - 1, y - 1) || (rim > 1 && !at(x - 2, y - 2) && at(x + 2, y + 2))) c = Lc;
      if (!mode.flat) { if (!at(x + 2, y + 2)) c = Sc; if (!at(x + 1, y + 1) || (!at(x, y + 1) && !mode.soft)) c = Dc; }
      if (pat) {
        const [px, py] = inv(sc, x + 0.5 - sc.ox, y + 0.5 - oy);
        V.x = px; V.y = py;
        for (const p of pat) if (num(p.if, V) !== 0) { c = colour(p.c, C, V); break; }
      }
      sc.px[y * w + x] = c;
    }
  }
  function dot(sc, x, y, col, w, h) {
    const [fx, fy] = fwd(sc, x, y);
    const X0 = fl(fx + sc.ox + 0.5), Y0 = fl(fy + sc.oy + sc.dy + 0.5);
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
      const X = X0 + i, Y = Y0 + j;
      if (X >= 0 && Y >= 0 && X < sc.w && Y < sc.h) sc.px[Y * sc.w + X] = col;
    }
  }
  function finish(sc) {   // the outline: an empty pixel by a painted one gets its colour, dark
    const { w, h } = sc, out = new Array(w * h).fill(null), k = (D.shade || {}).outline || 0.3;
    const get = (x, y) => (x < 0 || y < 0 || x >= w || y >= h) ? null : sc.px[y * w + x];
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let c = get(x, y);
      if (!c) { const n = get(x, y - 1) || get(x - 1, y) || get(x + 1, y) || get(x, y + 1); if (n) c = sh(n, k); }
      out[y * w + x] = c;
    }
    return out;
  }

  // --- ops: {s: shape, c: colour, m: 'soft flat rim1', pat: [{if, c}], when, sides} | {dot: [x, y, w, h], c}
  //          | {ops: name, with: {var: expr}, cwith: {name: colour}}
  function onSide(o, side) { return !o.sides || o.sides.indexOf(side) >= 0; }
  function drawOps(sc, ops, C, V, side) {
    if (!Array.isArray(ops)) return;
    for (const o of ops) {
      if (!o || !onSide(o, side)) continue;
      if (o.when !== undefined && num(o.when, V) === 0) continue;
      if (o.ops) {
        const W = Object.assign({}, V), K = Object.assign({}, C);
        for (const [k, e] of Object.entries(o.with || {})) W[k] = num(e, V);
        for (const [k, e] of Object.entries(o.cwith || {})) K[k] = colour(e, C, V);
        drawOps(sc, (D.ops || {})[o.ops], K, W, side);
        continue;
      }
      const col = colour(o.c || 'skin', C, V);
      if (o.dot) { dot(sc, num(o.dot[0], V), num(o.dot[1], V), col, o.dot[2] ? num(o.dot[2], V) : 1, o.dot[3] ? num(o.dot[3], V) : 1); continue; }
      if (!o.s) continue;
      const mode = {}; for (const wd of String(o.m || '').split(' ')) if (wd) mode[wd] = true;
      paint(sc, shape(o.s, V), col, mode, o.pat || null, C, V);
    }
  }

  // --- a figure ------------------------------------------------------------------------
  const pick = (list, k) => Array.isArray(list) && list.length ? list[((k % list.length) + list.length) % list.length] : undefined;
  // The look a spec resolves to: people, outfit, hair, beard, colours, numbers. Exported for the sheet and tests.
  function resolve(spec) {
    const P = D.peoples[spec.people] || D.peoples[D.default_people] || Object.values(D.peoples)[0];
    const v = spec.v | 0, fem = P.sex === 'f' ? true : P.sex === 'm' ? false : !!spec.fem, child = !!spec.child;
    const O = spec.outfit && D.outfits ? D.outfits[spec.outfit] : null;
    const hairs = fem ? P.hair_f : P.hair_m;
    let hair = spec.hair || pick(hairs, v) || (fem ? 'long' : 'short');
    hair = (D.hair_alias || {})[hair] || hair;
    let beard = 'none';
    if (!fem && !child) beard = spec.beard || pick(P.beards, v * 7 + 3) || 'none';
    beard = (D.beard_alias || {})[beard] || beard;
    return { P, O, v, fem, child, hair, beard };
  }
  function draw(spec, side, pose) {
    if (!D) return null;
    pose = pose || {};
    const R = resolve(spec), { P, O, v, fem, child } = R;
    side = side === 'W' ? 'E' : side;
    const cen = !!P.cen, adult = !child;
    // the numbers the data may name
    const V = Object.assign({}, D.numbers || {}, P.flags || {}, (O && O.flags) || {});
    Object.assign(V, { v, fem: fem ? 1 : 0, cen: cen ? 1 : 0, child: child ? 1 : 0, adult: adult ? 1 : 0, hide: spec.hide ? 1 : 0, kit: spec.kit ? 1 : 0,
      S: side === 'S' ? 1 : 0, E: side === 'E' ? 1 : 0, N: side === 'N' ? 1 : 0,
      step: pose.step === undefined ? -1 : pose.step, bob: pose.bob | 0, swing: pose.swing | 0, dip: pose.dip | 0, blink: pose.blink ? 1 : 0,
      hastool: spec.tool && D.tools && D.tools[spec.tool] ? 1 : 0, dressed: O ? 1 : 0, covered: O && O.covered ? 1 : 0, helm: O && O.helm ? 1 : 0, x: 0, y: 0 });
    // the colours the data may name
    const C = {};
    for (const [k, c] of Object.entries(D.colours || {})) C[k] = hex(c);
    const fromList = (list, k, def) => { const c = pick(list, k); return hex(c || def); };
    C.skin = spec.skin ? hex(spec.skin) : fromList(P.skins, v, '#d09a70');
    C.hair = spec.hairColour ? hex(spec.hairColour) : fromList(P.hairs, v + 1, '#3a2418');
    C.iris = fromList(P.irises, v, '#4a3a2a');
    if (P.fur) C.fur = fromList(P.fur, v, '#6b4a2f');
    if (P.coats) { const cc = pick(P.coats, v); C.coat = hex(cc[0]); C.coat_d = hex(cc[1]); }
    for (const [k, c] of Object.entries(P.colours || {})) C[k] = Array.isArray(c) ? fromList(c, v, '#ff00ff') : hex(c);
    if (O) for (const [k, c] of Object.entries(O.colours || {})) C[k] = Array.isArray(c) ? fromList(c, v, '#ff00ff') : hex(c);
    const CU = spec.culture && D.cultures ? D.cultures[spec.culture] : null;
    if (CU) for (const [k, c] of Object.entries(CU.colours || {})) C[k] = Array.isArray(c) ? fromList(c, v, '#ff00ff') : hex(c);
    if (spec.dye && O && O.dye) C[O.dye] = hex(spec.dye);
    // the chest: what covers it (bare, it is the skin)
    let chest = null;
    if (O && O.chest) for (const ch of (Array.isArray(O.chest) ? O.chest : [{ c: O.chest }])) if (ch.when === undefined || num(ch.when, V) !== 0) { chest = colour(ch.c, C, V); break; }
    V.haschest = chest ? 1 : 0;
    const bare = !chest || String(chest) === String(C.skin);
    V.bare = bare ? 1 : 0;
    V.modest = bare && (spec.hide || child) ? 1 : 0;   // the hide-nudity option, and always for children
    if (V.modest) { chest = colour(fem ? (D.modest_f || '#e6dcc4') : (D.modest_m || '#e6dcc4'), C, V); V.bare = 0; V.haschest = 1; }
    C.chest = chest || C.skin;
    for (const [k, e] of (Array.isArray(D.vars) ? D.vars : Object.entries(D.vars || {}))) V[k] = num(e, V);   // derived numbers, in order
    // the build: the people's, a child's, the side's
    const T = Object.assign({ sx: 1, sy: 1 }, (fem && P.t_f) || P.t || {});
    if (child) { const k = D.child || { sx: 0.8, sy: 0.62 }; T.sx *= k.sx; T.sy *= k.sy; }
    T.hx = (T.sx + 1) / 2;
    if (side === 'E') { const e = (D.views || {})[cen ? 'E_cen' : 'E'] || {}; T.sx *= e.sx || 1; T.hx *= e.hx || 1; }
    const cv = cen ? D.canvas_cen : D.canvas;
    const sc = scene(cv, T);
    sc.dy = -(pose.bob | 0);
    const srcs = [D.base || {}, P.layers || {}, (O && O.layers) || {}, (D.hairs || {})[R.hair] || {}, (D.beards || {})[R.beard] || {},
      spec.tool ? ((D.tools || {})[spec.tool] || {}) : {}];
    for (const L of D.order || []) {
      const name = typeof L === 'string' ? L : L.l;
      sc.dy = -(pose.bob | 0) + (typeof L === 'object' && L.dy !== undefined ? num(L.dy, V) : 0);
      for (const s of srcs) drawOps(sc, s[name], C, V, side);
    }
    return { w: sc.w, h: sc.h, px: finish(sc), anchor: cv.anchor, scale: P.scale || 1 };
  }
  // the pose of a person at `ms`, as pixels2d.cpp's pawnPose
  function pose(moving, walked, working, ms, id) { return typeof Pawns !== 'undefined' ? Pawns.pose(moving, walked, working, ms, id) : {}; }
  const cssOf = c => c ? 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')' : null;
  const hexOf = c => c ? '#' + c.map(v => v.toString(16).padStart(2, '0')).join('') : null;
  // a canvas of the figure (browser), cached by everything that changes it
  function sprite(spec, side, ps) {
    const k = JSON.stringify([spec, side, ps || {}]);
    let cv = sprites.get(k);
    if (cv) return cv;
    const f = draw(spec, side, ps);
    if (!f) return null;
    cv = document.createElement('canvas'); cv.width = f.w; cv.height = f.h;
    const g = cv.getContext('2d');
    for (let y = 0; y < f.h; y++) for (let x = 0; x < f.w; x++) {
      const c = f.px[y * f.w + (side === 'W' ? f.w - 1 - x : x)];
      if (c) { g.fillStyle = cssOf(c); g.fillRect(x, y, 1, 1); }
    }
    cv.anchor = f.anchor;
    if (sprites.size > 4000) sprites.clear();
    sprites.set(k, cv);
    return cv;
  }
  return { setData, data, draw, resolve, sprite, pose, hexOf, compile };
})();
if (typeof module !== 'undefined') module.exports = Figures;
