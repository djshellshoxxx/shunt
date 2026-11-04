#!/usr/bin/env node
// UI smoke test (FS-TEST section 1). Starts shunt_headless on 127.0.0.1 (ephemeral port) plus shuntsim,
// opens the web UI in Chromium via Playwright, visits every page found in the nav DOM (nothing is
// hard-coded, so pages registered by features are picked up) and fails on any pageerror,
// console.error, failed request or HTTP status >= 400. Screenshots at 1440 and 360 px width go to
// the output directory. Budget: under 90 s (a watchdog enforces 85 s).
//
// Usage: node tools/ui_smoke.js [--build-dir build] [--out ui-smoke-out] [--scenario onair]
// Needs the `playwright` npm package resolvable (NODE_PATH ok) and its Chromium installed.
// Original Shunt code.
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 && args[i + 1] ? args[i + 1] : def; };
const root = path.resolve(__dirname, '..');
const buildDir = path.resolve(opt('build-dir', process.env.BUILD_DIR || path.join(root, 'build')));
const outDir = path.resolve(opt('out', process.env.UI_SMOKE_OUT || path.join(root, 'ui-smoke-out')));
const scenario = opt('scenario', 'onair');
const started = Date.now();
const log = (...a) => console.log(`[ui-smoke ${((Date.now() - started) / 1000).toFixed(1)}s]`, ...a);

let playwright;
try { playwright = require('playwright'); } catch (e) {
  try { playwright = require('playwright-core'); } catch (e2) {
    console.error('ui-smoke: cannot require("playwright"): ' + e.message); process.exit(2);
  }
}

const children = [];
function cleanup() { for (const c of children) { try { c.kill('SIGTERM'); } catch (e) { /* gone */ } } }
process.on('exit', cleanup);
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { cleanup(); process.exit(130); });
const watchdog = setTimeout(() => { console.error('ui-smoke: exceeded the 85 s budget'); cleanup(); process.exit(1); }, 85000);
watchdog.unref();

function getJson(port, p) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port, path: p, timeout: 3000 }, res => {
      let b = ''; res.on('data', d => b += d);
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function startHeadless(tmp) {
  const exe = path.join(buildDir, 'shunt_headless');
  if (!fs.existsSync(exe)) throw new Error('missing ' + exe + ' (build first or pass --build-dir)');
  const p = spawn(exe, ['--config', path.join(tmp, 'config.json'), '--data', path.join(tmp, 'data'),
    '--addr', '127.0.0.1', '--port', '0'], { stdio: ['ignore', 'pipe', 'pipe'] });
  children.push(p);
  let out = '';
  p.stderr.on('data', d => { out += d; });
  const port = await new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('shunt_headless did not report its port: ' + out)), 10000);
    p.stdout.on('data', d => {
      out += d;
      const m = /http:\/\/localhost:(\d+)\//.exec(out);
      if (m) { clearTimeout(t); resolve(Number(m[1])); }
    });
    p.on('exit', code => { clearTimeout(t); reject(new Error('shunt_headless exited early (' + code + '): ' + out)); });
  });
  return port;
}

function startSim() {
  const exe = path.join(buildDir, 'shuntsim');
  if (!fs.existsSync(exe)) throw new Error('missing ' + exe);
  const p = spawn(exe, ['--dest', '127.0.0.1', '--scenario', scenario, '--duration', '120', '--quiet'], { stdio: 'ignore' });
  children.push(p);
  return p;
}

async function main() {
  fs.mkdirSync(outDir, { recursive: true });
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shunt-ui-smoke-'));
  const problems = [];
  const note = (where, what) => { problems.push(`${where}: ${what}`); log('PROBLEM', where, what); };

  const port = await startHeadless(tmp);
  const base = `http://127.0.0.1:${port}`;
  log('shunt_headless on', base);
  startSim();

  // Let the simulator's devices show up so pages render real data.
  let status = null;
  for (let i = 0; i < 40; i++) {
    try { status = await getJson(port, '/api/status'); if (status.devices && status.devices.length) break; } catch (e) { /* retry */ }
    await sleep(250);
  }
  if (!status || !status.devices || !status.devices.length) log('warning: no simulated devices seen in /api/status after 10 s');

  const browser = await playwright.chromium.launch();
  try {
    let pageNames = null;
    for (const [label, width, height] of [['1440', 1440, 900], ['360', 360, 740]]) {
      const ctx = await browser.newContext({ viewport: { width, height } });
      const page = await ctx.newPage();
      let where = 'load';
      page.on('pageerror', e => note(`${label}px ${where}`, 'pageerror: ' + e.message));
      page.on('console', m => { if (m.type() === 'error') note(`${label}px ${where}`, 'console.error: ' + m.text()); });
      page.on('requestfailed', r => note(`${label}px ${where}`, `request failed: ${r.url()} ${r.failure() && r.failure().errorText}`));
      page.on('response', r => { if (r.status() >= 400) note(`${label}px ${where}`, `HTTP ${r.status()} ${r.url()}`); });

      await page.goto(base + '/', { waitUntil: 'load' });
      await page.waitForSelector('nav a[href^="#/"]', { timeout: 10000 });
      const links = await page.$$eval('nav a[href^="#/"]', as => as.map(a => ({ href: a.getAttribute('href'), text: a.textContent.trim() })));
      if (!links.length) note(label + 'px', 'no nav links found');
      if (pageNames === null) { pageNames = links.map(l => l.href); log('nav pages:', pageNames.join(' ')); }
      else if (JSON.stringify(pageNames) !== JSON.stringify(links.map(l => l.href))) note(label + 'px', 'nav differs between viewports: ' + links.map(l => l.href).join(' '));

      for (const link of links) {
        const id = link.href.replace(/^#\/?/, '') || 'live';
        where = id;
        // Click the real link (scrolls the nav on narrow screens) rather than setting the hash.
        const sel = `nav a[href="${link.href}"]`;
        await page.locator(sel).scrollIntoViewIfNeeded();
        await page.click(sel);
        await page.waitForFunction(h => location.hash === h, link.href, { timeout: 5000 }).catch(() => note(`${label}px ${id}`, 'hash did not change'));
        await page.waitForTimeout(700);
        const cur = await page.$eval(sel, a => a.getAttribute('aria-current')).catch(() => null);
        if (cur !== 'page') note(`${label}px ${id}`, `nav link not marked current (aria-current=${cur})`);
        const mounted = await page.evaluate(() => { const p = document.querySelector('#page'); return !!p && p.children.length > 0; });
        if (!mounted) note(`${label}px ${id}`, 'page container is empty (page did not mount)');
        const hscroll = await page.evaluate(() => document.documentElement.scrollWidth > document.documentElement.clientWidth + 1);
        if (hscroll && width <= 400) note(`${label}px ${id}`, 'horizontal page scroll at 360 px');
        await page.screenshot({ path: path.join(outDir, `${id.replace(/[^\w.-]/g, '_')}-${label}.png`), fullPage: true });
      }
      where = 'close';
      await ctx.close();
    }
  } finally {
    await browser.close();
  }

  // /api/status: every feature entry is an object, and `features` is an object when present.
  try {
    const s = await getJson(port, '/api/status');
    if (s.features !== undefined) {
      if (s.features === null || typeof s.features !== 'object' || Array.isArray(s.features)) note('/api/status', 'features is not an object');
      else for (const [k, v] of Object.entries(s.features)) if (v === null || typeof v !== 'object') note('/api/status', `features.${k} is not an object`);
    }
  } catch (e) { note('/api/status', e.message); }

  cleanup();
  fs.rmSync(tmp, { recursive: true, force: true });
  const secs = (Date.now() - started) / 1000;
  if (problems.length) {
    console.error(`ui-smoke: FAILED, ${problems.length} problem(s) in ${secs.toFixed(1)} s`);
    for (const p of problems) console.error('  - ' + p);
    process.exit(1);
  }
  log(`OK in ${secs.toFixed(1)} s, screenshots in ${outDir}`);
  process.exit(0);
}

main().catch(e => { console.error('ui-smoke: ' + (e && e.stack || e)); cleanup(); process.exit(1); });
