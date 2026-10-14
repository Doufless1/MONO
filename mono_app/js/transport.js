/* ============================================================================
   transport.js  -  a line link to the wallet over Web Bluetooth
   ----------------------------------------------------------------------------
   The same shape as simLink() in sim/wallet-sim.js, so the session above it
   cannot tell a real wallet from the simulated one:

       link.open({ onLine, onClose })   onClose('lost' | 'closed')
       link.write(line)
       link.close()

   The things that are easy to get wrong with Web Bluetooth all live here:
     - one GATT operation at a time. Chrome rejects a write while another is
       in flight ("GATT operation already in progress"); v2 fired STATUS and
       GET back to back and lost the second. Writes are queued.
     - 20 bytes per write, the payload of the default ATT MTU.
     - the notification listener is attached before notifications start, or
       the wallet's first lines can arrive with nobody listening.
     - a streaming UTF-8 decoder, so a character split across two
       notifications is not mangled.
     - the device object is kept, so a lost link reconnects with
       device.gatt.connect() - no chooser, no tap needed.
   ========================================================================= */

import { NUS } from './protocol.js';

const CHUNK = 20;

export function webBluetoothAvailable() {
  return !!globalThis.navigator?.bluetooth && !!globalThis.isSecureContext;
}

/** Shows the browser's device chooser. Must be called from a user gesture. */
export async function chooseWallet() {
  const device = await navigator.bluetooth.requestDevice({
    filters: [{ namePrefix: 'MONO' }, { namePrefix: 'ONYX' }],
    optionalServices: [NUS.service],
  });
  return bleLink(device);
}

/** Wallets this origin was already allowed to use, where the browser can say. */
export async function rememberedWallets() {
  try {
    const devices = await navigator.bluetooth.getDevices?.();
    return (devices ?? []).filter(d => /^(MONO|ONYX)/.test(d.name ?? '')).map(bleLink);
  } catch {
    return [];
  }
}

export function bleLink(device) {
  let rx = null, tx = null, handlers = null;
  let decoder = new TextDecoder(), pending = '';
  let queue = Promise.resolve();
  let closing = false;

  const onNotify = event => {
    pending += decoder.decode(event.target.value, { stream: true });
    let i;
    while ((i = pending.indexOf('\n')) >= 0) {
      const line = pending.slice(0, i).replace(/\r$/, '');
      pending = pending.slice(i + 1);
      if (line.trim()) handlers?.onLine(line);
    }
  };

  const onDisconnected = () => {
    tx?.removeEventListener('characteristicvaluechanged', onNotify);
    rx = tx = null;
    const h = handlers;
    handlers = null;
    h?.onClose(closing ? 'closed' : 'lost');
    closing = false;
  };
  device.addEventListener('gattserverdisconnected', onDisconnected);   // once, not once per connect

  return {
    kind: 'ble',
    id: device.id,
    name: device.name || 'MONO',
    get connected() { return !!handlers && device.gatt.connected; },

    async open(h) {
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(NUS.service);
      rx = await service.getCharacteristic(NUS.rx);
      tx = await service.getCharacteristic(NUS.tx);
      decoder = new TextDecoder();
      pending = '';
      handlers = h;
      tx.addEventListener('characteristicvaluechanged', onNotify);
      await tx.startNotifications();
    },

    write(line) {
      const bytes = new TextEncoder().encode(line + '\n');
      const job = queue.then(async () => {
        if (!rx) throw new Error('Not connected');
        const fast = rx.properties?.writeWithoutResponse && rx.writeValueWithoutResponse;
        for (let i = 0; i < bytes.length; i += CHUNK) {
          const chunk = bytes.subarray(i, i + CHUNK);
          await (fast ? rx.writeValueWithoutResponse(chunk) : rx.writeValue(chunk));
        }
      });
      queue = job.catch(() => {});       // one failed write must not jam every later one
      return job;
    },

    async close() {
      closing = true;
      if (device.gatt.connected) device.gatt.disconnect();
      else onDisconnected();
    },
  };
}
