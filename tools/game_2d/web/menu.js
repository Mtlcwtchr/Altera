// AL-99: the main menu of the 2D client and what it leads to, in the "Резной
// рубеж" frame (gdd/ui/client_2d/ui_h3/UI_H3.md): Continue, New game (seed,
// world size, season -> the world is made -> whom to be: a person of one of
// the families or a newcomer with a backstory, or only watch), Save, Load
// (slots 1-6 and the autosave 0), Quit. The page opens on the menu; Esc opens
// it when nothing else is open; the sim waits while it is up.
// Routes (tools/game_2d/main.cpp, src/client2d/session2d): /api/menu,
// /api/newgame, /api/dynasties, /api/begin, /api/saves, /api/save, /api/load,
// /api/delete, /api/quit.
// URL: ?menu=main|new|save|load opens that window, ?ng=pick the pick (after a new world).
'use strict';
(() => {
  const G = window.G;
  G.screens = G.screens || {};
  const css = document.createElement('link');
  css.rel = 'stylesheet'; css.href = '/web/menu.css';
  document.head.appendChild(css);

  const post = (u, body) => fetch(u, { method: 'POST', body: JSON.stringify(body || {}) }).then(r => r.json());
  const esc = s => G.esc(String(s == null ? '' : s));
  let M = null;            // /api/menu
  let lastPaused = false;  // the sim's pause as the last poll had it
  let heldPause = null;    // the pause before the menu took it (null: the menu holds none)
  G.hooks.poll.push(s => { lastPaused = !!s.paused; });

  const loadMenu = () => G.getJson('/api/menu').then(m => { M = m; return m; });
  function hold() {
    if (heldPause === null) { heldPause = lastPaused; if (!lastPaused) G.cmd({ op: 'pause', on: true }); }
  }
  function release() {
    if (heldPause === null) return;
    const was = heldPause; heldPause = null;
    if (!was) G.cmd({ op: 'pause', on: false });
  }
  const anyMenuOpen = () => ['menu', 'newgame', 'saves', 'pick'].some(id => G.win.isOpen(id));
  function closed() { setTimeout(() => { if (!anyMenuOpen()) release(); }, 0); }
  function busy(body, text) { body.innerHTML = `<div class="mn-busy">${esc(text)}</div>`; }
  function sizeName(id) { const s = M && M.options.sizes.find(x => x.id === id); return s ? s.name : id; }
  function when(sec) {
    if (!sec) return '';
    const d = new Date(sec * 1000);
    return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short' }) + ' ' + d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  }
  // a pawn of the person when the page has them, else a lettered stone
  function face(id, name, px) {
    const p = G.byId.get(id);
    if (p) return G.portrait(p, px);
    const d = document.createElement('div'); d.className = 'mn-glyph'; d.style.width = d.style.height = px + 'px';
    d.textContent = (name || '?').slice(0, 1).toUpperCase();
    return d;
  }

  // --- the main menu -------------------------------------------------------------
  function openMain() {
    hold();
    const w = G.win.open({
      id: 'menu', small: true, title: G.L('ui2d.menu.title'),
      render(body) {
        if (!M) { busy(body, G.L('ui2d.loading')); return; }
        const inGame = M.me != null && M.me !== 0;
        const items = [
          ['continue', inGame, () => { w.close(); }],
          ['new', true, () => { w.close(); openNew(); }],
          ['save', inGame, () => { w.close(); openSaves('save'); }],
          ['load', M.saves.some(s => s.used), () => { w.close(); openSaves('load'); }],
          ['quit', true, () => quit()],
        ];
        body.innerHTML = `<div class="mn-crest"><div class="mn-gem"></div><div class="mn-name">${esc(G.L('ui2d.menu.crest'))}</div>
          <div class="mn-sub">${esc(G.L('ui2d.menu.world', { seed: M.game.seed, size: sizeName(M.game.size) }))}</div></div><div class="mn-list"></div>
          <div class="mn-foot">${esc(G.L('ui2d.menu.keys'))}</div>`;
        const list = body.querySelector('.mn-list');
        for (const [id, on, go] of items) {
          const b = document.createElement('button');
          b.className = 'btn mn-big'; b.disabled = !on; b.dataset.id = id;
          b.textContent = G.L('ui2d.menu.' + id);
          b.onclick = go;
          list.appendChild(b);
        }
      },
      onClose: closed,
    });
    loadMenu().then(() => w.refresh());
    return w;
  }
  G.screens.menu = openMain;

  function quit() {
    post('/api/quit', { autosave: true }).catch(() => {}).finally(() => {
      document.body.innerHTML = `<div class="mn-bye carved"><h2>${esc(G.L('ui2d.menu.crest'))}</h2><div class="parch">${esc(G.L('ui2d.menu.bye'))}</div></div>`;
    });
  }

  // --- new game ----------------------------------------------------------------------
  const ng = { seed: '', size: null, season: '' };
  function openNew() {
    hold();
    const w = G.win.open({
      id: 'newgame', title: G.L('ui2d.ng.title'),
      render(body) {
        if (!M) { busy(body, G.L('ui2d.loading')); return; }
        const o = M.options;
        if (ng.size === null) { ng.size = o.defaults.size; ng.seed = String(Math.floor(Math.random() * 1e9)); }
        body.innerHTML = `
          <h3>${esc(G.L('ui2d.ng.seed'))}</h3>
          <div class="mn-seed"><input id="ng-seed" class="parch" maxlength="40" value="${esc(ng.seed)}"><button class="btn" id="ng-dice" title="${esc(G.L('ui2d.ng.dice'))}">⚄</button></div>
          <div class="mn-note">${esc(G.L('ui2d.ng.seed.hint'))}</div>
          <h3>${esc(G.L('ui2d.ng.size'))}</h3>
          <div class="mn-cards">${o.sizes.map(s => `<button class="mn-card${s.id === ng.size ? ' on' : ''}" data-size="${s.id}">
              <b>${esc(s.name)}</b><span>${esc(G.L('ui2d.ng.size.km', { km: s.size_m / 1000, f: s.families }))}</span><small>${esc(s.desc)}</small></button>`).join('')}</div>
          <h3>${esc(G.L('ui2d.ng.season'))}</h3>
          <div class="mn-row">${[{ id: '', name: G.L('ui2d.ng.season.any') }].concat(o.seasons).map(s =>
            `<button class="btn tab${s.id === ng.season ? ' on' : ''}" data-season="${s.id}">${esc(s.name)}</button>`).join('')}</div>
          <div class="mn-actions"><button class="btn mn-big" id="ng-back">${esc(G.L('ui2d.menu.back'))}</button>
            <button class="btn mn-big mn-go" id="ng-make">${esc(G.L('ui2d.ng.make'))}</button></div>`;
        const seed = body.querySelector('#ng-seed');
        seed.oninput = () => { ng.seed = seed.value; };
        seed.onkeydown = e => e.stopPropagation();
        body.querySelector('#ng-dice').onclick = () => { ng.seed = String(Math.floor(Math.random() * 1e9)); w.refresh(); };
        for (const b of body.querySelectorAll('[data-size]')) b.onclick = () => { ng.size = b.dataset.size; w.refresh(); };
        for (const b of body.querySelectorAll('[data-season]')) b.onclick = () => { ng.season = b.dataset.season; w.refresh(); };
        body.querySelector('#ng-back').onclick = () => { w.close(); openMain(); };
        body.querySelector('#ng-make').onclick = () => {
          busy(body, G.L('ui2d.ng.making'));
          post('/api/newgame', { seed: ng.seed.trim(), size: ng.size, season: ng.season }).then(r => {
            if (r.result !== 'ok') { G.toast && G.toast(r.text || r.result); w.refresh(); return; }
            location.href = '/?ng=pick';   // the new world's map, then whom to be
          }).catch(e => { G.toast && G.toast(String(e)); w.refresh(); });
        };
      },
      onClose: closed,
    });
    if (!M) loadMenu().then(() => w.refresh());
    return w;
  }

  // --- whom to be --------------------------------------------------------------------
  const pick = { id: null, back: null, fams: null };
  function openPick() {
    hold();
    const w = G.win.open({
      id: 'pick', wide: true, title: G.L('ui2d.pick.title'),
      tabs: [['kin', G.L('ui2d.pick.kin')], ['newcomer', G.L('ui2d.pick.newcomer')]], tab: 'kin',
      render(body, tab) {
        if (!M || !pick.fams) { busy(body, G.L('ui2d.loading')); return; }
        let h = '';
        if (tab === 'kin') {
          h += `<div class="mn-note">${esc(G.L('ui2d.pick.kin.hint'))}</div><div class="mn-fams">`;
          for (const f of pick.fams) {
            h += `<div class="mn-fam leather"><div class="mn-famhead"><b>${esc(f.dynasty)}</b>${f.dynasty.includes(f.name) ? '' : ` <span>${esc(f.name)}</span>`}
              <small>${esc(f.settlement)}${f.full ? '' : ' · ' + esc(G.L('ui2d.pick.far'))}</small></div>`;
            for (const p of f.people) {
              const tags = [p.chief ? G.L('ui2d.pick.chief') : '', p.head ? G.L('ui2d.pick.head') : '', p.adult ? '' : G.L('ui2d.pick.child')].filter(Boolean);
              h += `<div class="mn-who${pick.id === p.id ? ' sel' : ''}${p.adult ? '' : ' off'}" data-id="${p.id}" data-name="${esc(p.name)}" data-adult="${p.adult ? 1 : 0}">
                <span class="mn-face"></span><div class="nm">${esc(p.name)}<small>${esc(G.L('ui2d.pick.age', { n: p.age }))}${tags.length ? ' · ' + esc(tags.join(', ')) : ''}</small></div></div>`;
            }
            h += '</div>';
          }
          h += '</div>';
        } else {
          h += `<div class="mn-note">${esc(G.L('ui2d.pick.newcomer.hint'))}</div><div class="mn-backs">`;
          for (const b of M.options.backstories) {
            h += `<button class="mn-card mn-back${pick.back === b.id ? ' on' : ''}" data-back="${b.id}"><b>${esc(b.name)}</b><small>${esc(b.desc)}</small>
              <span class="chips">${b.skills.map(s => `<i class="chip">${esc(s.name)} ${Math.round(s.v * 100)}</i>`).join('')}${b.traits.map(t => `<i class="chip">${esc(t)}</i>`).join('')}</span>
              <span class="mn-gear">${b.gear.map(g => esc(g.name) + (g.n > 1 ? ' ×' + g.n : '')).join(', ')}${b.coins ? (b.gear.length ? ', ' : '') + esc(G.L('ui2d.pick.coins', { n: b.coins })) : ''}</span></button>`;
          }
          h += '</div>';
        }
        let who = '';
        if (tab === 'kin' && pick.id != null) for (const f of pick.fams) for (const p of f.people) if (p.id === pick.id) who = p.name;
        if (tab === 'newcomer' && pick.back) who = (M.options.backstories.find(b => b.id === pick.back) || {}).name || '';
        h += `<div class="mn-actions"><button class="btn mn-big" id="pk-watch">${esc(G.L('ui2d.pick.watch'))}</button>
          <button class="btn mn-big mn-go" id="pk-go"${who ? '' : ' disabled'}>${esc(who ? G.L(tab === 'kin' ? 'ui2d.pick.go' : 'ui2d.pick.come', { who }) : G.L('ui2d.pick.none'))}</button></div>`;
        body.innerHTML = h;
        for (const el of body.querySelectorAll('.mn-who')) {
          el.querySelector('.mn-face').appendChild(face(Number(el.dataset.id), el.dataset.name, 40));
          if (el.dataset.adult === '1') el.onclick = () => { pick.id = Number(el.dataset.id); G.select(pick.id); const p = G.byId.get(pick.id); if (p) { G.cam.x = p.x; G.cam.y = p.y; } w.refresh(); };
        }
        for (const el of body.querySelectorAll('[data-back]')) el.onclick = () => { pick.back = el.dataset.back; w.refresh(); };
        body.querySelector('#pk-watch').onclick = () => begin(w, { watch: true });
        body.querySelector('#pk-go').onclick = () => begin(w, tab === 'kin' ? { id: pick.id } : { backstory: pick.back });
      },
      onClose: closed,
    });
    Promise.all([M ? M : loadMenu(), G.getJson('/api/dynasties')]).then(([, d]) => {
      pick.fams = d.families;
      // the head of the full village is offered first, as the game would pick
      if (pick.id == null) for (const f of d.families) for (const p of f.people) if (pick.id == null && f.full && p.chief) pick.id = p.id;
      w.refresh();
    });
    return w;
  }
  G.screens.pick = openPick;
  function begin(w, args) {
    post('/api/begin', args).then(r => {
      if (r.result !== 'ok') { G.toast && G.toast(r.text || r.result); return; }
      heldPause = null;      // the game begins running whatever the pause was
      w.close();
      if (args.id != null) G.select(args.id);
      G.follow = !args.watch;
      G.poll();
    });
  }

  // --- save / load -------------------------------------------------------------------
  let sure = null;   // "save:3" / "del:2": the second click does it
  function openSaves(mode) {
    hold();
    sure = null;
    const w = G.win.open({
      id: 'saves', title: x => G.L(x.tab === 'save' ? 'ui2d.saves.save' : 'ui2d.saves.load'),
      tabs: [['save', G.L('ui2d.menu.save')], ['load', G.L('ui2d.menu.load')]], tab: mode,
      render(body, tab) {
        if (!M) { busy(body, G.L('ui2d.loading')); return; }
        const inGame = M.me != null && M.me !== 0;
        let h = `<div class="mn-slots">`;
        for (const s of M.saves) {
          const auto = s.slot === 0;
          const title = auto ? G.L('ui2d.saves.auto') : G.L('ui2d.saves.slot', { n: s.slot });
          const what = s.used
            ? `<b>${esc(s.person || G.L('ui2d.saves.nobody'))}</b>${s.family ? ' · ' + esc(s.family) : ''}<small>${esc(G.L('ui2d.saves.day', { n: s.day, season: G.L('ui2d.season.' + s.season) }))} · ${esc(G.L('ui2d.saves.alive', { n: s.alive }))} · ${esc(sizeName(s.size))} · ${esc(G.L('ui2d.saves.seed', { seed: s.seed }))}</small>`
            : `<i>${esc(G.L('ui2d.saves.empty'))}</i>`;
          let btns = '';
          if (tab === 'save' && !auto) btns += `<button class="btn" data-do="save" data-slot="${s.slot}"${inGame ? '' : ' disabled'}>${esc(sure === 'save:' + s.slot ? G.L('ui2d.saves.overwrite') : G.L('ui2d.saves.here'))}</button>`;
          if (tab === 'load' && s.used) btns += `<button class="btn" data-do="load" data-slot="${s.slot}">${esc(G.L('ui2d.saves.open'))}</button>`;
          if (s.used && !auto) btns += `<button class="btn" data-do="del" data-slot="${s.slot}" title="${esc(G.L('ui2d.saves.delete'))}">${sure === 'del:' + s.slot ? esc(G.L('ui2d.saves.sure')) : '✖'}</button>`;
          h += `<div class="mn-slot parch${s.used ? '' : ' empty'}"><div class="mn-sn">${esc(title)}<small>${esc(when(s.saved_at))}</small></div><div class="mn-sw">${what}</div><div class="mn-sb">${btns}</div></div>`;
        }
        h += `</div><div class="mn-note">${esc(G.L(M.autosave_min > 0 ? 'ui2d.saves.autonote' : 'ui2d.saves.autooff', { n: M.autosave_min }))}</div>`;
        body.innerHTML = h;
        for (const b of body.querySelectorAll('[data-do]')) b.onclick = () => act(w, b.dataset.do, Number(b.dataset.slot));
      },
      onClose: closed,
    });
    loadMenu().then(() => w.refresh());
    return w;
  }
  G.screens.saves = openSaves;
  function act(w, what, slot) {
    const s = M.saves.find(x => x.slot === slot);
    if ((what === 'del' || (what === 'save' && s && s.used)) && sure !== what + ':' + slot) { sure = what + ':' + slot; w.refresh(); return; }
    sure = null;
    if (what === 'save') {
      post('/api/save', { slot, view: { x: Math.round(G.cam.x), y: Math.round(G.cam.y), zoom: G.zoom } }).then(r => {
        if (r.saves) M.saves = r.saves;
        G.toast && G.toast(r.result === 'ok' ? G.L('ui2d.saves.done', { n: slot }) : G.L('ui2d.saves.failed', { why: r.text || r.result }));
        if (r.result === 'ok') { w.close(); } else w.refresh();
      });
    } else if (what === 'del') {
      post('/api/delete', { slot }).then(r => { if (r.saves) M.saves = r.saves; w.refresh(); });
    } else if (what === 'load') {
      busy(w.body, G.L('ui2d.saves.loading'));
      post('/api/load', { slot }).then(r => {
        if (r.result !== 'ok') { G.toast && G.toast(G.L('ui2d.saves.failed', { why: r.text || r.result })); w.refresh(); return; }
        const v = r.view || {};
        const q = new URLSearchParams({ loaded: slot });
        if (v.x != null) { q.set('x', v.x); q.set('y', v.y); }
        if (v.zoom) q.set('zoom', v.zoom);
        location.href = '/?' + q.toString();   // the loaded world's map from scratch
      });
    }
  }

  // --- keys and the start --------------------------------------------------------------
  G.hooks.key.push(e => {
    if (e.key !== 'Escape' || e.ctrlKey || e.altKey || e.metaKey) return false;
    if (G.win.top()) return false;
    if (G.selId != null && G.selId !== G.me) return false;   // the first Esc lets go of someone else selected
    openMain();
    return true;
  });
  let started = false;
  G.hooks.poll.push(() => {
    if (started) return;
    started = true;
    const q = new URLSearchParams(location.search);
    const m = q.get('menu');
    if (q.get('ng') === 'pick') openPick();
    else if (m === 'new') openNew();
    else if (m === 'save' || m === 'load') openSaves(m);
    else if (m || !location.search) openMain();
    else if (q.get('loaded') != null) G.toast && G.toast(G.L('ui2d.saves.loaded', { n: q.get('loaded') }));
    if (q.get('ng') || q.get('loaded') != null) history.replaceState(null, '', '/');
  });
})();
