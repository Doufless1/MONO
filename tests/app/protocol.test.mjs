// Unit tests for the app's pure protocol layer.   node --test tests/app/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  parseLine, parseStatus, parseSettings, parseList, parseHello,
  batteryPercent, nearestPreset, setCommands, ago, describeFailure, ENROLL_FAILURE, PRESETS,
} from '../../mono_app/js/protocol.js';
import { hexToBytes, bytesToHex, authAnswer } from '../../mono_app/js/crypto.js';

test('a line splits into head, words, key=value pairs and bare flags', () => {
  const l = parseLine('  ENROLL fail no_match ');
  assert.equal(l.head, 'ENROLL');
  assert.deepEqual(l.flags, ['fail', 'no_match']);
  const s = parseLine('STATUS latch=open fingers=2');
  assert.deepEqual(s.kv, { latch: 'open', fingers: '2' });
  assert.equal(parseLine('HELP').words.length, 0);
});

test('STATUS parses into what the screens show', () => {
  const s = parseStatus(parseLine(
    'STATUS latch=open sensor=ready alarm=1 enroll=idle fingers=3 vbat=3.98 link=1 auth=1 pair=0 setup=0 leash=ok').kv);
  assert.equal(s.latch, 'open');
  assert.equal(s.alarm, true);
  assert.equal(s.fingers, 3);
  assert.equal(s.vbat, 3.98);
  assert.equal(s.authed, true);
  assert.equal(s.leash, 'ok');
});

test('missing STATUS fields stay unknown rather than defaulting to a lie', () => {
  const s = parseStatus({});
  assert.equal(s.latch, null);
  assert.equal(s.fingers, null);
  assert.equal(s.vbat, null);
});

test('GET, LIST and HELLO', () => {
  assert.deepEqual(parseSettings(parseLine('GET relock=6000 sepwarn=0 phones=2').kv),
                   { relock: 6000, sepwarn: 0, phones: 2 });
  assert.deepEqual(parseList(parseLine('LIST count=2 ids=1,3').kv), [1, 3]);
  assert.deepEqual(parseList(parseLine('LIST count=0 ids=').kv), []);
  const h = parseHello(parseLine('HELLO v=3 fw=3.0.0 phones=1 setup=0 nonce=-').kv);
  assert.equal(h.version, 3);
  assert.equal(h.nonce, null, 'no nonce means there is nothing to answer');
});

test('battery percent follows a lithium curve, not a straight line', () => {
  assert.equal(batteryPercent(4.2), 100);
  assert.equal(batteryPercent(3.7), 50);
  assert.equal(batteryPercent(3.0), 0);
  assert.equal(batteryPercent(null), null);
  assert.ok(batteryPercent(3.95) > 75 && batteryPercent(3.95) < 100);
});

test('settings snap to the nearest preset, and say nothing before GET arrives', () => {
  assert.equal(nearestPreset('nudge', { tamper: 2600 }).label, 'Normal');
  assert.equal(nearestPreset('relock', { relock: 15000 }).v, 15);
  assert.equal(nearestPreset('relock', {}), null);
});

test('"Never" really switches the leash off (v2 faked it as an hour)', () => {
  assert.deepEqual(setCommands('leash', 0), ['SET sepwarn 0']);
  assert.equal(nearestPreset('leash', { sepwarn: 0 }).label, 'Never');
  assert.deepEqual(setCommands('leash', 60), ['SET sepwarn 60', 'SET sepalert 180']);
});

test('every preset converts to the wallet and back to itself', () => {
  for (const [kind, p] of Object.entries(PRESETS)) {
    for (const o of p.options) {
      const settings = Object.fromEntries(Object.entries(p.toWallet(o.v)));
      assert.equal(nearestPreset(kind, settings).v, o.v, `${kind} ${o.label}`);
    }
  }
});

test('time since, in words', () => {
  const now = 1e12;
  assert.equal(ago(now - 10e3, now), 'Just now');
  assert.equal(ago(now - 5 * 60e3, now), '5 minutes ago');
  assert.equal(ago(now - 3 * 3600e3, now), '3 hours ago');
  assert.equal(ago(now - 30 * 3600e3, now), 'Yesterday', 'v2 said "30 hours ago"');
  assert.equal(ago(now - 72 * 3600e3, now), '3 days ago');
});

test('failure reasons become sentences', () => {
  assert.match(describeFailure(ENROLL_FAILURE, 'no_match'), /same finger/);
  assert.match(describeFailure(ENROLL_FAILURE, 'gremlins'), /gremlins/);
});

test('hex round trip, and malformed hex is refused', () => {
  assert.equal(bytesToHex(hexToBytes('00ff10ab')), '00ff10ab');
  assert.throws(() => hexToBytes('abc'));
  assert.throws(() => hexToBytes('zz'));
});

test('the AUTH answer is HMAC-SHA256(key, "MONO-AUTH-1" || nonce)', async () => {
  // Cross-checked against Python: hmac.new(bytes(16), b"MONO-AUTH-1" + bytes(range(16)), "sha256")
  const answer = await authAnswer('00'.repeat(16), bytesToHex(Uint8Array.from({ length: 16 }, (_, i) => i)));
  assert.equal(answer, 'e5456b04775fe6f8b50b15d7478b856f430fd8076c7352f336c69fd730ef3657');
});
