// AL-96: leading a person in the 2D client (the 3D game's controls, from above):
// WASD walks the player's person in real time (the sim's "steer"), walls and
// water hold them; LMB on the ground walks there by the cells (A* over the
// loaded map); RMB on a thing, a person, a beast or oneself opens the act menu
// the sim checks (can / why / time / gains) and orders "perform"; the quick bar
// 1-8 holds acts and goals; the act going on shows as a cast bar; what people
// say runs on a gradient bottom left and over their heads. X breaks off the act.
'use strict';
(() => {
  const G = window.G;
  const { L, esc, $ } = G;
  const WALK = 4.2, RUN = 7.5;               // metres (cells) a second, real time
  let path = null;                           // [[x, y]...] cell centres to walk
  let lastSteer = 0, lastMove = 0, steerAt = null;
  let toastTimer = 0;

  // --- the map as the walk sees it: water cells and the walls of the ground storey ----
  let blockKey = '', walls = new Set();
  function chunkAt(x, y) { return G.chunks.get(Math.floor(x / 64) + ',' + Math.floor(y / 64)); }
  function groundAt(x, y) {
    const c = chunkAt(x, y);
    if (!c) return null;
    const j = c.json, i = (Math.floor(y) - j.y0) * j.w + (Math.floor(x) - j.x0);
    return G.ST.ground[j.ground.charCodeAt(i) - 97];
  }
  function rebuildWalls() {
    let k = '';
    for (const [id, c] of G.chunks) k += id + c.at;
    if (k === blockKey) return;
    blockKey = k;
    walls = new Set();
    for (const c of G.chunks.values())
      for (const e of c.json.edges) {
        if (e.z !== 0 || e.kind === 'door' || !e.built) continue;
        for (let i = 0; i < e.len; i++) walls.add(e.axis === 'h' ? `h${e.x + i},${e.y}` : `v${e.x},${e.y + i}`);
      }
  }
  const passCell = (x, y) => { const g = groundAt(x, y); return g !== null && g !== 'water'; };
  // can one go from cell (ax, ay) to the next cell (bx, by) (4-neighbours)
  function stepOk(ax, ay, bx, by) {
    if (!passCell(bx + 0.5, by + 0.5)) return false;
    if (bx !== ax) return !walls.has(`v${Math.max(ax, bx)},${ay}`);
    if (by !== ay) return !walls.has(`h${ax},${Math.max(ay, by)}`);
    return true;
  }
  // a straight move a -> b (short), stopped by a wall line or water
  function moveOk(ax, ay, bx, by) {
    const cx0 = Math.floor(ax), cy0 = Math.floor(ay), cx1 = Math.floor(bx), cy1 = Math.floor(by);
    if (cx0 === cx1 && cy0 === cy1) return true;
    if (cx0 !== cx1 && cy0 !== cy1) return stepOk(cx0, cy0, cx1, cy0) && stepOk(cx1, cy0, cx1, cy1) || stepOk(cx0, cy0, cx0, cy1) && stepOk(cx0, cy1, cx1, cy1);
    return stepOk(cx0, cy0, cx1, cy1);
  }
  function findPath(sx, sy, tx, ty) {
    sx = Math.floor(sx); sy = Math.floor(sy); tx = Math.floor(tx); ty = Math.floor(ty);
    if (!passCell(tx + 0.5, ty + 0.5)) return null;
    const key = (x, y) => x * 100003 + y;
    const open = [[Math.abs(tx - sx) + Math.abs(ty - sy), 0, sx, sy]], came = new Map(), cost = new Map([[key(sx, sy), 0]]);
    let n = 0;
    while (open.length && n++ < 40000) {
      // a small binary heap would be faster; the walks are short
      let bi = 0;
      for (let i = 1; i < open.length; i++) if (open[i][0] < open[bi][0]) bi = i;
      const [, g, x, y] = open.splice(bi, 1)[0];
      if (x === tx && y === ty) {
        const out = [];
        let k = key(x, y), cx = x, cy = y;
        while (came.has(k)) { out.push([cx + 0.5, cy + 0.5]); [cx, cy] = came.get(k); k = key(cx, cy); }
        return smooth(out.reverse(), sx, sy);
      }
      if (g > (cost.get(key(x, y)) ?? Infinity)) continue;
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
        const nx = x + dx, ny = y + dy;
        if (!stepOk(x, y, nx, ny)) continue;
        const ng = g + 1, nk = key(nx, ny);
        if (ng >= (cost.get(nk) ?? Infinity)) continue;
        cost.set(nk, ng); came.set(nk, [x, y]);
        open.push([ng + Math.abs(tx - nx) + Math.abs(ty - ny), ng, nx, ny]);
      }
    }
    return null;
  }
  // drop the cells a straight walk passes anyway (line of sight over the cells)
  function sight(ax, ay, bx, by) {
    const d = Math.hypot(bx - ax, by - ay), n = Math.ceil(d * 3);
    let px = ax, py = ay;
    for (let i = 1; i <= n; i++) {
      const x = ax + (bx - ax) * i / n, y = ay + (by - ay) * i / n;
      if (!moveOk(px, py, x, y)) return false;
      px = x; py = y;
    }
    return true;
  }
  function smooth(pts, sx, sy) {
    const out = [];
    let ax = sx + 0.5, ay = sy + 0.5, i = 0;
    while (i < pts.length) {
      let j = pts.length - 1;
      while (j > i && !sight(ax, ay, pts[j][0], pts[j][1])) j--;
      out.push(pts[j]); [ax, ay] = pts[j]; i = j + 1;
    }
    return out;
  }

  // --- walking the player's person ------------------------------------------------------
  function myVis() { return G.me != null ? G.vis.get(G.me) : null; }
  G.hooks.vis.push(dt => {
    const v = myVis();
    if (!v) return;
    rebuildWalls();
    const k = G.keys;
    let dx = (k.has('d') ? 1 : 0) - (k.has('a') ? 1 : 0), dy = (k.has('s') ? 1 : 0) - (k.has('w') ? 1 : 0);
    const now = performance.now();
    if (dx || dy) path = null;
    if (!dx && !dy && path && path.length) {
      const [px, py] = path[0], d = Math.hypot(px - v.x, py - v.y);
      if (d < 0.15) { path.shift(); if (!path.length) path = null; }
      else { dx = (px - v.x) / d; dy = (py - v.y) / d; }
    }
    if (dx || dy) {
      const len = Math.hypot(dx, dy), sp = (k.has('Shift') ? RUN : WALK) * dt;
      let s = sp;
      if (path && path.length) s = Math.min(sp, Math.hypot(path[0][0] - v.x, path[0][1] - v.y));
      const nx = v.x + dx / len * s, ny = v.y + dy / len * s;
      const before = Math.floor(v.x) + Math.floor(v.y);
      if (moveOk(v.x, v.y, nx, ny)) { v.x = nx; v.y = ny; }
      else if (moveOk(v.x, v.y, nx, v.y)) v.x = nx;       // slide along the wall
      else if (moveOk(v.x, v.y, v.x, ny)) v.y = ny;
      else path = null;
      if (Math.floor(v.x) + Math.floor(v.y) !== before) v.bob ^= 1;
      v.face = Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'E' : 'W') : (dy > 0 ? 'S' : 'N');
      v.moving = true;
      v.driven = true;
      lastMove = now;
      G.follow = true;
    } else if (v.driven) v.moving = false;
    // tell the sim where the person is, a few times a second
    if (v.driven && now - lastSteer > 120 && (!steerAt || Math.hypot(steerAt[0] - v.x, steerAt[1] - v.y) > 0.05)) {
      lastSteer = now;
      steerAt = [v.x, v.y];
      G.cmd({ op: 'cmd', type: 'steer', args: { id: G.me, x: v.x, y: v.y, run: k.has('Shift') } }).then(r => { if (r.result) toast(r.text || r.result); });
    }
    // standing still a while: the sim's own walking takes over again
    if (v.driven && now - lastMove > 700 && now - lastSteer > 300) v.driven = false;
  });
  G.hooks.click.push((e, w, p) => {
    if (G.me == null || e.shiftKey) return false;
    if (p) return false;                      // a person: select as usual
    const v = myVis();
    if (!v) return false;
    rebuildWalls();
    const found = findPath(v.x, v.y, w[0], w[1]);
    if (!found) { toast(L('ui2d.no_way')); return true; }
    path = found;
    marker = { x: Math.floor(w[0]) + 0.5, y: Math.floor(w[1]) + 0.5, at: performance.now() };
    return true;
  });
  let marker = null;

  // --- the act menu ------------------------------------------------------------------------
  const menu = $('actmenu');
  function closeMenu() { menu.hidden = true; }
  function thingAt(wx, wy) {
    let best = null, bd = 0.9;
    for (const c of G.chunks.values())
      for (const t of c.json.things) {
        if ((G.MAP.hidden || []).includes(t[1])) continue;
        const d = Math.hypot(t[2] - wx, t[3] - wy);
        if (d < bd) { bd = d; best = t; }
      }
    return best;
  }
  function openMenu(e, target, title) {
    if (G.me == null) { toast(L('ui2d.lead_first')); return; }
    const r = G.canvas.getBoundingClientRect();
    G.getJson('/api/acts' + (target != null ? '?target=' + target : '')).then(acts => {
      let h = `<h3>${esc(title)}</h3>`;
      if (!acts.length) h += `<div class="parch">${esc(L('ui2d.no_acts'))}</div>`;
      acts.sort((a, b) => (b.can - a.can));
      for (const a of acts) {
        const gains = a.gains.concat(a.to_target.map(x => '→ ' + x)).join(', ');
        h += `<button class="act${a.can ? '' : ' off'}" data-id="${esc(a.id)}">${esc(a.name)}<span class="m">${esc(a.minutes >= 60 ? L('ui2d.hours', { n: Math.round(a.minutes / 60) }) : L('ui2d.minutes', { n: a.minutes }))}</span>` +
          (gains ? `<span class="g">${esc(gains)}</span>` : '') + (a.legal.length ? `<span class="w">${esc(a.legal.join(', '))}</span>` : '') +
          (!a.can && a.why ? `<span class="w">${esc(a.why)}</span>` : '') + `<span class="pin" data-pin="${esc(a.id)}" title="${esc(L('ui2d.pin'))}">＋</span></button>`;
      }
      menu.innerHTML = h;
      menu.hidden = false;
      const mx = e.clientX - r.left + 14, my = e.clientY - r.top;
      menu.style.left = Math.min(mx, r.width - menu.offsetWidth - 10) + 'px';
      menu.style.top = Math.max(10, Math.min(my - 20, r.height - menu.offsetHeight - 10)) + 'px';
      for (const b of menu.querySelectorAll('.act')) b.onclick = ev => {
        const pin = ev.target.dataset && ev.target.dataset.pin;
        if (pin) { pinToBar({ kind: 'act', id: pin, target: target != null && (G.byId.get(target) && !G.byId.get(target).sp) }); ev.stopPropagation(); return; }
        const a = acts.find(x => x.id === b.dataset.id);
        if (!a || !a.can) return;
        closeMenu();
        perform(a.id, target);
      };
    });
  }
  function perform(id, target) {
    path = null;
    const v = myVis(); if (v) v.driven = false;
    const args = { id: G.me, interaction: id };
    if (target != null) args.target = target;
    G.cmd({ op: 'cmd', type: 'perform', args }).then(r => { if (r.result) toast(r.text || r.result); G.poll(); });
  }
  G.hooks.rclick.push((e, w, p) => {
    if (p) { openMenu(e, p.id === G.me ? null : p.id, p.id === G.me ? L('ui2d.self') : p.name); return true; }
    const t = thingAt(w[0], w[1]);
    if (t) { openMenu(e, t[0], (G.ST.objects && G.ST.objects[t[1]]) || t[1]); return true; }
    closeMenu();
    return true;
  });
  window.addEventListener('mousedown', e => { if (!menu.hidden && !menu.contains(e.target) && e.button === 0) closeMenu(); });

  // --- the quick bar 1-8 (kept per person in the browser) ---------------------------------
  const START = [{ kind: 'act', id: 'chat', target: true }, { kind: 'goal', id: 'eat' }, { kind: 'goal', id: 'drink' }, { kind: 'goal', id: 'rest' }, null, null, null, null];
  let barFor = null, slots = START.slice(), checked = [];
  function storeKey() { return 'altera2d.bar.' + G.me; }
  function loadBar() {
    barFor = G.me;
    slots = START.slice();
    try { const s = JSON.parse(localStorage.getItem(storeKey()) || 'null'); if (Array.isArray(s) && s.length === 8) slots = s; } catch (e) { /* no storage */ }
    renderBar();
  }
  function saveBar() { try { localStorage.setItem(storeKey(), JSON.stringify(slots)); } catch (e) { /* no storage */ } }
  function pinToBar(ref) {
    const i = slots.findIndex(s => !s);
    if (i < 0) { toast(L('ui2d.bar_full')); return; }
    slots[i] = ref; saveBar(); checkBar(); toast(L('ui2d.pinned', { n: i + 1 }));
  }
  function renderBar() {
    const box = $('hotbar');
    box.hidden = G.me == null;
    box.innerHTML = '';
    slots.forEach((s, i) => {
      const c = checked[i];
      const d = document.createElement('div');
      d.className = 'slot' + (s ? '' : ' empty') + (s && c && !c.can ? ' off' : '');
      d.innerHTML = `<span class="k">${i + 1}</span>${s ? esc((c && c.name) || s.id) : ''}`;
      if (s && c) d.title = c.can ? c.name : (c.name + ': ' + (c.why || ''));
      d.onclick = () => useSlot(i);
      d.oncontextmenu = ev => { ev.preventDefault(); slots[i] = null; saveBar(); checkBar(); };
      box.appendChild(d);
    });
  }
  function checkBar() {
    if (G.me == null) { renderBar(); return; }
    const list = slots.map(s => s || { kind: '', id: '' });
    fetch('/api/bar', { method: 'POST', body: JSON.stringify({ slots: list, target: G.selId && G.selId !== G.me ? G.selId : 0 }) })
      .then(r => r.json()).then(c => { checked = c; renderBar(); }).catch(() => {});
  }
  function useSlot(i) {
    const s = slots[i];
    if (!s || G.me == null) return;
    const c = checked[i];
    if (c && !c.can) { toast(c.why || L('ui2d.cannot')); return; }
    if (s.kind === 'goal') {
      path = null;
      G.cmd({ op: 'cmd', type: 'set_goal', args: { id: G.me, goal: s.id } }).then(r => { if (r.result) toast(r.text || r.result); });
    } else perform(s.id, c && c.needs_target ? G.selId : undefined);
  }
  setInterval(checkBar, 1200);

  G.hooks.key.push(e => {
    if (G.me == null) return false;
    if (/^[1-8]$/.test(e.key) && !e.altKey) { useSlot(Number(e.key) - 1); return true; }
    if (e.key === 'x' || e.key === 'X') {
      path = null;
      G.cmd({ op: 'cmd', type: 'cancel_goal', args: { id: G.me } }).then(() => G.poll());
      return true;
    }
    if (e.key === 'Escape' && !menu.hidden) { closeMenu(); return true; }
    return false;
  });

  // --- speech, the cast bar, toasts -----------------------------------------------------------
  const bubbles = new Map();          // speaker -> {line, until}
  const lines = [];
  function toast(text) {
    if (!text) return;
    const t = $('toast');
    t.textContent = text; t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { t.hidden = true; }, 3200);
  }
  G.toast = toast;
  let selectedMe = false;
  G.hooks.poll.push(s => {
    if (G.me !== barFor) loadBar();
    if (G.me != null && !selectedMe && G.selId == null) { selectedMe = true; G.select(G.me); }
    for (const e of s.speech || []) {
      bubbles.set(e.who, { line: e.line, until: performance.now() + (e.bark ? 3500 : 5000), bark: e.bark });
      if (!e.bark) { lines.push({ text: e.full, at: performance.now() }); if (lines.length > 7) lines.shift(); }
    }
    const box = $('speech');
    const now = performance.now();
    box.innerHTML = lines.filter(l => now - l.at < 40000).map(l => `<div class="${now - l.at > 15000 ? 'old' : ''}">${esc(l.text)}</div>`).join('');
    // the cast bar: the player's person's act going on
    const meP = G.me != null ? G.byId.get(G.me) : null;
    const cast = $('cast');
    if (meP && meP.prog != null && meP.act) {
      cast.hidden = false;
      $('cast-name').textContent = meP.act;
      $('cast-fill').style.width = Math.round(meP.prog * 100) + '%';
    } else cast.hidden = true;
  });
  G.hooks.draw.push((ctx, sc, tp) => {
    const now = performance.now();
    // the walk's target
    if (marker && now - marker.at < 1500 && path) {
      const [x, y] = G.toScreen(marker.x, marker.y);
      ctx.strokeStyle = '#e8c060'; ctx.lineWidth = sc;
      const r = (4 + (now - marker.at) / 300) * sc;
      ctx.strokeRect(x - r, y - r, 2 * r, 2 * r);
    }
    if (path && G.me != null) {
      ctx.fillStyle = 'rgba(232,192,96,0.7)';
      for (const [x, y] of path) { const [sx, sy] = G.toScreen(x, y); ctx.fillRect(sx - sc, sy - sc, 2 * sc, 2 * sc); }
    }
    // the player's person: a ring of the player's colour under the feet
    const v = myVis();
    if (v) {
      const [sx, sy] = G.toScreen(v.x, v.y);
      ctx.strokeStyle = '#c03028'; ctx.lineWidth = Math.max(1, sc * 0.75);
      ctx.beginPath(); ctx.ellipse(sx, sy + sc, 7 * sc, 3 * sc, 0, 0, Math.PI * 2); ctx.stroke();
    }
    // lines over the heads
    ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
    ctx.font = `${Math.round(5.5 * Math.max(2, sc))}px Alegreya, serif`;
    for (const [who, b] of bubbles) {
      if (now > b.until) { bubbles.delete(who); continue; }
      const pv = G.vis.get(who);
      if (!pv) continue;
      const [sx, sy] = G.toScreen(pv.x, pv.y);
      const text = b.line.length > 60 ? b.line.slice(0, 58) + '…' : b.line;
      const w = ctx.measureText(text).width + 10 * G.dpr, h = 9 * Math.max(2, sc) * 0.75, y = sy - 26 * sc;
      ctx.globalAlpha = b.bark ? 0.75 : 0.95;
      ctx.fillStyle = '#1a1410'; ctx.fillRect(sx - w / 2 - 1, y - h - 1, w + 2, h + 2);
      ctx.fillStyle = b.bark ? '#d8ccb0' : '#e8d8b0'; ctx.fillRect(sx - w / 2, y - h, w, h);
      ctx.fillStyle = '#3a2414'; ctx.fillText(text, sx, y - h * 0.12);
      ctx.globalAlpha = 1;
    }
  });
})();
