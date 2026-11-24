// AL-94: the build mode of the 2D client (B): planning from above as RimWorld
// has it. A carved tool panel over the map (Резной рубеж): room, wall, door,
// window, floor, roof, pillar, furniture, strike; material, roof, storey,
// for the household or for the settlement. A stroke is dragged (or a point
// clicked) on the map; the server expands it into the sim's modules and checks
// each as the sim does (/api/build/preview), the ghost shows what goes (green)
// and what not (red, with why), and letting go lays it into the plan
// (/api/build/place). Planning is free and has no limit; the builders bring the
// materials and raise the pieces. The plan stays on the map (planned footings,
// walls, roofs, furniture) in and out of the mode (/api/build/plan).
'use strict';
(() => {
  const G = window.G;
  const TOOLS = ['room', 'wall', 'door', 'window', 'floor', 'roof', 'pillar', 'thing', 'remove'];
  const POINT = new Set(['door', 'window', 'pillar', 'thing']);
  const KEYS = { room: '1', wall: '2', door: '3', window: '4', floor: '5', roof: '6', pillar: '7', thing: '8', remove: '9' };
  const S = {
    on: false, tool: 'room', material: 'log', roof: 'roof_thatch', roofed: true, side: 0, rot: 0, z: 0,
    forTown: false, thing: 'thing_bed', thingGroup: null,
  };
  let CAT = null, plan = [], planAt = 0, planBusy = false;
  let stroke = null, hover = null, preview = null, previewKey = '', previewBusy = false, previewWant = null;
  let panel = null, lastPlaced = null;
  try { const s = JSON.parse(localStorage.getItem('altera2d.build') || '{}'); Object.assign(S, s, { on: false }); } catch (e) { /* no storage */ }
  const save = () => { try { localStorage.setItem('altera2d.build', JSON.stringify(Object.assign({}, S, { on: false }))); } catch (e) { /* no storage */ } };
  const cellM = () => (CAT ? CAT.cell_m : 2);

  function loadCatalog() {
    if (CAT) return Promise.resolve(CAT);
    return G.getJson('/api/build/catalog').then(c => { CAT = c; return c; });
  }
  function loadPlan(force) {
    if (planBusy || (!force && performance.now() - planAt < 1500)) return;
    const cv = G.canvas, tp = 16 * G.zoom * G.dpr;
    const wM = Math.ceil(cv.width / tp) + 8, hM = Math.ceil(cv.height / tp) + 8;
    const x = Math.floor(G.cam.x - wM / 2), y = Math.floor(G.cam.y - hM / 2);
    planBusy = true;
    G.getJson(`/api/build/plan?x=${x}&y=${y}&w=${wM}&h=${hM}`).then(r => { plan = r.pieces || []; planAt = performance.now(); })
      .catch(() => {}).finally(() => { planBusy = false; });
  }
  // the chunks under a stroke paint again (planned walls show in them)
  function staleChunks() { for (const c of G.chunks.values()) c.at = 0; }

  // --- the stroke ------------------------------------------------------------------
  function body(a, b) {
    return {
      tool: S.tool, a, b: b || a, z: S.z, rot: S.rot, side: S.side, material: S.material, roof: S.roof,
      roofed: S.roofed, thing: S.thing, for: S.forTown ? 'settlement' : 'self',
    };
  }
  const post = (u, b) => fetch(u, { method: 'POST', body: JSON.stringify(b) }).then(r => r.json());
  function wantPreview(a, b) {
    const q = body(a, b), c = cellM();
    // the same cells, the same answer: ask again only when the stroke covers others
    const sn = v => S.tool === 'thing' ? Math.floor(v) : (POINT.has(S.tool) ? Math.round(v * 4 / c) : Math.floor(v / c));
    const key = JSON.stringify([q.tool, sn(a[0]), sn(a[1]), sn(q.b[0]), sn(q.b[1]), q.z, q.rot, q.side, q.material, q.roof, q.roofed, q.thing, q.for]);
    if (key === previewKey) return;
    previewKey = key;
    previewWant = q;
    pumpPreview();
  }
  function pumpPreview() {
    if (previewBusy || !previewWant) return;
    const q = previewWant;
    previewWant = null;
    previewBusy = true;
    post('/api/build/preview', q).then(r => { preview = r; renderInfo(); })
      .catch(() => {}).finally(() => { previewBusy = false; pumpPreview(); });
  }
  function place(a, b) {
    const q = body(a, b);
    post('/api/build/place', q).then(r => {
      lastPlaced = r;
      preview = null; previewKey = '';
      staleChunks(); loadPlan(true);
      const msg = S.tool === 'remove'
        ? G.L('ui2d.build.removed', { n: r.placed })
        : (r.bad ? G.L('ui2d.build.placed_bad', { n: r.placed, bad: r.bad, why: why(r.why) }) : G.L('ui2d.build.placed', { n: r.placed }));
      G.toast && G.toast(msg);
      renderInfo();
      if (hover) wantPreview(hover, hover);
    });
  }
  const why = k => k ? G.L('ui2d.build.why.' + k) : '';

  // --- input on the map (only while the mode is on; the middle button still pans) ----
  const wrap = document.getElementById('mapwrap');
  function worldOf(e) {
    const r = G.canvas.getBoundingClientRect();
    return G.toWorld((e.clientX - r.left) * G.dpr, (e.clientY - r.top) * G.dpr);
  }
  const onMap = e => e.target === G.canvas;
  wrap.addEventListener('mousedown', e => {
    if (!S.on || !onMap(e)) return;
    if (e.button === 2 && !e.shiftKey) { e.stopImmediatePropagation(); e.preventDefault(); if (stroke) { stroke = null; preview = null; previewKey = ''; renderInfo(); } else setMode(false); return; }
    if (e.button !== 0) return;
    e.stopImmediatePropagation(); e.preventDefault();
    const w = worldOf(e);
    if (POINT.has(S.tool)) { place(w, w); return; }
    stroke = { a: w, b: w };
    wantPreview(w, w);
  }, true);
  wrap.addEventListener('mousemove', e => {
    if (!S.on || !onMap(e)) return;
    const w = worldOf(e);
    hover = w;
    if (stroke) { stroke.b = w; wantPreview(stroke.a, w); }
    else if (POINT.has(S.tool)) wantPreview(w, w);
    else { preview = null; previewKey = ''; }
  }, true);
  window.addEventListener('mouseup', e => {
    if (!S.on || e.button !== 0 || !stroke) return;
    const s = stroke;
    stroke = null;
    place(s.a, s.b);
  }, true);
  wrap.addEventListener('click', e => { if (S.on && onMap(e) && e.button === 0) { e.stopImmediatePropagation(); e.preventDefault(); } }, true);
  wrap.addEventListener('mouseleave', () => { hover = null; if (!stroke) { preview = null; previewKey = ''; } });

  // before the quick bar's 1-8 and the roofs' R (play.js, game2d.js), after a window's Esc
  G.hooks.key.splice(1, 0, e => {
    if (!S.on || e.ctrlKey || e.metaKey || e.altKey || (G.win.top && G.win.top())) return false;
    const k = e.key;
    if (k === 'Escape') { if (stroke) { stroke = null; preview = null; } else setMode(false); return true; }
    if (k === 'r' || k === 'R') { if (S.tool === 'room') S.side = (S.side + 1) % 4; else S.rot = (S.rot + 1) % 4; repreview(); render(); return true; }
    if (k === 'PageUp' || k === ']') { S.z = Math.min(4, S.z + 1); repreview(); render(); return true; }
    if (k === 'PageDown' || k === '[') { S.z = Math.max(0, S.z - 1); repreview(); render(); return true; }
    if (k === 't' || k === 'T') { S.forTown = !S.forTown; repreview(); render(); return true; }
    if (k === 'x' || k === 'X') { pick('remove'); return true; }
    for (const [tool, key] of Object.entries(KEYS)) if (k === key && !e.shiftKey) { pick(tool); return true; }
    return false;
  });
  function repreview() { previewKey = ''; save(); if (stroke) wantPreview(stroke.a, stroke.b); else if (hover && POINT.has(S.tool)) wantPreview(hover, hover); }
  function pick(tool) { S.tool = tool; preview = null; previewKey = ''; save(); render(); repreview(); }

  // --- the panel ---------------------------------------------------------------------
  function setMode(on) {
    S.on = on;
    if (on) {
      G.win.closeAll && G.win.closeAll();
      loadCatalog().then(() => { render(); loadPlan(true); });
    } else { stroke = null; preview = null; previewKey = ''; }
    if (panel) panel.hidden = !on;
    G.canvas.classList.toggle('building', on);
  }
  G.build = {
    setMode, get on() { return S.on; }, reloadPlan: () => loadPlan(true),
    pieceName: id => { const p = CAT && CAT.pieces.find(q => q.id === id); return p ? p.name : null; },
  };
  G.screens.build = () => setMode(!S.on);

  function btn(label, on, act, title) {
    return `<button class="btn${on ? ' on' : ''}" data-act="${act}"${title ? ` title="${G.esc(title)}"` : ''}>${G.esc(label)}</button>`;
  }
  function render() {
    if (!panel) {
      panel = document.createElement('div');
      panel.id = 'buildbar';
      panel.className = 'carved';
      wrap.appendChild(panel);
      panel.addEventListener('mousedown', e => e.stopPropagation());
      panel.addEventListener('click', e => {
        const b = e.target.closest('[data-act]');
        if (!b) return;
        const [k, v] = b.dataset.act.split(':');
        if (k === 'tool') pick(v);
        else if (k === 'mat') { S.material = v; repreview(); render(); }
        else if (k === 'roof') { S.roof = v; repreview(); render(); }
        else if (k === 'roofed') { S.roofed = !S.roofed; repreview(); render(); }
        else if (k === 'for') { S.forTown = v === 'town'; repreview(); render(); }
        else if (k === 'z') { S.z = Math.max(0, Math.min(4, S.z + Number(v))); repreview(); render(); }
        else if (k === 'turn') { if (S.tool === 'room') S.side = (S.side + 1) % 4; else S.rot = (S.rot + 1) % 4; repreview(); render(); }
        else if (k === 'thing') { S.thing = v; pick('thing'); }
        else if (k === 'group') { S.thingGroup = S.thingGroup === v ? null : v; render(); }
        else if (k === 'close') setMode(false);
        else if (k === 'colony') G.openScreen('colony');
      });
    }
    panel.hidden = !S.on;
    if (!CAT) return;
    let h = `<div class="bb-head"><h3>${G.esc(G.L('ui2d.screen.build'))}</h3>` +
      btn(G.L('ui2d.build.for.self'), !S.forTown, 'for:self', 'T') + btn(G.L('ui2d.build.for.town'), S.forTown, 'for:town', 'T') +
      `<button class="btn x" data-act="close" title="Esc">✖</button></div>`;
    h += '<div class="bb-tools">';
    for (const t of TOOLS) h += btn(G.L('ui2d.build.tool.' + t), S.tool === t, 'tool:' + t, KEYS[t]);
    h += '</div><div class="bb-row">';
    if (['room', 'wall', 'door', 'window', 'roof'].includes(S.tool))
      for (const m of ['log', 'daub', 'stone']) h += btn(G.L('ui2d.build.mat.' + m), S.material === m, 'mat:' + m);
    if (S.tool === 'room' || S.tool === 'roof') {
      h += '<span class="sep"></span>';
      for (const r of CAT.pieces.filter(p => p.cat === 'roof')) h += btn(r.name, S.roof === r.id, 'roof:' + r.id);
    }
    if (S.tool === 'room') h += btn(G.L(S.roofed ? 'ui2d.build.roofed' : 'ui2d.build.open'), S.roofed, 'roofed');
    h += '</div><div class="bb-row">' +
      `<span class="lbl">${G.esc(G.L('ui2d.build.storey', { n: S.z }))}</span>` + btn('−', false, 'z:-1', 'PgDn') + btn('+', false, 'z:1', 'PgUp');
    if (S.tool === 'room' || S.tool === 'thing')
      h += btn(G.L('ui2d.build.turn') + ' ' + '↑→↓←'[S.tool === 'room' ? S.side : S.rot], false, 'turn', 'R');
    h += btn(G.L('ui2d.screen.colony'), false, 'colony', 'O') + '</div>';
    if (S.tool === 'thing') h += furnitureHtml();
    h += `<div class="bb-tip parch">${G.esc(G.L('ui2d.build.tip.' + S.tool))}</div><div id="bb-info"></div>`;
    panel.innerHTML = h;
    renderInfo();
  }
  // furniture grouped by the rooms it makes (rooms.json needs), then the rest
  function furnitureHtml() {
    const things = CAT.pieces.filter(p => p.slot === 'thing');
    const byObj = new Map(things.map(p => [p.makes, p]));
    const used = new Set();
    let h = '<div class="bb-things">';
    for (const r of CAT.rooms) {
      const list = [];
      for (const n of r.needs) for (const o of n.any) if (byObj.has(o) && !list.includes(o)) list.push(o);
      if (!list.length) continue;
      const open = S.thingGroup === r.id;
      h += `<div class="bb-group${open ? ' open' : ''}"><button class="btn tab${open ? ' on' : ''}" data-act="group:${r.id}">${G.esc(r.name)}</button>`;
      if (open) for (const o of list) { const p = byObj.get(o); used.add(o); h += thingBtn(p); }
      h += '</div>';
      list.forEach(o => used.add(o));
    }
    const rest = things.filter(p => !used.has(p.makes));
    if (rest.length) {
      const open = S.thingGroup === '_other';
      h += `<div class="bb-group${open ? ' open' : ''}"><button class="btn tab${open ? ' on' : ''}" data-act="group:_other">${G.esc(G.L('ui2d.build.other'))}</button>`;
      if (open) for (const p of rest) h += thingBtn(p);
      h += '</div>';
    }
    const cur = CAT.pieces.find(p => p.id === S.thing);
    if (cur) h += `<div class="bb-cur">${G.esc(G.L('ui2d.build.thing_now', { what: cur.name }))} · ${costText(cur.cost)}</div>`;
    return h + '</div>';
  }
  function thingBtn(p) {
    return `<button class="act bb-thing${S.thing === p.id ? ' on' : ''}" data-act="thing:${p.id}"><canvas data-obj="${p.makes}" width="16" height="16"></canvas>${G.esc(p.name)}<span class="m">${costText(p.cost)}</span></button>`;
  }
  const costText = list => (list || []).map(c => `${G.esc(c.name)} ${c.n}`).join(', ');
  function paintIcons() {
    if (!panel) return;
    for (const cv of panel.querySelectorAll('canvas[data-obj]')) {
      if (cv.dataset.done) continue;
      const look = (G.MAP.objects && G.MAP.objects[cv.dataset.obj]) || G.MAP.objects._default;
      const sp = look && Sprites.get(look.sprite, look.recolour);
      const g = cv.getContext('2d');
      g.imageSmoothingEnabled = false;
      if (sp) g.drawImage(sp, 0, 0);
      cv.dataset.done = '1';
    }
  }
  function renderInfo() {
    const el = document.getElementById('bb-info');
    if (!el) return;
    paintIcons();
    const r = preview;
    let h = '';
    if (r && r.pieces) {
      if (S.tool === 'remove') h += `<div>${G.esc(G.L('ui2d.build.will_remove', { n: r.ok }))}${r.bad ? ' · ' + G.esc(G.L('ui2d.build.not_yours', { n: r.bad })) : ''}</div>`;
      else {
        h += `<div class="${r.bad ? 'bad' : 'good'}">${G.esc(r.bad ? G.L('ui2d.build.can_bad', { n: r.ok, bad: r.bad, why: why(r.why) }) : G.L('ui2d.build.can', { n: r.ok }))}</div>`;
        if (r.cost && r.cost.length) h += `<div class="cost">${G.esc(G.L('ui2d.build.will_bring'))}: ${costText(r.cost)}</div>`;
      }
    } else h += `<div class="dim">${G.esc(G.L('ui2d.build.free'))}</div>`;
    // what the plan in sight still waits for
    const mine = plan.filter(p => !p.built && (p.who === 'mine' || p.who === 'settlement'));
    if (mine.length) {
      const lack = new Map();
      let ready = 0;
      for (const p of mine) { if (!p.lack.length && p.ready) ready++; for (const l of p.lack) lack.set(l.name, (lack.get(l.name) || 0) + l.n); }
      h += `<div class="dim">${G.esc(G.L('ui2d.build.sites', { n: mine.length, ready }))}`;
      if (lack.size) h += ` · ${G.esc(G.L('ui2d.build.lack'))}: ${[...lack].map(([k, n]) => G.esc(k) + ' ' + n).join(', ')}`;
      h += '</div>';
    }
    el.innerHTML = h;
  }

  // --- drawing: the plan and the ghost --------------------------------------------------
  // a piece's shape on the map, in metres: cell rect, edge line, corner point
  function shape(p) {
    const c = cellM(), r = ((p.rot % 4) + 4) % 4;
    if (p.slot === 'thing') return { kind: 'thing', x: p.cx, y: p.cy };
    if (p.slot === 'cell' || p.slot === 'roof') return { kind: p.slot === 'roof' ? 'roof' : 'cell', x: p.x * c, y: p.y * c, w: c, h: c, rot: r };
    if (p.slot === 'edge') {
      if (r === 0) return { kind: 'edge', x0: p.x * c, y0: p.y * c, x1: (p.x + 1) * c, y1: p.y * c };
      if (r === 1) return { kind: 'edge', x0: (p.x + 1) * c, y0: p.y * c, x1: (p.x + 1) * c, y1: (p.y + 1) * c };
      if (r === 2) return { kind: 'edge', x0: p.x * c, y0: (p.y + 1) * c, x1: (p.x + 1) * c, y1: (p.y + 1) * c };
      return { kind: 'edge', x0: p.x * c, y0: p.y * c, x1: p.x * c, y1: (p.y + 1) * c };
    }
    const dx = (r === 1 || r === 2) ? 1 : 0, dy = r >= 2 ? 1 : 0;
    return { kind: 'corner', x: (p.x + dx) * c, y: (p.y + dy) * c };
  }
  function drawPiece(ctx, sc, tp, p, colour, alpha, ghost) {
    const s = shape(p);
    ctx.globalAlpha = alpha;
    ctx.strokeStyle = colour; ctx.fillStyle = colour;
    if (s.kind === 'cell' || s.kind === 'roof') {
      const [x, y] = G.toScreen(s.x, s.y), w = s.w * tp, h = s.h * tp;
      if (s.kind === 'cell') {
        ctx.globalAlpha = alpha * 0.28; ctx.fillRect(x, y, w, h);
        ctx.globalAlpha = alpha; ctx.lineWidth = Math.max(1, sc); ctx.setLineDash([3 * sc, 3 * sc]);
        ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1); ctx.setLineDash([]);
      } else {
        // a slope: hatched toward its eave
        ctx.globalAlpha = alpha * 0.5; ctx.lineWidth = Math.max(1, sc * 0.75);
        ctx.beginPath();
        for (let k = 4 * sc; k < w; k += 6 * sc) {
          if (s.rot === 0 || s.rot === 2) { ctx.moveTo(x + k, y); ctx.lineTo(x + k, y + h); } else { ctx.moveTo(x, y + k); ctx.lineTo(x + w, y + k); }
        }
        ctx.stroke();
        // the eave side thicker
        ctx.globalAlpha = alpha; ctx.lineWidth = 2 * sc; ctx.beginPath();
        if (s.rot === 0) { ctx.moveTo(x, y + 1); ctx.lineTo(x + w, y + 1); } else if (s.rot === 2) { ctx.moveTo(x, y + h - 1); ctx.lineTo(x + w, y + h - 1); }
        else if (s.rot === 1) { ctx.moveTo(x + w - 1, y); ctx.lineTo(x + w - 1, y + h); } else { ctx.moveTo(x + 1, y); ctx.lineTo(x + 1, y + h); }
        ctx.stroke();
      }
    } else if (s.kind === 'edge') {
      const [x0, y0] = G.toScreen(s.x0, s.y0), [x1, y1] = G.toScreen(s.x1, s.y1);
      ctx.lineWidth = (p.cat === 'gable' ? 2 : 4) * sc;
      ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
      if (p.door || /door/.test(p.kind)) { ctx.fillStyle = '#1a1410'; const mx = (x0 + x1) / 2, my = (y0 + y1) / 2; ctx.fillRect(mx - 3 * sc, my - 3 * sc, 6 * sc, 6 * sc); }
      else if (/window/.test(p.kind)) { ctx.fillStyle = '#a8d0e8'; const mx = (x0 + x1) / 2, my = (y0 + y1) / 2; ctx.fillRect(mx - 2 * sc, my - 2 * sc, 4 * sc, 4 * sc); }
    } else if (s.kind === 'corner') {
      const [x, y] = G.toScreen(s.x, s.y);
      ctx.fillRect(x - 3 * sc, y - 3 * sc, 6 * sc, 6 * sc);
    } else if (s.kind === 'thing') {
      const [x, y] = G.toScreen(s.x, s.y);
      const obj = p.makes || (CAT && (CAT.pieces.find(q => q.id === p.kind) || {}).makes);
      const look = obj && ((G.MAP.objects && G.MAP.objects[obj]) || G.MAP.objects._default);
      const sp = look && Sprites.get(look.sprite, look.recolour);
      if (sp) { ctx.globalAlpha = alpha * (ghost ? 0.8 : 0.6); ctx.drawImage(sp, x - 8 * sc, y - 8 * sc, 16 * sc, 16 * sc); }
      ctx.globalAlpha = alpha; ctx.lineWidth = Math.max(1, sc);
      ctx.strokeRect(x - tp / 2 + 1, y - tp / 2 + 1, tp - 2, tp - 2);
    }
    ctx.globalAlpha = 1;
  }
  G.hooks.draw.push((ctx, sc, tp) => {
    if (!CAT) return;
    const z = S.on ? S.z : 0;
    // the plan: planned pieces (walls of the ground storey are in the chunks already)
    for (const p of plan) {
      // roofs show over the storeys under them (lightly), the rest on their own storey
      if (!p.built && p.cat === 'roof' && p.z > z) { drawPiece(ctx, sc, tp, p, '#c89a60', S.on ? 0.35 : 0.3, false); continue; }
      if (p.built || p.z !== z) continue;
      if (!S.on && p.slot === 'edge' && p.z === 0) continue;
      const col = p.who === 'mine' ? (p.lack.length ? '#e8c060' : '#9ad07a') : p.who === 'settlement' || p.who === 'common' ? '#78b8e8' : '#b0a090';
      drawPiece(ctx, sc, tp, p, col, S.on ? 0.9 : 0.55, false);
    }
    if (S.on && preview && preview.pieces) {
      for (const p of preview.pieces) {
        if (p.z !== z && S.tool !== 'room' && S.tool !== 'roof') continue;
        const col = S.tool === 'remove' ? (p.ok ? '#e05040' : '#806060') : (p.ok ? '#70e070' : '#e05040');
        drawPiece(ctx, sc, tp, p, col, p.z === z ? 0.95 : 0.5, true);
      }
    }
    if (S.on && stroke && !POINT.has(S.tool)) {
      // the dragged rectangle in cells
      const c = cellM();
      const x0 = Math.floor(Math.min(stroke.a[0], stroke.b[0]) / c) * c, y0 = Math.floor(Math.min(stroke.a[1], stroke.b[1]) / c) * c;
      const x1 = (Math.floor(Math.max(stroke.a[0], stroke.b[0]) / c) + 1) * c, y1 = (Math.floor(Math.max(stroke.a[1], stroke.b[1]) / c) + 1) * c;
      const [sx, sy] = G.toScreen(x0, y0), [ex, ey] = G.toScreen(x1, y1);
      ctx.strokeStyle = '#f0d080'; ctx.lineWidth = Math.max(1, sc); ctx.setLineDash([4 * sc, 2 * sc]);
      ctx.strokeRect(sx + 0.5, sy + 0.5, ex - sx - 1, ey - sy - 1); ctx.setLineDash([]);
      ctx.font = `600 ${12 * Math.max(1, sc / 2)}px "Alegreya SC", serif`; ctx.fillStyle = '#f0d080'; ctx.textAlign = 'left'; ctx.textBaseline = 'bottom';
      ctx.fillText(`${Math.round((x1 - x0) / c)}×${Math.round((y1 - y0) / c)}`, sx + 4, sy - 2);
    }
    if (S.on && hover) {
      // the cell grid round the pointer (2 m cells, a finer one for furniture)
      const c = S.tool === 'thing' ? (CAT.thing_m || 1) : cellM();
      const gx = Math.floor(hover[0] / c) * c, gy = Math.floor(hover[1] / c) * c;
      ctx.strokeStyle = 'rgba(240,208,128,0.25)'; ctx.lineWidth = 1;
      ctx.beginPath();
      for (let k = -4; k <= 5; k++) {
        const [ax, ay] = G.toScreen(gx + k * c, gy - 4 * c), [bx, by] = G.toScreen(gx + k * c, gy + 5 * c);
        ctx.moveTo(ax + 0.5, ay); ctx.lineTo(bx + 0.5, by);
        const [cx, cy] = G.toScreen(gx - 4 * c, gy + k * c), [dx, dy] = G.toScreen(gx + 5 * c, gy + k * c);
        ctx.moveTo(cx, cy + 0.5); ctx.lineTo(dx, dy + 0.5);
      }
      ctx.stroke();
    }
  });
  G.hooks.poll.push(() => {
    if (!CAT && !planBusy) loadCatalog().then(() => loadPlan(true)).catch(() => {});
    loadPlan(false);
    if (S.on) renderInfo();
  });
})();
