// AL-100: frames of the character sheet's five pages. Start lr_game_2d, then
//   node tools/game_2d/shots_sheet.js [out_dir] [port] [person index]
// Frames go to terrain_lab_out/shots_for_user/ by default (2d_sheet_*.png).
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const who = Number(process.argv[4] || 0);
const base = `http://127.0.0.1:${port}/`;
const TABS = ['overview', 'relations', 'things', 'skills', 'chronicle'];
(async () => {
  fs.mkdirSync(out, { recursive: true });
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  pg.on('pageerror', e => console.error('page error:', e.message));
  await pg.goto(base + `?zoom=2&select=${who}`, { waitUntil: 'load' });
  await pg.waitForFunction(() => window.G && window.G.selId != null, null, { timeout: 15000 });
  await pg.evaluate(() => window.G.screens.sheet());
  await pg.waitForTimeout(2000);
  for (let i = 0; i < TABS.length; ++i) {
    await pg.click(`.win .tabs .btn:nth-child(${i + 1})`);
    await pg.waitForTimeout(700);
    const file = path.join(out, `2d_sheet_${i + 1}_${TABS[i]}.png`);
    await pg.screenshot({ path: file });
    console.log(file);
  }
  await b.close();
})();
