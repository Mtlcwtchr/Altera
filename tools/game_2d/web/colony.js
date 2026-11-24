// AL-94: the colony window (O) - the settlement from above, as one of its
// people sees it: who rules, the households and their people, what each one
// is doing, the rooms and buildings by what they are (read from what stands
// in them), the plan's sites and what they still lack, the offices. The
// colony mode hands one's person to their own free will while the player runs
// the settlement (plans for it, watches it); it is lifted again here.
'use strict';
(() => {
  const G = window.G;
  let data = null, loadedAt = 0, busy = false;
  function load(w) {
    if (busy) return;
    busy = true;
    G.getJson('/api/colony').then(d => { data = d; loadedAt = performance.now(); if (w) w.refresh(); })
      .catch(() => {}).finally(() => { busy = false; });
  }
  const E = s => G.esc(String(s == null ? '' : s));
  const person = id => G.byId.get(id);
  function nameOf(id) { const p = person(id); return p ? p.name : '#' + id; }
  function personRow(id) {
    const p = person(id);
    if (!p) return '';
    const doing = p.act || p.goal || G.L('ui2d.colony.idle');
    return `<div class="who col-who" data-id="${id}"><canvas data-pawn="${id}" width="20" height="20"></canvas>` +
      `<div class="nm">${E(p.name)}${p.pl ? ' ◆' : ''}<small>${E(G.L('ui2d.age', { n: Math.floor(p.age) }))} · ${E(doing)}</small></div></div>`;
  }
  function overview() {
    const d = data;
    let h = `<div class="col-grid"><div class="parch"><h3>${E(d.name)}</h3>`;
    h += `<div>${E(G.L('ui2d.colony.chief'))}: <b>${E(d.chief ? nameOf(d.chief) : G.L('ui2d.colony.nobody'))}</b></div>`;
    h += `<div>${E(G.L('ui2d.colony.people', { n: d.people, grown: d.grown, kids: d.kids }))}</div>`;
    h += `<div>${E(G.L('ui2d.colony.households', { n: d.families.length }))}</div>`;
    if (d.my_rights && d.my_rights.length) h += `<div>${E(G.L('ui2d.colony.my_rights'))}: ${d.my_rights.map(r => E(G.L('ui2d.right.' + r))).join(', ')}</div>`;
    h += `<div class="cardbtns"><button class="btn wide${d.colony_mode ? ' on' : ''}" data-act="mode">${E(G.L(d.colony_mode ? 'ui2d.colony.mode_on' : 'ui2d.colony.mode_off'))}</button>` +
      `<button class="btn wide" data-act="build">${E(G.L('ui2d.screen.build'))}</button></div>`;
    h += `<div class="dim small">${E(G.L('ui2d.colony.mode_tip'))}</div></div>`;
    h += `<div class="parch"><h3>${E(G.L('ui2d.colony.buildings'))}</h3>`;
    h += d.buildings.length ? `<div class="chips ink">${d.buildings.map(b => `<span class="chip">${E(b.name)} ×${b.n}</span>`).join('')}</div>` : `<div>${E(G.L('ui2d.colony.none'))}</div>`;
    h += `<h3>${E(G.L('ui2d.colony.rooms'))}</h3>`;
    h += d.rooms.length ? `<div class="chips ink">${d.rooms.map(b => `<span class="chip">${E(b.name)} ×${b.n}</span>`).join('')}</div>` : `<div>${E(G.L('ui2d.colony.none'))}</div>`;
    h += `<div class="dim small">${E(G.L('ui2d.colony.rooms_tip'))}</div></div>`;
    h += `<div class="parch"><h3>${E(G.L('ui2d.colony.plan'))}</h3>`;
    h += `<div>${E(G.L('ui2d.colony.sites', { n: d.planned, ready: d.ready }))}</div>`;
    if (d.lack.length) h += `<div>${E(G.L('ui2d.build.lack'))}: ${d.lack.map(l => E(l.name) + ' ' + l.n).join(', ')}</div>`;
    h += `<div class="dim small">${E(G.L('ui2d.build.free'))}</div></div></div>`;
    return h;
  }
  function households() {
    let h = '<div class="col-fams">';
    for (const f of data.families) {
      h += `<div class="parch col-fam"><h3>${E(f.name)}</h3>`;
      if (f.planned) h += `<div class="small">${E(G.L('ui2d.colony.fam_sites', { n: f.planned, ready: f.ready }))}</div>`;
      h += `<div class="col-people">${f.members.map(personRow).join('')}</div></div>`;
    }
    return h + '</div>';
  }
  function people() {
    // everybody by what they are busy with
    const groups = new Map();
    for (const f of data.families) for (const id of f.members) {
      const p = person(id);
      if (!p) continue;
      const k = p.act || p.goal || G.L('ui2d.colony.idle');
      if (!groups.has(k)) groups.set(k, []);
      groups.get(k).push(id);
    }
    let h = '<div class="col-fams">';
    for (const [k, ids] of [...groups].sort((a, b) => b[1].length - a[1].length))
      h += `<div class="parch col-fam"><h3>${E(k)} <small>×${ids.length}</small></h3><div class="col-people">${ids.map(personRow).join('')}</div></div>`;
    return h + '</div>';
  }
  function sites() {
    const d = data;
    if (!d.sites.length) return `<div class="parch">${E(G.L('ui2d.colony.no_sites'))}</div>`;
    // by place: a site a row, nearest the planner's own first
    const famName = new Map(d.families.map(f => [f.id, f.name]));
    let h = `<div class="parch"><div>${E(G.L('ui2d.colony.sites', { n: d.planned, ready: d.ready }))}</div>`;
    if (d.lack.length) h += `<div>${E(G.L('ui2d.build.lack'))}: ${d.lack.map(l => E(l.name) + ' ' + l.n).join(', ')}</div>`;
    h += '</div><table class="col-sites"><tr>' + ['what', 'whose', 'state', 'lack'].map(k => `<th>${E(G.L('ui2d.colony.col.' + k))}</th>`).join('') + '</tr>';
    // alike pieces of one household in one state go in one row (a room is dozens of pieces)
    const rows = new Map();
    for (const st of d.sites) {
      const state = st.ready ? 'ready' : !st.held ? 'wait_hold' : 'wait_goods';
      const k = [st.kind, st.fam, st.z, state].join('|');
      if (!rows.has(k)) rows.set(k, { s: st, state, n: 0, lack: new Map() });
      const r = rows.get(k);
      r.n++;
      for (const l of st.lack) r.lack.set(l.name, (r.lack.get(l.name) || 0) + l.n);
    }
    const order = { ready: 0, wait_goods: 1, wait_hold: 2 };
    const list = [...rows.values()].sort((a, b) => (a.s.who === 'mine' ? 0 : 1) - (b.s.who === 'mine' ? 0 : 1) || order[a.state] - order[b.state] || a.s.z - b.s.z);
    const pieceName = id => (G.build && G.build.pieceName(id)) || id;
    for (const r of list) {
      const s = r.s;
      const whose = s.who === 'settlement' ? G.L('ui2d.build.for.town') : (famName.get(s.fam) || '');
      h += `<tr class="col-site" data-x="${s.cx}" data-y="${s.cy}"><td>${E(pieceName(s.kind))}${r.n > 1 ? ' ×' + r.n : ''}${s.z ? ' · ' + E(G.L('ui2d.build.storey', { n: s.z })) : ''}</td><td>${E(whose)}</td>` +
        `<td>${E(G.L('ui2d.colony.st.' + r.state))}</td><td>${[...r.lack].map(([k, n]) => E(k) + ' ' + n).join(', ')}</td></tr>`;
    }
    return h + '</table>';
  }
  function offices() {
    const d = data;
    let h = '<div class="col-fams">';
    for (const o of d.offices) {
      h += `<div class="parch col-fam"><h3>${E(G.L('ui2d.right.' + o.right))}</h3><div class="small">${E(G.L('ui2d.right_tip.' + o.right))}</div>`;
      h += o.holders.length ? `<div class="col-people">${o.holders.map(personRow).join('')}</div>` : `<div>${E(G.L('ui2d.colony.nobody'))}</div>`;
      h += '</div>';
    }
    return h + '</div>';
  }
  function paintPawns(body) {
    for (const cv of body.querySelectorAll('canvas[data-pawn]')) {
      const p = person(Number(cv.dataset.pawn));
      if (p && G.portrait) {
        const img = G.portrait(p, 20);
        if (img && img.tagName === 'CANVAS') { cv.getContext('2d').drawImage(img, 0, 0, 20, 20); }
      }
    }
  }
  G.screens.colony = () => {
    const w = G.win.open({
      id: 'colony', wide: true, title: () => data ? `${G.L('ui2d.screen.colony')} · ${data.name}` : G.L('ui2d.screen.colony'),
      tabs: [['overview', G.L('ui2d.colony.tab.overview')], ['families', G.L('ui2d.colony.tab.families')], ['people', G.L('ui2d.colony.tab.people')],
        ['sites', G.L('ui2d.colony.tab.sites')], ['offices', G.L('ui2d.colony.tab.offices')]],
      tab: 'overview',
      render(body, tab) {
        if (!data) { body.innerHTML = `<div class="parch">${E(G.L('ui2d.loading'))}</div>`; return; }
        const f = { overview, families: households, people, sites, offices }[tab] || overview;
        body.innerHTML = f();
        paintPawns(body);
        for (const el of body.querySelectorAll('.col-who')) el.onclick = () => { G.select(Number(el.dataset.id)); const p = person(Number(el.dataset.id)); if (p) { G.cam.x = p.x; G.cam.y = p.y; } };
        for (const el of body.querySelectorAll('.col-site')) el.onclick = () => { G.cam.x = Number(el.dataset.x); G.cam.y = Number(el.dataset.y); G.follow = false; G.win.get('colony').close(); };
        const mode = body.querySelector('[data-act="mode"]');
        if (mode) mode.onclick = () => {
          fetch('/api/colony/mode', { method: 'POST', body: JSON.stringify({ on: !data.colony_mode }) }).then(r => r.json())
            .then(d => { data = d; G.follow = false; G.poll(); const cw = G.win.get('colony'); if (cw) cw.refresh(); });
        };
        const b = body.querySelector('[data-act="build"]');
        if (b) b.onclick = () => { G.win.get('colony').close(); G.build && G.build.setMode(true); };
      },
      onPoll(s, w) { if (performance.now() - loadedAt > 2500) load(w); },
    });
    load(w);
  };
})();
