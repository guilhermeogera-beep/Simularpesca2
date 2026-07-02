// Cache do SHELL do app (HTML/JS/JSON/ícones). Os VÍDEOS não passam por aqui:
// ficam no IndexedDB do navegador (importados manualmente em index.html).
const SHELL_CACHE = 'simpesca2-shell-v2';

const APP_SHELL = [
  '/Simularpesca2/',
  '/Simularpesca2/index.html',
  '/Simularpesca2/player.html',
  '/Simularpesca2/dashboard.html',
  '/Simularpesca2/ranking.html',
  '/Simularpesca2/manifest.json',
  '/Simularpesca2/curvas.json',
  '/Simularpesca2/config.json',
  '/Simularpesca2/icons/icon-192.png',
  '/Simularpesca2/icons/icon-512.png'
];

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(SHELL_CACHE).then(cache =>
      Promise.all(APP_SHELL.map(url => cache.add(url).catch(() => {})))
    )
  );
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys
        .filter(k => k !== SHELL_CACHE)
        .map(k => caches.delete(k)))   // remove caches antigos (inclui o antigo cache de vídeos)
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;
  const url = event.request.url;

  // App (HTML/JS/JSON/ícones): rede-primeiro (pega versão nova online),
  // cai pro cache quando offline.
  event.respondWith(
    fetch(event.request).then(net => {
      if (net && net.ok && url.startsWith(self.location.origin)) {
        const c = net.clone(); caches.open(SHELL_CACHE).then(x => x.put(event.request, c));
      }
      return net;
    }).catch(() => caches.match(event.request).then(r => r || caches.match('/Simularpesca2/index.html')))
  );
});
