/* ============================================================================
   session.js  -  one conversation with one wallet
   ----------------------------------------------------------------------------
   Sits between the screens and a link (Web Bluetooth or the simulator) and
   owns everything about talking to the wallet:

     - the handshake: HELLO, then AUTH with this phone's key, or PAIR
     - matching each request to its reply (every reply starts with the
       command's name), with a timeout, so a screen can await the answer and
       say "Opening..." until the wallet has really opened. v2 announced
       "Open" whether or not the command was even sent.
     - the wallet's pushed STATUS, its events and enrolment steps, re-emitted
       as DOM events
     - reconnecting with backoff when the radio drops, and never after the
       user hung up

   No DOM in here. tests/app/session.test.mjs drives it against the real
   firmware compiled to WebAssembly.

   States:  idle -> connecting -> handshake -> unpaired -> pair-wait -> ready
                                                         \______________/
            ready -> reconnecting -> handshake -> ready        closed
   ========================================================================= */

import { PROTOCOL_VERSION, parseLine, parseStatus, parseSettings, parseList, parseHello } from './protocol.js';
import { authAnswer } from './crypto.js';

export class WalletError extends Error {
  constructor(message, line = null) {
    super(message);
    this.name = 'WalletError';
    this.line = line;
  }
}

export class PairError extends Error {
  constructor(reason) {
    super(`pairing failed: ${reason}`);
    this.name = 'PairError';
    this.reason = reason;
  }
}

/* Every timer the session uses goes through a clock, so a test can run the
   app on the wallet's virtual time instead of racing it against the wall. */
export const realClock = Object.freeze({
  now: () => Date.now(),
  set: (fn, ms) => setTimeout(fn, ms),
  clear: handle => clearTimeout(handle),
});

export class WalletSession extends EventTarget {
  #pending = [];          // requests waiting for their reply, oldest first
  #waiters = [];          // one-off "tell me when a line like this arrives"
  #userClosed = false;

  /**
   * @param link   simLink() or bleLink(): { id, open, write, close }
   * @param keys   { get(id), set(id, key), delete(id) } - where this phone keeps its keys
   * @param clock  { now, set, clear } - realClock unless a test says otherwise
   */
  constructor({ link, keys, clock = realClock, timeoutMs = 4000, retry = { first: 1000, max: 30000 } }) {
    super();
    this.link = link;
    this.keys = keys;
    this.clock = clock;
    this.timeoutMs = timeoutMs;
    this.retry = retry;
    this.state = 'idle';
    this.hello = null;
    this.status = null;
    this.settings = {};
    this.fingers = [];
    this.pairDeadline = 0;
  }

  get ready() { return this.state === 'ready'; }

  /** Open the link and say hello. Ends 'ready' (we hold a key) or 'unpaired'. */
  async connect() {
    this.#userClosed = false;
    this.#setState('connecting');
    try {
      await this.#open();
      await this.#handshake();
    } catch (err) {
      if (this.state !== 'closed' && this.state !== 'reconnecting') this.#setState('idle');
      throw err;
    }
  }

  /** Ask to be paired. Resolves once trusted; may wait for the owner's touch. */
  async pair() {
    if (this.state !== 'unpaired') throw new Error(`cannot pair from state ${this.state}`);
    let result = await this.request('PAIR');
    if (result.flags[0] === 'wait') {
      this.pairDeadline = this.clock.now() + 20000;
      this.#setState('pair-wait');
      result = await this.#waitFor(line => line.head === 'PAIR', 25000);
    }
    if (result.flags[0] !== 'ok') {
      if (this.state === 'pair-wait') this.#setState('unpaired');
      throw new PairError(result.flags[1] ?? 'unknown');
    }
    this.keys.set(this.link.id, result.kv.key);
    await this.#becomeReady();
  }

  /** Hang up on purpose: say goodbye first so the wallet does not cry "lost". */
  async disconnect() {
    this.#userClosed = true;
    if (this.state === 'ready') {
      try { await this.request('BYE', { timeout: 1500 }); } catch { /* going anyway */ }
    }
    this.#setState('closed');
    await this.link.close();
  }

  /** Send one command and resolve with its parsed reply; rejects on ERR or timeout. */
  request(command, { timeout = this.timeoutMs } = {}) {
    const cmd = command.split(' ')[0].toUpperCase();
    return new Promise((resolve, reject) => {
      const entry = { cmd, resolve, reject, timer: null };
      entry.timer = this.clock.set(() => {
        this.#pending = this.#pending.filter(p => p !== entry);
        reject(new WalletError(`The wallet did not answer ${cmd}.`));
      }, timeout);
      this.#pending.push(entry);
      this.#emit('line', { dir: 'tx', text: command });
      this.link.write(command).catch(err => {
        this.clock.clear(entry.timer);
        this.#pending = this.#pending.filter(p => p !== entry);
        reject(err);
      });
    });
  }

  refreshSettings() { return this.request('GET'); }
  refreshFingers()  { return this.request('LIST'); }

  // ---------------------------------------------------------------- inside --
  async #open() {
    await this.link.open({
      onLine: text => this.#onLine(text),
      onClose: reason => this.#onClose(reason),
    });
  }

  async #handshake() {
    this.#setState('handshake');
    const hello = parseHello((await this.request('HELLO')).kv);
    this.hello = hello;
    if (hello.version !== PROTOCOL_VERSION) {
      throw new WalletError(`This wallet speaks protocol ${hello.version} and the app speaks ` +
                            `${PROTOCOL_VERSION}. Flash the matching firmware.`);
    }
    const key = this.keys.get(this.link.id);
    if (key && hello.nonce) {
      const reply = await this.request('AUTH ' + await authAnswer(key, hello.nonce));
      if (reply.flags[0] === 'ok') return this.#becomeReady();
      this.keys.delete(this.link.id);          // the wallet no longer knows this phone
      this.#emit('forgotten');
    }
    this.#setState('unpaired');
  }

  async #becomeReady() {
    this.#setState('ready');
    await Promise.all([this.request('GET'), this.request('LIST'), this.request('STATUS')]);
  }

  #onLine(text) {
    const line = parseLine(text);
    this.#emit('line', { dir: 'rx', text });
    this.#apply(line);

    if (line.head === 'ERR') {
      // "ERR auth STATUS", "ERR unknown FOO" name the command; "ERR toolong" does not.
      const target = line.flags[1];
      const i = this.#pending.findIndex(p => !target || p.cmd === target);
      if (i >= 0) this.#settle(i, 'reject', new WalletError(text, line));
      if (line.flags[0] === 'auth' && this.state === 'ready') this.#emit('forgotten');
    } else {
      const i = this.#pending.findIndex(p => p.cmd === line.head);
      if (i >= 0) this.#settle(i, 'resolve', line);
    }

    for (const w of [...this.#waiters]) {
      if (w.test(line)) {
        this.clock.clear(w.timer);
        this.#waiters = this.#waiters.filter(x => x !== w);
        w.resolve(line);
      }
    }
  }

  // Everything the wallet says updates what we know, asked for or not.
  #apply(line) {
    const ok = line.flags[0] === 'ok';
    switch (line.head) {
      case 'STATUS':
        this.status = parseStatus(line.kv);
        this.#emit('status', this.status);
        break;
      case 'GET':
        this.settings = { ...this.settings, ...parseSettings(line.kv) };
        this.#emit('settings', this.settings);
        break;
      case 'SET':
        if (ok) {
          this.settings = { ...this.settings, ...parseSettings(line.kv) };
          this.#emit('settings', this.settings);
        }
        break;
      case 'LIST':
        this.fingers = parseList(line.kv);
        this.#emit('fingers', this.fingers);
        break;
      case 'ENROLL':
        this.#emit('enroll', line);
        if (ok) this.refreshFingers().catch(() => {});
        break;
      case 'DELETE':
      case 'DELETEALL':
        if (ok) this.refreshFingers().catch(() => {});
        break;
      case 'EVT':
        this.#emit('event', line);
        break;
    }
  }

  #onClose(reason) {
    for (const p of this.#pending) { this.clock.clear(p.timer); p.reject(new WalletError('The connection dropped.')); }
    for (const w of this.#waiters) { this.clock.clear(w.timer); w.reject(new WalletError('The connection dropped.')); }
    this.#pending = [];
    this.#waiters = [];
    if (this.#userClosed || reason === 'closed') { this.#setState('closed'); return; }
    this.#setState('reconnecting');
    this.#reconnect();
  }

  async #reconnect() {
    let wait = this.retry.first;
    while (this.state === 'reconnecting') {
      await new Promise(resolve => this.clock.set(resolve, wait));
      if (this.state !== 'reconnecting') return;
      try {
        await this.#open();
        await this.#handshake();
        return;
      } catch {
        if (this.#userClosed) return;
        this.#setState('reconnecting');
        wait = Math.min(wait * 2, this.retry.max);
      }
    }
  }

  #waitFor(test, timeout) {
    return new Promise((resolve, reject) => {
      const w = { test, resolve, reject, timer: null };
      w.timer = this.clock.set(() => {
        this.#waiters = this.#waiters.filter(x => x !== w);
        reject(new WalletError('The wallet did not answer in time.'));
      }, timeout);
      this.#waiters.push(w);
    });
  }

  #settle(i, how, value) {
    const [p] = this.#pending.splice(i, 1);
    this.clock.clear(p.timer);
    p[how](value);
  }

  #setState(state) {
    if (this.state === state) return;
    this.state = state;
    this.#emit('state', state);
  }

  #emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }
}
