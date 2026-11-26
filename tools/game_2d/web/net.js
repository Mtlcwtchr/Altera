// LR-160: the page as a client of a multiplayer game. The game is the server's
// (tools/game_2d/main.cpp): open http://<the host's address>:8766/ from any
// computer of the network and you are a player. Loaded first, so every /api/
// request carries who we are:
//   X-LR-Player - a token kept in sessionStorage (a reload is the same player;
//                 another tab is another player), ?player=<token> to pick one;
//   X-LR-Name   - the name the others see (?name=..., then remembered).
// After the game's scripts: the players' panel in the column, toasts for what
// the others did to the shared things (pause, speed, a new game, a load), and a
// new world made by another player loads the page anew.
'use strict';
(() => {
  const q = new URLSearchParams(location.search);
  const store = (s, k, v) => { try { if (v === undefined) return s.getItem(k); s.setItem(k, v); } catch (e) { /* private mode */ } return v; };
  const rnd = () => Array.from(crypto.getRandomValues(new Uint8Array(12)), b => b.toString(16).padStart(2, '0')).join('');
  let token = q.get('player') || store(sessionStorage, 'lr.token');
  if (!token) token = rnd();
  store(sessionStorage, 'lr.token', token);
  let name = q.get('name') || store(localStorage, 'lr.name') || '';
  if (q.get('name')) store(localStorage, 'lr.name', name);
  const net = { token, name, players: [], you: 0 };
  window.LRNet = net;
  net.setName = n => { net.name = n; store(localStorage, 'lr.name', n); };

  const plain = window.fetch.bind(window);
  window.fetch = (url, opts) => {
    const u = typeof url === 'string' ? url : (url && url.url) || '';
    if (!u.startsWith('/api/')) return plain(url, opts);
    opts = Object.assign({}, opts || {});
    const h = new Headers(opts.headers || {});
    h.set('X-LR-Player', net.token);
    h.set('X-LR-Client', 'web');
    if (net.name) h.set('X-LR-Name', encodeURIComponent(net.name));
    opts.headers = h;
    return plain(url, opts).then(r => {
      if (r.status === 503) full();
      return r;
    });
  };
  function full() {
    if (document.getElementById('net-full')) return;
    const d = document.createElement('div');
    d.id = 'net-full'; d.className = 'carved';
    d.textContent = (window.G && G.L) ? G.L('ui2d.net.full') : 'The game is full';
    document.body.appendChild(d);
  }

  window.addEventListener('DOMContentLoaded', () => {
    const G = window.G;
    if (!G || !G.hooks) return;
    const esc = s => G.esc(String(s == null ? '' : s));
    // the players' panel at the top of the column
    const side = document.getElementById('side');
    const sec = document.createElement('section');
    sec.className = 'panel leather'; sec.id = 'net-panel'; sec.hidden = true;
    sec.innerHTML = '<h2 id="net-title"></h2><div id="net-list"></div>';
    if (side) side.insertBefore(sec, side.firstChild);
    let gen = null, seq = null;
    G.hooks.poll.push(s => {
      const n = s.net || {};
      net.players = n.players || []; net.you = n.you || 0;
      // another player's new world or load: the page starts over on it
      if (s.world_gen != null) {
        if (gen != null && s.world_gen !== gen) { location.href = s.me ? '/' : '/?ng=pick'; return; }
        gen = s.world_gen;
      }
      for (const e of n.events || []) {
        if (seq != null && e.seq > seq && e.by !== net.you && G.toast) {
          G.toast(G.L('ui2d.net.ev.' + e.what, { who: e.name, n: Math.round(e.tps * 10) / 10 }));
        }
      }
      for (const e of n.events || []) seq = Math.max(seq == null ? 0 : seq, e.seq);
      if (seq == null) seq = 0;
      sec.hidden = net.players.length < 2;
      if (sec.hidden) return;
      document.getElementById('net-title').textContent = G.L('ui2d.net.players', { n: net.players.length, max: '' }).replace(/\s*\/\s*$/, '');
      document.getElementById('net-list').innerHTML = net.players.map(p =>
        `<div class="net-p${p.online ? '' : ' away'}${p.you ? ' you' : ''}"><b>${esc(p.name)}</b> <small>${esc(p.person || G.L('ui2d.net.watching'))}${p.online ? '' : ' · ' + esc(G.L('ui2d.net.away'))}</small></div>`).join('');
    });
  });
})();
