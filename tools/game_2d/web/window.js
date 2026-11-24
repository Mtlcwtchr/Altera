// AL-96: windows of the 2D client in the "Резной рубеж" style (gdd/ui/client_2d/
// ui_h3/UI_H3.md): a carved walnut frame with bronze bevels and gem corners over
// the darkened map, a title, optional tabs, a close button; Esc closes the top one.
// The screens (character sheet, talk, trade, workshop, settlement, build) are
// separate files that call G.win.open(...) and fill the body.
//   const w = G.win.open({ id: 'sheet', title: 'Лист', tabs: [['overview', 'Обзор'], ...], tab: 'overview',
//                          wide: true, render: (body, tab, w) => { body.innerHTML = ... }, onClose });
//   w.refresh(); w.close(); G.win.isOpen('sheet'); G.win.top()
'use strict';
(() => {
  const G = window.G;
  const stack = [];
  const host = document.getElementById('windows');
  function open(spec) {
    const old = stack.find(w => w.id === spec.id);
    if (old) { old.spec = Object.assign(old.spec, spec); old.tab = spec.tab || old.tab; old.refresh(); raise(old); return old; }
    const el = document.createElement('div');
    el.className = 'win carved' + (spec.wide ? ' wide' : '') + (spec.small ? ' small' : '');
    el.innerHTML = `<div class="win-head"><h2></h2><div class="tabs"></div><button class="btn x" title="Esc">✖</button></div><div class="win-body"></div>`;
    host.appendChild(el);
    host.hidden = false;
    const w = {
      id: spec.id, spec, el, tab: spec.tab || (spec.tabs && spec.tabs[0] && spec.tabs[0][0]),
      body: el.querySelector('.win-body'),
      refresh() {
        el.querySelector('h2').textContent = typeof w.spec.title === 'function' ? w.spec.title(w) : (w.spec.title || '');
        const tabs = el.querySelector('.tabs');
        tabs.innerHTML = '';
        for (const [id, name] of w.spec.tabs || []) {
          const b = document.createElement('button');
          b.className = 'btn tab' + (id === w.tab ? ' on' : '');
          b.textContent = name;
          b.onclick = () => { w.tab = id; w.refresh(); };
          tabs.appendChild(b);
        }
        if (w.spec.render) w.spec.render(w.body, w.tab, w);
      },
      close() {
        const i = stack.indexOf(w);
        if (i >= 0) stack.splice(i, 1);
        el.remove();
        host.hidden = stack.length === 0;
        if (w.spec.onClose) w.spec.onClose(w);
      },
    };
    el.querySelector('.x').onclick = () => w.close();
    el.addEventListener('mousedown', () => raise(w));
    stack.push(w);
    w.refresh();
    return w;
  }
  function raise(w) {
    const i = stack.indexOf(w);
    if (i < 0) return;
    stack.splice(i, 1); stack.push(w);
    stack.forEach((x, k) => { x.el.style.zIndex = 10 + k; });
  }
  host.addEventListener('mousedown', e => { if (e.target === host && stack.length) stack[stack.length - 1].close(); });
  G.hooks.key.unshift(e => {
    if (e.key === 'Escape' && stack.length) { stack[stack.length - 1].close(); return true; }
    return false;
  });
  G.win = {
    open,
    isOpen: id => stack.some(w => w.id === id),
    get: id => stack.find(w => w.id === id) || null,
    top: () => stack[stack.length - 1] || null,
    closeAll: () => { while (stack.length) stack[stack.length - 1].close(); },
  };
  // the windows refresh with the sim while open (each decides what to redraw)
  G.hooks.poll.push(s => { for (const w of stack) if (w.spec.onPoll) w.spec.onPoll(s, w); });
})();
