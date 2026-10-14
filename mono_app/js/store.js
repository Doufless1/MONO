/* ============================================================================
   store.js  -  what this phone remembers, and nothing more
   ----------------------------------------------------------------------------
   The wallet keeps the fingerprints. The phone keeps: its key for each wallet,
   the names people give their fingers, where it last lost the wallet (only if
   asked to), and a couple of preferences.

   Storage throws in some embedded and preview contexts and comes back empty
   in private windows; everything here falls back to memory, so the app works
   either way - it just forgets on reload.
   ========================================================================= */

const memory = new Map();
const raw = {
  get(k)    { try { return localStorage.getItem(k); } catch { return memory.get(k) ?? null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { memory.set(k, v); } },
  del(k)    { try { localStorage.removeItem(k); } catch { memory.delete(k); } },
};
const json = {
  get(k, fallback) { try { return JSON.parse(raw.get(k)) ?? fallback; } catch { return fallback; } },
  set(k, v) { raw.set(k, JSON.stringify(v)); },
};

/** This phone's pairing key per wallet. Not a secure enclave: see docs/SECURITY.md. */
export const keys = {
  get: id => raw.get(`onyx.key.${id}`),
  set: (id, key) => raw.set(`onyx.key.${id}`, key),
  delete: id => raw.del(`onyx.key.${id}`),
};

/** Keys that vanish with the page - for the demo wallet, which is new every visit. */
export function memoryKeys() {
  const m = new Map();
  return { get: id => m.get(id), set: (id, k) => m.set(id, k), delete: id => m.delete(id) };
}

/** Finger names and dates, per wallet, per template id. The wallet cannot store names. */
export const fingers = {
  all: wallet => json.get(`onyx.fingers.${wallet}`, {}),
  get(wallet, id) { return this.all(wallet)[id] ?? null; },
  set(wallet, id, entry) {
    const all = this.all(wallet);
    all[id] = { ...all[id], ...entry };
    json.set(`onyx.fingers.${wallet}`, all);
  },
  /** Keep exactly the ids the wallet reports; ids it added elsewhere get a plain name. */
  reconcile(wallet, ids) {
    const all = this.all(wallet), next = {};
    for (const id of ids) next[id] = all[id] ?? { name: `Finger ${id}`, added: null };
    json.set(`onyx.fingers.${wallet}`, next);
    return next;
  },
  clear: wallet => raw.del(`onyx.fingers.${wallet}`),
};

/** Where the phone was when it last lost the wallet. */
export const lastFix = {
  get: () => json.get('onyx.lastfix', null),
  set: fix => json.set('onyx.lastfix', fix),
  clear: () => raw.del('onyx.lastfix'),
};

/** The wallet this phone used last, so a reload can reconnect without the chooser. */
export const lastWallet = {
  get: () => raw.get('onyx.wallet'),
  set: id => raw.set('onyx.wallet', id),
  clear: () => raw.del('onyx.wallet'),
};

export const prefs = {
  get: (name, fallback) => json.get(`onyx.pref.${name}`, fallback),
  set: (name, value) => json.set(`onyx.pref.${name}`, value),
};

export const session = {
  seenVeil() { try { return sessionStorage.getItem('onyx.veil') === '1'; } catch { return false; } },
  markVeil() { try { sessionStorage.setItem('onyx.veil', '1'); } catch { /* fine */ } },
};
