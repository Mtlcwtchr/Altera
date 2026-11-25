// AL-97: frames of the 2D client's workshop for the user. Start lr_game_2d
// first (a fresh world: the trial kit goes to the player's person), then
//   node tools/game_2d/shots_craft.js [out_dir] [port]
// Frames: the bench with a draft laid by the helper, the helper, the outcome,
// the recipe book, the things. Default out: terrain_lab_out/shots_for_user/.
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const base = `http://127.0.0.1:${port}/`;
(async () => {
  fs.mkdirSync(out, { recursive: true });
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  pg.on('pageerror', e => console.error('page error:', e.message));
  await fetch(base + 'api/cmd', { method: 'POST', body: JSON.stringify({ op: 'pause', on: true }) }).catch(() => {});
  await pg.goto(base + '?zoom=2&kit=1', { waitUntil: 'load' });
  await pg.waitForTimeout(3000);
  const shot = async name => { const f = path.join(out, name + '.png'); await pg.screenshot({ path: f }); console.log(f); };
  const tab = async n => { await pg.click(`.win .tabs .btn:nth-child(${n})`); await pg.waitForTimeout(700); };
  await pg.keyboard.press('k');
  await pg.waitForTimeout(1500);
  await shot('2d_craft_01_bench_empty');
  // D: the helper, wish "cut", the step that can start now laid on the bench
  await tab(4);
  await pg.click('.wishb:nth-child(1)');
  await pg.waitForTimeout(1200);
  await shot('2d_craft_02_helper');
  const lay = await pg.$('.ws-cand .btn:not(.off)');
  if (lay) { await lay.click(); await pg.waitForTimeout(1500); }
  await shot('2d_craft_03_bench_draft');
  // hold E: the attempt, its outcome card
  await pg.keyboard.down('e');
  await pg.waitForTimeout(1000);
  await pg.keyboard.up('e');
  await pg.waitForTimeout(1500);
  await shot('2d_craft_04_outcome');
  // B + A by hand: a process on the ring, a material dragged onto the piece
  const proc = await pg.$('.ws-proc:not(.off)');
  if (proc) { await proc.click(); await pg.waitForTimeout(800); }
  if (await pg.$('.bagtok:not(.gone)') && await pg.$('.ws-slot')) {
    await pg.dragAndDrop('.bagtok:not(.gone)', '.ws-slot');
    await pg.waitForTimeout(1200);
  }
  await shot('2d_craft_05_drag');
  await tab(2);
  await shot('2d_craft_06_book');
  await tab(3);
  await shot('2d_craft_07_things');
  await pg.keyboard.press('Escape');
  await pg.waitForTimeout(800);
  await shot('2d_craft_08_map_stations');
  await b.close();
})();
