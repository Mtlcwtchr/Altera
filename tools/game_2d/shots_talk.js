// AL-98: frames of the talk and trade windows (start lr_game_2d first):
//   node tools/game_2d/shots_talk.js [out_dir] [port]
// Picks the nearest grown person of another household, has the player's person
// chat with them for a while, then shoots the talk window and the trade window
// with a draft (coin for their first good).
const { chromium } = require('playwright');
const path = require('path');
const fs = require('fs');
const out = process.argv[2] || path.join(__dirname, '..', '..', 'terrain_lab_out', 'shots_for_user');
const port = process.argv[3] || 8766;
const base = `http://127.0.0.1:${port}/`;
const api = (p, body) => fetch(base + p, body ? { method: 'POST', body: JSON.stringify(body) } : {}).then(r => r.json());
(async () => {
  fs.mkdirSync(out, { recursive: true });
  await api('api/cmd', { op: 'pause', on: true });
  const st = await api('api/state');
  const me = st.people.find(p => p.id === st.me);
  const other = st.people.filter(p => !p.sp && p.alive && p.id !== st.me && p.famId !== me.famId && p.age >= 16)
    .sort((a, b) => Math.hypot(a.x - me.x, a.y - me.y) - Math.hypot(b.x - me.x, b.y - me.y))[0];
  // a few exchanges: chat, a joke, a compliment, each played out
  for (const act of ['chat', 'joke', 'compliment']) {
    await api('api/cmd', { op: 'cmd', type: 'perform', args: { id: me.id, interaction: act, target: other.id, tone: 'warm' } });
    await api('api/cmd', { op: 'step', ticks: 6 });
  }
  const b = await chromium.launch();
  const pg = await b.newPage({ viewport: { width: 1920, height: 1080 } });
  await pg.goto(base + '?zoom=2', { waitUntil: 'load' });
  await pg.waitForTimeout(2500);
  await pg.evaluate(id => { G.select(id); G.screens.talk(id); }, other.id);
  await pg.waitForTimeout(2000);
  let file = path.join(out, '2d_talk_01.png');
  await pg.screenshot({ path: file }); console.log(file);
  await pg.evaluate(() => G.win.closeAll());
  await pg.evaluate(id => G.screens.trade(id), other.id);
  await pg.waitForTimeout(1500);
  // the draft: 3 coins from my pack, their first non-coin good
  for (let i = 0; i < 3; i++) { await pg.click('[data-side="give"] .slot[data-item="coin"]').catch(() => {}); await pg.waitForTimeout(250); }
  await pg.click('[data-side="take"] .slot:not([data-item="coin"])').catch(() => {});
  await pg.waitForTimeout(800);
  await pg.hover('[data-side="take"] .slot:not([data-item="coin"])').catch(() => {});
  await pg.waitForTimeout(300);
  file = path.join(out, '2d_trade_01.png');
  await pg.screenshot({ path: file }); console.log(file);
  await b.close();
})();
