// AL-95: frames of the 2D client for the user (gdd/ui/client_2d): start
// altera_game_2d first, then
//   node tools/game_2d/shots.js [out_dir] [port]
// (needs Node with playwright: npm i -g playwright && npx playwright install chromium).
// Frames go to terrain_lab_out/shots_for_user/ by default.
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const base = `http://127.0.0.1:${port}/`;
const SHOTS = [
  ['2d_01_village_x1', 'zoom=1&roofs=all'],
  ['2d_02_selected_x2', 'zoom=2&select=0'],
  ['2d_03_selected_x3', 'zoom=3&select=3'],
  ['2d_04_roofs_off_x2', 'zoom=2&roofs=none&select=6'],
  ['2d_05_glyphs_x2', 'zoom=2&glyphs=1&roofs=none&select=0'],
];
(async () => {
  fs.mkdirSync(out, { recursive: true });
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  await fetch(base + 'api/cmd', { method: 'POST', body: JSON.stringify({ op: 'speed', tps: 0.5 }) }).catch(() => {});
  for (const [name, q] of SHOTS) {
    await pg.goto(base + '?' + q, { waitUntil: 'load' });
    await pg.waitForTimeout(3500);
    const file = path.join(out, name + '.png');
    await pg.screenshot({ path: file });
    console.log(file);
  }
  await b.close();
})();
