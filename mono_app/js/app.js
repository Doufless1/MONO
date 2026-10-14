/* ============================================================================
   ONYX  -  companion app for the MONO wallet
   ----------------------------------------------------------------------------
   The screens are the ONYX design; the data behind them is the wallet itself,
   reached through session.js over Web Bluetooth - or, for the demo, over a
   simulated link to the same firmware running as WebAssembly.

   One state object, one render() pass, and one rule: nothing on screen claims
   more than the wallet has confirmed. Buttons that need the wallet are
   disabled while it is away; "Open" means the wallet said UNLOCK ok; a
   setting shows a spinner until the wallet has stored it; a finger leaves
   the list when the wallet says it deleted it. v2 broke each of these.
   ========================================================================= */

import { WalletSession, PairError } from './session.js';
import { webBluetoothAvailable, chooseWallet, rememberedWallets } from './transport.js';
import { PRESETS, nearestPreset, setCommands, batteryPercent, ago,
         describeFailure, ENROLL_FAILURE, PAIR_FAILURE } from './protocol.js';
import { cryptoAvailable } from './crypto.js';
import * as store from './store.js';
import { confirmSheet, promptSheet, sheet, toast, pressAndHold, longPress } from './ui.js';
import { startDemo, stopDemo } from './demo.js';

const $ = id => document.getElementById(id);
const LOCKOUT_MS = 30000, PAIR_WINDOW_MS = 20000, HOLD_MS = 950, MAX_FINGERS = 10;
const params = new URLSearchParams(location.search);
// Dates in the language the copy is written in, not whatever the browser is set to.
const LOCALE = document.documentElement.lang || 'en';

// ================================================================= state ==
const S = {
  screen: 'home',
  choice: null,
  demo: false,
  hostedDemo: false,
  session: null,
  lastState: null,
  wasLost: false,               // between losing the wallet and getting it back
  status: null,                 // the wallet's last STATUS, kept (and shown dimmed) when it is away
  settings: {},
  fingers: [],
  openSince: 0,                 // when the latch was last reported opening, for the countdown
  lockoutSince: 0,
  ringingUntil: 0,
  pairing: false,
  pair: { phase: 'idle', error: null, claimed: false },
  enroll: { step: 'idle', id: null, fail: null },
  hold: { open: false, phase: 'idle', intent: 'unlock', error: null, doneAt: 0 },
  pendingChoice: null,
};

const ready = () => S.session?.state === 'ready';
// Browsers refuse vibration before the person has touched the page; ask only when it can work.
const buzz = pattern => { if (navigator.userActivation?.hasBeenActive) navigator.vibrate?.(pattern); };
const walletId = () => S.session?.link.id ?? store.lastWallet.get();
const fingerName = id => store.fingers.get(walletId(), id)?.name ?? `Finger ${id}`;

// ============================================================== sessions ==
function attach(link, keys) {
  detach();
  const s = new WalletSession({ link, keys });
  S.session = s;
  s.addEventListener('state', onSessionState);
  s.addEventListener('status', e => onStatus(e.detail));
  s.addEventListener('settings', e => { S.settings = e.detail; render(); });
  s.addEventListener('fingers', e => {
    S.fingers = e.detail;
    store.fingers.reconcile(walletId(), S.fingers);
    render();
  });
  s.addEventListener('event', e => onWalletEvent(e.detail));
  s.addEventListener('enroll', e => onEnroll(e.detail));
  s.addEventListener('line', e => logLine(e.detail.dir, e.detail.text));
  s.addEventListener('forgotten', () => toast('The wallet no longer knows this phone. Pair it again.', { icon: 'i-unlink' }));
  render();
}

function detach() {
  S.session = null;
  S.lastState = null;
}

function onSessionState(e) {
  const state = e.detail, was = S.lastState;
  S.lastState = state;
  logLine('ev', `session: ${state}`);

  if (state === 'ready') {
    if (S.screen === 'pair' && S.pair.phase !== 'done') S.pair.phase = 'done';
    // A reconnect passes through 'handshake', so remember the loss itself.
    if (S.wasLost) { S.wasLost = false; toast('Back in range.', { icon: 'i-bt' }); }
    if (S.session.link.kind === 'ble') store.lastWallet.set(S.session.link.id);
  }
  if (state === 'unpaired' && !S.pairing) beginPairing();
  if (state === 'reconnecting' && was === 'ready') {
    S.wasLost = true;
    recordFix('lost');
    if (S.screen === 'enroll' && ['start', 'place', 'lift', 'again'].includes(S.enroll.step)) enrollFailed('link');
    if (S.hold.phase === 'sending') holdFailed('The wallet went out of range before it answered.');
    toast('Lost the wallet. ONYX reconnects when it is back in range.', { icon: 'i-unlink', ms: 4200 });
  }
  render();
}

function onStatus(next) {
  const prev = S.status;
  S.status = next;
  if (next.latch === 'open' && prev?.latch !== 'open') S.openSince = Date.now();
  if (next.sensor === 'lockout' && prev?.sensor !== 'lockout') S.lockoutSince = Date.now();
  if (next.latch === 'locked' && prev?.latch === 'open' && S.hold.open && S.hold.phase === 'done' && S.hold.intent === 'unlock') {
    setTimeout(() => { if (S.hold.phase === 'done') closeHold(); }, 1400);   // it relocked: the sheet has said its piece
  }
  render();
}

function onWalletEvent(line) {
  switch (line.flags[0]) {
    case 'rejected': toast('A finger the wallet does not know touched the sensor.', { icon: 'i-finger' }); break;
    case 'lockout':  toast('Too many wrong fingers. The sensor rests for 30 seconds.', { icon: 'i-alert', ms: 4500 }); break;
    case 'tamper':
      toast('Your wallet felt a grab and locked itself down.', { icon: 'i-alert', ms: 5000 });
      buzz([140, 70, 140]);
      break;
    case 'lockdown': toast('Alarm off.', { icon: 'i-check' }); break;
    case 'unlocked':
      if (line.kv.id && S.hold.phase !== 'sending') toast(`Opened with ${fingerName(line.kv.id)}.`, { icon: 'i-lock-open' });
      break;
  }
}

async function connectWallet() {
  if (S.session && ['closed', 'idle'].includes(S.session.state)) {
    try { await S.session.connect(); } catch (err) { toast(`Could not reach the wallet. ${err.message}`, { icon: 'i-alert' }); }
    return;
  }
  if (!webBluetoothAvailable()) { toast('This browser has no Web Bluetooth. Try the demo wallet instead.', { icon: 'i-bt' }); return; }
  if (!cryptoAvailable()) { toast('This page needs a secure origin (HTTPS) to pair.', { icon: 'i-alert' }); return; }
  let link;
  try { link = await chooseWallet(); }
  catch (err) { if (err.name !== 'NotFoundError') toast(err.message, { icon: 'i-alert' }); return; }
  attach(link, store.keys);
  try { await S.session.connect(); }
  catch (err) { toast(`Could not reach the wallet. ${err.message}`, { icon: 'i-alert' }); }
}

// After a reload Chrome can hand back a wallet this site already used, so a
// paired phone reconnects without the chooser or a tap.
async function resume() {
  const id = store.lastWallet.get();
  if (!id || !store.keys.get(id) || !webBluetoothAvailable()) return;
  const link = (await rememberedWallets()).find(l => l.id === id);
  if (!link) return;
  attach(link, store.keys);
  try { await S.session.connect(); } catch { /* stays idle; the pill offers Connect */ }
}

async function hangUp() {
  try { await S.session?.disconnect(); } catch { /* already gone */ }
  render();
}

// ================================================================ pairing ==
async function beginPairing() {
  S.pairing = true;
  S.pair = { phase: 'asking', error: null, claimed: !!S.session?.hello?.setup };
  go('pair');
  try {
    await S.session.pair();
    S.pair.phase = 'done';
  } catch (err) {
    S.pair = { phase: 'error', claimed: false,
               error: err instanceof PairError ? describeFailure(PAIR_FAILURE, err.reason)
                                               : (err.message || 'The wallet did not answer.') };
  } finally {
    S.pairing = false;
    render();
  }
}

// ================================================================= demo ===
let demoStarting = false;
async function enterDemo() {
  if (S.demo || demoStarting) return;          // one simulated wallet at a time
  demoStarting = true;
  if (S.session) await hangUp();
  let demo;
  try { demo = await startDemo(); }
  catch (err) { toast('The demo wallet is not available on this page.', { icon: 'i-alert' }); console.error(err); return; }
  finally { demoStarting = false; }
  S.demo = true;
  S.hostedDemo = demo.hosted;
  $('ribbon').hidden = demo.hosted;
  document.body.classList.toggle('has-ribbon', !demo.hosted);
  store.fingers.clear(demo.link.id);
  attach(demo.link, store.memoryKeys());
  try { await S.session.connect(); } catch (err) { toast(err.message, { icon: 'i-alert' }); }
}

async function exitDemo() {
  await hangUp();
  stopDemo();
  detach();
  Object.assign(S, { demo: false, status: null, settings: {}, fingers: [], screen: 'home', choice: null });
  $('ribbon').hidden = true;
  document.body.classList.remove('has-ribbon');
  render();
}

// ============================================================ navigation ==
function go(screen) {
  S.screen = screen;
  S.choice = null;
  render();
  window.scrollTo(0, 0);
  // Move focus to the new screen's heading, so a screen reader hears where it is.
  const h = document.querySelector(`#s-${screen} h1, #s-${screen} h2`);
  if (h && screen !== 'home') { h.tabIndex = -1; h.focus({ preventScroll: true }); }
}

function back() {
  if (S.choice) { S.choice = null; render(); return; }
  if (S.screen === 'console') { go(S.consoleFrom ?? 'home'); return; }
  if (S.screen !== 'home') go(S.screen === 'enroll' ? 'fingers' : 'home');
}

// ================================================================= render ==
function render() {
  const showing = S.choice ? 'choice' : S.screen;
  for (const sec of document.querySelectorAll('.screen')) sec.classList.toggle('on', sec.id === `s-${showing}`);
  renderTop();
  renderHome();
  renderPair();
  renderFind();
  renderFingers();
  renderEnroll();
  renderSettings();
  if (S.choice) renderChoice(S.choice);
  renderHold();
}

const txt = (id, v) => { const n = $(id); if (n && n.textContent !== String(v)) n.textContent = v; };
const show = (id, on) => { const n = $(id); if (n) n.hidden = !on; };
const enable = (id, on) => { const n = $(id); if (n) n.disabled = !on; };

function renderTop() {
  const conn = S.session?.state ?? 'none';
  $('pill').dataset.conn = conn;
  txt('pillLabel', {
    none: 'Not paired', idle: 'Connect', closed: 'Connect', connecting: 'Connecting',
    handshake: 'Connecting', unpaired: 'Pairing', 'pair-wait': 'Pairing',
    ready: 'Nearby', reconnecting: 'Out of range',
  }[conn] ?? conn);
}

function secondsLeftOpen() {
  const relock = S.settings.relock ?? 6000;
  return Math.max(0, Math.ceil((S.openSince + relock - Date.now()) / 1000));
}

function latchWord() {
  const st = S.status;
  if (!st || !st.latch) return '—';
  if (st.alarm) return 'Locked down';
  if (st.latch === 'open') { const s = secondsLeftOpen(); return s > 0 ? `Open · locks in ${s} s` : 'Open'; }
  if (st.sensor === 'lockout') {
    const s = Math.ceil((S.lockoutSince + LOCKOUT_MS - Date.now()) / 1000);
    return s > 0 ? `Locked · resting ${s} s` : 'Locked';
  }
  return 'Locked';
}

function renderHome() {
  const conn = S.session?.state ?? 'none';
  const st = S.status;
  const bt = webBluetoothAvailable();

  show('n-unsupported', !S.session && !globalThis.navigator?.bluetooth);
  show('n-insecure', !S.session && !!globalThis.navigator?.bluetooth && !globalThis.isSecureContext);
  show('alarmCard', ready() && !!st?.alarm);

  const fix = store.lastFix.get();
  let label, title, sub, linked = false, near = false;
  if (!S.session) {
    label = 'Not paired yet'; title = 'Meet your wallet';
    sub = bt ? 'Pair the wallet in your pocket, or try a simulated one that runs the real firmware right here.'
             : 'Try a simulated wallet: the real firmware, compiled to WebAssembly, running in this tab.';
  } else if (conn === 'connecting' || conn === 'handshake') {
    label = 'Connecting'; title = 'Finding your wallet'; sub = 'Keep your phone close to it.';
  } else if (conn === 'unpaired' || conn === 'pair-wait') {
    label = 'Pairing'; title = 'Say yes on the wallet'; sub = 'A new phone is let in by a finger the wallet already knows.';
  } else if (conn === 'ready') {
    label = 'With you'; title = 'It is close by'; linked = near = true;
    sub = S.demo ? 'Connected to the demo wallet. Touch, shake or walk away from it with the controls below.'
                 : 'Connected over Bluetooth, so it is within a few metres of you.';
  } else if (conn === 'reconnecting') {
    label = 'Last seen'; title = fix ? ago(fix.ts) : 'Just now'; linked = true;
    sub = 'Out of range. ONYX keeps looking and reconnects by itself.';
  } else {
    label = 'Not connected'; title = 'Your wallet'; sub = 'Connect to open it, ring it or change how it behaves.';
  }
  txt('heroLabel', label); txt('heroTitle', title); txt('heroSub', sub);
  $('hero').dataset.near = near ? '1' : '0';
  show('heroLinked', linked);
  show('heroIdle', !linked && (!S.session || conn === 'closed' || conn === 'idle'));
  show('b-pair', bt || !!S.session);
  txt('b-pairLabel', S.session ? 'Connect' : 'Pair a wallet');
  show('b-demo', !S.session);
  $('b-demo').className = `btn ${bt ? 'btn-ghost' : 'btn-solid'}`;

  const ringing = Date.now() < S.ringingUntil;
  txt('ringLabel', ringing ? 'Beeping' : 'Ring it');
  enable('b-ring', ready() && !ringing);

  // status card: last known values, dimmed while the wallet is away
  show('statusCard', !!st);
  $('statusCard').classList.toggle('stale', !!st && !ready());
  txt('latchWord', latchWord());
  $('latchIcon').setAttribute('href', st?.latch === 'open' ? '#i-lock-open' : '#i-lock');
  const pct = batteryPercent(st?.vbat);
  txt('battWord', pct === null ? '—' : `${pct}%`);
  $('battFill').setAttribute('width', pct === null ? '0' : Math.max(0.8, 9 * pct / 100).toFixed(2));
  txt('fingerCount', st?.fingers ?? '—');

  show('homeActions', !!S.session);
  enable('b-holdopen', ready());
  txt('openLabel', ready() ? 'Hold to open' : 'Connect to open');
}

function renderPair() {
  if (S.screen !== 'pair') return;
  const conn = S.session?.state;
  const p = S.pair;
  let mode = 'busy', glyph = 'i-shield', title = 'Saying hello', body = 'Checking who this wallet belongs to.';
  let cta = null, alt = 'Cancel', ring = 0;

  if (p.phase === 'done') {
    const fingers = S.status?.fingers ?? 0;
    mode = 'done'; glyph = 'i-check';
    title = p.claimed ? 'The wallet is yours' : 'This phone is paired';
    body = fingers ? 'It can open the wallet, ring it and change how it behaves.'
                   : 'Next, teach the wallet your finger. It takes two touches.';
    cta = fingers ? 'Done' : 'Add your first finger';
    alt = fingers ? null : 'Later';
  } else if (p.phase === 'error') {
    mode = 'fail'; glyph = 'i-alert'; title = 'Not paired'; body = p.error; cta = 'Try again';
  } else if (conn === 'pair-wait') {
    mode = 'listen'; glyph = 'i-print'; title = 'Touch the sensor to say yes';
    body = 'Only a finger the wallet already knows can let a new phone in.';
    ring = Math.max(0, S.session.pairDeadline - Date.now()) / PAIR_WINDOW_MS;
  } else if (conn === 'connecting') {
    title = 'Finding your wallet'; body = 'Keep your phone close to it.';
  }
  $('pairPad').dataset.mode = mode;
  $('pairGlyph').setAttribute('href', `#${glyph}`);
  $('pairArc').setAttribute('stroke-dashoffset', (603.2 * (1 - ring)).toFixed(1));
  txt('pairTitle', title); txt('pairBody', body);
  show('pairCta', !!cta); if (cta) txt('pairCta', cta);
  show('pairAlt', !!alt); if (alt) txt('pairAlt', alt);
}

function renderFind() {
  if (S.screen !== 'find') return;
  const fix = store.lastFix.get();
  if (fix) {
    txt('fixAgo', ago(fix.ts));
    txt('fixPos', `${fix.lat.toFixed(4)}, ${fix.lon.toFixed(4)}`);
    const d = new Date(fix.ts);
    const clock = d.toLocaleTimeString(LOCALE, { hour: '2-digit', minute: '2-digit' });
    txt('fixWhen', d.toDateString() === new Date().toDateString() ? `Today, ${clock}` : `${d.toLocaleDateString(LOCALE)}, ${clock}`);
    txt('fixAcc', `Within ${Math.round(fix.acc)} m`);
  } else {
    txt('fixAgo', 'No position recorded yet');
    txt('fixPos', '—'); txt('fixWhen', '—'); txt('fixAcc', '—');
  }
  enable('b-maps', !!fix);
  show('fixHint', !store.prefs.get('geo', false));
}

let fingersSig = '';
function renderFingers() {
  if (S.screen !== 'fingers') return;
  const live = ready();
  const sig = JSON.stringify([S.fingers, store.fingers.all(walletId()), live]);
  if (sig !== fingersSig) {
    fingersSig = sig;
    const host = $('fingerList');
    host.replaceChildren(...S.fingers.map(id => fingerRow(id, live)));
  }
  enable('b-goenroll', live && S.fingers.length < MAX_FINGERS);
  show('fingersOffline', !live);
  show('b-removeall', S.fingers.length > 0);
  enable('b-removeall', live);
}

function fingerRow(id, live) {
  const entry = store.fingers.get(walletId(), id);
  const row = document.createElement('div');
  row.className = 'row tall';
  row.innerHTML = '<svg class="i" aria-hidden="true"><use href="#i-finger"/></svg>';
  const name = document.createElement('button');
  name.type = 'button';
  name.className = 'grow namebtn';
  name.setAttribute('aria-label', `Rename ${fingerName(id)}`);
  const n = document.createElement('span'); n.className = 't-row'; n.textContent = fingerName(id);
  const m = document.createElement('span'); m.className = 't-sub';
  m.textContent = entry?.added
    ? `Added ${new Date(entry.added).toLocaleDateString(LOCALE, { day: 'numeric', month: 'long' })} · slot ${id}`
    : `Added before this phone was paired · slot ${id}`;
  name.append(n, m);
  name.onclick = () => renameFinger(id);
  const rm = document.createElement('button');
  rm.type = 'button'; rm.className = 'btn btn-small'; rm.textContent = 'Remove';
  rm.disabled = !live;
  rm.setAttribute('aria-label', `Remove ${fingerName(id)}`);
  rm.onclick = () => removeFinger(id);
  row.append(name, rm);
  return row;
}

function renderEnroll() {
  if (S.screen !== 'enroll') return;
  const e = S.enroll;
  const bars = {
    start: ['now', ''], place: ['now', ''], lift: ['done', ''], again: ['done', 'now'],
    ok: ['done', 'done'], fail: ['', ''],
  }[e.step] ?? ['', ''];
  $('bar1').className = bars[0]; $('bar2').className = bars[1];
  const view = {
    start: ['busy',   'i-print', 'Getting the sensor ready', 'One moment.'],
    place: ['listen', 'i-print', 'Rest your finger on the sensor', 'Cover the small silver circle on the front of the wallet and hold still.'],
    lift:  ['lift',   'i-up',    'Now lift it off', 'The wallet has the first picture.'],
    again: ['listen', 'i-print', 'Place the same finger again', 'A second look, to be sure it is you.'],
    ok:    ['done',   'i-check', 'That finger can open it now',
            `Saved as ${fingerName(e.id)}. Try it: touch the sensor and the wallet opens.`],
    fail:  ['fail',   'i-alert', 'That did not work', `${describeFailure(ENROLL_FAILURE, e.fail)} Nothing was saved.`],
  }[e.step] ?? ['busy', 'i-print', '', ''];
  $('enrollPad').dataset.mode = view[0];
  $('enrollGlyph').setAttribute('href', `#${view[1]}`);
  txt('enrollTitle', view[2]); txt('enrollBody', view[3]);
  const finished = e.step === 'ok' || e.step === 'fail';
  txt('enrollCta', e.step === 'ok' ? 'Add another finger' : e.step === 'fail' ? 'Try again' : 'Waiting for the sensor');
  enable('enrollCta', finished && ready() && (e.step !== 'ok' || S.fingers.length < MAX_FINGERS));
  txt('enrollAlt', e.step === 'ok' ? 'Done' : e.step === 'fail' ? 'Back to fingers' : 'Cancel');
}

function renderSettings() {
  if (S.screen !== 'settings') return;
  for (const n of document.querySelectorAll('[data-word]')) {
    const o = nearestPreset(n.dataset.word, S.settings);
    const v = o ? (o.short ?? o.label) : '—';
    if (n.textContent !== v) n.textContent = v;
  }
  show('settingsOffline', !ready());
  $('b-geo').setAttribute('aria-checked', String(!!store.prefs.get('geo', false)));
  txt('walletName', S.session?.link.name ?? (store.lastWallet.get() ? 'MONO' : 'Not paired'));
  txt('walletFw', S.session?.hello?.firmware ? `v${S.session.hello.firmware}` : '—');
  txt('walletPhones', S.settings.phones ?? '—');
  enable('b-forget', !!walletId() && !S.demo);
  enable('b-unpair', ready());
}

let choiceSig = '';
function renderChoice(kind) {
  const p = PRESETS[kind];
  const cur = nearestPreset(kind, S.settings)?.v;
  const pending = S.pendingChoice;
  txt('choiceTitle', p.title); txt('choiceBody', p.body);
  txt('choiceFoot', ready() ? p.foot : 'Connect to the wallet to change this. It keeps the setting itself.');
  const sig = JSON.stringify([kind, cur, pending, ready()]);
  if (sig === choiceSig) return;
  choiceSig = sig;
  const focused = document.activeElement?.dataset?.v;
  $('choiceList').replaceChildren(...p.options.map(o => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'choice'; b.dataset.v = o.v;
    b.setAttribute('role', 'radio');
    b.setAttribute('aria-checked', String(o.v === cur));
    b.disabled = !ready() || !!pending;
    const wrap = document.createElement('span'); wrap.style.flex = '1';
    const lab = document.createElement('b'); lab.textContent = o.label;
    const note = document.createElement('small'); note.textContent = o.note;
    wrap.append(lab, note);
    const tick = document.createElement('span'); tick.className = 'tick';
    tick.innerHTML = pending?.value === o.v ? '<i class="spinner" style="color:var(--fg)"></i>'
                                            : '<svg class="i" aria-hidden="true"><use href="#i-check"/></svg>';
    b.append(wrap, tick);
    b.onclick = () => pickChoice(kind, o.v);
    return b;
  }));
  if (focused !== undefined) $('choiceList').querySelector(`[data-v="${focused}"]`)?.focus();
}

function renderHold() {
  const h = S.hold;
  $('hold').classList.toggle('on', h.open);
  if (!h.open) return;
  const clear = h.intent === 'clear';
  let title, body, cta, glyph = clear ? 'i-bell' : 'i-lock';
  if (h.phase === 'idle') {
    title = clear ? 'Hold to stop the alarm' : 'Hold to open';
    body = 'Press and keep your finger down for a second. It will not happen on a single tap.';
    cta = 'Not now';
  } else if (h.phase === 'holding') {
    title = 'Keep holding'; body = 'Almost there. Let go now and nothing happens.'; cta = 'Cancel';
  } else if (h.phase === 'sending') {
    title = clear ? 'Stopping the alarm' : 'Opening'; body = 'Waiting for the wallet to say it has done it.'; cta = 'Hide';
  } else if (h.phase === 'done') {
    glyph = clear ? 'i-check' : 'i-lock-open';
    title = clear ? 'Alarm off' : 'Open';
    if (clear) body = 'The wallet has stopped beeping. It stays locked.';
    else if (S.status?.latch === 'open') {
      const s = secondsLeftOpen();
      body = s > 0 ? `It locks itself again in ${s} second${s === 1 ? '' : 's'}.` : 'Locking itself again now.';
    } else body = 'Locked again.';
    cta = 'Done';
  } else {
    glyph = 'i-alert'; title = 'Nothing happened'; body = h.error; cta = 'Close';
  }
  $('holdpad').dataset.phase = h.phase;
  $('holdGlyph').setAttribute('href', `#${glyph}`);
  txt('holdTitle', title); txt('holdBody', body); txt('b-holdcta', cta);
  $('holdpad').disabled = h.phase === 'sending' || h.phase === 'done';
}

// The countdowns (relock, lockout, pairing) need a heartbeat; nothing else does.
setInterval(() => {
  const busy = S.status?.latch === 'open' || S.status?.sensor === 'lockout' ||
               S.session?.state === 'pair-wait' || Date.now() < S.ringingUntil + 500;
  if (busy) render();
}, 250);

// ================================================================== hold ===
let holdTimer = null;

function openHold(intent) {
  if (!ready()) return;
  S.hold = { open: true, phase: 'idle', intent, error: null, doneAt: 0 };
  render();
  $('holdpad').focus();
}

function closeHold() {
  clearTimeout(holdTimer);
  S.hold.open = false;
  S.hold.phase = 'idle';
  render();
  $(S.hold.intent === 'clear' ? 'b-alarmstop' : 'b-holdopen')?.focus();
}

function holdFailed(message) {
  S.hold.phase = 'error';
  S.hold.error = message;
  render();
}

async function holdComplete() {
  S.hold.phase = 'sending';
  render();
  try {
    await S.session.request(S.hold.intent === 'clear' ? 'CLEAR' : 'UNLOCK');
    S.hold.phase = 'done';
    S.hold.doneAt = Date.now();
    buzz(30);
  } catch (err) {
    holdFailed(err.message?.startsWith('ERR') ? 'The wallet refused. Pair this phone again from Settings.'
                                              : 'The wallet did not answer. Is it nearby?');
  }
  render();
}

pressAndHold($('holdpad'), {
  onDown: () => {
    if (!['idle', 'error'].includes(S.hold.phase) || !ready()) return;
    S.hold.phase = 'holding';
    render();
    clearTimeout(holdTimer);
    holdTimer = setTimeout(holdComplete, HOLD_MS);
  },
  onUp: () => {
    if (S.hold.phase !== 'holding') return;
    clearTimeout(holdTimer);
    S.hold.phase = 'idle';
    render();
  },
});

// ============================================================== enrolment ==
async function startEnroll() {
  if (!ready()) return;
  S.enroll = { step: 'start', id: null, fail: null };
  go('enroll');
  try {
    const r = await S.session.request('ENROLL');
    if (r.flags[0] === 'start') { S.enroll.id = Number(r.kv.id); if (S.enroll.step === 'start') S.enroll.step = 'place'; }
    else enrollFailed(r.flags[0] === 'busy' ? 'busy' : r.flags[1]);
  } catch (err) {
    enrollFailed(err.line ? 'busy' : 'link');
  }
  render();
}

function onEnroll(line) {
  if (S.screen !== 'enroll') return;
  const [what, why] = line.flags;
  if (what === 'start') S.enroll.id = Number(line.kv.id);
  else if (what === 'place') S.enroll.step = 'place';
  else if (what === 'lift') S.enroll.step = 'lift';
  else if (what === 'again') S.enroll.step = 'again';
  else if (what === 'ok') {
    S.enroll.step = 'ok';
    S.enroll.id = Number(line.kv.id);
    store.fingers.set(walletId(), S.enroll.id, { name: `Finger ${S.enroll.id}`, added: Date.now() });
    buzz(40);
  } else if (what === 'fail') enrollFailed(why);
  render();
}

function enrollFailed(reason) {
  S.enroll.step = 'fail';
  S.enroll.fail = reason;
  render();
}

// =============================================================== fingers ===
async function renameFinger(id) {
  const name = await promptSheet('Name this finger', 'Only this phone sees the name. The wallet just knows it as a slot.',
                                 fingerName(id), 'Left thumb');
  if (!name) return;
  store.fingers.set(walletId(), id, { name });
  fingersSig = '';
  render();
}

async function removeFinger(id) {
  const name = fingerName(id);
  if (!ready()) return;
  if (!await confirmSheet(`Remove ${name}?`, 'It will no longer open the wallet. You can add it again later.', 'Remove', { danger: true })) return;
  try { await S.session.request(`DELETE ${id}`); toast(`${name} removed.`, { icon: 'i-check' }); }
  catch { toast('The wallet did not remove it. Try again.', { icon: 'i-alert' }); }
}

async function removeAll() {
  if (!ready()) return;
  if (!await confirmSheet('Remove every finger?', 'Nothing will open the wallet with a touch until you add a finger again. Your paired phones still can.',
                          'Remove every finger', { danger: true })) return;
  try { await S.session.request('DELETEALL'); toast('Every finger removed.', { icon: 'i-check' }); }
  catch { toast('The wallet did not answer.', { icon: 'i-alert' }); }
}

// ============================================================== settings ===
async function pickChoice(kind, value) {
  if (!ready() || S.pendingChoice) return;
  S.pendingChoice = { kind, value };
  render();
  try {
    for (const cmd of setCommands(kind, value)) await S.session.request(cmd);
  } catch {
    toast('The wallet did not take that. Try again.', { icon: 'i-alert' });
  }
  S.pendingChoice = null;
  render();
}

function position() {
  return new Promise((resolve, reject) => navigator.geolocation
    ? navigator.geolocation.getCurrentPosition(resolve, reject, { enableHighAccuracy: true, timeout: 10000, maximumAge: 30000 })
    : reject(new Error('no geolocation')));
}

async function recordFix(why, { force = false } = {}) {
  if (!force && !store.prefs.get('geo', false)) return false;
  try {
    const p = await position();
    store.lastFix.set({ lat: p.coords.latitude, lon: p.coords.longitude, acc: p.coords.accuracy, ts: Date.now(), why });
    logLine('ev', `position recorded (${why})`);
    render();
    return true;
  } catch (err) {
    logLine('er', `location unavailable: ${err.message}`);
    return false;
  }
}

async function toggleGeo() {
  const on = !store.prefs.get('geo', false);
  if (!on) { store.prefs.set('geo', false); render(); return; }
  if (await recordFix('opt-in', { force: true })) {
    store.prefs.set('geo', true);
    toast('ONYX will remember where you lose the wallet.', { icon: 'i-pin' });
  } else {
    toast('Location is blocked for this site, so ONYX cannot record a position.', { icon: 'i-alert' });
  }
  render();
}

async function forgetWallet() {
  if (!await confirmSheet('Forget this wallet?',
      'The fingers stay on the wallet. This phone forgets its key, the finger names and the last position, and will need pairing again.',
      'Forget it', { danger: true })) return;
  const id = walletId();
  store.keys.delete(id); store.fingers.clear(id); store.lastFix.clear(); store.lastWallet.clear();
  await hangUp();
  detach();
  Object.assign(S, { status: null, settings: {}, fingers: [] });
  go('home');
}

async function unpairAll() {
  if (!ready()) return;
  if (!await confirmSheet('Unpair every phone?',
      'Every paired phone loses its key, this one too. Pairing a phone again will need a finger the wallet knows.',
      'Unpair every phone', { danger: true })) return;
  try {
    await S.session.request('UNPAIR');
    store.keys.delete(walletId());
    toast('Every phone is unpaired.', { icon: 'i-check' });
    await hangUp();
    detach();
    Object.assign(S, { status: null, settings: {}, fingers: [] });
    go('home');
  } catch {
    toast('The wallet did not answer.', { icon: 'i-alert' });
  }
}

// =============================================================== console ===
function logLine(dir, text) {
  const log = $('log');
  const stamp = new Date().toLocaleTimeString(LOCALE, { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const line = document.createElement('div');
  const t = document.createElement('span'); t.className = 't'; t.textContent = `${stamp}  `;
  const b = document.createElement('span');
  b.className = dir === 'tx' ? 'tx' : dir === 'ev' ? 'ev' : dir === 'er' ? 'er'
              : text.startsWith('EVT') ? 'ev' : text.startsWith('ERR') ? 'er' : 'rx';
  // The console is for reading over a shoulder; a pairing key is not.
  const shown = text.replace(/\bkey=[0-9a-f]+/i, 'key=•••• (kept on this phone)');
  b.textContent = dir === 'tx' ? `> ${shown}` : shown;
  line.append(t, b);
  const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
  log.append(line);
  while (log.childElementCount > 400) log.firstChild.remove();
  if (atBottom) log.scrollTop = log.scrollHeight;
}

function sendConsole(cmd) {
  if (!cmd) return;
  if (!S.session || ['closed', 'idle', 'reconnecting', 'connecting'].includes(S.session.state)) {
    logLine('er', 'not connected');
    return;
  }
  S.session.request(cmd).catch(err => logLine('er', err.message));
}

function openConsole() {
  S.consoleFrom = S.choice ? 'settings' : S.screen;
  go('console');
  const log = $('log');
  log.scrollTop = log.scrollHeight;
}

// ================================================================ wiring ===
$('pill').onclick = async () => {
  const conn = S.session?.state;
  if (!S.session) { if (webBluetoothAvailable()) connectWallet(); else enterDemo(); return; }
  if (conn === 'closed' || conn === 'idle') { connectWallet(); return; }
  if (conn === 'ready' || conn === 'reconnecting') {
    const r = await sheet({
      title: conn === 'ready' ? (S.demo ? 'Connected to the demo wallet' : 'Connected to your wallet') : 'Looking for your wallet',
      body: conn === 'ready' ? 'Disconnecting tells the wallet you are hanging up on purpose, so it does not sound the walk-away alarm.'
                             : 'ONYX reconnects by itself when the wallet is back in range.',
      actions: [
        { label: conn === 'ready' ? 'Disconnect' : 'Stop looking', action: 'hang', kind: 'primary' },
        { label: 'Wallet console', action: 'console', kind: 'light' },
        { label: 'Cancel', action: 'cancel', kind: 'alt' },
      ],
    });
    if (r?.action === 'hang') S.demo ? exitDemo() : hangUp();
    if (r?.action === 'console') openConsole();
  }
};

$('b-pair').onclick = connectWallet;
$('b-demo').onclick = enterDemo;
$('b-exitdemo').onclick = exitDemo;
$('b-seemap').onclick = () => go('find');
$('b-gofingers').onclick = () => go('fingers');
$('b-gosettings').onclick = () => go('settings');
$('b-holdopen').onclick = () => openHold('unlock');
$('b-alarmstop').onclick = () => openHold('clear');
$('b-ring').onclick = async () => {
  if (!ready()) return;
  try {
    await S.session.request('RING');
    S.ringingUntil = Date.now() + 1600;
  } catch { toast('The wallet did not answer.', { icon: 'i-alert' }); }
  render();
};
$('b-closehold').onclick = closeHold;
$('b-holdcta').onclick = () => {
  if (S.hold.phase === 'error' && ready()) { S.hold.phase = 'idle'; render(); return; }
  closeHold();
};

for (const b of document.querySelectorAll('.nav-home')) b.onclick = () => go('home');
for (const b of document.querySelectorAll('[data-choice]')) b.onclick = () => { S.choice = b.dataset.choice; choiceSig = ''; render(); window.scrollTo(0, 0); };
$('b-backsettings').onclick = () => { S.choice = null; render(); };

$('b-pairback').onclick = $('pairAlt').onclick = async () => {
  const done = S.pair.phase === 'done';
  if (!done && S.session && S.session.state !== 'ready') { await hangUp(); if (S.demo) await exitDemo(); }
  go('home');
};
$('pairCta').onclick = () => {
  if (S.pair.phase === 'done') { if (S.status?.fingers) go('home'); else startEnroll(); return; }
  if (S.session?.state === 'unpaired') beginPairing();
  else if (S.demo) enterDemo();
  else connectWallet();
};

$('b-maps').onclick = () => {
  const f = store.lastFix.get();
  if (f) window.open(`https://www.openstreetmap.org/?mlat=${f.lat}&mlon=${f.lon}#map=18/${f.lat}/${f.lon}`, '_blank', 'noopener');
};
$('b-updatefix').onclick = async () => {
  txt('fixBtnLabel', 'Finding you…');
  const ok = await recordFix('manual', { force: true });
  txt('fixBtnLabel', ok ? 'Position updated' : 'Location unavailable');
  setTimeout(() => txt('fixBtnLabel', 'Record where I am now'), 2500);
};

$('b-goenroll').onclick = startEnroll;
$('b-removeall').onclick = removeAll;
$('enrollCta').onclick = startEnroll;
$('enrollAlt').onclick = () => {
  if (['start', 'place', 'lift', 'again'].includes(S.enroll.step) && ready()) S.session.request('CANCEL').catch(() => {});
  S.enroll = { step: 'idle', id: null, fail: null };
  go('fingers');
};

$('b-geo').onclick = toggleGeo;
$('b-goconsole').onclick = openConsole;
$('b-forget').onclick = forgetWallet;
$('b-unpair').onclick = unpairAll;
$('b-consoleback').onclick = back;
$('consoleForm').onsubmit = e => { e.preventDefault(); const i = $('consoleCmd'); sendConsole(i.value.trim()); i.value = ''; };
for (const b of document.querySelectorAll('[data-cmd]')) b.onclick = () => sendConsole(b.dataset.cmd);
$('b-clearlog').onclick = () => $('log').replaceChildren();
longPress($('wordmark'), 600, openConsole);

document.addEventListener('keydown', e => {
  if (e.key !== 'Escape' || document.querySelector('#sheetHost .sheet')) return;
  if (S.hold.open) closeHold(); else back();
});

// ================================================================== boot ===
if (params.get('theme') === 'dark' || params.get('theme') === 'light') document.documentElement.dataset.theme = params.get('theme');

const veil = $('veil');
if (store.session.seenVeil() || params.has('demo') || matchMedia('(prefers-reduced-motion: reduce)').matches) veil.classList.add('quick');
store.session.markVeil();
veil.addEventListener('animationend', e => { if (e.animationName === 'veil') veil.classList.add('gone'); });
setTimeout(() => veil.classList.add('gone'), 2000);

if ('serviceWorker' in navigator && isSecureContext && !params.has('demo')) {
  navigator.serviceWorker.register('sw.js').catch(() => { /* offline support is a bonus */ });
}

render();
logLine('ev', 'ready - pair a wallet, or try the demo');
if (params.has('demo')) enterDemo();
else resume();
