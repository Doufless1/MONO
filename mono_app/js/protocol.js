/* ============================================================================
   protocol.js  -  the MONO line protocol, as pure functions
   ----------------------------------------------------------------------------
   Everything here is data in, data out: no DOM, no Bluetooth, no storage, so
   it is unit-tested in Node (tests/app/protocol.test.mjs). The wire format
   itself is documented in docs/PROTOCOL.md.
   ========================================================================= */

/** The Nordic UART service the firmware's BLEUart exposes. */
export const NUS = Object.freeze({
  service: '6e400001-b5a3-f393-e0a9-e50e24dcca9e',
  rx:      '6e400002-b5a3-f393-e0a9-e50e24dcca9e',   // phone -> wallet
  tx:      '6e400003-b5a3-f393-e0a9-e50e24dcca9e',   // wallet -> phone
});

export const PROTOCOL_VERSION = 3;

/**
 * "STATUS latch=open fingers=2" -> { head:'STATUS', words:[...], kv:{latch:'open', fingers:'2'}, flags:[] }
 * "ENROLL fail no_match"        -> { head:'ENROLL', flags:['fail','no_match'], ... }
 */
export function parseLine(line) {
  const raw = String(line).trim();
  const sp = raw.indexOf(' ');
  const head = sp < 0 ? raw : raw.slice(0, sp);
  const words = sp < 0 ? [] : raw.slice(sp + 1).split(/\s+/).filter(Boolean);
  const kv = {}, flags = [];
  for (const w of words) {
    const i = w.indexOf('=');
    if (i > 0) kv[w.slice(0, i)] = w.slice(i + 1);
    else flags.push(w);
  }
  return { head, words, kv, flags, raw };
}

const int = v => (v === undefined || v === '' ? null : Number.parseInt(v, 10));
const num = v => (v === undefined || v === '' ? null : Number.parseFloat(v));
const bit = v => v === '1';

/** STATUS -> what the app shows. Unknown fields stay null rather than lying. */
export function parseStatus(kv) {
  return {
    latch:   kv.latch ?? null,          // 'open' | 'locked'
    sensor:  kv.sensor ?? null,         // 'ready' | 'scan' | 'reject' | 'lockout'
    alarm:   bit(kv.alarm),
    enroll:  kv.enroll ?? 'idle',       // 'idle' | 'place' | 'lift' | 'again'
    fingers: int(kv.fingers),
    vbat:    num(kv.vbat),
    authed:  bit(kv.auth),
    pairing: bit(kv.pair),
    setup:   bit(kv.setup),
    leash:   kv.leash ?? null,          // 'off' | 'ok' | 'lost' | 'warn' | 'alert'
  };
}

/** GET -> the wallet's settings, in the wallet's own units. */
export function parseSettings(kv) {
  const out = {};
  for (const k of ['relock', 'tamper', 'sepwarn', 'sepalert', 'maxfails',
                   'servolock', 'servorel', 'findmy', 'phones']) {
    if (kv[k] !== undefined) out[k] = int(kv[k]);
  }
  return out;
}

/** LIST count=2 ids=1,3 -> [1, 3] */
export function parseList(kv) {
  return kv.ids ? kv.ids.split(',').map(Number).filter(Number.isInteger) : [];
}

/** HELLO v=3 fw=3.0.0 phones=1 setup=0 nonce=<hex> */
export function parseHello(kv) {
  return {
    version: int(kv.v),
    firmware: kv.fw ?? null,
    phones: int(kv.phones) ?? 0,
    setup: bit(kv.setup),
    nonce: kv.nonce && kv.nonce !== '-' ? kv.nonce : null,
  };
}

/** What a failure reason means, in words a person would use. */
export const ENROLL_FAILURE = Object.freeze({
  timeout:   'Nothing touched the sensor for 20 seconds.',
  bad_image: 'The sensor could not get a clear picture of that finger. Dry or cold fingers are the usual cause.',
  no_match:  'The two touches did not match. Use the same finger both times.',
  store:     'The wallet could not save the finger. Try again.',
  full:      'The wallet already holds ten fingers. Remove one first.',
  busy:      'The wallet is already adding a finger.',
  nosensor:  'The wallet cannot find its fingerprint sensor.',
  link:      'The connection to the wallet dropped part way through.',
});

export const PAIR_FAILURE = Object.freeze({
  timeout:  'Nobody touched the sensor in time.',
  full:     'This wallet already has four phones. Unpair them from a phone that is already paired.',
  nosensor: 'The wallet cannot find its fingerprint sensor, so it cannot confirm a new phone.',
  rng:      'The wallet could not make a key. Try again.',
  port:     'Pairing has to happen over Bluetooth.',
});

export function describeFailure(table, reason) {
  return table[reason] ?? `The wallet said: ${reason || 'no reason given'}.`;
}

/* ------------------------------------------------------------ battery --
   The firmware reports volts; people read percent. A single-cell lithium
   curve is far from linear near the top, so interpolate a table. */
const CURVE = [[3.30, 0], [3.60, 25], [3.70, 50], [3.85, 75], [4.20, 100]];

export function batteryPercent(volts) {
  if (volts === null || volts === undefined || Number.isNaN(volts)) return null;
  if (volts <= CURVE[0][0]) return 0;
  if (volts >= CURVE.at(-1)[0]) return 100;
  for (let i = 1; i < CURVE.length; i++) {
    const [v0, p0] = CURVE[i - 1], [v1, p1] = CURVE[i];
    if (volts <= v1) return Math.round(p0 + (volts - v0) / (v1 - v0) * (p1 - p0));
  }
  return null;
}

/* ----------------------------------------------------------- settings --
   The design offers presets in human units; the wallet stores numbers in its
   own. Each kind converts both ways, and the screen snaps the wallet's value
   to the nearest preset rather than showing a bare figure. */
export const PRESETS = Object.freeze({
  relock: {
    title: 'Lock itself again',
    body: 'After you open the wallet with your finger, it closes and locks on its own. Choose how long it waits.',
    foot: 'The wallet holds this setting itself, so it works even when your phone is not nearby.',
    options: [
      { v: 3,  label: 'Right away',       short: '3 sec',  note: 'Locks 3 seconds after you open it' },
      { v: 6,  label: 'Normal',           short: 'Normal', note: 'Locks 6 seconds after you open it' },
      { v: 15, label: 'Give me a moment', short: '15 sec', note: 'Locks 15 seconds after you open it' },
      { v: 30, label: 'Take your time',   short: '30 sec', note: 'Locks 30 seconds after you open it' },
    ],
    toWallet: v => ({ relock: v * 1000 }),
    fromWallet: c => (c.relock == null ? null : Math.round(c.relock / 1000)),
  },
  tries: {
    title: 'Wrong-finger tries',
    body: 'If the wrong finger is used too many times in a row, the wallet stops listening for half a minute.',
    foot: 'A wrong reading is common on dry or cold hands, so fewer tries is not always safer. A smudged reading never counts.',
    options: [
      { v: 3,  label: '3 tries',  note: 'Strictest. Waits 30 seconds after the third wrong finger' },
      { v: 5,  label: '5 tries',  note: 'A good balance for most people' },
      { v: 10, label: '10 tries', note: 'Kindest to dry or cold hands' },
    ],
    toWallet: v => ({ maxfails: v }),
    fromWallet: c => c.maxfails ?? null,
  },
  leash: {
    title: 'Tell me if I walk off',
    body: 'If your phone goes out of Bluetooth range, the wallet gives a quiet warning beep, then a full alarm.',
    foot: 'Only a lost connection counts. Closing the app or tapping Disconnect never sets it off.',
    options: [
      { v: 0,   label: 'Never',              short: 'Never',   note: 'No warning when you walk away' },
      { v: 20,  label: 'Straight away',      short: 'At once', note: 'Warns after 20 seconds out of range' },
      { v: 60,  label: 'After a minute',     short: '1 min',   note: 'Warns once you are properly gone' },
      { v: 300, label: 'After five minutes', short: '5 min',   note: 'Only if you really left it behind' },
    ],
    // v2 had to fake "Never" as an hour, because the firmware could not switch
    // the leash off. v3 can: sepwarn=0. The full alarm trails the warning 3x.
    toWallet: v => (v === 0 ? { sepwarn: 0 } : { sepwarn: v, sepalert: Math.min(7200, Math.max(10, v * 3)) }),
    fromWallet: c => c.sepwarn ?? null,
  },
  nudge: {
    title: 'Alarm if it is grabbed',
    body: 'The wallet feels movement. Three sharp knocks in a row and it locks down and beeps until you touch the sensor.',
    foot: 'Higher sensitivity means more false alarms in a bag or on a bike.',
    options: [
      { v: 4000, label: 'Only a real grab', short: 'Real grabs', note: 'Ignores pockets, bags and bumpy roads' },
      { v: 2500, label: 'Normal',           short: 'Normal',     note: 'A firm tug sets off the alarm' },
      { v: 1200, label: 'Very sensitive',   short: 'Sensitive',  note: 'Even a knock sets it off. Expect false alarms' },
    ],
    toWallet: v => ({ tamper: v }),
    fromWallet: c => c.tamper ?? null,
  },
});

/** The preset closest to what the wallet holds, or null before GET arrives. */
export function nearestPreset(kind, settings) {
  const p = PRESETS[kind];
  const cur = p.fromWallet(settings);
  if (cur === null) return null;
  return p.options.reduce((best, o) => (Math.abs(o.v - cur) < Math.abs(best.v - cur) ? o : best));
}

/** SET lines for one preset choice, in the order the firmware wants them. */
export function setCommands(kind, value) {
  return Object.entries(PRESETS[kind].toWallet(value)).map(([k, v]) => `SET ${k} ${v}`);
}

/* --------------------------------------------------------------- time -- */
export function ago(ts, now = Date.now()) {
  const mins = Math.round((now - ts) / 60000);
  if (mins < 1) return 'Just now';
  if (mins === 1) return '1 minute ago';
  if (mins < 60) return `${mins} minutes ago`;
  const hours = Math.round(mins / 60);
  if (hours < 24) return hours === 1 ? '1 hour ago' : `${hours} hours ago`;
  const days = Math.round(hours / 24);
  return days === 1 ? 'Yesterday' : `${days} days ago`;
}
