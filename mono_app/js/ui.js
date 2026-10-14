/* ============================================================================
   ui.js  -  sheets, toasts and gestures
   ----------------------------------------------------------------------------
   v2 used prompt() and confirm(): they block the page, look like the browser
   rather than the app, and some browsers suppress them inside an iframe -
   which is exactly where the live demo runs. These are small, keyboard
   friendly, and give focus back where they found it.
   ========================================================================= */

const host = id => document.getElementById(id);

function el(tag, attrs = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') n.className = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v);
  }
  for (const k of kids) if (k != null) n.append(k);
  return n;
}

/**
 * A bottom sheet. Resolves { action, value } or null if dismissed.
 * actions: [{ label, action, kind: 'primary' | 'light' | 'danger' | 'alt' }]
 */
export function sheet({ title, body = '', actions = [], input = null }) {
  return new Promise(resolve => {
    const back = document.activeElement;
    const root = host('sheetHost');
    let field = null;

    const close = result => {
      document.removeEventListener('keydown', onKey, true);
      root.replaceChildren();
      back?.focus?.();
      resolve(result);
    };
    const onKey = e => {
      if (e.key === 'Escape') { e.preventDefault(); close(null); }
      if (e.key === 'Tab') {                              // keep focus inside the sheet
        const f = [...root.querySelectorAll('button, input')];
        const i = f.indexOf(document.activeElement);
        if (e.shiftKey && i <= 0) { e.preventDefault(); f.at(-1).focus(); }
        else if (!e.shiftKey && i === f.length - 1) { e.preventDefault(); f[0].focus(); }
      }
    };

    if (input) {
      field = el('input', { class: 'field', value: input.value ?? '', placeholder: input.placeholder ?? '',
                            maxlength: String(input.maxlength ?? 40), 'aria-label': title });
      field.addEventListener('keydown', e => {
        if (e.key === 'Enter') { e.preventDefault(); close({ action: actions[0]?.action ?? 'ok', value: field.value.trim() }); }
      });
    }
    const buttons = actions.map(a => el('button', {
      type: 'button',
      class: `btn btn-wide ${{ primary: 'btn-primary', danger: 'btn-danger', alt: 'btn-alt', light: 'btn-light' }[a.kind ?? 'light']}`,
      onclick: () => close({ action: a.action, value: field?.value.trim() ?? null }),
    }, a.label));

    const panel = el('div', { class: 'sheet', role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': 'sheetTitle' },
      el('div', { class: 'grab', 'aria-hidden': 'true' }),
      el('h3', { id: 'sheetTitle' }, title),
      body ? el('p', {}, body) : null,
      field,
      el('div', { class: 'stack' }, ...buttons));
    root.replaceChildren(el('div', { class: 'scrim', onclick: () => close(null) }), panel);
    document.addEventListener('keydown', onKey, true);
    (field ?? buttons[0])?.focus();
    if (field) field.select();
  });
}

export async function confirmSheet(title, body, yes, { danger = false, no = 'Cancel' } = {}) {
  const r = await sheet({ title, body, actions: [
    { label: yes, action: 'yes', kind: danger ? 'danger' : 'primary' },
    { label: no, action: 'no', kind: 'alt' },
  ] });
  return r?.action === 'yes';
}

export async function promptSheet(title, body, value, placeholder = '') {
  const r = await sheet({ title, body, input: { value, placeholder }, actions: [
    { label: 'Save', action: 'save', kind: 'primary' },
    { label: 'Cancel', action: 'cancel', kind: 'alt' },
  ] });
  return r?.action === 'save' && r.value ? r.value : null;
}

let toastTimer = null;
/** One toast at a time; a new one replaces the last. */
export function toast(text, { icon = 'i-info', ms = 3400 } = {}) {
  clearTimeout(toastTimer);
  const t = el('div', { class: 'toast', role: 'status' });
  t.innerHTML = `<svg class="i" aria-hidden="true"><use href="#${icon}"/></svg>`;
  t.append(text);
  host('toastHost').replaceChildren(t);
  toastTimer = setTimeout(() => host('toastHost').replaceChildren(), ms);
}

/**
 * Press-and-hold, by pointer or keyboard (Space / Enter). onDown fires on
 * press, onUp on release - including when the pointer slides off or the
 * browser cancels the gesture, so a "finger on the sensor" can never stick.
 */
export function pressAndHold(target, { onDown, onUp }) {
  let down = false;
  const start = e => {
    if (down) return;
    if (e.type === 'pointerdown') {
      if (e.button !== 0) return;
      e.preventDefault();
    }
    down = true;
    onDown(e);
    // Capture keeps the release coming to us if the finger slides off. It can
    // throw (no such active pointer, or it has already gone), and a nicety
    // must never cost the press itself.
    if (e.type === 'pointerdown') {
      try { target.setPointerCapture(e.pointerId); } catch { /* release still ends it */ }
    }
  };
  const end = e => { if (!down) return; down = false; onUp(e); };
  target.addEventListener('pointerdown', start);
  // pointerleave only fires here when capture failed (a captured pointer gets
  // no boundary events), and then it is the only way the release reaches us.
  for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture', 'pointerleave']) target.addEventListener(ev, end);
  target.addEventListener('keydown', e => {
    if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); start(e); }
  });
  target.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); end(e); } });
  target.addEventListener('blur', end);
  target.addEventListener('contextmenu', e => e.preventDefault());
}

export function longPress(target, ms, fn) {
  let t = null;
  target.addEventListener('pointerdown', () => { clearTimeout(t); t = setTimeout(fn, ms); });
  for (const ev of ['pointerup', 'pointerleave', 'pointercancel']) target.addEventListener(ev, () => clearTimeout(t));
  target.addEventListener('contextmenu', e => e.preventDefault());
}
