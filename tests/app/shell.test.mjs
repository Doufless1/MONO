// The app installs, and opens with no signal.   node --test "tests/app/*.test.mjs"
//
// Both depend on lists that nothing else checks: the icons in the manifest and
// the files the service worker caches. Add a module and forget to cache it, and
// the app still works - until the first time it is opened offline. [REVIEW A13]
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const APP = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'mono_app');
const read = f => readFileSync(join(APP, f), 'utf8');
const manifest = JSON.parse(read('manifest.webmanifest'));
const shell = [...read('sw.js').match(/const SHELL = \[([\s\S]*?)\];/)[1].matchAll(/'([^']+)'/g)].map(m => m[1]);

/** A PNG's pixel size, from its IHDR chunk. */
function pngSize(file) {
  const b = readFileSync(file);
  assert.equal(b.toString('latin1', 12, 16), 'IHDR', `${file} is not a PNG`);
  return `${b.readUInt32BE(16)}x${b.readUInt32BE(20)}`;
}

/** Every module app.js pulls in, following relative imports that stay inside the app. */
function modules(entry = 'js/app.js', seen = new Set()) {
  if (seen.has(entry)) return seen;
  seen.add(entry);
  for (const [, spec] of read(entry).matchAll(/(?:from|import\()\s*'(\.[^']+)'/g)) {
    const path = normalize(join(dirname(entry), spec)).replaceAll('\\', '/');
    if (!path.startsWith('..')) modules(path, seen);    // ../../sim/ is the demo's wallet, not the app
  }
  return seen;
}

test('every icon the manifest names exists, at the size it claims', () => {
  assert.ok(manifest.icons.some(i => i.sizes === '512x512' && i.purpose === 'maskable'), 'a maskable icon');
  for (const icon of [...manifest.icons, ...manifest.shortcuts.flatMap(s => s.icons)]) {
    const file = join(APP, icon.src);
    assert.ok(existsSync(file), `${icon.src} exists`);
    if (icon.src.endsWith('.png')) assert.equal(pngSize(file), icon.sizes, icon.src);
  }
});

test('every file the service worker caches exists', () => {
  for (const f of shell) assert.ok(existsSync(join(APP, f === './' ? 'index.html' : f)), f);
});

test('every module the app is made of is in the offline shell', () => {
  const cached = new Set(shell.map(f => f.replace(/^\.\//, '')));
  const used = [...modules()];
  assert.ok(used.length >= 8, `found the app's modules (${used.length})`);
  for (const m of used) assert.ok(cached.has(m), `${m} is cached for offline use`);
  for (const f of readdirSync(join(APP, 'js'))) assert.ok(used.includes(`js/${f}`), `js/${f} is used by the app`);
});

test('the page links the manifest and the app registers the worker', () => {
  assert.match(read('index.html'), /<link rel="manifest" href="manifest\.webmanifest">/);
  assert.match(read('js/app.js'), /serviceWorker\.register\('sw\.js'\)/);
  assert.equal(manifest.display, 'standalone');
  assert.equal(manifest.start_url, './');
});
