// The app's session layer against the real firmware, compiled to WebAssembly.
// No browser, no Bluetooth, no mocks of the wallet: this is mono_firmware.ino.
//
//   python sim/build_wasm.py && node --test tests/app/
//
// Time is virtual, for the app as well as the wallet: the session's timers run
// on a clock driven by the wallet's, so a 60-second separation alarm takes
// milliseconds and a reconnect backoff can never race the firmware.
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { WalletSim, simLink } from '../../sim/wallet-sim.js';
import { WalletSession, WalletError, PairError } from '../../mono_app/js/session.js';

const WASM = fileURLToPath(new URL('../../sim/mono_wallet.wasm', import.meta.url));
const tick = () => new Promise(resolve => setImmediate(resolve));

let bytes;
before(() => {
  if (!existsSync(WASM)) throw new Error('sim/mono_wallet.wasm missing - run: python sim/build_wasm.py');
  bytes = readFileSync(WASM);
});

/** The session's clock, made of the wallet's time. Timers fire as it passes. */
function virtualClock(sim) {
  let timers = [], seq = 0;
  return {
    now: () => sim.now,
    set(fn, ms) { timers.push({ id: ++seq, at: sim.now + ms, fn }); return seq; },
    clear(id) { timers = timers.filter(t => t.id !== id); },
    fire() {
      const due = timers.filter(t => t.at <= sim.now);
      timers = timers.filter(t => t.at > sim.now);
      for (const t of due) t.fn();
    },
  };
}

async function freshWallet() {
  const sim = await WalletSim.load(bytes);
  sim.boot([1, 2]);
  const usb = [];
  sim.addEventListener('usb', e => usb.push(e.detail));
  return { sim, link: simLink(sim), clock: virtualClock(sim), usb };
}

function keyring() {
  const m = new Map();
  return { get: id => m.get(id), set: (id, k) => m.set(id, k), delete: id => m.delete(id) };
}

function session(w, keys) {
  return new WalletSession({ link: w.link, keys, clock: w.clock, retry: { first: 500, max: 4000 } });
}

/** Run wallet and app together until done() holds, or fail after maxMs of wallet time. */
async function runUntil(w, done, maxMs = 30000) {
  const end = w.sim.now + maxMs;
  while (!done()) {
    if (w.sim.now >= end) throw new Error(`still waiting after ${maxMs} ms of wallet time`);
    w.sim.advance(5);
    w.clock.fire();
    await tick();
  }
}

async function drive(w, promise, maxMs) {
  let settled = false, value, error;
  promise.then(v => { settled = true; value = v; }, e => { settled = true; error = e; });
  await runUntil(w, () => settled, maxMs);
  if (error) throw error;
  return value;
}

const wait = (w, ms) => { const end = w.sim.now + ms; return runUntil(w, () => w.sim.now >= end, ms + 50); };

async function pairedOwner() {
  const w = await freshWallet();
  const keys = keyring();
  const s = session(w, keys);
  await drive(w, s.connect());
  await drive(w, s.pair());
  return { ...w, keys, s };
}

async function enroll(w, who = 1) {
  let done = null;
  w.s.addEventListener('enroll', e => { if (e.detail.flags[0] === 'ok') done = e.detail; });
  await drive(w, w.s.request('ENROLL'));
  for (const step of ['!touch ' + who, '!lift', '!touch ' + who, '!lift']) {
    w.sim.bench(step);
    await wait(w, 300);
  }
  await runUntil(w, () => done);
  return done;
}

test('a factory-fresh wallet pairs straight away and the session becomes ready', async () => {
  const w = await freshWallet();
  const keys = keyring();
  const s = session(w, keys);
  await drive(w, s.connect());
  assert.equal(s.state, 'unpaired');
  assert.equal(s.hello.setup, true);
  await drive(w, s.pair());
  assert.equal(s.state, 'ready');
  assert.match(keys.get('simulated-mono'), /^[0-9a-f]{32}$/);
  assert.equal(s.status.latch, 'locked');
  assert.equal(s.settings.relock, 6000);
  assert.deepEqual(s.fingers, []);
});

test('before pairing, the wallet refuses everything that matters', async () => {
  const w = await freshWallet();
  const s = session(w, keyring());
  await drive(w, s.connect());
  await assert.rejects(drive(w, s.request('UNLOCK')),
                       err => err instanceof WalletError && /ERR auth UNLOCK/.test(err.message));
  assert.equal(w.sim.peek().latch, 'locked');
});

test("the browser's WebCrypto HMAC and the firmware's C HMAC agree: a saved key reconnects with no pairing", async () => {
  const w = await pairedOwner();
  await drive(w, w.s.disconnect());
  const again = session(w, w.keys);
  await drive(w, again.connect());
  assert.equal(again.state, 'ready', 'AUTH succeeded on the first try');
});

test('a key the wallet has forgotten is dropped and the phone is asked to pair again', async () => {
  const w = await pairedOwner();
  await drive(w, w.s.disconnect());
  w.keys.set('simulated-mono', 'ff'.repeat(16));
  const s = session(w, w.keys);
  let forgotten = false;
  s.addEventListener('forgotten', () => { forgotten = true; });
  await drive(w, s.connect());
  assert.equal(s.state, 'unpaired');
  assert.equal(forgotten, true);
  assert.equal(w.keys.get('simulated-mono'), undefined);
});

test('enrolling through the app stores the finger, and the list updates itself', async () => {
  const w = await pairedOwner();
  const ok = await enroll(w, 1);
  assert.equal(ok.kv.id, '1');
  await runUntil(w, () => w.s.fingers.length === 1);
  assert.deepEqual(w.s.fingers, [1]);
  assert.equal(w.s.status.fingers, 1);
});

test('UNLOCK resolves only when the wallet says it opened; the relock arrives as a push', async () => {
  const w = await pairedOwner();
  const reply = await drive(w, w.s.request('UNLOCK'));
  assert.equal(reply.raw, 'UNLOCK ok');
  await runUntil(w, () => w.s.status.latch === 'open');
  assert.equal(w.sim.peek().servo, 95, 'the servo really moved');
  await runUntil(w, () => w.s.status.latch === 'locked', 8000);
});

test("a second phone has to wait for the owner's finger", async () => {
  const w = await pairedOwner();
  await enroll(w, 1);
  await drive(w, w.s.disconnect());

  const phone2 = session(w, keyring());
  await drive(w, phone2.connect());
  assert.equal(phone2.state, 'unpaired');
  const pairing = phone2.pair();
  await runUntil(w, () => phone2.state === 'pair-wait');
  w.sim.bench('!touch 1');
  await drive(w, pairing);
  w.sim.bench('!lift');
  assert.equal(phone2.state, 'ready');
  assert.equal(w.sim.peek().latch, 'locked', 'confirming a phone does not open the wallet');
});

test('a stranger cannot confirm a pairing; it times out', async () => {
  const w = await pairedOwner();
  await enroll(w, 1);
  await drive(w, w.s.disconnect());
  const phone2 = session(w, keyring());
  await drive(w, phone2.connect());
  const pairing = phone2.pair();
  await runUntil(w, () => phone2.state === 'pair-wait');
  w.sim.bench('!touch 0');
  await wait(w, 400);
  w.sim.bench('!lift');
  await assert.rejects(drive(w, pairing), err => err instanceof PairError && err.reason === 'timeout');
  assert.equal(phone2.state, 'unpaired');
});

test('walking away: the wallet warns, then the phone comes back, reconnects by itself and the alarm stands down', async () => {
  const w = await pairedOwner();
  w.link.walkAway();
  await runUntil(w, () => w.s.state === 'reconnecting');
  await wait(w, 21000);
  assert.ok(w.usb.includes('EVT separation warn'), 'the wallet noticed');
  await wait(w, 45000);
  assert.ok(w.usb.includes('EVT separation alert'));
  assert.equal(w.sim.peek().adv, 32, 'fast beacon while lost');
  assert.equal(w.s.state, 'reconnecting', 'the app kept trying the whole time');

  w.link.comeBack();
  await runUntil(w, () => w.s.state === 'ready', 10000);
  await wait(w, 100);
  assert.equal(w.sim.peek().leash, 'ok');
  assert.equal(w.sim.peek().adv, 244, 'beacon back to normal once the paired phone is back');
});

test('hanging up on purpose says BYE, so the wallet never cries "lost"', async () => {
  const w = await pairedOwner();
  await drive(w, w.s.disconnect());
  assert.equal(w.s.state, 'closed');
  await wait(w, 90000);
  assert.ok(!w.usb.some(l => l.startsWith('EVT separation')));
  assert.equal(w.s.state, 'closed', 'and the app did not try to reconnect');
});

test('a tamper alarm reaches the app as a push, and CLEAR from the app ends it', async () => {
  const w = await pairedOwner();
  w.sim.bench('!shake on');
  await runUntil(w, () => w.s.status.alarm === true, 2000);
  w.sim.bench('!shake off');
  await drive(w, w.s.request('CLEAR'));
  await runUntil(w, () => w.s.status.alarm === false, 1000);
  await wait(w, 100);
  assert.equal(w.sim.peek().adv, 244);
});

test('a preset choice is stored by the wallet and reflected back from its reply', async () => {
  const w = await pairedOwner();
  await drive(w, w.s.request('SET relock 15000'));
  assert.equal(w.s.settings.relock, 15000);
  await drive(w, w.s.request('SET sepwarn 0'));
  assert.equal(w.s.settings.sepwarn, 0);
});

test('a wallet that never answers times the request out instead of hanging the screen', async () => {
  const w = await pairedOwner();
  const quiet = { ...w.link, write: async () => {} };      // the line goes nowhere
  const s = new WalletSession({ link: quiet, keys: keyring(), clock: w.clock, timeoutMs: 3000 });
  await assert.rejects(drive(w, s.request('STATUS'), 5000), /did not answer STATUS/);
});
