// AL-96: the chronicle window (J): what happened in the village, newest first,
// a day a heading; a line with a person in it selects them.
'use strict';
(() => {
  const G = window.G;
  let since = 0, list = [];
  function load(w) {
    G.getJson('/api/chronicle?since=' + since).then(r => {
      list = list.concat(r.list).slice(-600);
      since = r.n;
      if (w) w.refresh();
    });
  }
  G.screens.chronicle = () => G.win.open({
    id: 'chronicle', title: G.L('ui2d.screen.chronicle'),
    render(body) {
      if (!list.length) { body.innerHTML = `<div class="parch">${G.esc(G.L('ui2d.chronicle.empty'))}</div>`; return; }
      let h = '', day = null;
      for (const e of list.slice().reverse()) {
        if (e.day !== day) { day = e.day; h += `<h3>${G.esc(G.L('ui2d.day', { n: day + 1 }))}</h3>`; }
        h += `<div class="parch chron" data-who="${e.who && e.who.length ? e.who[0] : ''}"><b>${G.esc(e.t)}</b> ${G.esc(e.text)}</div>`;
      }
      body.innerHTML = h;
      for (const el of body.querySelectorAll('.chron')) el.onclick = () => { if (el.dataset.who) G.select(Number(el.dataset.who)); };
    },
    onPoll(s, w) { if (performance.now() - (w.loadedAt || 0) > 3000) { w.loadedAt = performance.now(); load(w); } },
  }) && load(G.win.get('chronicle'));
})();
