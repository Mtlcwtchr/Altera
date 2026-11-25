// AL-112: frames of fields and kitchen gardens. Start altera_game_2d first
// (e.g. --no-browser --season summer --paused), then
//   node tools/game_2d/shots_crops.js [out_dir] [port] [prefix]
// Frames: the village from above, a field, a garden up close.
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const prefix = process.argv[4] || '2d_crops';
const base = `http://127.0.0.1:${port}/`;
const get = p => fetch(base + p).then(r => r.json());
(async () => {
  fs.mkdirSync(out, { recursive: true });
  const st = await get('api/static');
  const s = st.settlements.find(x => x.mode === 'full') || st.settlements[0];
  const R = 160;
  const g = await get(`api/grid?x=${Math.floor(s.x - R)}&y=${Math.floor(s.y - R)}&w=${2 * R}&h=${2 * R}`);
  const near = t => (g.plots || []).filter(p => p.type === t).sort((a, b) => Math.hypot(a.x - s.x, a.y - s.y) - Math.hypot(b.x - s.x, b.y - s.y))[0];
  const field = near('field'), garden = near('garden');
  console.log('field', JSON.stringify(field), 'garden', JSON.stringify(garden));
  const shots = [[`${prefix}_1_village`, `zoom=1&roofs=all`, s.x, s.y]];
  if (field) shots.push([`${prefix}_2_field`, 'zoom=2&roofs=all', field.x, field.y]);
  if (garden) shots.push([`${prefix}_3_garden`, 'zoom=3&roofs=all', garden.x, garden.y]);
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1600, height: 900 } });
  for (const [name, q, x, y] of shots) {
    await pg.goto(base + '?' + q, { waitUntil: 'load' });
    await pg.waitForTimeout(2500);
    // the page follows the led person: look at the plot instead
    await pg.evaluate(([x, y]) => { G.follow = false; G.cam.x = x; G.cam.y = y; }, [x, y]);
    await pg.waitForTimeout(2500);
    const file = path.join(out, name + '.png');
    await pg.screenshot({ path: file });
    console.log(file);
  }
  await b.close();
})();
