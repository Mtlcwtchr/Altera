// AL-98: the talk (screen 03) and the trade (screen 05) windows of the 2D client
// in the "Резной рубеж" style (gdd/ui/client_2d/ui_h3/UI_H3.md §3), as a meeting
// of two: the player's person on the left, the other on the right.
//   G.screens.talk(id)  - how they feel about you (and how it moved in this talk),
//                         the lines the two said (the dialogue system, in the page's
//                         language) and what they mutter aside, the topics to bring
//                         up, the wheel's acts by sphere with the chance in the tone
//                         (7 steps, mouse wheel), an ask of theirs to answer.
//   G.screens.trade(id) - your pack and their goods with their own price and taste
//                         of each, what you give / get, the scales, how they will
//                         take it, a counter waiting, the last word of the deal.
// Data: /api/talk?target=&tone=, POST /api/trade {target, give, take} (src/client2d/talk2d).
// Acts: perform {tone}, say_topic, answer_social, trade, trade_accept.
// Opened from the act menu on a person (two buttons on top) or with T on the selected one.
'use strict';
(() => {
  const G = window.G;
  const { L, esc } = G;
  G.hooks.menu = G.hooks.menu || [];

  // --- small pieces -------------------------------------------------------------------------
  const ICONS = [[/coin|silver/, 'it_coin'], [/bread|loaf|pie|cake|meal|porridge|stew|pottage/, 'it_bread'], [/ale|mead|cider|beer|wine/, 'it_ale'],
    [/sword|knife|blade|dagger|spear/, 'it_sword'], [/apple|fruit|berr|^pear/, 'it_apple'], [/herb|leaf|greens|flower/, 'it_herb'], [/ingot|iron|bronze|copper|metal/, 'it_ingot'], [/ore/, 'it_ore'],
    [/wheat|grain|flour|seed|barley|oat|rye/, 'it_wheat'], [/hammer|axe|tool|pick|saw|sickle/, 'it_hammer']];
  function icon(item, px) {
    const hit = ICONS.find(([re]) => re.test(item));
    const sp = hit && typeof Sprites !== 'undefined' && Sprites.get(hit[1]);
    if (!sp) return null;
    const c = document.createElement('canvas'); c.width = c.height = 16;
    c.getContext('2d').drawImage(sp, 0, 0);
    c.style.width = c.style.height = px + 'px';
    return c;
  }
  // a slot of a thing: the icon (or the name's first letter as a glyph), the count, a mark of taste
  function slotHtml(g, n, extra) {
    return `<div class="slot tg${g.taste > 0 ? ' like' : g.taste < 0 ? ' dislike' : ''}${extra || ''}" data-item="${esc(g.item)}" title="${esc(g.name)}">` +
      `<span class="gl" data-icon="${esc(g.item)}">${esc((g.name || g.item).slice(0, 1).toUpperCase())}</span>` +
      `<span class="cnt">${n}</span><span class="nm">${esc(g.name)}</span></div>`;
  }
  function fillIcons(root, px) {
    for (const el of root.querySelectorAll('[data-icon]')) {
      const c = icon(el.dataset.icon, px);
      if (c) { el.textContent = ''; el.appendChild(c); el.classList.add('ic'); }
    }
  }
  function portraitInto(el, id, px) {
    const p = G.byId.get(id);
    el.innerHTML = '';
    if (p) el.appendChild(G.portrait(p, px));
  }
  const pct = v => Math.round(v * 100) + '%';
  const sign = v => (v > 0 ? '+' : v < 0 ? '−' : '±') + Math.abs(Math.round(v * 100));
  function person(id) { const p = G.byId.get(id); return p && !p.sp ? p : null; }
  function canTalk(id) {
    if (G.me == null) { G.toast(L('ui2d.lead_first')); return false; }
    if (id == null || id === G.me || !person(id)) { G.toast(L('ui2d.talk.no_person')); return false; }
    return true;
  }

  // --- the talk window ----------------------------------------------------------------------
  const talk = { id: null, data: null, start: null, tone: null, sphere: null, busy: false, at: 0 };
  function loadTalk(w) {
    if (talk.busy || talk.id == null) return;
    talk.busy = true;
    G.getJson('/api/talk?target=' + talk.id + (talk.tone ? '&tone=' + encodeURIComponent(talk.tone) : '')).then(d => {
      talk.busy = false; talk.at = performance.now();
      if (!d.ok) { talk.data = d; if (w) w.refresh(); return; }
      if (talk.data && talk.data.ok && talk.data.them.id === d.them.id && JSON.stringify(talk.data) === JSON.stringify(d)) return;
      if (!talk.start || talk.start.id !== d.them.id) talk.start = { id: d.them.id, rel: Object.assign({}, d.rel) };
      if (!talk.tone) talk.tone = d.tone;
      if (!talk.sphere || !d.spheres.some(s => s.id === talk.sphere)) talk.sphere = d.spheres.length ? d.spheres[0].id : null;
      talk.data = d;
      if (w) w.refresh();
    }).catch(() => { talk.busy = false; });
  }
  const AXES = ['affinity', 'trust', 'fear', 'respect'];
  function relBars(d) {
    let h = '';
    for (const a of AXES) {
      const v = d.rel[a] || 0, dv = v - ((talk.start && talk.start.rel[a]) || 0);
      const left = v >= 0 ? 50 : 50 + v * 50, width = Math.abs(v) * 50;
      const col = a === 'fear' ? 'var(--bad)' : a === 'trust' ? '#4a8ab8' : a === 'respect' ? 'var(--gold)' : 'var(--good)';
      h += `<div class="relrow"><span>${esc(d.rel_names[a] || a)}</span><div class="sbar"><i style="left:${left}%;width:${width}%;background:${col}"></i><b></b></div>` +
        `<span class="dv ${dv > 0.005 ? 'up' : dv < -0.005 ? 'down' : ''}">${Math.abs(dv) > 0.005 ? sign(dv) : ''}</span></div>`;
    }
    return h;
  }
  function chanceText(a) {
    if (a.kind === 'certain' || a.chance == null) return L('ui2d.talk.certain');
    if (a.kind === 'win') return L('ui2d.talk.win', { p: Math.round(a.chance * 100) });
    return pct(a.chance);
  }
  function renderTalk(body, w) {
    const d = talk.data;
    if (!d) { body.innerHTML = `<div class="parch">${esc(L('ui2d.loading'))}</div>`; return; }
    if (!d.ok) { body.innerHTML = `<div class="parch">${esc(L('ui2d.talk.dead'))}</div>`; return; }
    const meName = d.me.name, themName = d.them.name;
    let h = `<div class="meet">
      <div class="side l"><div class="pt" data-pt="${d.me.id}"></div><div><div class="nm">${esc(meName)}</div><div class="sub">${esc(d.me.role)}</div></div></div>
      <div class="rel"><h3>${esc(L('ui2d.talk.rel', { who: d.them.name_gen }))}</h3>${relBars(d)}</div>
      <div class="side r"><div><div class="nm">${esc(themName)}</div><div class="sub">${esc(d.them.role)}</div><div class="sub">${esc(L(d.them.vy ? 'ui2d.talk.vy' : 'ui2d.talk.ty'))}</div></div><div class="pt" data-pt="${d.them.id}"></div></div>
    </div>`;
    // the lines
    h += `<div class="parch talklog">`;
    if (!d.lines.length) h += `<div class="none">${esc(L('ui2d.talk.log_empty'))}</div>`;
    for (const l of d.lines)
      h += `<div><span class="t">${esc(l.t)}</span><b class="${l.who === d.me.id ? 'me' : 'them'}">${esc(l.who === d.me.id ? meName : themName)}:</b> ${esc(l.text)}</div>`;
    if (d.aside) h += `<div class="aside"><b class="them">${esc(themName)}</b> <i>${esc(L('ui2d.talk.aside'))}</i>: ${esc(d.aside)}</div>`;
    h += `</div>`;
    if (d.ask) {
      h += `<div class="ask leather"><span>${esc(L(d.ask.tone ? 'ui2d.talk.asks_tone' : 'ui2d.talk.asks', { who: themName, act: d.ask.name, tone: d.ask.tone }))}</span>` +
        `<button class="btn" data-ans="1">${esc(L('ui2d.talk.yes'))}</button><button class="btn" data-ans="0">${esc(L('ui2d.talk.no'))}</button></div>`;
    }
    // topics | acts
    h += `<div class="talkgrid"><div class="topics leather"><h3>${esc(L('ui2d.talk.topics'))}</h3>`;
    if (!d.topics.length) h += `<div class="sub">${esc(L('ui2d.talk.no_topics'))}</div>`;
    for (const t of d.topics)
      h += `<button class="topic" data-topic="${t.i}"><span class="verb">${esc(t.verb)}</span> ${esc(t.label)}${t.rumor ? `<span class="chip rumor">${esc(L('ui2d.talk.rumor'))}</span>` : ''}</button>`;
    h += `</div><div class="acts"><div class="spheres">`;
    for (const s of d.spheres) h += `<button class="btn tab${s.id === talk.sphere ? ' on' : ''}" data-sphere="${esc(s.id)}">${esc(s.name)}</button>`;
    h += `</div><div class="actgrid">`;
    const sp = d.spheres.find(s => s.id === talk.sphere);
    for (const a of sp ? sp.acts : []) {
      h += `<button class="btn ta${a.can ? '' : ' off'}" data-act="${esc(a.id)}" title="${esc(a.gains.concat(a.to_target.map(x => '→ ' + x)).join(', '))}">${esc(a.name)}` +
        `<span class="ch">${esc(a.can ? chanceText(a) : a.why)}</span></button>`;
    }
    h += `</div><div class="tonerow"><span class="h">${esc(L('ui2d.talk.tone'))}</span><div class="tones">`;
    d.tones.forEach((t, i) => { h += `<button class="tone${t.id === d.tone ? ' on' : ''}" data-tone="${esc(t.id)}" title="${esc(t.name)}"></button>`; });
    const cur = d.tones.find(t => t.id === d.tone);
    h += `</div><span class="tn">«${esc(cur ? cur.name : '')}»</span><span class="sub">${esc(L('ui2d.talk.tone_hint'))}</span></div>`;
    h += `<div class="talkbtns">${d.trade ? `<button class="btn wide" data-go="trade">${esc(L('ui2d.talk.trade'))}</button>` : ''}` +
      `<div class="agent sub">${esc(L('ui2d.talk.agent'))}</div><button class="btn wide" data-go="leave">${esc(L('ui2d.talk.leave'))}</button></div>`;
    h += `</div></div>`;
    body.innerHTML = h;
    for (const el of body.querySelectorAll('[data-pt]')) portraitInto(el, Number(el.dataset.pt), 96);
    const log = body.querySelector('.talklog'); log.scrollTop = log.scrollHeight;
    for (const b of body.querySelectorAll('[data-sphere]')) b.onclick = () => { talk.sphere = b.dataset.sphere; w.refresh(); };
    for (const b of body.querySelectorAll('[data-tone]')) b.onclick = () => setTone(b.dataset.tone, w);
    body.querySelector('.tones').addEventListener('wheel', e => {
      e.preventDefault();
      const i = d.tones.findIndex(t => t.id === d.tone), j = Math.max(0, Math.min(d.tones.length - 1, i + (e.deltaY > 0 ? 1 : -1)));
      if (j !== i) setTone(d.tones[j].id, w);
    }, { passive: false });
    for (const b of body.querySelectorAll('[data-act]')) b.onclick = () => {
      const a = sp.acts.find(x => x.id === b.dataset.act);
      if (!a || !a.can) return;
      if (/^trade_/.test(a.id) && d.trade) { G.screens.trade(talk.id); return; }   // the deal is made in the trade window
      const go = id => G.cmd({ op: 'cmd', type: 'perform', args: { id: G.me, interaction: id, target: talk.id, tone: d.tone } })
        .then(r => { G.toast(r.result ? (r.text || r.result) : a.name); setTimeout(() => loadTalk(w), 400); });
      if (a.choices && G.chooseAct) { G.chooseAct(a, go); return; }   // LR-123: what to lend / give / trade
      go(a.id);
    };
    for (const b of body.querySelectorAll('[data-topic]')) b.onclick = () => {
      const t = d.topics.find(x => x.i === Number(b.dataset.topic));
      G.cmd({ op: 'say_topic', target: talk.id, index: Number(b.dataset.topic) })
        .then(r => { G.toast(r.result ? (r.text || r.result) : L('ui2d.talk.said', { what: t ? t.label : '' })); loadTalk(w); });
    };
    for (const b of body.querySelectorAll('[data-ans]')) b.onclick = () =>
      G.cmd({ op: 'cmd', type: 'answer_social', args: { id: G.me, accept: b.dataset.ans === '1' } }).then(r => { if (r.result) G.toast(r.text || r.result); loadTalk(w); });
    const go = body.querySelector('[data-go="trade"]'); if (go) go.onclick = () => G.screens.trade(talk.id);
    body.querySelector('[data-go="leave"]').onclick = () => w.close();
  }
  function setTone(id, w) { talk.tone = id; talk.data.tone = id; loadTalk(w); w.refresh(); }
  G.screens.talk = id => {
    if (id == null) id = G.selId;
    if (!canTalk(id)) return null;
    if (talk.id !== id) { talk.id = id; talk.data = null; talk.start = null; talk.sphere = null; }
    talk.tone = null;
    const w = G.win.open({
      id: 'talk', wide: true, title: () => L('ui2d.talk.title', { who: talk.data && talk.data.ok ? talk.data.them.name_ins : (person(talk.id) || {}).name || '' }),
      render: (body, tab, w) => renderTalk(body, w),
      onPoll: (s, w) => { if (performance.now() - talk.at > 1200) loadTalk(w); },
      // LR-157: closing a talk the player leads ends it; an ask left unanswered is a no
      onClose() {
        const d = talk.data || {};
        if (d.led) G.cmd({ op: 'cmd', type: 'end_talk', args: { id: G.me } });
        else if (d.ask) G.cmd({ op: 'cmd', type: 'answer_social', args: { id: G.me, accept: false } });
      },
    });
    loadTalk(w);
    return w;
  };
  // LR-157: an NPC asks the player's person: the talk window opens with them (the world waits)
  let lastInvite = '';
  G.hooks.poll.push(s => {
    const inv = s && s.invite;
    if (!inv) return;
    const key = inv.who + ':' + inv.since;
    if (key === lastInvite) return;
    lastInvite = key;
    G.screens.talk(inv.who);
  });

  // --- the trade window ---------------------------------------------------------------------
  const trade = { id: null, give: {}, take: {}, data: null, seq: 0, at: 0 };
  function loadTrade(w) {
    if (trade.id == null) return;
    const seq = ++trade.seq;
    fetch('/api/trade', { method: 'POST', body: JSON.stringify({ target: trade.id, give: trade.give, take: trade.take }) }).then(r => r.json()).then(d => {
      if (seq !== trade.seq) return;
      trade.at = performance.now();
      // what left the packs leaves the draft
      if (d.ok) for (const [side, list] of [['give', d.mine], ['take', d.theirs]])
        for (const k of Object.keys(trade[side])) {
          const g = list.find(x => x.item === k);
          trade[side][k] = Math.min(trade[side][k], g ? g.n : 0);
          if (!trade[side][k]) delete trade[side][k];
        }
      trade.data = d;
      if (w) w.refresh();
    });
  }
  function add(side, item, k, w) {
    const d = trade.data, list = side === 'give' ? d.mine : d.theirs, g = list.find(x => x.item === item);
    if (!g) return;
    const n = Math.max(0, Math.min(g.n, (trade[side][item] || 0) + k));
    if (n) trade[side][item] = n; else delete trade[side][item];
    loadTrade(w);
  }
  function dealBox(d, side) {
    const list = side === 'give' ? d.mine : d.theirs;
    let h = '';
    for (const [item, n] of Object.entries(trade[side])) {
      const g = list.find(x => x.item === item);
      if (g) h += slotHtml(g, n, ' in');
    }
    return h || `<div class="sub empty">${esc(L('ui2d.trade.empty'))}</div>`;
  }
  function sumOf(d, side) {
    const list = side === 'give' ? d.mine : d.theirs;
    let s = 0;
    for (const [item, n] of Object.entries(trade[side])) { const g = list.find(x => x.item === item); if (g) s += g.worth * n; }
    return s;
  }
  function scales(tilt) {
    // a pixel beam on a post, tilted toward the heavier side (-1 their side down .. 1 yours down)
    const a = Math.max(-1, Math.min(1, tilt)) * 8;
    return `<svg class="scales" width="240" height="96" viewBox="0 0 100 40" shape-rendering="crispEdges"><g fill="#b08a4a" stroke="#100a06" stroke-width="1">
      <rect x="48" y="6" width="4" height="30"/><rect x="40" y="35" width="20" height="4"/>
      <g transform="rotate(${a} 50 9)"><rect x="12" y="8" width="76" height="3"/><rect x="13" y="11" width="1" height="8"/><rect x="86" y="11" width="1" height="8"/>
      <rect x="6" y="19" width="16" height="3"/><rect x="78" y="19" width="16" height="3"/></g></g></svg>`;
  }
  function renderTrade(body, w) {
    const d = trade.data;
    if (!d) { body.innerHTML = `<div class="parch">${esc(L('ui2d.loading'))}</div>`; return; }
    if (!d.ok) { body.innerHTML = `<div class="parch">${esc(L('ui2d.trade.no_trade'))}</div>`; return; }
    const themName = d.them.name;
    const given = sumOf(d, 'give'), got = sumOf(d, 'take');
    const keep = d.keep || 1, bal = d.verdict ? d.gain - d.loss * keep : given - got;
    const any = Object.keys(trade.give).length || Object.keys(trade.take).length;
    let h = `<div class="tradegrid">
      <div class="pack leather"><div class="ph"><div class="pt" data-pt="${d.me.id}"></div><h3>${esc(L('ui2d.trade.mine'))}</h3></div>
        <div class="grid" data-side="give">${d.mine.map(g => slotHtml(g, g.n - (trade.give[g.item] || 0), (trade.give[g.item] || 0) >= g.n ? ' used' : '')).join('')}</div>
        <div class="sub">${esc(L('ui2d.trade.coin', { n: d.coin }))}</div></div>
      <div class="deal">
        <div class="boxes"><div><h3>${esc(L('ui2d.trade.give'))}</h3><div class="box" data-box="give">${dealBox(d, 'give')}</div></div>
          <div><h3>${esc(L('ui2d.trade.take'))}</h3><div class="box" data-box="take">${dealBox(d, 'take')}</div></div></div>
        ${scales(any ? (bal >= 0 ? -1 : 1) * Math.min(1, Math.abs(bal) / Math.max(1, given + got) * 2) : 0)}
        <div class="bal">${esc(!any ? L('ui2d.trade.weigh') : Math.abs(bal) < 0.5 ? L('ui2d.trade.balance_even')
          : bal > 0 ? L('ui2d.trade.balance_theirs', { v: bal.toFixed(1) }) : L('ui2d.trade.balance_yours', { v: (-bal).toFixed(1) }))}</div>`;
    if (d.verdict) {
      const v = d.verdict === 'accept' ? Math.min(1, 0.6 + (d.ratio - 1) * 0.4) : d.verdict === 'counter' ? 0.4 : 0.12;
      h += `<div class="how"><span>${esc(L('ui2d.trade.how'))}</span>${G.bar(v, G.tone(v))}<b>${esc(L('ui2d.trade.verdict.' + d.verdict))}</b></div>`;
      if (d.reason) h += `<div class="sub why">${esc(d.reason)}</div>`;
      if (d.their_counter) h += `<div class="sub">${esc(L('ui2d.trade.would_counter', { text: d.their_counter.text }))}</div>`;
    }
    if (d.counter) h += `<div class="parch counter">${esc(L('ui2d.trade.counter', { text: d.counter.text }))}</div>`;
    h += `<div class="tradebtns"><button class="btn wide" data-do="offer"${any ? '' : ' disabled'}>${esc(L('ui2d.trade.offer'))}</button>` +
      `<button class="btn wide" data-do="bargain"${d.counter ? '' : ' disabled'}>${esc(L('ui2d.trade.bargain'))}</button>` +
      `<button class="btn wide" data-do="gift"${Object.keys(trade.give).length ? '' : ' disabled'}>${esc(L('ui2d.trade.gift'))}</button>` +
      `<button class="btn wide" data-do="clear">${esc(L('ui2d.trade.clear'))}</button><button class="btn wide" data-do="cancel">${esc(L('ui2d.trade.cancel'))}</button></div>`;
    h += `<div class="sub hint">${esc(L('ui2d.trade.hint'))}</div>`;
    if (d.last.length) h += `<div class="parch last"><b>${esc(L('ui2d.trade.last'))}:</b> ${d.last.map(esc).join('<br>')}</div>`;
    h += `</div>
      <div class="pack leather"><div class="ph"><h3>${esc(L('ui2d.trade.theirs', { who: d.them.name_gen }))}</h3><div class="pt" data-pt="${d.them.id}"></div></div>
        ${d.role ? `<div class="sub">${esc(d.role)}</div>` : ''}
        <div class="grid" data-side="take">${d.theirs.map(g => slotHtml(g, g.n - (trade.take[g.item] || 0), (trade.take[g.item] || 0) >= g.n ? ' used' : '')).join('')}</div>
        <div class="sub">${esc(L('ui2d.trade.their_coin', { n: d.their_coin }))}</div>
        ${d.likes.length ? `<div class="sub good">${esc(L('ui2d.trade.likes', { list: d.likes.join(', ') }))}</div>` : ''}
        ${d.dislikes.length ? `<div class="sub bad">${esc(L('ui2d.trade.dislikes', { list: d.dislikes.join(', ') }))}</div>` : ''}
        <div class="price parch" hidden></div></div>
    </div>`;
    body.innerHTML = h;
    fillIcons(body, 32);
    for (const el of body.querySelectorAll('[data-pt]')) portraitInto(el, Number(el.dataset.pt), 56);
    const price = body.querySelector('.price');
    const bySide = side => (side === 'give' ? d.mine : d.theirs);
    for (const grid of body.querySelectorAll('[data-side]')) {
      const side = grid.dataset.side;
      for (const s of grid.querySelectorAll('.slot')) {
        const g = bySide(side).find(x => x.item === s.dataset.item);
        s.onclick = e => add(side, s.dataset.item, e.shiftKey ? 5 : 1, w);
        s.oncontextmenu = e => { e.preventDefault(); add(side, s.dataset.item, e.shiftKey ? -5 : -1, w); };
        s.onmouseenter = () => {
          if (!g) return;
          price.hidden = false;
          price.textContent = `${g.name}: ` + L('ui2d.trade.price', { who: themName, v: g.worth.toFixed(1) }) +
            (g.taste ? ` · ${L(g.taste > 0 ? 'ui2d.trade.likes_mark' : 'ui2d.trade.dislikes_mark')}` : '');
        };
      }
    }
    for (const box of body.querySelectorAll('[data-box]'))
      for (const s of box.querySelectorAll('.slot')) {
        const take = e => { e.preventDefault(); add(box.dataset.box, s.dataset.item, -(e.shiftKey ? 5 : 1), w); };
        s.onclick = take; s.oncontextmenu = take;
      }
    const send = (type, args, ok) => G.cmd({ op: 'cmd', type, args: Object.assign({ id: G.me, target: trade.id }, args) })
      .then(r => { G.toast(r.result ? (r.text || r.result) : ok); if (!r.result) { trade.give = {}; trade.take = {}; } setTimeout(() => loadTrade(w), 400); });
    body.querySelector('[data-do="offer"]').onclick = () =>
      send('trade', { mode: d.mode, give: trade.give, take: trade.take }, L('ui2d.trade.sent', { mode: d.mode_name || d.mode }));
    body.querySelector('[data-do="gift"]').onclick = () =>
      send('trade', { mode: 'gift', give: trade.give, take: {} }, L('ui2d.trade.sent', { mode: L('ui2d.trade.gift') }));
    body.querySelector('[data-do="bargain"]').onclick = () => send('trade_accept', {}, L('ui2d.trade.bargain'));
    body.querySelector('[data-do="clear"]').onclick = () => { trade.give = {}; trade.take = {}; loadTrade(w); };
    body.querySelector('[data-do="cancel"]').onclick = () => w.close();
  }
  G.screens.trade = id => {
    if (id == null) id = G.selId;
    if (!canTalk(id)) return null;
    if (trade.id !== id) { trade.id = id; trade.give = {}; trade.take = {}; trade.data = null; }
    const w = G.win.open({
      id: 'trade', wide: true, title: () => L('ui2d.trade.title', { who: trade.data && trade.data.ok ? trade.data.them.name_ins : (person(trade.id) || {}).name || '' }),
      render: (body, tab, w) => renderTrade(body, w),
      onPoll: (s, w) => { if (performance.now() - trade.at > 2500) loadTrade(w); },
    });
    loadTrade(w);
    return w;
  };

  // --- ways in: the act menu on a person, T on the selected one -----------------------------
  G.hooks.menu.push((menu, target) => {
    if (target == null || !person(target) || target === G.me) return;
    const row = document.createElement('div');
    row.className = 'menutop';
    row.innerHTML = `<button class="btn wide" data-m="talk">${esc(L('ui2d.talk.open'))}</button>` +
      `<button class="btn wide" data-m="trade">${esc(L('ui2d.talk.trade'))}</button>`;
    const h3 = menu.querySelector('h3');
    if (h3) h3.after(row); else menu.prepend(row);
    for (const b of row.querySelectorAll('button')) b.onclick = () => { menu.hidden = true; G.screens[b.dataset.m](target); };
  });
  G.hooks.key.push(e => {
    if (e.ctrlKey || e.metaKey || e.altKey || (e.key !== 't' && e.key !== 'T')) return false;
    if (G.win.top()) return false;
    G.screens.talk(G.selId);
    return true;
  });
})();
