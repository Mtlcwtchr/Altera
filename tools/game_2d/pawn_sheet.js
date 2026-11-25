// LR-139: a sheet of pawns straight from the data, no build and no game: every kind of
// content/species (body.pawn) as a man and a woman, bare and dressed, four sides, and a
// row of every cut and beard of content/sprites_2d/pawns2d.json. Uses web/pawns.js, the
// same drawing as the page (and pixels2d.cpp, kept pixel-equal).
//   node tools/game_2d/pawn_sheet.js [out.png] [scale]
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm'), zlib = require('zlib');
const root = path.resolve(__dirname, '../..');
const out = process.argv[2] || path.join(root, 'terrain_lab_out/shots_for_user/pawn_sheet.png');
const Z = +(process.argv[3] || 4);
const ctx = { document: { createElement: () => { const cv = { px: {}, getContext: () => ({ set fillStyle(v) { this.f = v; }, get fillStyle() { return this.f; }, fillRect(x, y) { cv.px[y * 20 + x] = this.f; } }) }; return cv; } } };
vm.runInNewContext(fs.readFileSync(path.join(root, 'tools/game_2d/web/pawns.js'), 'utf8') + ';this.P = Pawns;', ctx);
const parts = JSON.parse(fs.readFileSync(path.join(root, 'content/sprites_2d/pawns2d.json'), 'utf8'));
ctx.P.setParts(parts);
const S = 20, CELL = S + 2, BG = [0x6a, 0x8a, 0x4a];
const rows = [];
const words = s => (s || '').split(/\s+/).filter(Boolean);
for (const f of fs.readdirSync(path.join(root, 'content/species')).filter(n => n.endsWith('.json')).sort()) {
  const d = JSON.parse(fs.readFileSync(path.join(root, 'content/species', f), 'utf8'));
  if (!d.id || !d.body || d.id === 'dragon') continue;
  let pw = Object.assign({}, d.body.pawn || {}), skin = d.body.skin;
  if (d.blend) {   // a blend: its parents' look, the second over the first, then its own
    const par = d.blend.map(id => JSON.parse(fs.readFileSync(path.join(root, 'content/species', id + '.json'), 'utf8')));
    pw = Object.assign({}, par[0].body.pawn || {}, par[1].body.pawn || {}, d.body.pawn || {});
    skin = skin || par[1].body.skin || par[0].body.skin;
  }
  for (const sex of ['m', 'f']) {
    const hs = words(pw[sex === 'f' ? 'hair_f' : 'hair_m']), bs = words(pw.beards);
    const spec = { skin: skin || 'light', body: sex === 'f' ? 'fem' : 'norm', hair: [hs[0] || (sex === 'f' ? 'long' : 'short'), '#5a3a20'],
      ears: pw.ears, head: pw.head, nose: pw.nose, brow: pw.brow, eyes: pw.eyes, tusks: pw.tusks, build: pw.build, skin_pattern: pw.skin_pattern, wear: [] };
    if (sex === 'm' && pw.beard !== 'never' && (bs.length || pw.beard === 'long')) { spec.beard = true; spec.beard_style = bs[0] || ''; }
    rows.push({ label: d.id + ' ' + sex, spec });
  }
}
for (const h of Object.keys(parts.hair || {})) rows.push({ label: 'hair ' + h, spec: { body: 'fem', hair: [h, '#7a5030'], wear: [] } });
for (const b of Object.keys(parts.beards || {})) rows.push({ label: 'beard ' + b, spec: { hair: ['short', '#5a3a20'], beard: true, beard_style: b, wear: [] } });
const COLS = 12, W = COLS * CELL, H = rows.length * CELL;
const img = Buffer.alloc(W * H * 3);
for (let i = 0; i < W * H; i++) img.set(BG, i * 3);
rows.forEach((r, row) => {
  const dressed = JSON.parse(JSON.stringify(r.spec));
  dressed.wear = r.spec.body === 'fem' ? [['robe', '#6a4a6a'], ['belt', '#7a4a28']] : [['shirt', '#7a5a3a'], ['pants', '#5a4630'], ['belt', '#4a2c18']];
  const frames = [...'SENW'].map(d => [r.spec, d, 0]).concat([...'SENW'].map(d => [dressed, d, 0]), [0, 1, 2, 3].map(st => [dressed, 'E', { step: st, bob: st & 1 }]));
  frames.forEach(([sp, d, pose], col) => {
    const cv = ctx.P.sprite(sp, d, pose);
    for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
      const c = cv.px[y * S + x];
      if (!c) continue;
      const o = ((row * CELL + 1 + y) * W + col * CELL + 1 + x) * 3;
      img[o] = parseInt(c.substr(1, 2), 16); img[o + 1] = parseInt(c.substr(3, 2), 16); img[o + 2] = parseInt(c.substr(5, 2), 16);
    }
  });
});
// scale up and write a PNG (no packages: zlib + crc32)
const SW = W * Z, SH = H * Z, raw = Buffer.alloc((SW * 3 + 1) * SH);
for (let y = 0; y < SH; y++) {
  raw[y * (SW * 3 + 1)] = 0;
  for (let x = 0; x < SW; x++) img.copy(raw, y * (SW * 3 + 1) + 1 + x * 3, ((Math.floor(y / Z) * W) + Math.floor(x / Z)) * 3, ((Math.floor(y / Z) * W) + Math.floor(x / Z)) * 3 + 3);
}
const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc = b => { let c = -1; for (const v of b) c = crcT[(c ^ v) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(SW, 0); ihdr.writeUInt32BE(SH, 4); ihdr[8] = 8; ihdr[9] = 2;
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log(out, rows.map(r => r.label).join(', '));
