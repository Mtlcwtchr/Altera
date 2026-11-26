// AL-95: the 16x16 sprite set (content/sprites_2d/sprites.json: grids of
// palette letters) painted into small canvases, recoloured by letters
// (resources.json / map2d.json "recolour"), cached per sprite + recolour.
'use strict';
const Sprites = (() => {
  let PAL = {}, GRIDS = {};
  const cache = new Map();
  function load(json) { PAL = json.pal; GRIDS = json.sprites; cache.clear(); }
  // AL-112: more grids in the same palette (crops2d.json)
  function merge(json) { if (json && json.sprites) Object.assign(GRIDS, json.sprites); cache.clear(); }
  function has(name) { return !!GRIDS[name]; }
  function colour(letter) { return PAL[letter] || '#ff00ff'; }
  // A canvas of the sprite (null when there is no such sprite).
  function get(name, recolour) {
    const rk = recolour && Object.keys(recolour).length ? JSON.stringify(recolour) : '';
    const k = name + '|' + rk;
    let cv = cache.get(k);
    if (cv !== undefined) return cv;
    const g = GRIDS[name];
    if (!g) { cache.set(k, null); return null; }
    cv = document.createElement('canvas');
    // 16x16 as a rule; LR-159: a bigger grid (a horse, a wagon: 32x32) keeps its size
    const n = Math.min(64, Math.max(16, g.length, ...g.map(r => r.length)));
    cv.width = cv.height = n;
    const ctx = cv.getContext('2d');
    for (let y = 0; y < n; y++) {
      const row = g[y] || '';
      for (let x = 0; x < n; x++) {
        let ch = row[x];
        if (!ch || ch === '.') continue;
        if (recolour && recolour[ch]) ch = recolour[ch];
        ctx.fillStyle = colour(ch);
        ctx.fillRect(x, y, 1, 1);
      }
    }
    cache.set(k, cv);
    return cv;
  }
  return { load, merge, get, has, colour };
})();
