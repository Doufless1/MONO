/* ============================================================================
   demo.js  -  a wallet to try without owning one
   ----------------------------------------------------------------------------
   Loads sim/wallet-sim.js: the real mono_firmware.ino, compiled to
   WebAssembly, on the same virtual hardware the C++ test suite uses. The app
   talks to it through a link with the same shape as the Bluetooth one, so the
   demo exercises every line of the real app - pairing included.

   The bench along the bottom stands in for the physical world: press and
   hold to rest a finger on the sensor, shake it, carry the phone out of range.

   When the app runs inside the project page (site/), that page owns the
   simulator - it draws the wallet in 3D next to the phone - and hands it over
   as window.parent.MONO_DEMO. The app then skips its own bench.
   ========================================================================= */

import { pressAndHold } from './ui.js';

let sim = null, link = null, hosted = false, bench = null, onTick = null, benchResize = null;

function hostDemo() {
  try { return window.parent !== window ? window.parent.MONO_DEMO ?? null : null; }
  catch { return null; }                       // cross-origin parent: not ours
}

export async function startDemo() {
  const host = hostDemo();
  if (host) {
    ({ sim, link } = host);
    hosted = true;
    return { sim, link, hosted };
  }
  const { WalletSim, simLink } = await import('../../sim/wallet-sim.js');
  sim = await WalletSim.load();
  sim.boot();
  sim.start();
  link = simLink(sim);
  hosted = false;
  mountBench();
  return { sim, link, hosted };
}

export function stopDemo() {
  if (!hosted) sim?.stop();
  if (onTick) sim?.removeEventListener('tick', onTick);
  benchResize?.disconnect();
  bench?.remove();
  document.body.classList.remove('has-bench');
  document.body.style.removeProperty('--bench-h');
  sim = link = bench = onTick = null;
}

// Fingers have identities (sim/bench.h): 1 is yours, 2 is one other person's.
// Someone else is turned away until you enrol their finger - then it opens,
// exactly as a partner's finger would on the real wallet.
const KEYS = [
  { id: 'owner',    icon: 'i-finger', label: 'Your finger',     down: '!touch 1', up: '!lift' },
  { id: 'other',    icon: 'i-print',  label: 'Someone else',    down: '!touch 2', up: '!lift' },
  { id: 'shake',    icon: 'i-shake',  label: 'Shake it',        down: '!shake on', up: '!shake off' },
];

function mountBench() {
  bench = document.createElement('div');
  bench.className = 'bench';
  bench.setAttribute('role', 'region');
  bench.setAttribute('aria-label', 'The demo wallet in your hand');
  bench.innerHTML = `
    <div class="bench-head">
      <span class="label">The wallet in your hand</span>
      <span class="bench-state" aria-live="off">
        <i class="led" id="benchLed" title="Status LED"></i>
        <svg class="i buzz" id="benchBuzz" aria-label="Buzzer"><use href="#i-bell"/></svg>
        <span id="benchLatch">Locked</span>
      </span>
    </div>
    <div class="bench-grid"></div>`;
  const grid = bench.querySelector('.bench-grid');

  for (const k of KEYS) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'bench-key';
    b.setAttribute('aria-pressed', 'false');
    b.innerHTML = `<svg class="i" aria-hidden="true"><use href="#${k.icon}"/></svg><span>${k.label}</span>`;
    b.title = 'Press and hold';
    pressAndHold(b, {
      onDown: () => { sim.bench(k.down); b.setAttribute('aria-pressed', 'true'); },
      onUp:   () => { sim.bench(k.up);   b.setAttribute('aria-pressed', 'false'); },
    });
    grid.append(b);
  }

  const away = document.createElement('button');
  away.type = 'button';
  away.className = 'bench-key';
  away.setAttribute('aria-pressed', 'false');
  away.innerHTML = '<svg class="i" aria-hidden="true"><use href="#i-away"/></svg><span>Walk away</span>';
  away.onclick = () => {
    const gone = away.getAttribute('aria-pressed') !== 'true';
    if (gone) link.walkAway(); else link.comeBack();
    away.setAttribute('aria-pressed', String(gone));
    away.querySelector('span').textContent = gone ? 'Come back' : 'Walk away';
  };
  grid.append(away);

  document.getElementById('benchHost').append(bench);
  document.body.classList.add('has-bench');
  // Publish the bench's real height, so screens, sheets and toasts clear it exactly.
  benchResize = new ResizeObserver(() => document.body.style.setProperty('--bench-h', `${bench.offsetHeight}px`));
  benchResize.observe(bench);

  // Mirror what the wallet's own LED, buzzer and latch are doing.
  const led = bench.querySelector('#benchLed'), buzz = bench.querySelector('#benchBuzz');
  const latch = bench.querySelector('#benchLatch');
  let last = 0;
  onTick = () => {
    const now = performance.now();
    if (now - last < 60) return;
    last = now;
    const p = sim.peek();
    led.classList.toggle('on', p.led === 1);
    buzz.classList.toggle('on', p.buzz === 1);
    latch.textContent = p.alarm ? 'Alarm' : p.latch === 'open' ? 'Open' : 'Locked';
  };
  sim.addEventListener('tick', onTick);
}
