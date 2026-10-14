#!/usr/bin/env node
/* ============================================================================
   snap.mjs  -  screenshots of the app and the project page, scripted
   ----------------------------------------------------------------------------
   Drives headless Chrome over the DevTools protocol with nothing but Node's
   own fetch and WebSocket - no Puppeteer, no install. Each shot loads a page
   at an exact viewport, runs a few steps (press the bench's "Your finger",
   wait for the wallet), and saves a PNG into docs/images/ - or, for the
   project page's QA shots, into build/snaps/. A clip records the steps
   instead, and tools/gif.py (Pillow) makes a GIF of them.

       python tools/serve.py &           # the pages, on localhost:8080
       node tools/snap.mjs               # every shot and clip
       node tools/snap.mjs app-home demo # just these

   CHROME=/path/to/chrome overrides where Chrome is looked for.
   ========================================================================= */

import { spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(ROOT, 'docs', 'images');
const QA = join(ROOT, 'build', 'snaps');
const BASE = process.env.MONO_BASE ?? 'http://localhost:8080';
const PORT = 9333;
const sleep = ms => new Promise(r => setTimeout(r, ms));

const CHROMES = [
  process.env.CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);

// ------------------------------------------------------------------- CDP --
// Stills draw WebGL with SwiftShader, the CPU rasteriser every machine has.
// It manages about 5 frames a second of the twin, so clips use the GPU.
async function chrome({ gpu = false } = {}) {
  const bin = CHROMES.find(p => existsSync(p));
  if (!bin) throw new Error('Chrome not found; set CHROME=/path/to/chrome');
  const profile = mkdtempSync(join(tmpdir(), 'mono-snap-'));
  const gl = gpu ? ['--enable-gpu', '--ignore-gpu-blocklist'] : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'];
  const proc = spawn(bin, ['--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--hide-scrollbars', '--force-color-profile=srgb',
    ...gl, '--window-size=1440,900', 'about:blank'],
    { stdio: 'ignore' });
  for (let i = 0; i < 100; i++) {
    try { await fetch(`http://127.0.0.1:${PORT}/json/version`); break; } catch { await sleep(100); }
  }
  return { proc, profile };
}

async function openPage() {
  const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let seq = 0;
  const pending = new Map(), waiters = [], listeners = [];
  ws.onmessage = msg => {
    const m = JSON.parse(msg.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      m.error ? reject(new Error(m.error.message)) : resolve(m.result);
    } else if (m.method) {
      for (const w of [...waiters]) if (w.method === m.method) { waiters.splice(waiters.indexOf(w), 1); w.resolve(m.params); }
      for (const l of listeners) if (l.method === m.method) l.fn(m.params);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const once = method => new Promise(resolve => waiters.push({ method, resolve }));
  const on = (method, fn) => listeners.push({ method, fn });
  await send('Page.enable');
  await send('Runtime.enable');
  return { send, once, on, close: () => ws.close(), id: target.id };
}

async function evaluate(page, expression) {
  const r = await page.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
}

// ------------------------------------------------------------ the shots --
// Helpers injected into the page before each script.
const KIT = `
  window.__sleep = ms => new Promise(r => setTimeout(r, ms));
  window.__doc = () => document.getElementById('appFrame')?.contentDocument ?? document;
  window.__press = async (label, ms = 600, doc = document) => {
    const k = [...doc.querySelectorAll('.bench-key, .key')].find(b => b.textContent.includes(label));
    const o = { bubbles: true, pointerId: 1, button: 0, isPrimary: true };
    k.dispatchEvent(new PointerEvent('pointerdown', o)); await __sleep(ms); k.dispatchEvent(new PointerEvent('pointerup', o));
  };
  window.__click = (id, doc = document) => doc.getElementById(id).click();
  // wait for the UI to reach a state instead of guessing how long it takes
  window.__until = async (test, ms = 8000) => {
    const end = Date.now() + ms;
    while (!test()) { if (Date.now() > end) throw new Error('timed out waiting for: ' + test); await __sleep(50); }
  };
  window.__paired = (doc = document) => __until(() => doc.getElementById('pairTitle')?.textContent.match(/yours|paired/i)
                                                     && !doc.getElementById('pairCta').hidden);
  // app: the document the ONYX screens are in; bench: where the bench keys are
  // (the same document in the stand-alone app, the parent page on the site)
  window.__enrol = async (label, app, bench = app) => {
    __click('b-gofingers', app); await __sleep(200); __click('b-goenroll', app); await __sleep(400);
    await __press(label, 700, bench); await __sleep(450); await __press(label, 700, bench); await __sleep(450);
  };
`;

const APP = { width: 402, height: 874, scale: 2, mobile: true };
const SHOTS = {
  // ---- the app, in its own window, with its built-in demo wallet
  'app-home': { url: '/mono_app/?demo', view: APP, script: `
      await __paired(); __click('pairCta'); await __sleep(300);
      await __enrol('Your finger', document); __click('enrollAlt'); await __sleep(200);
      document.querySelector('#s-fingers .nav-home').click(); await __sleep(300);
      await __press('Your finger', 500); await __sleep(700);` },
  'app-enrol': { url: '/mono_app/?demo', view: APP, script: `
      await __paired(); __click('pairCta'); await __sleep(300);
      await __until(() => document.querySelector('#s-enroll.on'));
      await __press('Your finger', 700); await __sleep(900);` },
  'app-alarm': { url: '/mono_app/?demo&theme=light', view: APP, script: `
      await __paired(); __click('pairAlt'); await __sleep(300);
      await __press('Shake it', 800); await __sleep(5600);` },   // past the card's shake and the 5 s toast
  'app-settings': { url: '/mono_app/?demo', view: APP, script: `
      await __paired(); __click('pairAlt'); await __sleep(300);
      __click('b-gosettings'); await __sleep(400);` },
  'app-hold': { url: '/mono_app/?demo&theme=light', view: APP, script: `
      await __paired(); __click('pairAlt'); await __sleep(300);
      __click('b-holdopen'); await __sleep(200);
      const pad = document.getElementById('holdpad'); const o = { bubbles: true, pointerId: 1, button: 0, isPrimary: true };
      pad.dispatchEvent(new PointerEvent('pointerdown', o)); await __sleep(1300); pad.dispatchEvent(new PointerEvent('pointerup', o));
      await __sleep(600);` },
  // ---- the project page: QA shots no document shows, so they go to build/snaps/
  'site-hero': { qa: true, url: '/', view: { width: 1440, height: 900, scale: 1 }, script: `await __sleep(1200);` },
  'site-demo': { qa: true, url: '/#demo', view: { width: 1440, height: 1100, scale: 1 }, script: `
      document.getElementById('demo').scrollIntoView(); await __sleep(4000);
      const d = __doc(); await __paired(d); __click('pairCta', d); await __sleep(300);
      await __enrol('Your finger', d, document);
      __click('enrollAlt', d); await __sleep(200); d.querySelector('#s-fingers .nav-home').click(); await __sleep(200);
      await __press('Your finger', 600); await __sleep(1800);
      window.scrollTo(0, document.getElementById('demo').offsetTop - 20); await __sleep(400);` },
  'site-cad': { qa: true, url: '/#cad', view: { width: 1440, height: 900, scale: 1 }, script: `
      document.getElementById('cad').scrollIntoView(); await __sleep(5000);
      window.scrollTo(0, document.getElementById('cad').offsetTop - 20); await __sleep(600);` },
};

// ------------------------------------------------------------ the clips --
// Recorded rather than shot: `setup` runs first, then `script` runs while the
// screencast records, and tools/gif.py turns the frames into docs/images/NAME.gif.
const CLIPS = {
  // the project page's demo: a stranger is refused, a grab sets off the alarm,
  // the owner's finger stops it and opens the wallet. (Shaking an open wallet
  // does nothing - that is its owner taking a card out.)
  'demo': { url: '/#demo', view: { width: 1440, height: 1000 }, crop: '.demo-grid', gif: { width: 1000, fps: 12.5 },
    setup: `
      document.getElementById('demo').scrollIntoView(); await __sleep(4000);
      const d = __doc(); await __paired(d); __click('pairCta', d); await __sleep(300);
      await __enrol('Your finger', d, document);
      __click('enrollAlt', d); await __sleep(200); d.querySelector('#s-fingers .nav-home').click();
      // below the sticky nav, and instantly: the page scrolls smoothly
      const grid = document.querySelector('.demo-grid'), nav = document.querySelector('.nav').offsetHeight;
      window.scrollTo({ top: grid.getBoundingClientRect().top + scrollY - nav - 12, behavior: 'instant' });
      await __sleep(1500);`,
    script: `
      await __sleep(1200);
      await __press('Someone else', 700); await __sleep(2000);
      await __press('Shake it', 1100); await __sleep(3600);
      await __press('Your finger', 700); await __sleep(3200);` },
};

async function load(page, view, url) {
  const { width, height, scale = 1, mobile = false } = view;
  await page.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile });
  const loaded = page.once('Page.loadEventFired');
  await page.send('Page.navigate', { url: BASE + url });
  await loaded;
}

async function shoot(page, name, shot) {
  await load(page, shot.view, shot.url);
  await evaluate(page, `(async () => { ${KIT}; ${shot.script} })()`);
  const { data } = await page.send('Page.captureScreenshot', { format: 'png' });
  const dir = shot.qa ? QA : OUT;
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `${name}.png`), Buffer.from(data, 'base64'));
  const { width, height, scale = 1 } = shot.view;
  console.log(`  ${shot.qa ? 'build/snaps' : 'docs/images'}/${name}.png  (${width}x${height} @${scale}x)`);
}

// Chrome pushes a screencast frame each time the page repaints, stamped with
// the time it was drawn; gif.py resamples them onto an even timeline.
async function record(page, name, clip) {
  await load(page, clip.view, clip.url);
  await evaluate(page, `(async () => { ${KIT}; ${clip.setup} })()`);
  const box = await evaluate(page, `(() => {
      const r = document.querySelector(${JSON.stringify(clip.crop)}).getBoundingClientRect();
      return [r.x, r.y, r.width, Math.min(r.height, innerHeight - r.y)].map(Math.round); })()`);
  const dir = mkdtempSync(join(tmpdir(), 'mono-clip-'));
  const frames = [];
  page.on('Page.screencastFrame', ({ data, metadata, sessionId }) => {
    const file = `f${String(frames.length).padStart(4, '0')}.png`;
    writeFileSync(join(dir, file), Buffer.from(data, 'base64'));
    frames.push({ file, t: metadata.timestamp });
    page.send('Page.screencastFrameAck', { sessionId }).catch(() => {});
  });
  try {
    await page.send('Page.startScreencast', { format: 'png', everyNthFrame: 1 });
    await evaluate(page, `(async () => { ${clip.script} })()`);
    const end = Date.now() / 1000;        // the same clock as the frame stamps
    await page.send('Page.stopScreencast');
    await sleep(300);
    writeFileSync(join(dir, 'frames.json'), JSON.stringify({ frames, end }));
    const out = join(OUT, `${name}.gif`);
    const py = spawnSync(process.env.PYTHON ?? 'python', [join(ROOT, 'tools', 'gif.py'), dir, out,
      '--crop', box.join(','), '--width', String(clip.gif.width), '--fps', String(clip.gif.fps)], { encoding: 'utf8' });
    if (py.status !== 0) throw new Error(`gif.py failed: ${py.stderr || py.error}`);
    console.log(`  docs/images/${name}.gif  ${py.stdout.trim()}`);
    if (py.stderr.trim()) console.warn(`  ${py.stderr.trim()}`);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

const only = process.argv.slice(2);
const wanted = ([name]) => !only.length || only.includes(name);
let failed = 0;
for (const [jobs, run, gpu] of [[Object.entries(SHOTS).filter(wanted), shoot, false],
                                [Object.entries(CLIPS).filter(wanted), record, true]]) {
  if (!jobs.length) continue;
  if (gpu) await sleep(3000);       // let the stills' Chrome finish dying: a busy CPU halves a clip's frame rate
  const { proc, profile } = await chrome({ gpu });
  try {
    for (const [name, job] of jobs) {
      const page = await openPage();
      try { await run(page, name, job); }
      catch (err) { failed++; console.error(`  ${name}: ${err.message}`); }
      finally { page.close(); await fetch(`http://127.0.0.1:${PORT}/json/close/${page.id}`).catch(() => {}); }
    }
  } finally {
    proc.kill();
    await new Promise(r => proc.exitCode !== null ? r() : proc.once('exit', r));
    await sleep(300);
    try { rmSync(profile, { recursive: true, force: true }); } catch { /* Chrome may still hold it */ }
  }
}
process.exit(failed ? 1 : 0);
