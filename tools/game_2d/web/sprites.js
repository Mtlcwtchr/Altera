// AL-95: the 16x16 sprite set (content/sprites_2d/sprites.json: grids of
// palette letters) painted into small canvases, recoloured by letters
// (resources.json / map2d.json "recolour"), cached per sprite + recolour.
'use strict';
const Sprites = (() => {
  let PAL = {}, GRIDS = {};
  const cache = new Map();
  function load(json) { PAL = json.pal; GRIDS = json.sprites; cache.clear(); }
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
    cv.width = cv.height = 16;
    const ctx = cv.getContext('2d');
    for (let y = 0; y < 16; y++) {
      const row = g[y] || '';
      for (let x = 0; x < 16; x++) {
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
  return { load, get, has, colour };
})();
