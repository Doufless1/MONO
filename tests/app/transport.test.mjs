// The Web Bluetooth transport, against a fake GATT stack.   node --test "tests/app/*.test.mjs"
//
// bleLink() is handed a device object, so the browser's Bluetooth stack can be
// stood in for. The fake is as strict as Chrome in the ways that bit v2: one
// GATT operation in flight at a time, notifications delivered as events the
// moment they start, and bytes arriving in whatever pieces the radio chose.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bleLink } from '../../mono_app/js/transport.js';
import { NUS } from '../../mono_app/js/protocol.js';

const bytes = s => new TextEncoder().encode(s);
const quiet = { onLine() {}, onClose() {} };

function fakeWallet({ withoutResponse = false } = {}) {
  const device = new EventTarget();
  const sent = [];                       // every chunk the wallet received, in order
  const f = { device, sent, refused: [], failNext: false, onStart: null };
  let busy = false;

  // Chrome: "GATT operation already in progress" if a write starts mid-write
  const op = async (kind, chunk) => {
    if (busy) {
      const e = new Error('GATT operation already in progress.');
      f.refused.push(e);
      throw e;
    }
    busy = true;
    try {
      await new Promise(r => setTimeout(r, 2));          // a radio round trip
      if (f.failNext) { f.failNext = false; throw new Error('GATT write failed'); }
      sent.push({ kind, data: Buffer.from(chunk) });
    } finally {
      busy = false;
    }
  };
  f.rx = {
    properties: { write: true, writeWithoutResponse: withoutResponse },
    writeValue: c => op('with-response', c),
    writeValueWithoutResponse: c => op('without-response', c),
  };
  const tx = new EventTarget();
  tx.startNotifications = async () => { f.onStart?.(); return tx; };
  f.notify = data => {
    tx.value = new DataView(Uint8Array.from(data).buffer);
    tx.dispatchEvent(new Event('characteristicvaluechanged'));
  };
  const service = { getCharacteristic: async uuid => (uuid === NUS.rx ? f.rx : tx) };
  device.id = 'fake-mono';
  device.name = 'MONO';
  device.gatt = {
    connected: false,
    async connect() { this.connected = true; return { getPrimaryService: async () => service }; },
    disconnect() { this.connected = false; device.dispatchEvent(new Event('gattserverdisconnected')); },
  };
  f.walkAway = () => { device.gatt.connected = false; device.dispatchEvent(new Event('gattserverdisconnected')); };
  f.text = () => Buffer.concat(sent.map(s => s.data)).toString();
  return f;
}

test('writes go out one at a time: STATUS and GET sent back to back both arrive [REVIEW A2]', async () => {
  const f = fakeWallet();
  // the fake is as strict as Chrome: two writes at once, and the second is refused
  const [first, second] = await Promise.allSettled([f.rx.writeValue(bytes('A')), f.rx.writeValue(bytes('B'))]);
  assert.equal(first.status, 'fulfilled');
  assert.match(String(second.reason), /already in progress/);
  f.sent.length = 0;
  f.refused.length = 0;

  const link = bleLink(f.device);
  await link.open(quiet);
  await Promise.all([link.write('STATUS'), link.write('GET')]);
  assert.equal(f.refused.length, 0, 'no write started while another was in flight');
  assert.equal(f.text(), 'STATUS\nGET\n');
});

test('a long line goes out in 20-byte pieces, the default ATT payload [REVIEW A3]', async () => {
  const f = fakeWallet();
  const link = bleLink(f.device);
  await link.open(quiet);
  const auth = 'AUTH ' + 'ab'.repeat(32);              // 69 characters and a newline
  await link.write(auth);
  assert.deepEqual(f.sent.map(s => s.data.length), [20, 20, 20, 10]);
  assert.equal(f.text(), auth + '\n');
});

test('write-without-response is used when the wallet offers it', async () => {
  const f = fakeWallet({ withoutResponse: true });
  const link = bleLink(f.device);
  await link.open(quiet);
  await link.write('STATUS');
  assert.deepEqual(f.sent.map(s => s.kind), ['without-response']);
});

test('the listener is in place before notifications start, so the first line is not lost [REVIEW A4]', async () => {
  const f = fakeWallet();
  f.onStart = () => f.notify(bytes('EVT linked\n'));    // the wallet speaks the moment it can
  const lines = [];
  await bleLink(f.device).open({ onLine: l => lines.push(l), onClose() {} });
  assert.deepEqual(lines, ['EVT linked']);
});

test('lines and characters split across notifications arrive whole [REVIEW A4]', async () => {
  const f = fakeWallet();
  const lines = [];
  await bleLink(f.device).open({ onLine: l => lines.push(l), onClose() {} });
  const all = bytes('ENROLL ok id=1 ·\r\nSTATUS latch=open sensor=ready\n');
  const mid = all.indexOf(0xb7);                       // between the two bytes of the middle dot
  for (const piece of [all.subarray(0, mid), all.subarray(mid, 30), all.subarray(30)]) f.notify(piece);
  assert.deepEqual(lines, ['ENROLL ok id=1 ·', 'STATUS latch=open sensor=ready']);
});

test('one failed write does not jam every write after it', async () => {
  const f = fakeWallet();
  const link = bleLink(f.device);
  await link.open(quiet);
  f.failNext = true;
  await assert.rejects(link.write('RING'), /write failed/);
  await link.write('STATUS');
  assert.equal(f.text(), 'STATUS\n');
});

test("walking out of range reports 'lost', hanging up reports 'closed', and the same device reconnects", async () => {
  const f = fakeWallet();
  const ends = [];
  const link = bleLink(f.device);
  const session = { onLine() {}, onClose: why => ends.push(why) };
  await link.open(session);
  f.walkAway();
  await assert.rejects(link.write('STATUS'), /Not connected/);
  await link.open(session);                            // device.gatt.connect() again: no chooser
  assert.equal(link.connected, true);
  await link.close();
  assert.deepEqual(ends, ['lost', 'closed']);
});
