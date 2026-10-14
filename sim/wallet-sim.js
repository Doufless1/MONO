/* ============================================================================
   wallet-sim.js  -  the MONO wallet, running in a browser tab or in Node
   ----------------------------------------------------------------------------
   Loads mono_wallet.wasm (the real firmware on the same virtual hardware the
   C++ test suite uses), gives it a clock and a physical world, and offers the
   companion app a link to it with exactly the shape of the Web Bluetooth one.

     const sim = await WalletSim.load();        // browser: fetches ./mono_wallet.wasm
     sim.boot(); sim.start();                   // power on, run in real time
     sim.bench('!touch 1');                     // a finger on the sensor
     sim.addEventListener('ble', e => ...)      // a line the wallet sent the phone

   No dependencies, no build step. Node 20+ runs it as is (pass the bytes to
   load(), since Node's fetch cannot read files).
   ========================================================================= */

const ENOSYS = 52;

// The firmware never touches a file; libc only needs these three to link.
function wasiImports(memory) {
  const decoder = new TextDecoder();
  return new Proxy({
    fd_write(fd, iovs, iovsLen, nwrittenPtr) {
      const view = new DataView(memory().buffer);
      let written = 0, text = '';
      for (let i = 0; i < iovsLen; i++) {
        const ptr = view.getUint32(iovs + i * 8, true), len = view.getUint32(iovs + i * 8 + 4, true);
        text += decoder.decode(new Uint8Array(memory().buffer, ptr, len));
        written += len;
      }
      view.setUint32(nwrittenPtr, written, true);
      if (text.trim()) console.debug('[mono wasm]', text.trim());
      return 0;
    },
    fd_close: () => 0,
    fd_seek: () => 70,                         // ESPIPE: not seekable
    proc_exit(code) { throw new Error(`mono wasm exited (${code})`); },
  }, { get: (target, name) => target[name] ?? (() => ENOSYS) });
}

function parsePeek(line) {
  const out = {};
  for (const word of line.split(' ').slice(1)) {
    const i = word.indexOf('=');
    if (i < 0) continue;
    const k = word.slice(0, i), v = word.slice(i + 1);
    out[k] = /^-?\d+(\.\d+)?$/.test(v) ? Number(v) : v;
  }
  return out;
}

export class WalletSim extends EventTarget {
  /** @param source URL of the .wasm (browser) or its bytes (Node) */
  static async load(source = new URL('./mono_wallet.wasm', import.meta.url)) {
    const bytes = (source instanceof ArrayBuffer || ArrayBuffer.isView(source))
      ? source
      : await (await fetch(source)).arrayBuffer();
    let instance;
    const memory = () => instance.exports.memory;
    ({ instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasiImports(memory) }));
    instance.exports._initialize?.();
    return new WalletSim(instance.exports);
  }

  constructor(exports) {
    super();
    this.x = exports;
    this.enc = new TextEncoder();
    this.dec = new TextDecoder();
    this.timer = null;
    this.epoch = 0;
    this.booted = false;
  }

  /** Power on. A random seed makes every session pair with a different key. */
  boot(seed = globalThis.crypto.getRandomValues(new Uint32Array(2))) {
    this.x.sim_boot(seed[0] >>> 0, seed[1] >>> 0);
    this.booted = true;
    this.#drain();
    this.epoch = performance.now() - this.now;
    this.dispatchEvent(new CustomEvent('boot'));
  }

  /** The wallet's own clock, in milliseconds since power-on. */
  get now() { return this.x.sim_now() >>> 0; }

  /** Run the firmware until its clock reads `ms`. */
  run(ms) {
    this.x.sim_run(Math.max(0, Math.floor(ms)) >>> 0);
    this.#drain();
    this.dispatchEvent(new CustomEvent('tick'));
  }

  /** Run the firmware for another `ms` of virtual time. */
  advance(ms) { this.run(this.now + ms); }

  /** Keep the wallet's clock in step with the wall clock. */
  start(everyMs = 20) {
    if (this.timer) return;
    this.epoch = performance.now() - this.now;
    this.timer = setInterval(() => this.run(performance.now() - this.epoch), everyMs);
  }

  stop() { clearInterval(this.timer); this.timer = null; }

  /** A line from the phone, over BLE. Answered on the next run(). */
  phone(line) { this.#write(line); this.x.sim_phone(); }

  /** A line typed on the USB console. */
  usb(line) { this.#write(line); this.x.sim_usb(); }

  /** The physical world: '!touch 1', '!shake on', '!link 0' ... (sim/bench.h). */
  bench(command) {
    this.#write(command.startsWith('!') ? command : '!' + command);
    this.x.sim_bench();
    this.#drain();
  }

  /** What a digital twin needs to draw the wallet: servo angle, LED, buzzer, radio ... */
  peek() { return parsePeek(this.#cstring(this.x.sim_peek())); }

  #write(text) {
    const bytes = this.enc.encode(text);
    const size = this.x.sim_inbox_size();
    if (bytes.length >= size) throw new RangeError(`line longer than ${size - 1} bytes`);
    const inbox = new Uint8Array(this.x.memory.buffer, this.x.sim_inbox(), size);
    inbox.set(bytes);
    inbox[bytes.length] = 0;
  }

  #cstring(ptr) {
    const mem = new Uint8Array(this.x.memory.buffer);
    let end = ptr;
    while (mem[end]) end++;
    return this.dec.decode(mem.subarray(ptr, end));
  }

  #drain() {
    const text = this.#cstring(this.x.sim_outbox());
    if (!text) return;
    this.x.sim_outbox_clear();
    for (const line of text.split('\n')) {
      if (!line) continue;
      const type = { B: 'ble', U: 'usb', S: 'bench' }[line[0]] ?? 'usb';
      this.dispatchEvent(new CustomEvent(type, { detail: line.slice(2) }));
    }
  }
}

/* A link to the simulated wallet with the same shape as the app's Web
   Bluetooth link (mono_app/js/transport.js): open / write / close, lines in,
   one close callback saying why. It also models radio range, so the app's
   reconnect logic meets the same "out of range" a real walk would cause. */
export function simLink(sim) {
  let handlers = null;
  let inRange = true;
  const onLine = e => handlers?.onLine(e.detail);

  const drop = reason => {
    const h = handlers;
    handlers = null;
    sim.removeEventListener('ble', onLine);
    h?.onClose(reason);
  };

  return {
    kind: 'sim',
    id: 'simulated-mono',
    name: 'MONO (simulated)',
    get connected() { return !!handlers; },
    get inRange() { return inRange; },

    async open(h) {
      if (!inRange) throw new Error('The wallet is out of range');
      if (handlers) return;
      handlers = h;
      sim.addEventListener('ble', onLine);
      sim.bench('!link 1');
    },

    async write(line) {
      if (!handlers) throw new Error('Not connected');
      sim.phone(line);
    },

    /** The app hung up. */
    async close() {
      if (!handlers) return;
      sim.bench('!link bye');
      drop('closed');
    },

    /** The bench: carry the phone out of range, and back. */
    walkAway() {
      inRange = false;
      if (handlers) { sim.bench('!link 0'); drop('lost'); }
    },
    comeBack() { inRange = true; },
  };
}
