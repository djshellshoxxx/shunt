#!/usr/bin/env node
// FS-06 test N-T7 (UI smoke for the Nudge page): starts shunt_headless + shuntsim (wrongbar), opens #/nudge,
// checks the page mounts, TAP posts to /api/f/nudge/tap on pointerdown, a 409 shows its error text in the
// aria-live toast, a real tap against the simulator succeeds, hold-to-repeat sends several steps and release
// stops them. Usage: node tests/features/nudge_ui_test.js [--build-dir build]. Needs playwright. Original Shunt code.
'use strict';
const { spawn } = require('child_process');
const fs = require('fs'), os = require('os'), path = require('path'), http = require('http');
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 && args[i + 1] ? args[i + 1] : d; };
const buildDir = path.resolve(opt('build-dir', process.env.BUILD_DIR || path.join(__dirname, '..', '..', 'build')));
let playwright;
try { playwright = require('playwright'); } catch (e) { playwright = require('playwright-core'); }
const kids = [];
const cleanup = () => { for (const c of kids) try { c.kill('SIGTERM'); } catch (e) { /* gone */ } };
process.on('exit', cleanup);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const getJson = (port, p) => new Promise((res, rej) => http.get({ host: '127.0.0.1', port, path: p }, r => { let b = ''; r.on('data', d => b += d); r.on('end', () => { try { res(JSON.parse(b)); } catch (e) { rej(e); } }); }).on('error', rej));
const fails = [];
const check = (c, m) => { if (!c) { fails.push(m); console.error('FAIL', m); } else console.log('ok  ', m); };

(async () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shunt-nudge-ui-'));
  const hl = spawn(path.join(buildDir, 'shunt_headless'), ['--config', path.join(tmp, 'config.json'), '--data', path.join(tmp, 'data'), '--addr', '127.0.0.1', '--port', '0'], { stdio: ['ignore', 'pipe', 'pipe'] });
  kids.push(hl);
  const port = await new Promise((res, rej) => { let o = ''; const t = setTimeout(() => rej(new Error('no port')), 10000); hl.stdout.on('data', d => { o += d; const m = /http:\/\/localhost:(\d+)\//.exec(o); if (m) { clearTimeout(t); res(+m[1]); } }); });
  kids.push(spawn(path.join(buildDir, 'shuntsim'), ['--dest', '127.0.0.1', '--scenario', 'wrongbar', '--bar-shift', '2', '--duration', '120', '--quiet'], { stdio: 'ignore' }));
  for (let i = 0; i < 40; i++) { try { const s = await getJson(port, '/api/status'); if (s.state === 'locked') break; } catch (e) { /* retry */ } await sleep(250); }
  const browser = await playwright.chromium.launch();
  const page = await (await browser.newContext({ viewport: { width: 390, height: 800 } })).newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error' && !/status of 409/.test(m.text())) errors.push(m.text()); });
  await page.goto(`http://127.0.0.1:${port}/`, { waitUntil: 'load' });
  await page.click('nav a[href="#/nudge"]');
  await page.waitForSelector('.nd-tap');
  check(true, 'page mounts');
  await page.waitForFunction(() => document.querySelector('.nd-conf .nd-sent') && /beats arrived on time/.test(document.querySelector('.nd-conf .nd-sent').textContent), null, { timeout: 8000 });
  check(true, 'confidence sentence rendered');

  // 1. a forced 409 shows its text in the aria-live toast
  await page.route('**/api/f/nudge/tap', r => r.fulfill({ status: 409, contentType: 'application/json', body: '{"ok":false,"error":"tap closer to a beat"}' }));
  const reqP = page.waitForRequest(r => r.url().endsWith('/api/f/nudge/tap') && r.method() === 'POST');
  await page.dispatchEvent('.nd-tap', 'pointerdown', { pointerId: 1, bubbles: true });
  const req = await reqP;
  check(req.method() === 'POST', 'TAP posts on pointerdown');
  await page.waitForFunction(() => /tap closer to a beat/.test(document.querySelector('.nd-toast').textContent), null, { timeout: 3000 });
  check(await page.getAttribute('.nd-toast', 'aria-live') === 'polite', 'toast is aria-live');
  check(/err/.test(await page.getAttribute('.nd-toast', 'class')), 'error toast styled as error');
  await page.unroute('**/api/f/nudge/tap');

  // 2. real taps against the wrongbar simulator: one succeeds and the status offset is reported
  let ok = false;
  for (let i = 0; i < 10 && !ok; i++) {
    await page.keyboard.press('Space');
    await sleep(450);
    ok = /Downbeat set/.test(await page.textContent('.nd-toast'));
  }
  check(ok, 'real tap sets the downbeat (toast)');
  const st = await getJson(port, '/api/status');
  check(typeof st.features.nudge.barOffset === 'number' && st.features.nudge.formula.includes('hit'), 'status.features.nudge has barOffset and formula');
  check(await page.isEnabled('.nd-undo'), 'undo enabled after a tap');
  await page.click('.nd-undo');
  await page.waitForFunction(() => /Undone/.test(document.querySelector('.nd-toast').textContent), null, { timeout: 3000 });
  check(true, 'undo works');

  // 3. hold-to-repeat: a quick press sends one step, a 1.2 s hold sends several, release stops them
  let steps = 0;
  await page.route('**/api/**', r => { const u = r.request().url(); if (r.request().method() === 'POST' && (u.endsWith('/api/f/nudge/step') || u.endsWith('/api/latency'))) { steps++; return r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true,"latencyMs":1.5}' }); } return r.continue(); });
  const box = await page.locator('.nd-step').nth(2).boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down(); await page.mouse.up(); await sleep(300);
  check(steps === 1, 'a short press sends one step (got ' + steps + ')');
  steps = 0;
  await page.mouse.down(); await sleep(1200); await page.mouse.up();
  const held = steps;
  check(held >= 4, 'hold repeats (got ' + held + ')');
  await sleep(500);
  check(steps === held, 'release stops repeating');
  check(errors.length === 0, 'no page errors: ' + errors.join(' | '));
  await browser.close();
  fs.rmSync(tmp, { recursive: true, force: true });
  cleanup();
  if (fails.length) { console.error('nudge ui: FAILED ' + fails.length); process.exit(1); }
  console.log('nudge ui: OK');
  process.exit(0);
})().catch(e => { console.error(e); cleanup(); process.exit(1); });
