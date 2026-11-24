// AL-96: the column's eight screens (gdd/ui/client_2d/ui_h3/UI_H3.md §3, screen 01):
// Лист, Династия, Колония, Стройка, Карта, Мастерская, Хроника, Меню. A screen
// file registers itself as G.screens[id] = () => G.win.open({...}); a button
// without one says it is coming. Keys: C sheet, B build, K workshop, J chronicle, M map.
'use strict';
(() => {
  const G = window.G;
  G.screens = G.screens || {};
  const LIST = [['sheet', 'c'], ['dynasty', 'y'], ['colony', 'o'], ['build', 'b'], ['map', 'm'], ['workshop', 'k'], ['chronicle', 'j'], ['menu', null]];
  function openScreen(id) {
    const f = G.screens[id];
    if (f) f(); else G.toast && G.toast(G.L('ui2d.soon', { what: G.L('ui2d.screen.' + id) }));
  }
  G.openScreen = openScreen;
  const box = document.getElementById('screens');
  function render() {
    box.innerHTML = '';
    for (const [id, key] of LIST) {
      const b = document.createElement('button');
      b.className = 'btn';
      b.textContent = G.L('ui2d.screen.' + id);
      b.title = key ? key.toUpperCase() : 'Esc';
      b.onclick = () => openScreen(id);
      box.appendChild(b);
    }
  }
  let drawn = false;
  G.hooks.poll.push(() => { if (!drawn) { drawn = true; render(); } });
  G.hooks.key.push(e => {
    if (e.ctrlKey || e.metaKey || e.altKey) return false;
    const hit = LIST.find(([, k]) => k && e.key.toLowerCase() === k);
    if (hit) { openScreen(hit[0]); return true; }
    return false;
  });
})();
