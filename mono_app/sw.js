/* ONYX service worker: the app opens with no signal.
   Network first, so a deploy is never hidden behind a stale cache; the cache
   only answers when the network cannot. Bump VERSION when the shell changes. */

const VERSION = 'onyx-v3';
const SHELL = [
  './', './index.html', './styles.css', './manifest.webmanifest',
  './js/app.js', './js/session.js', './js/transport.js', './js/protocol.js',
  './js/crypto.js', './js/store.js', './js/ui.js', './js/demo.js',
  './icons/icon.svg', './icons/icon-192.png',
];

self.addEventListener('install', event => {
  event.waitUntil(caches.open(VERSION).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', event => {
  event.waitUntil(caches.keys()
    .then(names => Promise.all(names.filter(n => n !== VERSION).map(n => caches.delete(n))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', event => {
  const req = event.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return;
  event.respondWith(fetch(req).then(res => {
    if (res.ok) { const copy = res.clone(); caches.open(VERSION).then(c => c.put(req, copy)); }
    return res;
  }).catch(() => caches.match(req).then(hit => hit ?? caches.match('./index.html'))));
});
