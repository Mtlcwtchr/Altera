// LR-165: sheets of the 48x64 figures straight from content/sprites_2d/figures2d.json, no build
// and no game (web/figures.js): every belt's peoples in their cultures' dress (daily, festive,
// war, rite, bare), and a sheet of bodies (bare, hidden nudity, a child, the sides S E N W and
// the walk). --dump writes the pixels of a fixed set of figures as JSON, for the C++ check.
//   node tools/game_2d/figure_sheet.js [out_dir] [scale] [--dump file.json]
'use strict';
const fs = require('fs'), path = require('path'), zlib = require('zlib');
const root = path.resolve(__dirname, '../..');
const F = require(path.join(root, 'tools/game_2d/web/figures.js'));
const args = process.argv.slice(2);
const dumpAt = args.indexOf('--dump');
const dump = dumpAt >= 0 ? args.splice(dumpAt, 2)[1] : null;
const testAt = args.indexOf('--test');
const test = testAt >= 0 ? !!args.splice(testAt, 1) : false;
const outDir = args[0] || path.join(root, 'terrain_lab_out/shots_for_user');
const Z = +(args[1] || 2);
const D = JSON.parse(fs.readFileSync(path.join(root, 'content/sprites_2d/figures2d.json'), 'utf8'));
F.setData(D);
const BELTS = {
  mediterranean: { bg: [0xcd, 0xbf, 0x9b], rows: [['polis_hellene', 'hellene', 'm'], ['polis_hellene', 'hellene', 'f'], ['isle_sailor', 'hellene', 'm'], ['pastoral_hills', 'centaur', 'm'], ['pastoral_hills', 'centaur', 'f'],
    ['labyrinth', 'minotaur', 'm'], ['labyrinth', 'minotaur', 'f'], ['grove_nymph', 'nymph', 'f'], ['satyr_vine', 'satyr', 'm'], ['smith_cyclops', 'cyclops', 'm'], ['smith_cyclops', 'cyclops', 'f'],
    ['port_kobal', 'goblin', 'm'], ['port_kobal', 'goblin', 'f']] },
  nile: { bg: [0xd8, 0xc0, 0x8a], rows: [['nile_priest', 'nilote', 'm'], ['nile_priest', 'nilote', 'f']] },
  forest: { bg: [0x9f, 0xb0, 0x8a], rows: [['forest_elf', 'elf', 'm'], ['forest_elf', 'elf', 'f'], ['forest_elf', 'half_elf', 'm'], ['vila_glade', 'vila', 'f'],
    ['celtic_grove', 'woodfolk', 'm'], ['celtic_grove', 'woodfolk', 'f'], ['slav_village', 'woodfolk', 'm'], ['slav_village', 'woodfolk', 'f']] },
  north: { bg: [0x9f, 0xb0, 0xb6], rows: [['north_folk', 'northman', 'm'], ['north_folk', 'northman', 'f'], ['dwarf_hold', 'dwarf', 'm'], ['dwarf_hold', 'dwarf', 'f'],
    ['alv_hall', 'alv', 'm'], ['alv_hall', 'alv', 'f'], ['jotun_hold', 'ogre', 'm'], ['jotun_hold', 'ogre', 'f'], ['troll_fell', 'troll', 'm'], ['troll_fell', 'troll', 'f']] },
  steppe: { bg: [0xcf, 0xc0, 0x8c], rows: [['steppe_clan', 'steppe_folk', 'm'], ['steppe_clan', 'steppe_folk', 'f'], ['steppe_rider', 'orc', 'm'], ['steppe_rider', 'orc', 'f'],
    ['steppe_rider', 'steppe_centaur', 'm'], ['steppe_rider', 'steppe_centaur', 'f'], ['steppe_rider', 'half_orc', 'm']] },
};
const SLOTS = ['daily', 'festive', 'war', 'rite'];
function specOf(cul, people, sex, slot, v, extra) {
  const C = D.cultures[cul] || {};
  return Object.assign({ people, culture: cul, outfit: slot ? C[slot] : null, fem: sex === 'f', v, kit: true }, extra || {});
}
function png(file, W, H, img) {
  const SW = W * Z, SH = H * Z, raw = Buffer.alloc((SW * 3 + 1) * SH);
  for (let y = 0; y < SH; y++) {
    raw[y * (SW * 3 + 1)] = 0;
    for (let x = 0; x < SW; x++) img.copy(raw, y * (SW * 3 + 1) + 1 + x * 3, (Math.floor(y / Z) * W + Math.floor(x / Z)) * 3, (Math.floor(y / Z) * W + Math.floor(x / Z)) * 3 + 3);
  }
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = b => { let c = -1; for (const v of b) c = crcT[(c ^ v) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(SW, 0); ihdr.writeUInt32BE(SH, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}
function sheet(file, grid, bg) {   // grid: rows of [spec, side, pose] (null: empty)
  const CW = D.canvas.w, CH = D.canvas.h, cols = Math.max(...grid.map(r => r.length)), W = cols * (CW + 2), H = grid.length * (CH + 2);
  const img = Buffer.alloc(W * H * 3);
  for (let i = 0; i < W * H; i++) img.set(bg, i * 3);
  grid.forEach((row, ry) => row.forEach((cell, cx) => {
    if (!cell) return;
    const f = F.draw(cell[0], cell[1], cell[2]);
    for (let y = 0; y < f.h; y++) for (let x = 0; x < f.w; x++) {
      const c = f.px[y * f.w + (cell[1] === 'W' ? f.w - 1 - x : x)];
      if (!c) continue;
      img.set(c, ((ry * (CH + 2) + 1 + y) * W + cx * (CW + 2) + 1 + x) * 3);
    }
  }));
  png(file, W, H, img);
  return file;
}
if (test) {   // the rules a figure keeps, headless: exit 1 on a break
  let bad = 0, n = 0;
  const warn = console.warn; let warned = 0; console.warn = (...a) => { warned++; warn(...a); };
  const same = (a, b) => a.px.every((c, i) => String(c) === String(b.px[i]));
  const magenta = f => f.px.some(c => c && c[0] === 255 && c[1] === 0 && c[2] === 255);
  for (const [id, P] of Object.entries(D.peoples)) for (const sex of P.sex ? [P.sex] : ['m', 'f']) for (let v = 0; v < 4; v++) {
    const cul = P.culture, fem = sex === 'f';
    // a child is never drawn bare: bare = the same as with nudity hidden, and no adult's chest
    for (const side of 'SEN') {
      n++;
      const a = F.draw({ people: id, culture: cul, fem, child: true, v }, side, {}), b = F.draw({ people: id, culture: cul, fem, child: true, v, hide: true }, side, {});
      if (!same(a, b)) { bad++; console.log('FAIL child bare differs from hidden:', id, sex, v, side); }
    }
    // every outfit of the culture draws with known colours, in every side and pose
    for (const slot of ['daily', 'festive', 'war', 'rite', 'warm']) {
      const o = (D.cultures[cul] || {})[slot];
      if (!o) continue;
      for (const side of 'SEN') {
        n++;
        const f = F.draw({ people: id, culture: cul, outfit: o, fem, v, kit: true, tool: 'axe' }, side, { step: v, swing: v & 1 });
        if (magenta(f)) { bad++; console.log('FAIL unknown colour:', id, sex, o, side); }
      }
    }
  }
  if (warned) { bad++; console.log('FAIL bad expressions:', warned); }
  console.log(`${n} figures checked, ${bad} failures`);
  process.exit(bad ? 1 : 0);
} else if (dump) {   // a fixed set for the C++ port: every people bare and dressed, the sides, the walk
  const out = [];
  for (const [b, B] of Object.entries(BELTS)) B.rows.forEach(([cul, pp, sex], i) => {
    for (const slot of [null, ...SLOTS]) for (const side of 'SEN') out.push({ spec: specOf(cul, pp, sex, slot, i, { tool: slot === 'daily' ? 'axe' : undefined }), side, pose: { step: i % 4, bob: i & 1, dip: (i >> 1) & 1, blink: !(i % 5), swing: (i >> 2) & 1 } });
    out.push({ spec: specOf(cul, pp, sex, null, i, { hide: true }), side: 'S', pose: {} }, { spec: specOf(cul, pp, sex, null, i, { child: true }), side: 'S', pose: {} });
  });
  for (const t of Object.keys(D.tools)) out.push({ spec: specOf('polis_hellene', 'hellene', 'm', 'daily', 1, { tool: t, kit: false }), side: 'E', pose: {} });
  const res = out.map(o => { const f = F.draw(o.spec, o.side, o.pose); return Object.assign(o, { w: f.w, h: f.h, px: f.px.map(c => c ? F.hexOf(c) : '') }); });
  fs.writeFileSync(dump, JSON.stringify(res));
  console.log(dump, res.length, 'figures');
} else {
  const made = [];
  for (const [b, B] of Object.entries(BELTS))
    made.push(sheet(path.join(outDir, 'figures_' + b + '.png'), B.rows.map(([cul, pp, sex], i) => [null, ...SLOTS].map(slot => [specOf(cul, pp, sex, slot, i + SLOTS.indexOf(slot) + 1), 'S', {}])), B.bg));
  const bodies = [];
  for (const [id, P] of Object.entries(D.peoples)) {
    if (id === 'human') continue;
    for (const sex of P.sex ? [P.sex] : ['m', 'f']) {
      const cul = P.culture, s = (extra) => specOf(cul, id, sex, null, 1, extra), d = specOf(cul, id, sex, 'daily', 1);
      bodies.push([[s(), 'S', {}], [s({ hide: true }), 'S', {}], [s({ child: true }), 'S', {}], [specOf(cul, id, sex, 'daily', 2, { child: true }), 'S', {}],
        [d, 'E', {}], [d, 'N', {}], [d, 'W', {}], ...[0, 1, 2, 3].map(st => [d, 'E', { step: st, bob: st & 1 }]), [Object.assign({}, d, { tool: 'axe', kit: false }), 'S', { swing: 1 }]]);
    }
  }
  made.push(sheet(path.join(outDir, 'figures_bodies.png'), bodies, [0x8a, 0x9a, 0x6a]));
  console.log(made.join('\n'));
}
