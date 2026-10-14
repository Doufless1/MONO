/* ============================================================================
   crypto.js  -  proving to the wallet that this phone holds its key
   ----------------------------------------------------------------------------
   The wallet sends a fresh random nonce in HELLO; the phone answers
   HMAC-SHA256(key, "MONO-AUTH-1" || nonce). The key itself crosses the air
   exactly once, in PAIR, and never again. WebCrypto does the maths, so the
   key never sits in a JavaScript string longer than it takes to import it.

   Must match authMac() in mono_firmware.ino byte for byte. The Node
   integration test proves it does, against the real firmware in wasm.
   ========================================================================= */

export const AUTH_TAG = 'MONO-AUTH-1';

export function hexToBytes(hex) {
  if (typeof hex !== 'string' || hex.length % 2 || /[^0-9a-f]/i.test(hex)) {
    throw new TypeError('not a hex string');
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  return out;
}

export function bytesToHex(bytes) {
  return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
}

export function cryptoAvailable() {
  return !!globalThis.crypto?.subtle;
}

/** The AUTH answer for one challenge. */
export async function authAnswer(keyHex, nonceHex) {
  const key = await globalThis.crypto.subtle.importKey(
    'raw', hexToBytes(keyHex), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const tag = new TextEncoder().encode(AUTH_TAG);
  const nonce = hexToBytes(nonceHex);
  const msg = new Uint8Array(tag.length + nonce.length);
  msg.set(tag);
  msg.set(nonce, tag.length);
  return bytesToHex(new Uint8Array(await globalThis.crypto.subtle.sign('HMAC', key, msg)));
}
