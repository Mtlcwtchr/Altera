// AL-95: the 2D client, stage 1 (gdd/ui/client_2d/CLIENT_2D.md): the sim's
// map from above in 16x16 tiles (1 tile = 1 m), people as layered 20x20 pawns
// that slide over the cells with a 1 px hop a step, selection with gold
// corners, the walk's dots and a card. Reads /api/static, /api/grid (64x64
// chunks), /api/state; sends /api/cmd. Every string is a ui2d.* key
// (content/loc/<lang>/ui2d.json) or a name the server looked up.
'use strict';
(() => {
  const T = 16, CH = 64, PAWN = 20;
  const cv = document.getElementById('map');
  const ctx = cv.getContext('2d');
  const $ = id => document.getElementById(id);

  // --- state ------------------------------------------------------------------
  let ST = null, MAP = null, RES = null, PAWN_PARTS = null, CLIFFS = {};   // /api/static, map2d.json, resources.json, pawns2d.json (LR-139), cliffs2d.json (LR-156)
  let UI = {};                               // ui2d.* strings
  const chunks = new Map();                  // "cx,cy" -> {json, canvas, glyph, at, str}
  const pending = new Set();
  let people = [], byId = new Map(), vis = new Map();   // vis: id -> {x, y, face, bob, moving}
  let clock = null, paused = false, tps = 2;
  let selId = null, selCard = null, hoverWorld = null, hoverId = null;
  let cam = { x: 0, y: 0 }, zoom = 2, follow = false, glyphs = false, roofMode = 'auto';
  let altDown = false, lastFrame = performance.now();
  const keys = new Set();
  // AL-96: play.js (leading a person) and the windows hook in here
  const hooks = { draw: [], poll: [], click: [], rclick: [], key: [], vis: [] };
  let me = null;                             // the player's person (id) or null: watching

  const L = (k, args) => {
    let s = UI[k] || k;
    if (args) for (const [a, v] of Object.entries(args)) s = s.split('{' + a + '}').join(v);
    return s;
  };
  const getJson = u => fetch(u, { cache: 'no-store' }).then(r => { if (!r.ok) throw new Error(u + ' ' + r.status); return r.json(); });
  const cmd = body => fetch('/api/cmd', { method: 'POST', body: JSON.stringify(body) }).then(r => r.json());

  // --- view math -----------------------------------------------------------------
  const dpr = () => Math.max(1, Math.round(window.devicePixelRatio || 1));
  const scale = () => zoom * dpr();                       // screen px per sprite px
  const tilePx = () => T * scale();
  function resize() {
    const r = cv.getBoundingClientRect();
    cv.width = Math.max(1, Math.floor(r.width * dpr()));
    cv.height = Math.max(1, Math.floor(r.height * dpr()));
  }
  const toScreen = (wx, wy) => [Math.round((wx - cam.x) * tilePx() + cv.width / 2), Math.round((wy - cam.y) * tilePx() + cv.height / 2)];
  const toWorld = (sx, sy) => [(sx - cv.width / 2) / tilePx() + cam.x, (sy - cv.height / 2) / tilePx() + cam.y];
  function eventWorld(e) {
    const r = cv.getBoundingClientRect();
    return toWorld((e.clientX - r.left) * dpr(), (e.clientY - r.top) * dpr());
  }

  // --- ground chunks ---------------------------------------------------------------
  function tileSprite(gname, v) {
    const list = (MAP.tiles && MAP.tiles[gname]) || ['tl_grass'];
    return list[v % list.length];
  }
  // LR-156: the cliff pieces a cell shows, bottom first: its higher neighbours' mask (1 N, 2 E, 4 S,
  // 8 W, 16 NE, 32 SE, 64 SW, 128 NW), the rock's style, a ramp (a road climbs) and the variant
  function cliffPieces(m, style, ramp, v) {
    const p = 'cf_' + style + '_', out = [];
    const side = (d) => out.push(ramp ? p + 'ramp_' + d : p + d + '_' + v);
    if (m & 4) side('s');
    else {
      if ((m & 32) && !(m & 2)) out.push(p + 'c_se');
      if ((m & 64) && !(m & 8)) out.push(p + 'c_sw');
    }
    if (m & 2) side('e');
    if (m & 8) side('w');
    if (m & 1) side('n');
    else {
      if ((m & 16) && !(m & 2)) out.push(p + 'c_ne');
      if ((m & 128) && !(m & 8)) out.push(p + 'c_nw');
    }
    return out;
  }
  const WALL = { log: ['n', 'N'], daub: ['w', 'e'], stone: ['W', 'M'] };
  function wallColours(piece) {
    for (const [m, c] of Object.entries(WALL)) if (piece.includes(m)) return c;
    return ['n', 'N'];
  }
  // AL-112: the symbol of a field / garden tile in the glyph mode
  function cropGlyph(name) {
    if (name.includes('path')) return null;
    if (name.includes('over')) return [',', 'T'];
    if (name.includes('ripe')) return ['"', 'y'];
    if (name.includes('tilled') || name.includes('sown')) return ['=', 'n'];
    if (name.includes('fallow') || name.includes('bare') || name.includes('stubble')) return [',', 't'];
    return ['"', 'l'];
  }
  function paintChunk(c) {
    const j = c.json, cvs = document.createElement('canvas');
    cvs.width = j.w * T; cvs.height = j.h * T;
    const g = cvs.getContext('2d');
    g.imageSmoothingEnabled = false;
    const names = ST.ground;
    for (let y = 0; y < j.h; y++) for (let x = 0; x < j.w; x++) {
      const i = y * j.w + x, gi = j.ground.charCodeAt(i) - 97, v = parseInt(j.var.substr(i * 2, 2), 16);
      const gname = names[gi] || 'grass';
      if (glyphs) {
        const [ch, col] = (MAP.glyph_ground && MAP.glyph_ground[gname]) || ['.', 'l'];
        g.fillStyle = '#100c08'; g.fillRect(x * T, y * T, T, T);
        g.fillStyle = Sprites.colour(col); g.globalAlpha = 0.55;
        g.font = 'bold 13px monospace'; g.textAlign = 'center'; g.textBaseline = 'middle';
        g.fillText(ch, x * T + 8, y * T + 9); g.globalAlpha = 1;
      } else {
        const sp = Sprites.get(tileSprite(gname, v));
        if (sp) {
          // turn grass-like tiles by the cell's hash so the ground has no grid
          const rot = (gname === 'grass' || gname === 'meadow' || gname === 'forest' || gname === 'marsh' || gname === 'sand') ? (v >> 4) & 3 : 0;
          if (rot) { g.save(); g.translate(x * T + 8, y * T + 8); g.rotate(rot * Math.PI / 2); g.drawImage(sp, -8, -8); g.restore(); }
          else g.drawImage(sp, x * T, y * T);
        }
      }
    }
    // LR-156: the relief: lower ground a little darker, then the cliffs of higher neighbours
    if (j.level && !glyphs) {
      const R = CLIFFS.rules || {}, low = R.shade_low ?? 0.16, per = R.shade_per_level ?? 0.02;
      g.fillStyle = '#000';
      for (let y = 0; y < j.h; y++) for (let x = 0; x < j.w; x++) {
        const a = low - per * (j.level.charCodeAt(y * j.w + x) - 48);
        if (a > 0.005) { g.globalAlpha = a; g.fillRect(x * T, y * T, T, T); }
      }
      g.globalAlpha = 1;
    }
    const drawCliffs = () => {
      if (!j.cliff) return;
      const R = CLIFFS.rules || {}, nv = R.variants || 1, gl = R.glyph || {};
      for (let y = 0; y < j.h; y++) for (let x = 0; x < j.w; x++) {
        const i = y * j.w + x, m = parseInt(j.cliff.substr(i * 2, 2), 16);
        if (!m) continue;
        const sc = j.cliff_style.charCodeAt(i), ramp = sc < 97, style = j.cliff_styles[sc - (ramp ? 65 : 97)] || 'stone';
        if (glyphs) {
          g.fillStyle = Sprites.colour(gl.colour || 'x'); g.globalAlpha = 0.8;
          g.font = 'bold 13px monospace'; g.textAlign = 'center'; g.textBaseline = 'middle';
          g.fillText(ramp ? '≡' : (m & 1 ? gl.n : m & 2 ? gl.e : m & 8 ? gl.w : gl.s) || '#', x * T + 8, y * T + 9);
          g.globalAlpha = 1;
          continue;
        }
        const v = parseInt(j.var.substr(i * 2, 2), 16) % nv;
        for (const nm of cliffPieces(m, style, ramp, v)) { const sp = Sprites.get(nm); if (sp) g.drawImage(sp, x * T, y * T); }
      }
    };
    // AL-112: fields and gardens over the ground (one char per cell, '.' none)
    if (j.crop && j.crop_tiles) for (let y = 0; y < j.h; y++) for (let x = 0; x < j.w; x++) {
      const ch = j.crop.charCodeAt(y * j.w + x);
      if (ch === 46) continue;
      const [name, rot] = j.crop_tiles[ch - 48] || [];
      if (!name) continue;
      if (glyphs) {
        const gl = cropGlyph(name);
        if (!gl) continue;
        g.fillStyle = '#100c08'; g.fillRect(x * T, y * T, T, T);
        g.fillStyle = Sprites.colour(gl[1]); g.globalAlpha = 0.7;
        g.font = 'bold 13px monospace'; g.textAlign = 'center'; g.textBaseline = 'middle';
        g.fillText(gl[0], x * T + 8, y * T + 9); g.globalAlpha = 1;
        continue;
      }
      const sp = Sprites.get(name);
      if (!sp) continue;
      if (rot) { g.save(); g.translate(x * T + 8, y * T + 8); g.rotate(rot * Math.PI / 2); g.drawImage(sp, -8, -8); g.restore(); }
      else g.drawImage(sp, x * T, y * T);
    }
    drawCliffs();
    // walls, doors, windows of the ground storey on the cell lines (4 px strips)
    for (const e of j.edges) {
      if (e.z !== 0 || e.kind === 'gable') continue;
      const [a, b] = wallColours(e.piece);
      const lx = (e.x - j.x0) * T, ly = (e.y - j.y0) * T, len = e.len * T;
      const rect = e.axis === 'h' ? [lx, ly - 2, len, 4] : [lx - 2, ly, 4, len];
      g.globalAlpha = e.built ? 1 : 0.45;
      if (glyphs) {
        g.fillStyle = e.kind === 'door' ? Sprites.colour('o') : Sprites.colour('w');
        g.fillRect(...rect);
      } else {
        g.fillStyle = '#1a1410'; g.fillRect(rect[0] - 1, rect[1] - 1, rect[2] + 2, rect[3] + 2);
        g.fillStyle = Sprites.colour(a); g.fillRect(...rect);
        g.fillStyle = Sprites.colour(b);
        if (e.axis === 'h') for (let k = 3; k < len; k += 6) g.fillRect(lx + k, ly - 2, 1, 4);
        else for (let k = 3; k < len; k += 6) g.fillRect(lx - 2, ly + k, 4, 1);
        if (e.kind === 'door') {
          // an opening with a plank door drawn ajar
          g.fillStyle = Sprites.colour('n');
          if (e.axis === 'h') { g.fillRect(lx + 6, ly - 2, len - 12, 4); g.fillStyle = '#1a1410'; g.fillRect(lx + 6, ly - 2, 1, 4); g.fillRect(lx + len - 7, ly - 2, 1, 4); g.fillStyle = Sprites.colour('t'); g.fillRect(lx + 8, ly - 1, len - 16, 2); }
          else { g.fillRect(lx - 2, ly + 6, 4, len - 12); g.fillStyle = '#1a1410'; g.fillRect(lx - 2, ly + 6, 4, 1); g.fillRect(lx - 2, ly + len - 7, 4, 1); g.fillStyle = Sprites.colour('t'); g.fillRect(lx - 1, ly + 8, 2, len - 16); }
        } else if (e.kind === 'window') {
          g.fillStyle = Sprites.colour('U');
          if (e.axis === 'h') g.fillRect(lx + 10, ly - 1, len - 20, 2); else g.fillRect(lx - 1, ly + 10, 2, len - 20);
        }
      }
      g.globalAlpha = 1;
    }
    c.canvas = cvs;
    c.mode = glyphs;
  }
  function requestChunk(cx, cy) {
    const k = cx + ',' + cy;
    if (pending.has(k)) return;
    pending.add(k);
    getJson(`/api/grid?x=${cx * CH}&y=${cy * CH}&w=${CH}&h=${CH}`).then(j => {
      const s = JSON.stringify(j);
      const old = chunks.get(k);
      if (old && old.str === s) { old.at = performance.now(); return; }
      const c = { json: j, str: s, at: performance.now() };
      paintChunk(c);
      chunks.set(k, c);
    }).catch(() => {}).finally(() => pending.delete(k));
  }
  function visibleChunks() {
    const [x0, y0] = toWorld(0, 0), [x1, y1] = toWorld(cv.width, cv.height);
    const out = [];
    for (let cy = Math.floor(y0 / CH); cy <= Math.floor(y1 / CH); cy++)
      for (let cx = Math.floor(x0 / CH); cx <= Math.floor(x1 / CH); cx++)
        if (cx >= 0 && cy >= 0 && cx * CH < ST.size && cy * CH < ST.size) out.push([cx, cy]);
    return out;
  }

  // --- things (trees, beds, forges...) ------------------------------------------------
  const hidden = new Set();
  function thingLook(type) {
    const o = (MAP.objects && MAP.objects[type]) || (RES && RES.objects && RES.objects[type]) || MAP.objects._default;
    return o;
  }
  const TREE = /tree|grove|pine/;

  // --- pawns from what the sim says the person wears and holds -----------------------
  function hash(n, salt) { let h = (Number(n) * 2654435761 + salt * 40503) >>> 0; h ^= h >>> 15; h = Math.imul(h, 2246822519) >>> 0; h ^= h >>> 13; return h >>> 0; }
  const HAIR = ['#5a3a20', '#2a1a10', '#7a5030', '#d8a848', '#a85a30', '#3a2818'];
  function famColour(p) {
    const list = MAP.family_colours || ['#8a6a48'];
    return list[hash(p.famId || 0, 3) % list.length];
  }
  function pawnSpec(p) {
    // LR-139: how the people one meets look (pawns2d.json npc), as window2d.cpp's pawnSpec
    const NPC = (PAWN_PARTS && PAWN_PARTS.npc) || {};
    const pickN = (k, i, fb) => { const v = NPC[k]; return Array.isArray(v) && v.length ? v[i % v.length] : fb; };
    const numN = (k, fb) => (typeof NPC[k] === 'number' ? NPC[k] : fb);
    const h = hash(p.id, 1), child = p.age < numN('child_age', 13), f = p.sex === 'f';
    const spec = { child };
    spec.body = f ? pickN('bodies_f', h, 'fem') : pickN('bodies_m', h, 'norm');
    spec.skin = pickN('skins', h >>> 3, 'light');
    let hc = pickN('hair_colours', h >>> 6, HAIR[0]);
    if (p.age >= numN('grey_age', 55)) hc = pickN('grey', 0, '#b0b0a8');
    spec.hair = [f ? pickN('hair_f', h >>> 9, 'long') : pickN('hair_m', h >>> 9, 'short'), hc];
    if (!f && !child && p.age >= numN('beard_age', 18) && ((h >>> 12) % Math.max(1, numN('beard_one_in', 3)))) spec.beard = true;
    if (p.rskin) spec.skin = p.rskin;          // AL-114: the kind's skin
    if (p.stone) spec.skin = pickN('stone', 0, '#8c8c86');
    // LR-139: the kind's look (species body.pawn), as pixels2d.cpp's pawnSpec
    const rl = p.rlook || {};
    for (const k of ['ears', 'head', 'nose', 'brow', 'eyes', 'tusks', 'skin_pattern']) if (rl[k]) spec[k] = rl[k];
    if (rl.build && !child) spec.build = rl.build;
    if (child && spec.tusks === 'yes') spec.tusks = 'small';
    if (rl.beard === 'never') spec.beard = false;
    if (rl.beard === 'rare') spec.beard = spec.beard && ((h >>> 18) % Math.max(1, numN('rare_one_in', 4))) === 0;
    if (rl.beard === 'long' && !f && !child) spec.beard = spec.long_beard = true;
    if (rl.hair === 'none' || rl.hair === 'tuft') spec.hair[0] = rl.hair;
    // LR-139: the kind's cuts and beards, picked by the id (pixels2d.cpp's pawnSpec does the same)
    const pick = (set, k) => { const v = (set || '').split(/\s+/).filter(Boolean); return v.length ? v[k % v.length] : ''; };
    const h2 = hash(p.id, 5);
    if (!child) { const cut = pick(rl[f ? 'hair_f' : 'hair_m'], h2); if (cut) spec.hair[0] = cut; }
    if (spec.beard) { const bs = pick(rl.beards, h2 >>> 8); if (!bs && rl.beard !== 'long') spec.beard = false; else spec.beard_style = bs; }
    if (p.look) { // AL-136: the look chosen at creation
      const lk = p.look;
      if (lk.body && !child) spec.body = lk.body;
      if (lk.skin && !p.stone) spec.skin = lk.skin;
      if (lk.hair) spec.hair[0] = lk.hair;
      if (lk.hair_colour) spec.hair[1] = lk.hair_colour;
      if (lk.beard) { spec.beard = !f && !child && lk.beard !== 'no'; spec.beard_style = lk.beard === 'yes' || lk.beard === 'no' ? '' : lk.beard; spec.long_beard = spec.beard_style === 'long'; }
    }
    const wear = [];
    const wornKinds = new Set();
    for (const it of p.worn || []) {
      const w = MAP.wear && MAP.wear[it];
      if (!w) continue;
      // AL-109: a woman's tunic is a gown, "family" is the household's colour, a dyed thing its dye's
      const layer = f && w.layer_f ? w.layer_f : w.layer;
      let col = w.colour === 'family' ? famColour(p) : w.colour;
      if (p.dye && p.dye[it] && MAP.dyes && MAP.dyes[p.dye[it]]) col = MAP.dyes[p.dye[it]];
      wornKinds.add(layer);
      if (layer === 'cloak') spec.cloak = col;
      else if (layer === 'cap' || layer === 'helmet' || layer === 'hood') spec.hat = [layer, col];
      else wear.push([layer, col]);
    }
    // LR-139: only what the sim says is worn; nothing on the torso: the pawn is bare
    const naked = !['shirt', 'robe', 'mail', 'tabard'].some(k => wornKinds.has(k));
    const order = ['shirt', 'robe', 'mail', 'pants', 'boots', 'tabard', 'apron', 'belt', 'bag', 'pack'];
    wear.sort((a, b) => order.indexOf(a[0]) - order.indexOf(b[0]));
    spec.wear = wear;
    if (!spec.hat && f && !child && !naked && ((h >>> 15) % 100) < numN('kerchief_pct', 40)) spec.hat = ['kerchief', pickN('kerchief', 0, '#e8e0d0')];
    // AL-109: the sim says what is in hand now (the act's tool, else a carried weapon)
    for (const [pat, tool] of MAP.tools || []) if (p.held && p.held.includes(pat)) { spec.tool = tool; break; }
    return spec;
  }
  // beasts: a small blob in the colour of the kind
  const BEAST = { hare: '#a88868', deer: '#8a5a30', boar: '#4a3a30', wolf: '#7a7a78', hens: '#e8e0d0', goats: '#c8c0b0', pig: '#e0a090', fish_shoal: '#6aa8e0' };
  const beastCache = new Map();
  function beastSprite(sp, face) {
    const k = sp + face;
    if (beastCache.has(k)) return beastCache.get(k);
    const big = sp === 'deer' || sp === 'boar' || sp === 'wolf' || sp === 'pig' || sp === 'goats';
    const c = document.createElement('canvas'); c.width = c.height = 16;
    const g = c.getContext('2d');
    const col = BEAST[sp] || '#8a6a48', rx = big ? 5 : 3.5, ry = big ? 3.2 : 2.5, cx = 8, cy = 10;
    for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
      const a = (x + 0.5 - cx) / rx, b = (y + 0.5 - cy) / ry, d = a * a + b * b;
      const ah = (x + 0.5 - (cx + (face === 'W' ? -rx : rx))) / 1.8, bh = (y + 0.5 - (cy - 1.5)) / 1.6;
      const head = ah * ah + bh * bh <= 1;
      if (d <= 1 || head) { g.fillStyle = d > 0.6 && !head ? mulHex(col, 0.75) : col; g.fillRect(x, y, 1, 1); }
      else if (d <= 1.45 || ah * ah + bh * bh <= 1.6) { g.fillStyle = '#1a1410'; g.fillRect(x, y, 1, 1); }
    }
    beastCache.set(k, c);
    return c;
  }
  function mulHex(c, k) { return '#' + [1, 3, 5].map(i => Math.round(parseInt(c.substr(i, 2), 16) * k).toString(16).padStart(2, '0')).join(''); }
  function actIcon(p) {
    if (!p.alive) return null;
    const ia = p.ia || '';
    if (p.st === 'Sleep') return 'ic_sleep';
    for (const [re, sp] of MAP.actsRe || []) if (re.test(ia)) return sp;
    return null;
  }

  // --- smoothing: pawns slide over the cells toward where the sim puts them ---------
  function stepVis(dt) {
    for (const p of people) {
      let v = vis.get(p.id);
      if (!v) { v = { x: p.x, y: p.y, face: 'S', bob: 0, moving: false }; vis.set(p.id, v); }
      // AL-108: standing still, one looks where the sim turned one (at the act's target)
      const lookSim = () => { if (p.fx || p.fy) v.face = Math.abs(p.fx) > Math.abs(p.fy) ? (p.fx > 0 ? 'E' : 'W') : (p.fy > 0 ? 'S' : 'N'); };
      if (v.driven) { if (!v.moving) lookSim(); continue; }   // the player's person walking under the player's hand (play.js)
      const dx = p.x - v.x, dy = p.y - v.y, d = Math.hypot(dx, dy);
      if (d > 80) { v.x = p.x; v.y = p.y; v.moving = false; lookSim(); continue; }
      if (d < 0.02) { v.moving = false; lookSim(); continue; }
      const speed = Math.max(3.5, d * 1.6);                  // tiles per second
      const s = Math.min(d, speed * dt);
      const cellBefore = Math.floor(v.x) + Math.floor(v.y);
      v.x += dx / d * s; v.y += dy / d * s;
      v.walked = (v.walked || 0) + s;   // LR-139: the step of the walk cycle
      v.moving = true;
      if (Math.floor(v.x) + Math.floor(v.y) !== cellBefore) v.bob ^= 1;   // a hop a cell
      v.face = Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'E' : 'W') : (dy > 0 ? 'S' : 'N');
    }
  }

  // --- roofs: lifted over the house of the selected or hovered person, or by the toggle --
  function openBuildings() {
    const open = new Set();
    const mark = (x, y) => {
      for (const c of chunks.values())
        for (const r of c.json.rooms) if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) open.add(r.building);
    };
    if (selId != null && vis.get(selId)) { const v = vis.get(selId); mark(v.x, v.y); }
    if (hoverWorld) mark(hoverWorld[0], hoverWorld[1]);
    return open;
  }

  // --- drawing -------------------------------------------------------------------------
  function draw() {
    const now = performance.now(), dt = Math.min(0.1, (now - lastFrame) / 1000);
    lastFrame = now;
    if (!ST) { requestAnimationFrame(draw); return; }
    // camera keys
    const pan = 18 * dt * (keys.has('Shift') ? 3 : 1) / Math.max(1, zoom * 0.6);
    const wasd = me == null;   // leading a person, WASD walks them (play.js)
    if ((wasd && keys.has('a')) || keys.has('ArrowLeft')) cam.x -= pan * 3, follow = false;
    if ((wasd && keys.has('d')) || keys.has('ArrowRight')) cam.x += pan * 3, follow = false;
    if ((wasd && keys.has('w')) || keys.has('ArrowUp')) cam.y -= pan * 3, follow = false;
    if ((wasd && keys.has('s')) || keys.has('ArrowDown')) cam.y += pan * 3, follow = false;
    stepVis(dt);
    for (const h of hooks.vis) h(dt);
    const fid = follow ? (me != null ? me : selId) : null;
    if (fid != null && vis.get(fid)) { const v = vis.get(fid); cam.x += (v.x - cam.x) * Math.min(1, dt * 6); cam.y += (v.y - cam.y) * Math.min(1, dt * 6); }
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = glyphs ? '#100c08' : '#1a2a12';
    ctx.fillRect(0, 0, cv.width, cv.height);
    const sc = scale(), tp = tilePx();
    const vc = visibleChunks();
    for (const [cx, cy] of vc) {
      const c = chunks.get(cx + ',' + cy);
      if (!c) { requestChunk(cx, cy); continue; }
      if (c.mode !== glyphs) paintChunk(c);
      if (now - c.at > 8000) requestChunk(cx, cy);   // things change: felled trees, new pieces
      const [sx, sy] = toScreen(c.json.x0, c.json.y0);
      ctx.drawImage(c.canvas, sx, sy, c.json.w * tp, c.json.h * tp);
    }
    // drawables sorted by their foot: things, pawns
    const [wx0, wy0] = toWorld(-tp * 2, -tp * 2), [wx1, wy1] = toWorld(cv.width + tp * 2, cv.height + tp * 2);
    const items = [];
    for (const [cx, cy] of vc) {
      const c = chunks.get(cx + ',' + cy);
      if (!c) continue;
      for (const t of c.json.things) {
        const [id, type, x, y, storey] = t;
        if (storey > 0 || hidden.has(type)) continue;
        if (x < c.json.x0 || x >= c.json.x0 + c.json.w || y < c.json.y0 || y >= c.json.y0 + c.json.h) continue;
        if (x < wx0 || x > wx1 || y < wy0 || y > wy1) continue;
        const thin = MAP.thin && MAP.thin[type];
        if (thin > 1 && id % thin) continue;
        items.push({ y, kind: 't', id, type, x });
      }
    }
    for (const p of people) {
      const v = vis.get(p.id);
      if (!v || v.x < wx0 || v.x > wx1 || v.y < wy0 || v.y > wy1) continue;
      items.push({ y: v.y, kind: 'p', p, v });
    }
    items.sort((a, b) => a.y - b.y);
    for (const it of items) {
      if (it.kind === 't') drawThing(it, sc);
      else drawPawn(it.p, it.v, sc);
    }
    drawRoofs(sc, tp);
    drawRoomLabels(tp);
    drawSelection(sc, tp);
    drawOverheads(sc, tp);
    for (const h of hooks.draw) h(ctx, sc, tp);
    drawNight();
    requestAnimationFrame(draw);
  }
  function drawThing(it, sc) {
    const look = thingLook(it.type);
    const [sx, sy] = toScreen(it.x, it.y);
    if (glyphs) {
      ctx.fillStyle = Sprites.colour(look.colour || 'w');
      ctx.font = `bold ${13 * sc}px monospace`; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText(look.glyph || '?', sx, sy);
      return;
    }
    const sp = Sprites.get(look.sprite, look.recolour);
    if (!sp) return;
    const tree = TREE.test(it.type) || look.sprite === 'ob_tree' || look.sprite === 'ob_pine';
    // trees stand in a light 3/4: their foot on the spot, crown above; the rest lie flat, centred
    if (tree) {
      ctx.fillStyle = 'rgba(0,0,0,0.22)';
      ctx.beginPath(); ctx.ellipse(sx, sy - sc, 6 * sc, 2.5 * sc, 0, 0, Math.PI * 2); ctx.fill();
      ctx.drawImage(sp, sx - 8 * sc, sy - 15 * sc, 16 * sc, 16 * sc);
    } else ctx.drawImage(sp, sx - 8 * sc, sy - 8 * sc, 16 * sc, 16 * sc);
  }
  function drawPawn(p, v, sc) {
    const [sx, sy] = toScreen(v.x, v.y);
    if (glyphs) {
      ctx.fillStyle = p.sp ? Sprites.colour('t') : (p.id === selId ? '#ffe080' : famColour(p));
      ctx.font = `bold ${14 * sc}px monospace`; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.fillText(p.sp ? (p.name || p.sp)[0] : (p.alive ? (p.rg || '@') : '%'), sx, sy);  // AL-114: the kind's glyph
      return;
    }
    // shadow
    ctx.fillStyle = 'rgba(0,0,0,0.25)';
    ctx.beginPath(); ctx.ellipse(sx, sy + 1 * sc, (p.sp ? 4 : 5) * sc, 2 * sc, 0, 0, Math.PI * 2); ctx.fill();
    const lying = !p.alive || p.down || p.st === 'Sleep' || p.st === 'Incapacitated';
    if (p.sp) {
      const b = beastSprite(p.sp, v.face === 'W' ? 'W' : 'E');
      ctx.drawImage(b, sx - 8 * sc, sy - 11 * sc, 16 * sc, 16 * sc);
      return;
    }
    let spec = p._spec;
    if (!spec || p._specKey !== p._lookKey) { spec = p._spec = pawnSpec(p); p._specKey = p._lookKey; }
    // LR-139: the walk cycle, a blow at work, a breath and a blink standing
    const img = Pawns.sprite(spec, lying ? 'S' : v.face, lying ? 0 : Pawns.pose(v.moving, v.walked || 0, p.st === 'Work', performance.now(), p.id));
    // the 20x20 canvas sits over the cell: feet on the spot, head into the cell above
    if (lying) {
      ctx.save(); ctx.translate(sx, sy - 3 * sc); ctx.rotate(-Math.PI / 2);
      if (!p.alive) ctx.globalAlpha = 0.6;
      ctx.drawImage(img, -10 * sc, -10 * sc, PAWN * sc, PAWN * sc); ctx.restore();
    } else ctx.drawImage(img, sx - 10 * sc, sy - 18 * sc, PAWN * sc, PAWN * sc);
  }
  function drawRoofs(sc, tp) {
    if (roofMode === 'none') return;
    const open = roofMode === 'auto' ? openBuildings() : new Set();
    const roof = Sprites.get('tl_roof'), seen = new Set();
    for (const c of chunks.values()) for (const r of c.json.roofs) {
      if (open.has(r.building) && r.building >= 0) continue;
      const k = r.x + ',' + r.y;
      if (seen.has(k)) continue;
      seen.add(k);
      const [sx, sy] = toScreen(r.x, r.y);
      if (sx > cv.width || sy > cv.height || sx + r.len * tp < 0 || sy + r.len * tp < 0) continue;
      if (glyphs) { ctx.fillStyle = 'rgba(168,40,32,0.55)'; ctx.fillRect(sx, sy, r.len * tp, r.len * tp); continue; }
      for (let dy = 0; dy < r.len; dy++) for (let dx = 0; dx < r.len; dx++)
        ctx.drawImage(roof, sx + dx * tp, sy + dy * tp, tp, tp);
    }
  }
  function drawRoomLabels(tp) {
    if (zoom < 2) return;
    const open = roofMode === 'none' ? null : (roofMode === 'auto' ? openBuildings() : new Set());
    const done = new Set();
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    for (const c of chunks.values()) for (const r of c.json.rooms) {
      if (r.z !== 0 || done.has(r.id)) continue;
      done.add(r.id);
      if (open && !open.has(r.building)) continue;
      const [sx, sy] = toScreen((r.x0 + r.x1) / 2, r.y0 + 0.55);
      const name = (ST.rooms && ST.rooms[r.type]) || r.type;
      ctx.font = `600 ${Math.round(5.5 * scale())}px Alegreya, serif`;
      const w = ctx.measureText(name).width + 8 * dpr();
      ctx.fillStyle = 'rgba(232,216,176,0.88)'; ctx.fillRect(sx - w / 2, sy - 5 * scale(), w, 10 * scale());
      ctx.fillStyle = '#3a2414'; ctx.fillText(name, sx, sy);
    }
  }
  function drawSelection(sc, tp) {
    const p = byId.get(selId), v = vis.get(selId);
    if (!p || !v) return;
    const [sx, sy] = toScreen(v.x, v.y);
    // the walk: gold dots a cell apart to where the sim sends them
    if (p.dest) {
      const [dx, dy] = p.dest, d = Math.hypot(dx - v.x, dy - v.y);
      ctx.fillStyle = '#e8c060';
      for (let k = 1; k < d && k < 400; k += 1) {
        const [px, py] = toScreen(v.x + (dx - v.x) * k / d, v.y + (dy - v.y) * k / d);
        ctx.fillRect(px - sc, py - sc, 2 * sc, 2 * sc);
      }
      const [ex, ey] = toScreen(dx, dy);
      ctx.strokeStyle = '#e8c060'; ctx.lineWidth = sc;
      ctx.strokeRect(ex - 4 * sc, ey - 4 * sc, 8 * sc, 8 * sc);
    }
    // gold corners round the pawn's canvas
    const x0 = sx - 11 * sc, y0 = sy - 19 * sc, w = 22 * sc, h = 22 * sc, l = 5 * sc;
    ctx.fillStyle = '#1a1410';
    const corner = (x, y, ax, ay, col) => {
      ctx.fillStyle = col;
      ctx.fillRect(x, y, ax * l, sc); ctx.fillRect(x, y, sc, ay * l);
    };
    for (const [off, col] of [[sc, '#1a1410'], [0, '#e8c060']]) {
      corner(x0 + off, y0 + off, 1, 1, col);
      ctx.fillStyle = col;
      ctx.fillRect(x0 + w - l + off, y0 + off, l, sc); ctx.fillRect(x0 + w - sc + off, y0 + off, sc, l);
      ctx.fillRect(x0 + off, y0 + h - sc + off, l, sc); ctx.fillRect(x0 + off, y0 + h - l + off, sc, l);
      ctx.fillRect(x0 + w - l + off, y0 + h - sc + off, l, sc); ctx.fillRect(x0 + w - sc + off, y0 + h - l + off, sc, l);
    }
  }
  function drawOverheads(sc, tp) {
    ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
    for (const p of people) {
      if (p.sp) continue;
      const v = vis.get(p.id);
      if (!v) continue;
      const [sx, sy] = toScreen(v.x, v.y);
      if (sx < -50 || sy < -50 || sx > cv.width + 50 || sy > cv.height + 50) continue;
      const sel = p.id === selId, hov = p.id === hoverId;
      // the act's icon in a paper square over the head (at x2 and up for all, else for the chosen)
      const icon = actIcon(p);
      if (icon && !glyphs && (zoom >= 2 || sel || hov)) {
        const s = Math.max(1, Math.floor(sc / 2)) ;
        const box = 18 * s, bx = sx - box / 2, by = sy - 20 * sc - box - 2 * s;
        ctx.fillStyle = '#1a1410'; ctx.fillRect(bx - s, by - s, box + 2 * s, box + 2 * s);
        ctx.fillStyle = '#e8d8b0'; ctx.fillRect(bx, by, box, box);
        const sp = Sprites.get(icon);
        if (sp) ctx.drawImage(sp, bx + s, by + s, 16 * s, 16 * s);
      }
      if (altDown || sel || hov) {
        ctx.font = `600 ${Math.round(6 * Math.max(2, sc))}px Alegreya, serif`;
        const y = sy - 20 * sc - (icon && !glyphs && (zoom >= 2 || sel || hov) ? 20 * Math.max(1, Math.floor(sc / 2)) + 2 : 0);
        ctx.fillStyle = '#000'; ctx.fillText(p.name, sx + 1, y + 1);
        ctx.fillStyle = sel ? '#e8c060' : '#efe2c4'; ctx.fillText(p.name, sx, y);
      }
    }
  }
  function drawNight() {
    if (!clock) return;
    const [hh, mm] = (clock.time || '12:00').split(':').map(Number);
    const h = hh + mm / 60;
    // dark from 21 to 5, dusk and dawn an hour and a half each
    let a = 0;
    if (h >= 21 || h < 4.5) a = 0.55; else if (h >= 19.5) a = 0.55 * (h - 19.5) / 1.5; else if (h < 6) a = 0.55 * (6 - h) / 1.5;
    if (a > 0) { ctx.fillStyle = `rgba(14,18,48,${a})`; ctx.fillRect(0, 0, cv.width, cv.height); }
    if (clock.weather && clock.weather.rain > 0.5 && !glyphs) {
      ctx.strokeStyle = 'rgba(170,190,220,0.35)'; ctx.lineWidth = dpr();
      const t = performance.now() / 40;
      ctx.beginPath();
      for (let i = 0; i < 160; i++) {
        const x = (hash(i, 9) % cv.width + t * 3) % cv.width, y = (hash(i, 5) % cv.height + t * 9) % cv.height;
        ctx.moveTo(x, y); ctx.lineTo(x - 4 * dpr(), y + 10 * dpr());
      }
      ctx.stroke();
    }
  }

  // --- the side column: households and the card ---------------------------------------
  function bar(v, colour) { return `<div class="bar"><i style="width:${Math.round(Math.max(0, Math.min(1, v)) * 100)}%;background:${colour}"></i></div>`; }
  const tone = v => (v < 0.3 ? 'var(--bad)' : v < 0.6 ? 'var(--mid)' : 'var(--good)');
  const esc = s => String(s == null ? '' : s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  function portrait(p, px) {
    const c = document.createElement('canvas'); c.width = c.height = PAWN;
    const g = c.getContext('2d');
    g.drawImage(Pawns.sprite(pawnSpec(p), 'S', 0), 0, 0);
    c.style.width = c.style.height = px + 'px';
    return c;
  }
  let housesKey = '';
  function renderHouses() {
    const humans = people.filter(p => !p.sp);
    const key = humans.map(p => p.id + p.alive + (p.id === selId)).join(',');
    if (key === housesKey) { updateHouseBars(humans); return; }
    housesKey = key;
    const fams = new Map();
    for (const p of humans) { const k = p.fam || '—'; if (!fams.has(k)) fams.set(k, []); fams.get(k).push(p); }
    const box = $('houses');
    box.innerHTML = '';
    for (const [name, list] of fams) {
      const f = document.createElement('div'); f.className = 'fam';
      f.innerHTML = `<div class="fname">${esc(name)}</div>`;
      for (const p of list.sort((a, b) => b.age - a.age)) {
        const r = document.createElement('div'); r.className = 'who' + (p.id === selId ? ' sel' : ''); r.dataset.id = p.id;
        r.appendChild(portrait(p, 40));
        const n = document.createElement('div'); n.className = 'nm';
        n.innerHTML = `${esc(p.name)}<small>${esc(p.act || p.goal || L('ui2d.idle'))}</small>`;
        r.appendChild(n);
        const m = document.createElement('div'); m.className = 'mini'; m.innerHTML = `<i style="width:${p.hp * 100}%;background:${tone(p.hp)}"></i>`;
        r.appendChild(m);
        r.onclick = () => { select(p.id); const v = vis.get(p.id); if (v) { cam.x = v.x; cam.y = v.y; } };
        f.appendChild(r);
      }
      box.appendChild(f);
    }
  }
  function updateHouseBars(humans) {
    for (const p of humans) {
      const r = document.querySelector(`.who[data-id="${p.id}"]`);
      if (!r) continue;
      const s = r.querySelector('small'); if (s) s.textContent = p.act || p.goal || L('ui2d.idle');
    }
  }
  let cardKey = '';
  function renderCard() {
    const box = $('card');
    const d = selCard, p = byId.get(selId);
    if (!d || !p) { if (cardKey !== 'none') { box.innerHTML = `<div class="parch">${esc(L('ui2d.nobody'))}</div>`; cardKey = 'none'; } return; }
    const key = JSON.stringify(d) + '|' + me;
    if (key === cardKey) return;
    cardKey = key;
    let h = `<div class="head"><span id="cardpic"></span><div class="t"><div class="n">${esc(d.name)}</div>` +
      `<div class="sub">${esc(d.fam || d.spn || '')}${d.age != null ? ' · ' + esc(L('ui2d.age', { n: d.age })) : ''}</div></div></div>`;
    if (!d.alive) h += `<div class="parch">${esc(L('ui2d.dead'))}${d.cause ? ': ' + esc(d.cause) : ''}</div>`;
    if (!d.sp && d.alive && (d.id === me || d.lead !== false)) h += `<div class="cardbtns"><button class="btn wide" id="b-lead">${esc(L(d.id === me ? 'ui2d.release' : 'ui2d.lead'))}</button></div>`;
    h += `<div class="parch"><b>${esc(L('ui2d.doing'))}:</b> ${esc(d.act || L('ui2d.idle'))}` + (d.goal ? `<br><b>${esc(L('ui2d.wants'))}:</b> ${esc(d.goal)}` : '') + '</div>';
    h += `<div class="row"><span>${esc(L('ui2d.health'))}</span>${bar(d.hp, tone(d.hp))}<span>${Math.round(d.hp * 100)}</span></div>`;
    const mood = (d.mood + 1) / 2;
    h += `<div class="row"><span>${esc(L('ui2d.mood'))}</span>${bar(mood, tone(mood))}<span>${Math.round(d.mood * 100)}</span></div>`;
    h += `<div class="row"><span>${esc(L('ui2d.fatigue'))}</span>${bar(d.fatigue, tone(1 - d.fatigue))}<span>${Math.round(d.fatigue * 100)}</span></div>`;
    if (d.needs && d.needs.length) {
      h += `<h3>${esc(L('ui2d.needs'))}</h3>`;
      for (const n of d.needs) h += `<div class="row"><span>${esc(n.name)}</span>${bar(n.v, tone(n.v))}<span>${Math.round(n.v * 100)}</span></div>`;
    }
    if (d.worn && d.worn.length) h += `<h3>${esc(L('ui2d.wears'))}</h3><div class="chips">${d.worn.map(w => `<span class="chip">${esc(w.name)}</span>`).join('')}</div>`;
    if (d.inv && d.inv.length) h += `<h3>${esc(L('ui2d.carries'))}</h3><div class="chips">${d.inv.map(i => `<span class="chip">${esc(i.name)}${i.n > 1 ? ' ×' + i.n : ''}</span>`).join('')}</div>`;
    if (d.traits && d.traits.length) h += `<h3>${esc(L('ui2d.traits'))}</h3><div class="chips">${d.traits.map(t => `<span class="chip">${esc(t)}</span>`).join('')}</div>`;
    if (d.trace && d.trace.length) h += `<h3>${esc(L('ui2d.thoughts'))}</h3><div class="parch trace">${d.trace.map(t => `<div><b>${esc(t.t)}</b>${esc(t.text)}</div>`).join('')}</div>`;
    box.innerHTML = h;
    if (!p.sp) $('cardpic').appendChild(portrait(p, 80));
    const lead = $('b-lead');
    if (lead) lead.onclick = () => cmd(d.id === me ? { op: 'cmd', type: 'release_control', args: {} } : { op: 'cmd', type: 'take_control', args: { id: d.id } }).then(() => { cardKey = ''; poll(); });
  }
  function renderBar() {
    if (!clock) return;
    const w = clock.weather;
    $('clock').textContent = `${L('ui2d.day', { n: clock.day + 1 })} · ${L('ui2d.season.' + clock.season)} · ${clock.time}` +
      (w ? ` · ${Math.round(w.t)}° ${w.rain > 0.5 ? L('ui2d.rain') : L('ui2d.dry')}` : '');
    $('b-pause').textContent = paused ? '▶' : '❚❚';
    $('b-pause').classList.toggle('on', paused);
    $('speed').textContent = paused ? L('ui2d.paused') : L('ui2d.speed', { n: tps });
    $('b-zoom').textContent = L('ui2d.zoom', { n: zoom });
    $('b-roofs').textContent = L('ui2d.roofs.' + roofMode);
    $('b-mode').textContent = L(glyphs ? 'ui2d.mode.glyphs' : 'ui2d.mode.sprites');
    $('b-follow').textContent = L('ui2d.follow');
    $('b-follow').classList.toggle('on', follow);
  }
  function renderHint() {
    let s = '';
    const p = byId.get(hoverId);
    if (p) {
      s = L('ui2d.hint.person', { name: p.name, act: p.act || p.goal || L('ui2d.idle') });
      if (p.dest) s += ' · ' + L('ui2d.hint.walk', { m: Math.round(Math.hypot(p.dest[0] - p.x, p.dest[1] - p.y)) });
    } else if (hoverWorld) {
      const [x, y] = hoverWorld;
      let thing = null, room = null;
      for (const c of chunks.values()) {
        for (const t of c.json.things) if (!hidden.has(t[1]) && Math.abs(t[2] - x) < 0.6 && Math.abs(t[3] - y) < 0.6) thing = t;
        for (const r of c.json.rooms) if (r.z === 0 && x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) room = r;
      }
      const parts = [];
      if (thing) parts.push(L('ui2d.hint.thing', { name: (ST.objects && ST.objects[thing[1]]) || thing[1] }));
      if (room) parts.push(L('ui2d.hint.room', { room: (ST.rooms && ST.rooms[room.type]) || room.type }));
      s = parts.join(' · ');
    }
    $('hint').textContent = s || L('ui2d.hint.keys');
  }

  // --- selection and input -------------------------------------------------------------
  function pickPerson(wx, wy) {
    let best = null, bd = 1.1;
    for (const p of people) {
      const v = vis.get(p.id);
      if (!v) continue;
      // the pawn stands on its spot, its body a cell up
      const d = Math.hypot(v.x - wx, (v.y - 0.5) - wy);
      if (d < bd || (d === bd && !p.sp)) { bd = d; best = p; }
    }
    return best;
  }
  function select(id) {
    selId = id; selCard = null; cardKey = ''; housesKey = '';
    poll();
  }
  let drag = null;
  cv.addEventListener('mousedown', e => {
    if (e.button === 1 || (e.button === 2 && e.shiftKey)) { drag = { x: e.clientX, y: e.clientY, cx: cam.x, cy: cam.y }; e.preventDefault(); return; }
    if (e.button === 2) {
      const w = eventWorld(e);
      for (const h of hooks.rclick) if (h(e, w, pickPerson(...w))) return;
    }
  });
  window.addEventListener('mouseup', e => {
    if (drag && (e.button === 1 || e.button === 2)) { drag = null; return; }
  });
  cv.addEventListener('contextmenu', e => e.preventDefault());
  cv.addEventListener('mousemove', e => {
    if (drag) {
      cam.x = drag.cx - (e.clientX - drag.x) * dpr() / tilePx();
      cam.y = drag.cy - (e.clientY - drag.y) * dpr() / tilePx();
      follow = false;
      return;
    }
    hoverWorld = eventWorld(e);
    const p = pickPerson(...hoverWorld);
    hoverId = p ? p.id : null;
  });
  cv.addEventListener('mouseleave', () => { hoverWorld = null; hoverId = null; });
  cv.addEventListener('click', e => {
    const w = eventWorld(e), p = pickPerson(...w);
    for (const h of hooks.click) if (h(e, w, p)) return;
    select(p ? p.id : null);
  });
  cv.addEventListener('wheel', e => {
    e.preventDefault();
    const before = eventWorld(e);
    zoom = Math.max(1, Math.min(3, zoom + (e.deltaY < 0 ? 1 : -1)));
    const after = eventWorld(e);
    cam.x += before[0] - after[0]; cam.y += before[1] - after[1];
  }, { passive: false });
  window.addEventListener('keydown', e => {
    if (e.key === 'Alt') { altDown = true; e.preventDefault(); }
    keys.add(e.key.length === 1 ? e.key.toLowerCase() : e.key);
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA')) return;
    for (const h of hooks.key) if (h(e)) return;
    if (e.key === ' ') { e.preventDefault(); togglePause(); }
    if (e.key === 'z' || e.key === 'Z') zoom = zoom % 3 + 1;
    if (e.key === 'r' || e.key === 'R') cycleRoofs();
    if (e.key === 'g' || e.key === 'G') glyphs = !glyphs;
    if (e.key === 'f' || e.key === 'F') follow = !follow && (selId != null || me != null);
    if (e.key === 'Escape') select(null);
    if (e.key === '-') setSpeed(-1);
    if (e.key === '=' || e.key === '+') setSpeed(1);
  });
  window.addEventListener('keyup', e => { if (e.key === 'Alt') altDown = false; keys.delete(e.key.length === 1 ? e.key.toLowerCase() : e.key); });
  window.addEventListener('blur', () => { keys.clear(); altDown = false; });
  const SPEEDS = [0.5, 1, 2, 4, 8, 16, 48];
  function setSpeed(dir) {
    let i = SPEEDS.findIndex(s => s >= tps);
    if (i < 0) i = SPEEDS.length - 1;
    i = Math.max(0, Math.min(SPEEDS.length - 1, i + dir));
    tps = SPEEDS[i];
    cmd({ op: 'speed', tps });
  }
  function togglePause() { paused = !paused; cmd({ op: 'pause', on: paused }); renderBar(); }
  function cycleRoofs() { roofMode = { auto: 'all', all: 'none', none: 'auto' }[roofMode]; }
  $('b-pause').onclick = togglePause;
  $('b-slower').onclick = () => setSpeed(-1);
  $('b-faster').onclick = () => setSpeed(1);
  $('b-zoom').onclick = () => { zoom = zoom % 3 + 1; };
  $('b-roofs').onclick = cycleRoofs;
  $('b-mode').onclick = () => { glyphs = !glyphs; };
  $('b-follow').onclick = () => { follow = !follow && (selId != null || me != null); };
  window.addEventListener('resize', resize);

  // --- polling -----------------------------------------------------------------------------
  let polling = false;
  function poll() {
    if (polling || !ST) return;
    polling = true;
    const r = Math.ceil(Math.hypot(cv.width, cv.height) / tilePx()) + 30;
    const q = `/api/state?vx=${Math.round(cam.x)}&vy=${Math.round(cam.y)}&vr=${r}` + (selId != null ? `&sel=${selId}` : '');
    getJson(q).then(s => {
      clock = s; paused = s.paused; tps = s.tps;
      if (s.me && s.me !== me) follow = true;
      me = s.me || null;
      people = s.people;
      byId = new Map(people.map(p => [p.id, p]));
      for (const p of people) p._lookKey = (p.worn || []).join() + '|' + (p.held || '') + '|' + JSON.stringify(p.dye || {}) + p.age;
      for (const [id, p] of byId) { const old = oldSpecs.get(id); if (old && old.k === p._lookKey) { p._spec = old.s; p._specKey = old.k; } }
      oldSpecs.clear();
      for (const p of people) if (p._spec) oldSpecs.set(p.id, { k: p._specKey, s: p._spec });
      for (const id of vis.keys()) if (!byId.has(id)) vis.delete(id);
      selCard = s.sel || null;
      if (selId != null && !byId.has(selId)) selId = null;
      renderHouses(); renderCard(); renderBar();
      for (const h of hooks.poll) h(s);
    }).catch(e => console.warn(e)).finally(() => { polling = false; });
  }
  const oldSpecs = new Map();
  setInterval(poll, 250);
  setInterval(renderHint, 120);

  // --- start ---------------------------------------------------------------------------------
  Promise.all([getJson('/api/static'), getJson('/sprites/sprites.json'), getJson('/sprites/map2d.json'),
    getJson('/sprites/resources.json').catch(() => null), getJson('/sprites/crops2d.json').catch(() => null),
    getJson('/sprites/pawns2d.json').catch(() => null),
    getJson('/sprites/cliffs2d.json').catch(() => null)]).then(([st, sprites, map2d, res, crops, pawnParts, cliffs]) => {
    CLIFFS = cliffs || {};
    ST = st; MAP = map2d; RES = res; UI = st.ui || {};
    Pawns.setParts(pawnParts);   // LR-139: hair, beards, ears, builds as data
    PAWN_PARTS = pawnParts || {};
    Sprites.load(sprites);
    Sprites.merge(crops);   // AL-112: fields and gardens
    Sprites.merge(cliffs);  // LR-156: cliffs of the relief
    MAP.actsRe = (MAP.acts || []).map(([re, sp]) => [new RegExp(re), sp]);
    for (const h of MAP.hidden || []) hidden.add(h);
    document.title = L('ui2d.title');
    for (const el of document.querySelectorAll('[data-k]')) el.textContent = L(el.dataset.k);
    const s = st.settlements.find(x => x.mode === 'full') || st.settlements[0];
    if (s) { cam.x = s.x; cam.y = s.y; }
    const q = new URLSearchParams(location.search);
    if (q.get('zoom')) zoom = Math.max(1, Math.min(3, Number(q.get('zoom'))));
    if (q.get('x')) cam.x = Number(q.get('x'));
    if (q.get('y')) cam.y = Number(q.get('y'));
    if (q.get('glyphs')) glyphs = true;
    if (q.get('roofs')) roofMode = q.get('roofs');
    resize();
    $('loading').hidden = true;
    poll();
    if (q.get('select')) {
      // a shot script: select the first person (or the given index) and centre on them
      const wait = setInterval(() => {
        const humans = people.filter(p => !p.sp && p.alive);
        if (!humans.length) return;
        clearInterval(wait);
        const p = humans[Math.min(humans.length - 1, Number(q.get('select')) || 0)];
        select(p.id); cam.x = p.x; cam.y = p.y; follow = true;
      }, 100);
    }
    renderHint();
    requestAnimationFrame(draw);
  }).catch(e => { $('loading').textContent = String(e); });
  // AL-96: what play.js and the windows use
  window.G = {
    hooks, chunks, vis, L, esc, $, cmd, getJson, select, pickPerson, toScreen, toWorld, pawnSpec, portrait, bar, tone,
    get cam() { return cam; }, get people() { return people; }, get byId() { return byId; },
    get me() { return me; }, get selId() { return selId; }, get zoom() { return zoom; }, get ST() { return ST; },
    get MAP() { return MAP; }, get canvas() { return cv; }, get dpr() { return dpr(); }, get clock() { return clock; },
    set follow(v) { follow = v; }, keys, refreshCard() { cardKey = ''; renderCard(); }, poll() { poll(); },
  };
  window.Altera2D = window.G;
})();
