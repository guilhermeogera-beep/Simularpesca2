const CACHE_NAME = 'capacete-seco-v2'; // Mudamos a versão para forçar atualização
const assets = ['./index.html', './manifest.json']; // Tiramos a imagem daqui

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(assets)));
});

self.addEventListener('fetch', e => {
  e.respondWith(fetch(e.request).catch(() => caches.match(e.request)));
});