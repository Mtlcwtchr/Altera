// AL-97: the workshop (K) of the 2D client - the crafting UI the user chose
// (gdd/ui/CRAFT_UI_VARIANTS.md): C + B with drag from A, D as the helper, in the
// flat window of gdd/ui/client_2d/ui_h3 screen 04.
//   C: the workshop is where the person stands - the stations round them (their
//      sprites, the heat gauge, the mouse wheel works the bellows), and on the map
//      a gauge and a ring hang over each station near the person.
//   B: the processes as a ring round the work piece; the ones that cannot start
//      here are dim with the reason; a second row of chips picks the target shape
//      and the way (mode); the wheel over the piece sets the length.
//   A: materials and made things from the bag are dragged onto the piece's slots
//      (a click puts them into the first free one, a click on a slot takes it off).
//   D: the helper - a wish as a glyph, the designs that answer it with "fits N%",
//      their chain along the stations, and the step that can start now laid on the bench.
// One main button: "Make" held (E or the mouse). Reads /api/workshop (POST
// {draft, wish}); acts with the sim's craft_attempt / craft_fast / craft_inspect /
// craft_repair. Sprites: content/sprites_2d/craft2d.json (stations), resources.json
// (items); otherwise the glyph of the shape.
'use strict';
(() => {
  const G = window.G;
  const L = (k, a) => G.L(k, a);
  let LOOK = null, RES = null, TX = {};      // craft2d.json, resources.json, hud.craft.* strings
  let ws = null, draft = {}, wish = '', tab = 'bench', heat = -1, seq0 = null;
  let loading = false, kitAsked = false;
  const link = document.createElement('link');
  link.rel = 'stylesheet'; link.href = '/web/craft.css';
  document.head.appendChild(link);
  Promise.all([G.getJson('/sprites/craft2d.json'), G.getJson('/sprites/resources.json').catch(() => null)])
    .then(([l, r]) => { LOOK = l; RES = r; });
  // hud.craft.* (the 3D game's strings) where the page has none of its own
  const T = (k, a) => {
    let s = TX[k] || k;
    if (a) for (const [n, v] of Object.entries(a)) s = s.split('{' + n + '}').join(v);
    return s;
  };
  const esc = s => G.esc(String(s == null ? '' : s));
  const pct = v => Math.round(Math.max(0, Math.min(1, v)) * 100);

  // --- server -------------------------------------------------------------------------------
  function load(extra) {
    if (loading && !extra) return Promise.resolve();
    loading = true;
    const body = Object.assign({ draft: draftOut(), wish, texts: !Object.keys(TX).length }, extra || {});
    return fetch('/api/workshop', { method: 'POST', body: JSON.stringify(body) }).then(r => r.json()).then(r => {
      if (r.texts) TX = r.texts;
      ws = r.ws;
      if (ws && seq0 === null) seq0 = ws.last ? ws.last.seq : 0;
      if (ws && heat < 0) heat = ws.heat_max;
      if (ws && heat > ws.heat_max) heat = ws.heat_max;
      const w = G.win.get('workshop');
      if (w) w.refresh();
    }).catch(e => console.warn(e)).finally(() => { loading = false; });
  }
  function draftOut() {
    if (!draft.process) return {};
    const d = { process: draft.process, slots: draft.slots || {} };
    if (draft.shape) d.shape = draft.shape;
    if (draft.mode) d.mode = draft.mode;
    if (draft.len) d.len = draft.len;
    const p = proc(draft.process);
    if (p && p.heat_min > 0 || heat >= 0 && ws && ws.heat_max > 0) d.heat = Math.max(0, heat);
    return d;
  }
  function act(type, args) {
    if (!ws) return Promise.resolve();
    return G.cmd({ op: 'cmd', type, args: Object.assign({ id: ws.who }, args) }).then(r => {
      if (r.result && r.result !== 'ok' && !String(r.result).startsWith('ok')) G.toast && G.toast(r.text || r.result);
      return load({});
    });
  }
  const proc = id => ws && ws.processes.find(p => p.id === id);

  // --- looks: sprites and glyphs ------------------------------------------------------------------
  function paint(name, recolour, px) {
    const cv = document.createElement('canvas');
    cv.width = cv.height = 16;
    const grid = LOOK && LOOK.sprites[name];
    const ctx = cv.getContext('2d');
    if (grid) {
      for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
        let ch = grid[y][x];
        if (ch === '.') continue;
        if (recolour && recolour[ch]) ch = recolour[ch];
        ctx.fillStyle = Sprites.colour(ch); ctx.fillRect(x, y, 1, 1);
      }
    } else {
      const sp = Sprites.get(name, recolour);
      if (sp) ctx.drawImage(sp, 0, 0);
    }
    cv.style.width = cv.style.height = px + 'px';
    cv.className = 'px';
    return cv;
  }
  // AL-109: an action/process as its pixel icon (craft2d.json action_icons / process_icons); the name is the title
  function iconBtn(b, sprite, px = 32) {
    if (!sprite || !hasSprite(sprite)) return b;
    b.textContent = '';
    b.classList.add('ico');
    b.appendChild(paint(sprite, null, px));
    return b;
  }
  const actIcon = id => (LOOK && LOOK.action_icons || {})[id];
  function hasSprite(name) { return !!(LOOK && LOOK.sprites[name]) || Sprites.has(name); }
  // a thing of the bag: its own sprite when it has one, else the glyph of its shape / kind
  function thingIcon(t, px) {
    const own = LOOK && (LOOK.forms[t.form] || LOOK.items[t.item]);
    const r = own || (RES && RES.items && (RES.items[t.item] || RES.items[t.form]));
    if (r && r.sprite && hasSprite(r.sprite)) return paint(r.sprite, r.recolour, px);
    const span = document.createElement('span');
    span.className = 'glyph';
    let g = LOOK && LOOK.shapes[t.shape], c = 'w';
    if (t.kind && t.form && LOOK && LOOK.kinds[t.kind]) [g, c] = LOOK.kinds[t.kind];
    if (r && r.glyph) { g = r.glyph; c = r.colour || c; }
    span.textContent = g || '•';
    span.style.color = Sprites.colour(c);
    span.style.fontSize = Math.round(px * 0.75) + 'px';
    span.style.width = span.style.height = px + 'px';
    return span;
  }
  function stationIcon(id, px) {
    const name = LOOK && LOOK.stations[id];
    if (name && hasSprite(name)) return paint(name, null, px);
    const span = document.createElement('span');
    span.className = 'glyph';
    span.textContent = (LOOK && LOOK.station_glyphs[id]) || '¤';
    span.style.fontSize = Math.round(px * 0.7) + 'px';
    return span;
  }
  function procGlyph(id) { const g = LOOK && LOOK.processes[id]; return g ? g : ['?', 'w']; }
  function el(tag, cls, html) { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; }

  // --- the window ---------------------------------------------------------------------------------
  function open() {
    if (G.me == null) { G.toast && G.toast(L('ui2d.craft.nobody')); return; }
    if (!kitAsked && new URLSearchParams(location.search).get('kit')) { kitAsked = true; load({ kit: true }); } else load({});
    G.win.open({
      id: 'workshop', wide: true, tab,
      title: () => !ws ? L('ui2d.screen.workshop') : ws.stations.length
        ? L('ui2d.craft.at', { list: ws.stations.map(s => s.name).join(' · ') }) : L('ui2d.craft.at_none'),
      tabs: [['bench', L('ui2d.craft.tab.bench')], ['book', L('ui2d.craft.tab.book')], ['things', L('ui2d.craft.tab.things')],
        ['helper', L('ui2d.craft.tab.helper')]],
      render(body, t, w) {
        tab = t;
        body.innerHTML = '';
        if (!ws || !LOOK) { body.appendChild(el('div', 'parch', esc(ws === null ? L('ui2d.craft.nobody') : '…'))); return; }
        body.appendChild(({ bench: renderBench, book: renderBook, things: renderThings, helper: renderHelper })[t](w));
      },
      onPoll(s, w) { if (performance.now() - (w.loadedAt || 0) > 1500) { w.loadedAt = performance.now(); load(); } },
      onClose() { holdStop(); },
    });
  }
  G.screens.workshop = open;

  // --- the bench: stations (C) | ring (B) + bag (A) | forecast -----------------------------------
  function renderBench() {
    const root = el('div', 'ws-bench');
    root.appendChild(renderLeft());
    root.appendChild(renderRing());
    root.appendChild(renderRight());
    return root;
  }
  function renderLeft() {
    const col = el('div', 'ws-col');
    col.appendChild(el('h3', '', esc(L('ui2d.craft.stations'))));
    const box = el('div', 'ws-stations inset');
    if (!ws.stations.length) box.appendChild(el('div', 'ws-note', esc(L('ui2d.craft.no_station'))));
    for (const s of ws.stations) {
      const b = el('div', 'ws-station' + (s.temp_max > 0 ? ' hot' : ''));
      b.appendChild(stationIcon(s.id, 48));
      b.appendChild(el('span', '', esc(s.name)));
      b.title = s.temp_max > 0 ? L('ui2d.craft.heat_tip', { n: heat, max: ws.heat_max }) : s.name;
      if (s.temp_max > 0) b.addEventListener('wheel', wheelHeat, { passive: false });
      box.appendChild(b);
    }
    col.appendChild(box);
    // the heat gauge: cells up to the hottest station here; the wheel works the bellows
    const g = el('div', 'ws-heat');
    g.appendChild(el('span', 'h', esc(L('ui2d.craft.heat'))));
    if (ws.heat_max > 0) {
      const cells = el('div', 'cells');
      for (let i = 1; i <= ws.heat_max; i++) {
        const c = el('i', i <= heat ? 'on' : '');
        c.style.setProperty('--k', i / ws.heat_max);
        c.onclick = () => { heat = heat === i ? i - 1 : i; load({}); };
        cells.appendChild(c);
      }
      cells.addEventListener('wheel', wheelHeat, { passive: false });
      g.appendChild(cells);
      g.appendChild(el('span', 'sub', `${heat} / ${ws.heat_max}`));
    } else g.appendChild(el('span', 'sub', esc(L('ui2d.craft.heat_none'))));
    col.appendChild(g);
    col.appendChild(renderResult());
    col.appendChild(renderShortBook());
    return col;
  }
  // the book at the bench (screen 04): mastered first; a mastered one is made from memory on a click
  function renderShortBook() {
    const box = el('div', 'ws-short inset');
    box.appendChild(el('h3', '', esc(T('hud.craft.book'))));
    if (!ws.recipes.length) { box.appendChild(el('div', 'ws-note', esc(T('hud.craft.book_empty')))); return box; }
    const list = ws.recipes.slice().sort((a, b) => (b.master - a.master) || (b.mastery - a.mastery)).slice(0, 8);
    for (const r of list) {
      const row = el('div', 'rrow' + (r.master && r.can ? ' go' : ''));
      row.appendChild(thingIcon({ item: r.item, form: r.form, shape: 'lump' }, 20));
      row.appendChild(el('span', 'nm' + (r.known < r.total ? ' frag' : ''), esc(r.name)));
      row.appendChild(el('small', '', esc(r.master ? L('ui2d.craft.book_master') : L('ui2d.craft.book_fragment', { known: r.known, total: r.total }))));
      row.title = r.master ? (r.can ? L('ui2d.craft.from_memory') + ' · ' + T('hud.craft.fast_time', { time: r.time, stages: r.stages })
        : L('ui2d.craft.missing', { list: r.missing.join(', ') })) : L('ui2d.craft.not_master');
      if (r.master && r.can) row.onclick = () => act('craft_fast', { recipe: r.id, count: 1 });
      box.appendChild(row);
    }
    return box;
  }
  function wheelHeat(e) {
    e.preventDefault();
    const n = Math.max(0, Math.min(ws.heat_max, heat + (e.deltaY < 0 ? 1 : -1)));
    if (n !== heat) { heat = n; load({}); }
  }
  // the last outcome (board 11): what came out, the chain's links, the weak one, an insight
  function renderResult() {
    const box = el('div', 'ws-result');
    const r = ws.last;
    if (!r || r.seq <= seq0) return box;   // only what happened since the page opened
    const card = el('div', 'parch ws-card' + (r.breakthrough ? ' insight' : ''));
    card.appendChild(el('h4', '', esc(L('ui2d.craft.result'))));
    card.appendChild(el('div', 'head', esc(r.head) + (r.what ? ': <b>' + esc(r.what) + '</b>' : '')));
    if (r.tier) card.appendChild(el('div', 'sub', esc(r.tier)));
    if (r.links && r.links.length) {
      const ln = el('div', 'links');
      for (const k of r.links) {
        const i = el('i', '', esc(k.name));
        i.style.setProperty('--q', k.q);
        ln.appendChild(i);
      }
      card.appendChild(ln);
    }
    if (r.weak) card.appendChild(el('div', 'sub', esc(r.weak)));
    if (r.flaws && r.flaws.length) card.appendChild(el('div', 'bad', esc(r.flaws.join(', '))));
    if (r.breakthrough) card.appendChild(el('div', 'gold', esc(T('hud.craft.breakthrough')) + (r.recipe_name ? ' · ' + esc(r.recipe_name) : '')));
    box.appendChild(card);
    return box;
  }

  // B: the ring of processes round the piece; A: drop targets on the piece
  function renderRing() {
    const col = el('div', 'ws-ring inset');
    const ringBox = el('div', 'ringbox');
    const R = 196, cx = 250, cy = 232;
    const svg = `<svg width="500" height="470" class="ring"><circle cx="${cx}" cy="${cy}" r="${R}" fill="none" stroke="#b98b3e" stroke-width="3" stroke-dasharray="6 8"/><circle cx="${cx}" cy="${cy}" r="96" fill="rgba(0,0,0,.25)" stroke="#6a4a20" stroke-width="2"/></svg>`;
    ringBox.innerHTML = svg;
    col.appendChild(ringBox);
    const list = ws.processes;
    list.forEach((p, i) => {
      const a = i / list.length * Math.PI * 2 - Math.PI / 2;
      const [g, c] = procGlyph(p.id);
      const b = el('button', 'ws-proc btn' + (p.available ? '' : ' off') + (draft.process === p.id ? ' on' : ''), `<span style="color:${Sprites.colour(c)}">${esc(g)}</span>`);
      b.style.left = (cx + Math.cos(a) * R - 22) + 'px';
      b.style.top = (cy + Math.sin(a) * R - 22) + 'px';
      b.title = T('hud.craft.proc_tip', { name: p.name, time: p.time }) + (p.available ? '' : ' · ' + p.need);
      b.onclick = () => pickProcess(p.id);
      iconBtn(b, (LOOK && LOOK.process_icons || {})[p.id]);
      ringBox.appendChild(b);
    });
    // the piece in the middle: the chosen process's slots as drop targets
    const piece = el('div', 'ws-piece');
    piece.style.left = (cx - 92) + 'px'; piece.style.top = (cy - 92) + 'px';
    const p = proc(draft.process);
    if (!p) piece.appendChild(el('div', 'ws-hint', esc(L('ui2d.craft.pick'))));
    else {
      piece.appendChild(el('div', 'pname', esc(p.name)));
      const slots = p.slots.length ? p.slots : [{ id: 'work', name: T('hud.craft.slot.work') }];
      for (const s of slots) piece.appendChild(slotBox(s));
      piece.addEventListener('wheel', e => {   // the wheel over the piece: its length
        if (!draft.process) return;
        e.preventDefault();
        draft.len = Math.max(0, Math.min(12, (draft.len || 0) + (e.deltaY < 0 ? 1 : -1)));
        load({});
      }, { passive: false });
      piece.title = draft.len ? T('hud.craft.len_tip', { n: draft.len }) : T('hud.craft.len_default');
    }
    ringBox.appendChild(piece);
    // target shapes and ways under the ring
    if (p) col.appendChild(renderTargets(p));
    col.appendChild(renderBag());
    return col;
  }
  function pickProcess(id) {
    if (draft.process === id) return;
    const old = draft.slots || {};
    draft = { process: id, slots: {} };
    // keep what lay on the piece: it moves to the new process's first slot
    const p = proc(id), all = [].concat(...Object.values(old));
    if (p && all.length) draft.slots[(p.slots[0] || { id: 'work' }).id] = all;
    load({});
  }
  function slotBox(s) {
    const refs = (draft.slots && draft.slots[s.id]) || [];
    const box = el('div', 'ws-slot' + (refs.length ? ' full' : ''));
    box.appendChild(el('small', '', esc(s.name)));
    const row = el('div', 'toks');
    refs.forEach((ref, k) => {
      const t = thing(ref);
      const tok = el('span', 'tok');
      tok.appendChild(thingIcon(t || { shape: 'lump' }, 28));
      tok.title = (t ? t.name : ref) + ' · ' + L('ui2d.craft.slot_full', { slot: s.name });
      tok.onclick = () => { refs.splice(k, 1); draft.slots[s.id] = refs; load({}); };
      row.appendChild(tok);
    });
    if (!refs.length) row.appendChild(el('span', 'empty', esc(L('ui2d.craft.piece_empty'))));
    box.appendChild(row);
    box.title = refs.length ? '' : T('hud.craft.slot_empty', { slot: s.name });
    box.addEventListener('dragover', e => { e.preventDefault(); box.classList.add('over'); });
    box.addEventListener('dragleave', () => box.classList.remove('over'));
    box.addEventListener('drop', e => {
      e.preventDefault();
      const ref = e.dataTransfer.getData('text/plain');
      if (ref) putRef(ref, s.id);
    });
    return box;
  }
  function thing(ref) { return ws.materials.find(m => m.ref === ref) || ws.made.find(m => m.ref === ref) || null; }
  // a ref onto the piece: into `slot`, or the first free slot of the process (the first, when none)
  function putRef(ref, slot) {
    if (!draft.process) {   // no process yet: the first one that takes this shape here
      const t = thing(ref);
      const p = ws.processes.find(p => p.available && t && p.targets[t.shape]) || ws.processes.find(p => p.available);
      if (!p) return;
      draft = { process: p.id, slots: {} };
    }
    const p = proc(draft.process);
    const slots = p && p.slots.length ? p.slots.map(s => s.id) : ['work'];
    draft.slots = draft.slots || {};
    const s = slot || slots.find(id => !(draft.slots[id] || []).length) || slots[0];
    const list = draft.slots[s] || (draft.slots[s] = []);
    const t = thing(ref);
    const used = list.filter(r => r === ref).length + [].concat(...Object.values(draft.slots)).filter(r => r === ref).length / 2;
    if (ref.startsWith('made:') && list.includes(ref)) return;
    if (t && t.n && used >= t.n) return;
    list.push(ref);
    load({});
  }
  function renderTargets(p) {
    const box = el('div', 'ws-targets');
    const first = [].concat(...Object.values(draft.slots || {}))[0];
    const t = first && thing(first);
    const shapes = (t && p.targets[t.shape]) || [];
    if (shapes.length) {
      box.appendChild(el('span', 'h', esc(L('ui2d.craft.target'))));
      for (const s of shapes) {
        const b = el('button', 'chip btn' + (draft.shape === s.id ? ' on' : ''));
        b.appendChild(thingIcon({ shape: s.id }, 18));
        b.appendChild(document.createTextNode(s.name));
        b.title = T('hud.craft.target_tip', { name: s.name });
        b.onclick = () => { draft.shape = draft.shape === s.id ? '' : s.id; load({}); };
        box.appendChild(b);
      }
    }
    if (p.modes.length) {
      box.appendChild(el('span', 'h', esc(L('ui2d.craft.mode'))));
      for (const m of p.modes) {
        const b = el('button', 'chip btn' + (draft.mode === m.id ? ' on' : ''), esc(m.name));
        b.title = T('hud.craft.mode_tip', { name: m.name });
        b.onclick = () => { draft.mode = draft.mode === m.id ? '' : m.id; load({}); };
        box.appendChild(b);
      }
    }
    if (draft.len) box.appendChild(el('span', 'sub', esc(L('ui2d.craft.len')) + ' ' + draft.len));
    return box;
  }
  // A: the bag - materials (an arc for how well the person knows them) and made things
  function renderBag() {
    const box = el('div', 'ws-bag');
    box.appendChild(el('div', 'h', esc(L('ui2d.craft.bag'))));
    const row = el('div', 'toks');
    if (!ws.materials.length && !ws.made.length) row.appendChild(el('span', 'ws-note', esc(L('ui2d.craft.bag_empty'))));
    const used = {};
    for (const r of [].concat(...Object.values(draft.slots || {}))) used[r] = (used[r] || 0) + 1;
    for (const t of ws.materials.concat(ws.made)) {
      const left = (t.n || 1) - (used[t.ref] || 0);
      const tok = el('div', 'bagtok' + (left <= 0 ? ' gone' : '') + (t.ref.startsWith('made:') ? ' made' : ''));
      tok.draggable = left > 0;
      tok.appendChild(thingIcon(t, 32));
      if (t.n > 1) tok.appendChild(el('b', 'n', String(left)));
      if (t.know != null) { const k = el('i', 'know'); k.style.width = pct(t.know) + '%'; tok.appendChild(k); }
      tok.title = t.name + (t.know != null ? ' · ' + L('ui2d.craft.know', { n: pct(t.know) }) : '') + (t.tier ? ' · ' + t.tier : '');
      tok.addEventListener('dragstart', e => { e.dataTransfer.setData('text/plain', t.ref); });
      tok.onclick = () => { if (left > 0) putRef(t.ref); };
      row.appendChild(tok);
    }
    box.appendChild(row);
    return box;
  }
  function renderRight() {
    const col = el('div', 'ws-col');
    const f = ws.forecast;
    const sheet = el('div', 'parch ws-forecast' + (f && f.can && f.certainty < 0.5 ? ' vague' : ''));
    sheet.appendChild(el('h3', 'ink', esc(L('ui2d.craft.forecast'))));
    if (!f) sheet.appendChild(el('div', '', esc(T('hud.craft.sheet_none'))));
    else if (!f.can) sheet.appendChild(el('div', 'bad', esc(f.why || '…')));
    else {
      const sure = f.certainty >= 0.5;
      const rows = [
        [L('ui2d.craft.q'), (f.likely || T('hud.craft.unknown_form')) + ', ' + f.tier],
        [L('ui2d.craft.ruin'), sure ? pct(f.ruin) + '%' : '≈ ' + Math.round(pct(f.ruin) / 10) * 10 + '%'],
        [L('ui2d.craft.flaw'), sure ? pct(f.flaw) + '%' : '?'],
        [L('ui2d.craft.time'), L('ui2d.craft.minutes', { n: f.time * 10 })],
      ];
      for (const [k, v] of rows) sheet.appendChild(el('div', 'frow', `<span>${esc(k)}</span><b>${esc(v)}</b>`));
      const q = el('div', 'qbar');
      const i = el('i'); i.style.left = pct(f.q - f.spread) + '%'; i.style.width = Math.max(2, pct(2 * f.spread)) + '%';
      q.appendChild(i);
      sheet.appendChild(q);
      sheet.appendChild(el('div', 'note', esc(L(sure ? 'ui2d.craft.sure' : 'ui2d.craft.vague'))));
    }
    col.appendChild(sheet);
    const row = el('div', 'ws-act');
    const clear = el('button', 'btn sq', '✖');
    clear.title = L('ui2d.craft.clear'); iconBtn(clear, actIcon('clear'), 24);
    clear.onclick = () => { draft = {}; load({}); };
    const make = el('button', 'btn ok hold' + (f && f.can ? '' : ' off'), `<i class="fill"></i><span>${esc(L('ui2d.craft.hold'))}</span>`);
    make.title = L('ui2d.craft.hold') + (f && !f.can && f.why ? ' · ' + f.why : '');
    if (actIcon('make') && hasSprite(actIcon('make'))) make.querySelector('span').replaceWith(paint(actIcon('make'), null, 32));
    make.addEventListener('mousedown', e => { if (e.button === 0) holdStart(); });
    make.addEventListener('mouseup', holdStop);
    make.addEventListener('mouseleave', holdStop);
    row.appendChild(clear); row.appendChild(make);
    col.appendChild(row);
    return col;
  }
  // the one main button: held for a moment, then the attempt
  let holdT = null, holdAt = 0;
  const HOLD_MS = 700;
  function holdStart() {
    if (holdT || !ws || !ws.forecast || !ws.forecast.can) return;
    holdAt = performance.now();
    holdT = setInterval(() => {
      const k = Math.min(1, (performance.now() - holdAt) / HOLD_MS);
      const f = document.querySelector('.ws-act .hold .fill');
      if (f) f.style.width = (k * 100) + '%';
      if (k >= 1) { holdStop(); attempt(); }
    }, 30);
  }
  function holdStop() {
    if (holdT) clearInterval(holdT);
    holdT = null;
    const f = document.querySelector('.ws-act .hold .fill');
    if (f) f.style.width = '0';
  }
  function attempt() {
    const d = draftOut();
    if (!d.process) return;
    draft = {};
    act('craft_attempt', d);
  }
  G.hooks.key.unshift(e => {
    if (!G.win.isOpen('workshop') || e.key.toLowerCase() !== 'e' || e.repeat) return false;
    if (tab === 'bench') holdStart();
    return true;
  });
  window.addEventListener('keyup', e => { if (e.key.toLowerCase() === 'e') holdStop(); });

  // --- the recipe book (board 12): fragments, mastery, fast craft -------------------------------
  function renderBook() {
    const box = el('div', 'ws-book');
    if (!ws.recipes.length) { box.appendChild(el('div', 'parch', esc(T('hud.craft.book_empty')))); return box; }
    for (const r of ws.recipes) {
      const row = el('div', 'parch ws-recipe' + (r.master ? ' master' : ''));
      const ic = el('div', 'ic');
      ic.appendChild(thingIcon({ item: r.item, form: r.form, shape: 'lump' }, 32));
      row.appendChild(ic);
      const mid = el('div', 'mid');
      mid.appendChild(el('div', 'name', esc(r.name) + ' <small>' + esc(r.master ? L('ui2d.craft.book_master')
        : L('ui2d.craft.book_fragment', { known: r.known, total: r.total })) + '</small>'));
      const steps = el('div', 'steps');
      r.steps.forEach((s, k) => {
        const [g, c] = procGlyph(s.process);
        const i = el('span', 'st' + (s.known ? '' : ' unk'), s.known ? `<b style="color:${Sprites.colour(c)}">${esc(g)}</b>` : '?');
        i.title = (s.known ? s.name : T('hud.craft.step_unknown')) + ' · ' + L('ui2d.craft.stage', { n: k + 1 });
        steps.appendChild(i);
      });
      mid.appendChild(steps);
      mid.appendChild(el('div', 'sub', esc(L('ui2d.craft.mastery', { n: pct(r.mastery) })) + ' · ' + esc(T('hud.craft.successes', { n: r.successes })) +
        (r.missing.length ? ' · <span class="bad">' + esc(L('ui2d.craft.missing', { list: r.missing.join(', ') })) + '</span>' : '')));
      row.appendChild(mid);
      const b = el('button', 'btn' + (r.master && r.can ? '' : ' off'), esc(L('ui2d.craft.from_memory')));
      b.title = L('ui2d.craft.from_memory') + ' · ' + (r.master ? T('hud.craft.fast_time', { time: r.time, stages: r.stages }) : L('ui2d.craft.not_master'));
      iconBtn(b, actIcon('memory'));
      b.onclick = () => { if (r.master && r.can) { tab = 'bench'; act('craft_fast', { recipe: r.id, count: 1 }).then(() => { const w = G.win.get('workshop'); if (w) { w.tab = 'bench'; w.refresh(); } }); } };
      row.appendChild(b);
      box.appendChild(row);
    }
    return box;
  }

  // --- things (boards 14-15): inspect, repair, onto the piece -------------------------------------
  function renderThings() {
    const box = el('div', 'ws-book');
    if (!ws.made.length) { box.appendChild(el('div', 'parch', esc(T('hud.craft.things_empty')))); return box; }
    for (const t of ws.made) {
      const row = el('div', 'parch ws-recipe');
      const ic = el('div', 'ic'); ic.appendChild(thingIcon(t, 32)); row.appendChild(ic);
      const mid = el('div', 'mid');
      mid.appendChild(el('div', 'name', esc(t.name) + ' <small>' + esc(t.tier) + '</small>'));
      const notes = [];
      if (t.wear > 0.05) notes.push(T('hud.craft.worn', { n: pct(t.wear) }));
      if (t.flaws.length) notes.push('<span class="bad">' + esc(t.flaws.join(', ')) + '</span>');
      if (t.hidden_flaws) notes.push(esc(L('ui2d.craft.hidden')));
      mid.appendChild(el('div', 'sub', notes.join(' · ')));
      const wear = el('div', 'qbar wear'); const i = el('i'); i.style.width = pct(1 - t.wear) + '%'; wear.appendChild(i);
      mid.appendChild(wear);
      row.appendChild(mid);
      const btns = el('div', 'btns');
      const ins = iconBtn(el('button', 'btn sq', '⌕'), actIcon('inspect'), 24); ins.title = L('ui2d.craft.inspect');
      ins.onclick = () => act('craft_inspect', { item: t.i });
      const rep = el('button', 'btn sq' + (t.wear > 0.05 || t.flaws.length ? '' : ' off'), '✚'); rep.title = L('ui2d.craft.repair'); iconBtn(rep, actIcon('repair'), 24);
      rep.onclick = () => { if (t.wear > 0.05 || t.flaws.length) { act('craft_repair', { item: t.i }); } };
      const bench = iconBtn(el('button', 'btn sq', '⇧'), actIcon('to_bench'), 24); bench.title = L('ui2d.craft.to_bench');
      bench.onclick = () => { putRef(t.ref); const w = G.win.get('workshop'); if (w) { w.tab = 'bench'; w.refresh(); } };
      btns.appendChild(ins); btns.appendChild(rep); btns.appendChild(bench);
      row.appendChild(btns);
      box.appendChild(row);
    }
    return box;
  }

  // --- D: the helper by wish ---------------------------------------------------------------------
  function renderHelper() {
    const box = el('div', 'ws-helper');
    box.appendChild(el('h3', '', esc(L('ui2d.craft.wish'))));
    const wl = el('div', 'wishes');
    for (const w of ws.wishes) {
      const b = el('button', 'btn wishb' + (wish === w.id ? ' on' : ''), `<span class="glyph">${esc((LOOK.wishes || {})[w.id] || '?')}</span><small>${esc(w.name)}</small>`);
      b.onclick = () => { wish = wish === w.id ? '' : w.id; load({}); };
      wl.appendChild(b);
    }
    box.appendChild(wl);
    for (const c of ws.candidates || []) {
      const card = el('div', 'parch ws-cand');
      const ic = el('div', 'ic'); ic.appendChild(thingIcon({ item: c.item, form: c.form, shape: c.shape }, 40)); card.appendChild(ic);
      const mid = el('div', 'mid');
      mid.appendChild(el('div', 'name', esc(c.name) + ` <small>${esc(c.finished ? L('ui2d.craft.done') : L('ui2d.craft.fit', { n: pct(c.fit) }))}</small>`));
      const fit = el('div', 'qbar'); const fi = el('i'); fi.style.width = pct(c.fit) + '%'; fit.appendChild(fi); mid.appendChild(fit);
      const chain = el('div', 'chain');
      c.steps.forEach((s, k) => {
        const [g, col] = procGlyph(s.process);
        const st = el('span', 'cstep' + (k === c.next ? ' now' : '') + (s.here ? '' : ' far'));
        st.innerHTML = `<i>${k + 1}</i><b style="color:${Sprites.colour(col)}">${esc(g)}</b>`;
        if (s.station) st.appendChild(stationIcon(s.station, 20));
        st.title = s.name + (s.station_name ? ' · ' + s.station_name + ' · ' + L(s.here ? 'ui2d.craft.step_here' : 'ui2d.craft.step_go') : '');
        if (!s.here && s.x != null) { st.classList.add('go'); st.onclick = () => walkTo(s); }
        chain.appendChild(st);
      });
      mid.appendChild(chain);
      if (c.missing.length) mid.appendChild(el('div', 'bad', esc(T('hud.craft.missing', { list: c.missing.join(', ') }))));
      card.appendChild(mid);
      const lay = el('button', 'btn' + (c.draft ? '' : ' off'), esc(c.draft ? L('ui2d.craft.lay') : L('ui2d.craft.no_next')));
      lay.title = c.draft ? L('ui2d.craft.lay') : L('ui2d.craft.no_next'); iconBtn(lay, actIcon('lay'));
      lay.onclick = () => {
        if (!c.draft) return;
        draft = JSON.parse(JSON.stringify(c.draft));
        if (draft.heat != null) heat = Math.min(ws.heat_max, draft.heat);
        const w = G.win.get('workshop');
        load({}).then(() => { if (w) { w.tab = 'bench'; w.refresh(); } });
      };
      card.appendChild(lay);
      box.appendChild(card);
    }
    return box;
  }
  // a station far away: walk there (the same path as a click on the map, play.js)
  function walkTo(s) {
    if (s.x == null) return;
    G.win.closeAll();
    for (const h of G.hooks.click) if (h({ shiftKey: false }, [s.x, s.y], null)) break;
  }

  // --- C on the map: a ring and a heat gauge over each station near the person -------------------
  G.hooks.draw.push((ctx, sc) => {
    if (!ws || !ws.stations || G.me == null || !G.win.isOpen('workshop') && !nearShown) return;
    ctx.save();
    for (const s of ws.stations) {
      if (!s.placed) continue;
      const [x, y] = G.toScreen(s.x, s.y);
      ctx.strokeStyle = '#e0c080'; ctx.lineWidth = Math.max(1, sc);
      ctx.setLineDash([3 * sc, 3 * sc]);
      ctx.beginPath(); ctx.arc(x, y, 11 * sc, 0, Math.PI * 2); ctx.stroke();
      ctx.setLineDash([]);
      if (s.temp_max > 0) {   // the gauge: an arc of heat over the station
        const k = Math.min(heat, s.temp_max) / s.temp_max;
        ctx.lineWidth = 3 * sc;
        ctx.strokeStyle = '#2a1a10';
        ctx.beginPath(); ctx.arc(x, y - 14 * sc, 6 * sc, Math.PI, 0); ctx.stroke();
        ctx.strokeStyle = k > 0.66 ? '#f8e060' : k > 0.33 ? '#f08020' : '#a03a10';
        ctx.beginPath(); ctx.arc(x, y - 14 * sc, 6 * sc, Math.PI, Math.PI + Math.PI * k); ctx.stroke();
      }
    }
    ctx.restore();
  });
  // near a station the map says so (and keeps the workshop fresh for the gauges)
  let nearShown = false, nearAt = 0;
  G.hooks.poll.push(() => {
    if (G.me == null || G.win.isOpen('workshop')) return;
    if (performance.now() - nearAt < 4000) return;
    nearAt = performance.now();
    load({}).then(() => { nearShown = !!(ws && ws.stations.some(s => s.placed)); });
  });
})();
