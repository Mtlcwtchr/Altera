// LR-156: frames of the relief (heights, cliffs, ramps) on the debug page. Start
// lr_game_2d --no-browser first, then
//   node tools/game_2d/shots_relief.js [out_dir] [port]
// It looks for the windows with the most cliffs (/api/grid) and shoots them, the
// village and the glyph mode. Frames go to terrain_lab_out/shots_for_user/ by default.
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const base = `http://127.0.0.1:${port}/`;
const get = async (p) => (await fetch(base + p)).json();
(async () => {
  fs.mkdirSync(out, { recursive: true });
  const st = await get('api/static');
  const size = st.size || 12000;
  const found = [];
  for (let y = 0; y < size; y += 640)
    for (let x = 0; x < size; x += 640) {
      const j = await get(`api/grid?x=${x}&y=${y}&w=64&h=64`);
      let n = 0, top = 0;
      // rock cliffs count (not the soft banks of gentle ground)
      for (let i = 0; i < (j.cliff || '').length; i += 2)
        if (j.cliff.substr(i, 2) !== '00') {
          const k = j.cliff_style.charCodeAt(i / 2);
          if (j.cliff_styles[k - (k < 97 ? 65 : 97)] !== 'bank') n++;
        }
      for (const c of j.level || '') top = Math.max(top, c.charCodeAt(0) - 48);
      found.push({ n, top, x: x + 32, y: y + 32 });
    }
  found.sort((a, b) => b.n - a.n);
  const highest = [...found].sort((a, b) => b.top - a.top || b.n - a.n)[0];
  const s = st.settlements.find(v => v.mode === 'full') || st.settlements[0];
  const SHOTS = [
    ['relief_01_cliffs_x2', found[0], 'zoom=2'],
    ['relief_02_cliffs_x3', found[1], 'zoom=3'],
    ['relief_03_highest_x2', highest, 'zoom=2'],
    ['relief_04_cliffs_x1', found[2], 'zoom=1'],
    ['relief_05_village_x1', { x: s.x, y: s.y }, 'zoom=1&roofs=all'],
    ['relief_06_glyphs_x2', found[0], 'zoom=2&glyphs=1'],
  ];
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1600, height: 900 } });
  for (const [name, at, q] of SHOTS) {
    await pg.goto(`${base}?${q}&x=${at.x}&y=${at.y}`, { waitUntil: 'load' });
    // the page follows the led person: keep the camera on the spot instead
    for (let k = 0; k < 14; k++) {
      await pg.waitForTimeout(250);
      await pg.evaluate(([x, y]) => { if (window.G) { G.follow = false; const c = typeof G.cam === "function" ? G.cam() : G.cam; c.x = x; c.y = y; } }, [at.x, at.y]);
    }
    const file = path.join(out, name + '.png');
    await pg.screenshot({ path: file });
    console.log(file, at.x, at.y);
  }
  await b.close();
})();
