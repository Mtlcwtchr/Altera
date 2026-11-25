// AL-100: the character sheet (C) of the 2D client, screen 02 of gdd/ui/client_2d/
// ui_h3/UI_H3.md: a big carved window, ◀ portrait ▶ to page through the people,
// five pages as tabs - Overview, Relations, Things, Skills & traits, Chronicle.
// Data: /api/sheet?id= (src/client2d/sheet2d.cpp), refreshed while open.
// Strings: ui2d.sheet.* (content/loc/<lang>/ui2d_sheet.json).
//   G.screens.sheet(id?)  - open on a person (default: the selected one, else the led one)
'use strict';
(() => {
  const G = window.G;
  const L = (k, a) => G.L('ui2d.sheet.' + k, a);
  const esc = s => G.esc(s);
  const pct = v => Math.round(Math.max(0, Math.min(1, v)) * 100);
  // the page's own look, kept next to it
  const css = document.createElement('link');
  css.rel = 'stylesheet'; css.href = '/web/character.css';
  document.head.appendChild(css);

  const S = { id: null, data: null, key: '', prevNeeds: new Map(), trend: new Map(), relFilter: 'all', relSel: null, thing: null, at: 0 };

  // the people one pages through: the humans, by household then age
  function ring() {
    const humans = G.people.filter(p => !p.sp);
    return humans.sort((a, b) => (a.fam || '').localeCompare(b.fam || '') || b.age - a.age).map(p => p.id);
  }
  function step(dir) {
    const r = ring();
    if (!r.length) return;
    const i = r.indexOf(S.id);
    open(r[(i + dir + r.length) % r.length]);
  }

  function load(w) {
    if (S.id == null) return;
    const id = S.id;
    S.at = performance.now();
    G.getJson('/api/sheet?id=' + id).then(d => {
      if (id !== S.id) return;
      if (d) {
        // a need's trend: compared with the last read some seconds ago
        for (const n of d.overview.needs) {
          const old = S.prevNeeds.get(n.id);
          if (old != null && Math.abs(n.v - old) >= 0.005) S.trend.set(n.id, n.v > old ? 1 : -1);
          S.prevNeeds.set(n.id, n.v);
        }
      }
      const key = JSON.stringify(d);
      if (key === S.key) return;
      S.key = key; S.data = d;
      if (w) w.refresh();
    }).catch(e => console.warn(e));
  }

  // --- pieces -------------------------------------------------------------------------
  const bar = (v, colour) => G.bar(v, colour);
  const tone = v => G.tone(v);
  function meter(name, v, colour, txt, sub) {
    return `<div class="cs-meter inset"><div class="h">${esc(name)}</div><div class="v">${esc(txt)}</div>${bar(v, colour)}` +
      (sub ? `<div class="sub">${esc(sub)}</div>` : '') + '</div>';
  }
  function axisRow(name, v, signed) {
    // signed axes (-1..1) fill from the middle
    if (signed) {
      const w = Math.round(Math.abs(v) * 50), left = v >= 0 ? 50 : 50 - w;
      const c = v >= 0 ? 'var(--good)' : 'var(--bad)';
      return `<div class="row"><span>${esc(name)}</span><div class="bar mid"><i style="left:${left}%;width:${w}%;background:${c}"></i></div><span>${Math.round(v * 100)}</span></div>`;
    }
    return `<div class="row"><span>${esc(name)}</span>${bar(v, 'var(--bronze)')}<span>${Math.round(v * 100)}</span></div>`;
  }
  const SIGNED = new Set(['trust', 'affinity', 'respect', 'debt']);
  function memLine(m) {
    const src = m.src === 'seen' ? '' : L('src.' + m.src);
    const conf = m.conf < 0.99 ? L('believes', { n: Math.round(m.conf * 100) }) : '';
    const sign = m.v > 0.05 ? '<i class="plus">+</i>' : m.v < -0.05 ? '<i class="minus">−</i>' : '<i class="dot">·</i>';
    const tail = [src, conf].filter(Boolean).join(', ');
    return `<div class="cs-mem${m.src === 'rumor' ? ' rumor' : ''}">${sign}<b>${esc(G.L('ui2d.day', { n: m.day + 1 }))} ${esc(m.t)}</b> ${esc(m.text)}` +
      (tail ? ` <small>(${esc(tail)})</small>` : '') + '</div>';
  }
  const pronoun = sex => L('pron.' + (sex === 'f' ? 'f' : 'm'));

  // --- the head: ◀ portrait name ▶ and the buttons ---------------------------------------------------
  function head(d) {
    const sub = [d.role, d.occ, d.standing, G.L('ui2d.age', { n: d.age }), d.fam].filter(Boolean).join(' · ');
    const me = d.id === G.me;
    let h = `<div class="cs-head"><button class="btn sq" id="cs-prev" title="${esc(L('prev'))} (,)">◀</button>` +
      `<span class="cs-pic" id="cs-pic"></span>` +
      `<div class="cs-name"><div class="n">${esc(d.name)}</div><div class="sub">${esc(sub)}</div>` +
      (!d.alive ? `<div class="sub bad">${esc(G.L('ui2d.dead'))}${d.cause ? ': ' + esc(d.cause) : ''}</div>` : '') +
      (d.overview.captive ? `<div class="sub bad">${esc(L('captive'))}</div>` : '') + '</div>' +
      `<button class="btn sq" id="cs-next" title="${esc(L('next'))} (.)">▶</button><div class="cs-btns">` +
      `<button class="btn wide" id="cs-map">${esc(L('to_map'))}</button>`;
    if (d.alive && (me || d.lead !== false)) h += `<button class="btn wide" id="cs-lead">${esc(L(me ? 'release' : 'lead'))}</button>`;
    if (d.alive && G.me != null && !me && G.screens.talk) h += `<button class="btn wide" id="cs-talk">${esc(L('talk'))}</button>`;
    return h + '</div></div>';
  }
  function wireHead(body, d, w) {
    const p = G.byId.get(d.id);
    const pic = body.querySelector('#cs-pic');
    if (p && pic) pic.appendChild(G.portrait(p, 72));
    body.querySelector('#cs-prev').onclick = () => step(-1);
    body.querySelector('#cs-next').onclick = () => step(1);
    body.querySelector('#cs-map').onclick = () => { G.select(d.id); const v = G.vis.get(d.id); if (v) { G.cam.x = v.x; G.cam.y = v.y; } w.close(); };
    const lead = body.querySelector('#cs-lead');
    if (lead) lead.onclick = () => G.cmd(d.id === G.me ? { op: 'cmd', type: 'release_control', args: {} } : { op: 'cmd', type: 'take_control', args: { id: d.id } })
      .then(() => { S.key = ''; G.poll(); setTimeout(() => load(w), 300); });
    const talk = body.querySelector('#cs-talk');
    if (talk) talk.onclick = () => { w.close(); G.screens.talk(d.id); };
  }

  // --- pages ----------------------------------------------------------------------------------
  function pageOverview(d) {
    const o = d.overview;
    const good = o.moodlets.filter(m => m.v > 0).length, bad = o.moodlets.filter(m => m.v < 0).length;
    const mood01 = (o.mood + 1) / 2;
    const worstWound = o.wounds[0];
    let h = '<div class="cs-meters">';
    h += meter(L('health'), o.hp, tone(o.hp), pct(o.hp) + ' / 100', worstWound ? `${worstWound.sev}: ${worstWound.zone}` : '');
    h += meter(L('strength'), 1 - o.fatigue, tone(1 - o.fatigue), pct(1 - o.fatigue) + ' / 100', o.act || '');
    h += meter(L('blood'), o.blood, o.blood < 0.6 ? 'var(--bad)' : '#a0282a', pct(o.blood) + ' / 100', o.wounds.some(x => x.bleeding) ? L('bleeding') : '');
    h += meter(L('mood'), mood01, tone(mood01), (o.mood >= 0 ? '+' : '') + Math.round(o.mood * 100), `+${good} / −${bad}`);
    h += '</div><div class="cs-cols">';
    // now and next
    h += `<div class="inset cs-box"><h3>${esc(L('doing'))}</h3><div class="parch">${esc(o.act || G.L('ui2d.idle'))}` +
      (o.goal ? `<br><b>${esc(L('wants'))}:</b> ${esc(o.goal)}` : '') + '</div>';
    if (o.plan.length) h += `<h3>${esc(L('plan'))}</h3><ol class="cs-plan">${o.plan.map(a => `<li class="${a.now ? 'now' : ''}">${esc(a.name)}</li>`).join('')}</ol>`;
    if (o.wounds.length || o.injuries.length) {
      h += `<h3>${esc(L('wounds'))}</h3><div class="parch">`;
      h += o.wounds.map(x => `<div>${esc(x.sev)}: ${esc(x.zone)}` + (x.bleeding ? `, <span class="bad">${esc(L('bleeding'))}</span>` : '') +
        (x.infected ? `, <span class="bad">${esc(L('infected'))}</span>` : '') + ` <small>${esc(L('heals', { n: x.days }))}</small></div>`).join('');
      h += o.injuries.map(x => `<div>${esc(x.kind)} <small>(${esc(x.zone)})</small></div>`).join('');
      h += '</div>';
    }
    h += '</div>';
    // thoughts (+/-)
    h += `<div class="inset cs-box"><h3>${esc(L('thoughts'))}</h3>`;
    if (!o.moodlets.length) h += `<div class="parch dim">${esc(L('none'))}</div>`;
    else h += '<div class="parch">' + o.moodlets.map(m => `<div class="cs-thought"><i class="${m.v >= 0 ? 'plus' : 'minus'}">${m.v >= 0 ? '+' : '−'}${Math.round(Math.abs(m.v) * 100)}</i> ${esc(m.name)}` +
      ` <small>${esc(m.h < 0 ? L('lasting') : L('hours', { n: m.h }))}</small></div>`).join('') + '</div>';
    h += '</div>';
    // needs
    h += `<div class="inset cs-box"><h3>${esc(L('needs'))}</h3>`;
    for (const n of o.needs.slice().sort((a, b) => a.v - b.v)) {
      const tr = S.trend.get(n.id);
      const arrow = tr > 0 ? `<i class="up" title="${esc(L('trend.up'))}">▲</i>` : tr < 0 ? `<i class="down" title="${esc(L('trend.down'))}">▼</i>` : '<i></i>';
      h += `<div class="row need${n.v <= n.crit ? ' crit' : ''}"><span>${esc(n.name)}</span>${bar(n.v, tone(n.v))}<span>${pct(n.v)}${arrow}</span></div>`;
    }
    h += '</div></div>';
    return h;
  }

  function pageRelations(d) {
    const F = ['all', 'family', 'friends', 'foes', 'known'];
    const list = d.relations.filter(r => S.relFilter === 'all' || r.group === S.relFilter);
    if (S.relSel == null || !d.relations.some(r => r.id === S.relSel)) S.relSel = list.length ? list[0].id : null;
    let h = '<div class="cs-filters">' + F.map(f => {
      const n = f === 'all' ? d.relations.length : d.relations.filter(r => r.group === f).length;
      return `<button class="btn tab${S.relFilter === f ? ' on' : ''}" data-f="${f}">${esc(L('filter.' + f))} <small>${n}</small></button>`;
    }).join('') + '</div><div class="cs-rel">';
    h += '<div class="inset cs-table"><table><thead><tr>' +
      `<th>${esc(L('col.name'))}</th><th>${esc(L('col.who'))}</th><th>${esc(L('axis.affinity'))}</th><th>${esc(L('axis.trust'))}</th><th>${esc(L('axis.fear'))}</th></tr></thead><tbody>`;
    if (!list.length) h += `<tr><td colspan="5" class="dim">${esc(L('no_one'))}</td></tr>`;
    for (const r of list) {
      const who = r.kin.concat(r.pairs).join(', ') || L('who.' + r.group);
      const cell = (v, signed) => `<td class="num ${signed ? (v > 0.05 ? 'good' : v < -0.05 ? 'bad' : '') : (v > 0.3 ? 'bad' : '')}">${Math.round(v * 100)}</td>`;
      h += `<tr data-id="${r.id}" class="${r.id === S.relSel ? 'sel' : ''}${r.alive ? '' : ' dead'}"><td>${esc(r.name)}</td><td>${esc(who)}</td>` +
        cell(r.axes.affinity, true) + cell(r.axes.trust, true) + cell(r.axes.fear, false) + '</tr>';
    }
    h += '</tbody></table></div>';
    const r = d.relations.find(x => x.id === S.relSel);
    h += '<div class="inset cs-box cs-other">';
    if (r) {
      const who = r.kin.concat(r.pairs).join(', ') || L('stranger');
      h += `<div class="cs-ohead"><span id="cs-opic"></span><div><div class="n">${esc(r.name)}</div>` +
        `<div class="sub">${esc(L('rel_to', { pron: pronoun(r.sex), what: who }))}</div>` +
        `<div class="sub">${esc([r.fam, G.L('ui2d.age', { n: r.age })].filter(Boolean).join(' · '))}</div></div></div>`;
      if (r.traits.length) h += `<div class="chips">${r.traits.map(t => `<span class="chip">${esc(t)}</span>`).join('')}</div>`;
      h += `<h3>${esc(L('axes'))}</h3>`;
      for (const ax of ['familiarity', 'affinity', 'trust', 'respect', 'fear', 'enmity', 'attraction', 'debt'])
        if (ax === 'familiarity' || ax === 'affinity' || ax === 'trust' || Math.abs(r.axes[ax]) >= 0.01) h += axisRow(L('axis.' + ax), r.axes[ax], SIGNED.has(ax));
      h += `<h3>${esc(L(r.sex === 'f' ? 'remembers_f' : 'remembers'))}</h3>`;
      h += r.mem.length ? `<div class="parch">${r.mem.map(memLine).join('')}</div>` : `<div class="parch dim">${esc(L('none'))}</div>`;
      if (r.alive && (r.act || r.goal)) h += `<h3>${esc(L('busy'))}</h3><div class="parch">${esc(r.act || G.L('ui2d.idle'))}${r.goal ? `<br><b>${esc(L('wants'))}:</b> ${esc(r.goal)}` : ''}</div>`;
      h += `<div class="cardbtns"><button class="btn wide" id="cs-osheet">${esc(L(r.sex === 'f' ? 'her_sheet' : 'his_sheet'))}</button></div>`;
    } else h += `<div class="parch dim">${esc(L('no_one'))}</div>`;
    return h + '</div></div>';
  }
  function wireRelations(body, d) {
    for (const b of body.querySelectorAll('.cs-filters .btn')) b.onclick = () => { S.relFilter = b.dataset.f; S.relSel = null; rerender(); };
    for (const tr of body.querySelectorAll('tr[data-id]')) {
      tr.onclick = () => { S.relSel = Number(tr.dataset.id); rerender(); };
      tr.ondblclick = () => open(Number(tr.dataset.id));
    }
    const r = d.relations.find(x => x.id === S.relSel);
    const p = r && G.byId.get(r.id), pic = body.querySelector('#cs-opic');
    if (p && pic) pic.appendChild(G.portrait(p, 56));
    const os = body.querySelector('#cs-osheet');
    if (os && r) os.onclick = () => open(r.id);
  }

  // the body's eight places round the figure (a paper doll): four a side
  const PLACES = [['head', 1, 1], ['shoulder', 1, 2], ['hands', 1, 3], ['legs', 1, 4], ['back', 3, 1], ['torso', 3, 2], ['waist', 3, 3], ['feet', 3, 4]];
  function pageThings(d) {
    const t = d.things;
    const by = new Map();
    for (const x of t.worn) { if (!by.has(x.slot)) by.set(x.slot, []); by.get(x.slot).push(x); }
    const load = t.maxKg > 0 ? t.kg / t.maxKg : 0;
    let h = '<div class="cs-things"><div class="inset cs-box cs-body"><h3>' + esc(L('worn')) + '</h3><div class="cs-doll">';
    h += '<span class="cs-dollpic" id="cs-doll"></span>';
    for (const [slot, cx, cy] of PLACES) {
      const items = by.get(slot) || [];
      const sd = (t.slots || []).find(x => x.id === slot);
      if (!sd && !items.length) continue;
      h += `<div class="cs-place${items.length ? ' full' : ''}" style="grid-column:${cx};grid-row:${cy}"><div class="pn">${esc(sd ? sd.name : items[0].slotName)}</div>` +
        (items.length ? items.map(x => `<div class="pi" data-item="${esc(x.id)}" title="${esc(x.layerName)}">${esc(x.name)}<small>${esc(x.layerName)}</small></div>`).join('')
          : `<div class="pi empty">${esc(L('layer_empty'))}</div>`) + '</div>';
    }
    h += '</div></div>';
    h += `<div class="inset cs-box"><h3>${esc(L('carried'))}</h3>`;
    h += `<div class="row"><span>${esc(L('load', { kg: t.kg, max: t.maxKg }))}</span>${bar(load, load > 0.8 ? 'var(--bad)' : load > 0.45 ? 'var(--mid)' : 'var(--good)')}<span>${pct(load)}</span></div>`;
    if (!t.carried.length) h += `<div class="parch dim">${esc(L('none'))}</div>`;
    else h += '<div class="cs-grid">' + t.carried.map(x => `<div class="cs-cell${S.thing === x.id ? ' sel' : ''}" data-item="${esc(x.id)}"><span>${esc(x.name)}</span>${x.n > 1 ? `<b>×${x.n}</b>` : ''}</div>`).join('') + '</div>';
    const sel = t.carried.find(x => x.id === S.thing) || t.worn.find(x => x.id === S.thing);
    if (sel) {
      h += `<div class="parch cs-thing"><b>${esc(sel.name)}</b>` + (sel.n > 1 ? ` ×${sel.n}` : '') +
        `<br>${esc(L('kg', { kg: sel.kg }))}` + (sel.worth != null ? ` · ${esc(L('worth', { n: sel.worth }))}` : '') +
        (sel.layerName ? ` · ${esc(sel.slotName)}, ${esc(sel.layerName)}` : '') + '</div>';
    }
    return h + '</div></div>';
  }
  function wireThings(body, d) {
    const p = G.byId.get(d.id), pic = body.querySelector('#cs-doll');
    if (p && pic) pic.appendChild(G.portrait(p, 160));
    for (const el of body.querySelectorAll('[data-item]')) el.onclick = () => { S.thing = S.thing === el.dataset.item ? null : el.dataset.item; rerender(); };
  }

  function pageSkills(d) {
    const s = d.skills;
    let h = '<div class="cs-cols">';
    // skills by category, best first
    h += `<div class="inset cs-box"><h3>${esc(L('skills'))}</h3>`;
    if (!s.skills.length) h += `<div class="parch dim">${esc(L('none'))}</div>`;
    const cats = new Map();
    for (const k of s.skills) { const c = k.catName || L('other'); if (!cats.has(c)) cats.set(c, []); cats.get(c).push(k); }
    for (const [c, list] of cats) {
      h += `<div class="cs-cat">${esc(c)}</div>`;
      for (const k of list) h += `<div class="row"><span>${esc(k.name)}</span>${bar(k.v, 'linear-gradient(#f0d080,#b08a4a)')}<span>${pct(k.v)}</span></div>`;
    }
    if (s.knows.length) h += `<h3>${esc(L('knows'))}</h3><div class="chips">${s.knows.map(k => `<span class="chip">${esc(k.name)}</span>`).join('')}</div>`;
    h += '</div>';
    // traits, attributes
    h += `<div class="inset cs-box"><h3>${esc(L('traits'))}</h3>`;
    h += s.traits.length ? '<div class="parch">' + s.traits.map(t => `<div class="cs-trait"><b>${esc(t.name)}</b>${t.desc ? ' — ' + esc(t.desc) : ''}</div>`).join('') + '</div>'
      : `<div class="parch dim">${esc(L('none'))}</div>`;
    if (s.attrs.length) {
      h += `<h3>${esc(L('attrs'))}</h3>`;
      for (const a of s.attrs) h += `<div class="row" title="${esc(a.desc)}"><span>${esc(a.name)}</span>${bar(a.v, 'var(--bronze)')}<span>${pct(a.v)}</span></div>`;
    }
    h += '</div>';
    // who they are: past, standing, values, reputation
    h += `<div class="inset cs-box"><h3>${esc(L(d.sex === 'f' ? 'who_is_f' : 'who_is'))}</h3><div class="parch cs-who">`;
    const line = (k, v) => (v ? `<div><b>${esc(L(k))}:</b> ${esc(v)}</div>` : '');
    h += line('role_lbl', d.role) + line('house', d.house || d.fam) + line('occ_lbl', d.occ) + line('backstory', d.backstory) +
      line('standing_lbl', d.standing) + line('life_lbl', d.life);
    if (s.renown) h += line('renown', String(Math.round(s.renown * 100) / 100));
    if (s.purse) h += line('purse', String(s.purse));
    h += '</div>';
    if (s.values.length) { h += `<h3>${esc(L('values'))}</h3>`; for (const v of s.values) h += axisRow(v.name, v.v, false); }
    if (s.rep.length) { h += `<h3>${esc(L('rep'))}</h3>`; for (const r of s.rep) h += axisRow(r.name, r.v, true); }
    return h + '</div></div>';
  }

  function pageChronicle(d) {
    const c = d.chronicle;
    let h = '<div class="cs-cols two">';
    h += `<div class="inset cs-box"><h3>${esc(L('mem'))}</h3>`;
    h += c.mem.length ? `<div class="parch">${c.mem.map(memLine).join('')}</div>` : `<div class="parch dim">${esc(L('empty'))}</div>`;
    h += `</div><div class="inset cs-box"><h3>${esc(L(d.sex === 'f' ? 'log_f' : 'log'))}</h3>`;
    if (!c.log.length) h += `<div class="parch dim">${esc(L('empty'))}</div>`;
    else {
      let day = null;
      h += '<div class="parch">';
      for (const e of c.log) {
        if (e.day !== day) { day = e.day; h += `<div class="cs-day">${esc(G.L('ui2d.day', { n: day + 1 }))}</div>`; }
        h += `<div class="cs-mem"><b>${esc(e.t)}</b> ${esc(e.text)}</div>`;
      }
      h += '</div>';
    }
    return h + '</div></div>';
  }

  const PAGES = {
    overview: [pageOverview, null], relations: [pageRelations, wireRelations], things: [pageThings, wireThings],
    skills: [pageSkills, null], chronicle: [pageChronicle, null],
  };
  const TABS = Object.keys(PAGES);

  let win = null;
  function rerender() { if (win) win.refresh(); }
  function render(body, tab, w) {
    const d = S.data;
    if (!d || d.id !== S.id) { body.innerHTML = `<div class="parch">${esc(G.L('ui2d.loading'))}</div>`; return; }
    // keep the scroll of the page's boxes through a refresh
    const scrolls = [...body.querySelectorAll('.cs-box, .cs-table')].map(e => e.scrollTop);
    const [page, wire] = PAGES[tab] || PAGES.overview;
    body.innerHTML = `<div class="cs">${head(d)}<div class="cs-page">${page(d)}</div></div>`;
    wireHead(body, d, w);
    if (wire) wire(body, d, w);
    [...body.querySelectorAll('.cs-box, .cs-table')].forEach((e, i) => { if (scrolls[i]) e.scrollTop = scrolls[i]; });
  }

  function open(id) {
    if (id == null) id = G.selId != null && G.byId.get(G.selId) && !G.byId.get(G.selId).sp ? G.selId : G.me;
    if (id == null) id = ring()[0];
    if (id == null) return null;
    if (id !== S.id) { S.id = id; S.data = null; S.key = ''; S.prevNeeds.clear(); S.trend.clear(); S.relSel = null; S.thing = null; }
    win = G.win.open({
      id: 'sheet', wide: true,
      title: () => G.L('ui2d.screen.sheet'),
      tabs: TABS.map(t => [t, L('tab.' + t)]),
      tab: (win && G.win.isOpen('sheet') && win.tab) || 'overview',
      render,
      onPoll(s, w) { if (performance.now() - S.at > 1500) load(w); },
      onClose() { win = null; },
    });
    load(win);
    return win;
  }
  G.screens.sheet = open;
  // , and . page through the people while the sheet is open
  G.hooks.key.push(e => {
    if (!G.win.isOpen('sheet') || e.ctrlKey || e.metaKey || e.altKey) return false;
    if (e.key === ',' || e.key === '<') { step(-1); return true; }
    if (e.key === '.' || e.key === '>') { step(1); return true; }
    return false;
  });
})();
