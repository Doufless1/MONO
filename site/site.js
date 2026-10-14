/* ============================================================================
   site.js  -  the project page: one simulated wallet, three ways to see it
   ----------------------------------------------------------------------------
   Boots sim/mono_wallet.wasm (the real firmware) once, then shares it:
     - with the ONYX app in the phone frame, as window.MONO_DEMO, so the app
       pairs with it exactly as it would with a wallet over Web Bluetooth
     - with the 3D twin, which draws the wallet from the firmware's outputs
     - with the inspector: the firmware's state and every line on the air
   ========================================================================= */

import { WalletSim, simLink } from '../sim/wallet-sim.js';
import { pressAndHold } from '../mono_app/js/ui.js';

const $ = id => document.getElementById(id);
let sim = null, link = null, twin = null, started = false;

// ================================================================== demo ===
async function startDemo() {
  if (started) return;
  started = true;
  const start = $('startDemo');
  start.querySelector('b').textContent = 'Booting the wallet…';
  try {
    sim = await WalletSim.load();
  } catch (err) {
    start.querySelector('b').textContent = 'The wallet would not load';
    start.querySelector('span').textContent = String(err.message || err);
    started = false;
    return;
  }
  sim.boot();
  sim.start();
  link = simLink(sim);

  // Everything the phone sends goes through the link; everything the wallet
  // sends to the phone comes back as a 'ble' event. Both go on the trace.
  const write = link.write.bind(link);
  link.write = async line => { trace('tx', line); return write(line); };
  sim.addEventListener('ble', e => trace('rx', e.detail));

  window.MONO_DEMO = { sim, link };          // the app in the iframe picks this up
  $('appFrame').src = 'mono_app/?demo';
  $('appFrame').addEventListener('load', () => { start.hidden = true; }, { once: true });

  bench();
  let last = 0;
  sim.addEventListener('tick', () => {
    const now = performance.now();
    if (now - last < 33) return;
    last = now;
    const p = sim.peek();
    twin?.update(p);
    hud(p);
    inspect(p);
  });
  loadTwin();
}

// A stalled download must not leave an empty box: after 25 s, or on any
// error (no WebGL, the CDN unreachable), show the studio render instead.
const within = (ms, promise) => Promise.race([promise,
  new Promise((_, reject) => setTimeout(() => reject(new Error(`timed out after ${ms / 1000} s`)), ms))]);

async function loadTwin() {
  const hint = $('twinHint');
  hint.textContent = 'Loading the 3D model…';
  try {
    const { createTwin } = await within(25000, import('./twin.js'));
    twin = await within(25000, createTwin($('twin'), {
      url: 'cad/reference/mono-wallet_1.glb',
      onPress: () => sim?.bench('!touch 1'),
      onRelease: () => sim?.bench('!lift'),
    }));
    hint.textContent = 'Drag to turn it. Press and hold the sensor to touch it with your finger.';
  } catch (err) {
    console.warn('3D twin unavailable:', err);
    const img = Object.assign(document.createElement('img'), {
      src: 'docs/images/concept.jpg', alt: 'The MONO concept model',
      style: 'position:absolute;inset:0;width:100%;height:100%;object-fit:cover' });
    $('twin').replaceWith(img);
    hint.textContent = 'The 3D view could not load here; the wallet is still running - watch the inspector.';
  }
}

// ================================================================= bench ===
function bench() {
  for (const key of document.querySelectorAll('.key[data-hold]')) {
    const [down, up] = key.dataset.hold.split('|');
    pressAndHold(key, {
      onDown: () => { sim?.bench(down); key.classList.add('down'); },
      onUp: () => { sim?.bench(up); key.classList.remove('down'); },
    });
  }
  const walk = $('walk');
  walk.onclick = () => {
    const away = walk.getAttribute('aria-pressed') !== 'true';
    if (away) link.walkAway(); else link.comeBack();
    walk.setAttribute('aria-pressed', String(away));
    walk.querySelector('b').textContent = away ? 'Come back' : 'Walk away';
    walk.querySelector('span').textContent = away ? 'the app keeps looking' : 'take the phone with you';
  };
  const batt = $('batt');
  batt.oninput = () => {
    $('battOut').textContent = `${Number(batt.value).toFixed(2)} V`;
    sim?.bench(`!batt ${batt.value}`);
  };
}

// ============================================================ hud & state ===
function hud(p) {
  const latch = $('hudLatch');
  latch.textContent = p.latch === 'open' ? 'Open' : p.sensor === 'lockout' ? 'Locked out' : 'Locked';
  latch.classList.toggle('open', p.latch === 'open');
  const ms = Math.round(p.adv * 0.625);
  $('hudRadio').textContent = p.conn ? `Connected to the phone${p.auth ? ', trusted' : ''}` : `Advertising every ${ms} ms`;
  $('hudAlarm').hidden = !p.alarm;
  $('twinBox').classList.toggle('alarm', p.alarm === 1);
}

const ROWS = [
  ['Latch', p => `${p.latch} · servo ${p.servo}°`],
  ['Sensor', p => p.sensor + (p.fails ? ` · ${p.fails} wrong` : '')],
  ['Enrolment', p => p.enroll],
  ['Alarm', p => (p.alarm ? 'LOCKDOWN' : 'off')],
  ['Fingers', p => String(p.fingers)],
  ['Phones paired', p => String(p.phones)],
  ['Phone link', p => (!p.conn ? 'none' : p.auth ? 'connected, trusted' : 'connected, not trusted')],
  ['Walk-away leash', p => p.leash],
  ['Radio', p => `${Math.round(p.adv * 0.625)} ms${p.connectable ? '' : ' · Find My'}`],
  ['LED · buzzer', p => `${p.led ? 'on' : 'off'} · ${p.buzz ? p.hz + ' Hz' : 'quiet'}`],
  ['Battery', p => `${p.vbat} V`],
  ['Uptime', p => { const s = Math.floor(p.ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; }],
];
const cells = new Map();

function inspect(p) {
  const dl = $('state');
  if (!cells.size) {
    for (const [name] of ROWS) {
      const dt = document.createElement('dt'), dd = document.createElement('dd');
      dt.textContent = name;
      dl.append(dt, dd);
      cells.set(name, dd);
    }
  }
  for (const [name, fmt] of ROWS) {
    const dd = cells.get(name), v = fmt(p);
    if (dd.textContent === v) continue;
    const first = dd.textContent === '';
    dd.textContent = v;
    if (!first && name !== 'Uptime') {
      dd.classList.add('changed');
      clearTimeout(dd._t);
      dd._t = setTimeout(() => dd.classList.remove('changed'), 900);
    }
  }
}

function trace(dir, text) {
  const list = $('trace');
  const li = document.createElement('li');
  li.dataset.dir = dir === 'tx' ? '→' : '←';
  li.className = dir === 'tx' ? 'tx' : text.startsWith('EVT') ? 'evt' : text.startsWith('ERR') ? 'err' : '';
  // a pairing key is shown once, on purpose, so you can see it travel - and redacted after
  li.textContent = text.replace(/\bkey=([0-9a-f]{6})[0-9a-f]+/i, 'key=$1…');
  const atBottom = list.scrollTop + list.clientHeight >= list.scrollHeight - 12;
  list.append(li);
  while (list.childElementCount > 220) list.firstChild.remove();
  if (atBottom) list.scrollTop = list.scrollHeight;
}

// =================================================================== cad ===
let cadReady = false;
async function startCad() {
  if (cadReady) return;
  cadReady = true;
  try {
    const { createCadViewer } = await import('./cadview.js');
    const viewer = await createCadViewer($('cadView'), {
      base: 'cad/assembly', slider: $('explode'), onHover: t => { $('partLabel').textContent = t; },
    });
    for (const b of document.querySelectorAll('[data-model]')) {
      b.onclick = async () => {
        for (const x of document.querySelectorAll('[data-model]')) x.setAttribute('aria-checked', String(x === b));
        await viewer.show(b.dataset.model);
      };
    }
  } catch (err) {
    console.error(err);
    $('cadView').replaceWith(Object.assign(document.createElement('img'), {
      src: 'docs/images/model_b_open.jpg', alt: 'The works model, exploded', style: 'width:100%;height:100%;object-fit:cover' }));
  }
}

// ================================================================== boot ===
$('startDemo').onclick = startDemo;
const lazy = (el, fn) => new IntersectionObserver((entries, io) => {
  if (entries.some(e => e.isIntersecting)) { io.disconnect(); fn(); }
}, { rootMargin: '300px' }).observe(el);
lazy($('demo'), startDemo);
lazy($('cad'), startCad);
