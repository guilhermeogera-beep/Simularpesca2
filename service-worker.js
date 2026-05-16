const CACHE_NAME = 'capacete-seco-v1';
const assets = ['./index.html', './manifest.json', './logo-capacete.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(assets)));
});

self.addEventListener('fetch', e => {
  e.respondWith(fetch(e.request).catch(() => caches.match(e.request)));
});