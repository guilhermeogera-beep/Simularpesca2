// Cache do SHELL do app (HTML/JS/JSON/ícones). Os VÍDEOS não passam por aqui:
// ficam no IndexedDB do navegador (importados manualmente em index.html).
const SHELL_CACHE = 'simpesca2-shell-v6';

const APP_SHELL = [
  '/Simularpesca2/',
  '/Simularpesca2/index.html',
  '/Simularpesca2/player.html',
  '/Simularpesca2/dashboard.html',
  '/Simularpesca2/ranking.html',
  '/Simularpesca2/desafio.html',
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
  const mesmaOrigem = url.startsWith(self.location.origin);

  // VÍDEOS nunca passam pelo cache do shell (ficam no IndexedDB) — senão encheriam o
  // armazenamento do totem (o motivo de o v1 ter tirado vídeo do SW). Deixa o browser buscar direto.
  if (/\.mp4($|\?)/i.test(url)) return;

  // App (HTML/JS/JSON/ícones): rede-primeiro (pega versão nova online), cai pro cache quando offline.
  event.respondWith(
    fetch(event.request).then(net => {
      if (net && net.ok && mesmaOrigem) {   // só cacheia o shell same-origin (não cacheia api.github/fonts)
        const c = net.clone(); caches.open(SHELL_CACHE).then(x => x.put(event.request, c));
      }
      return net;
    }).catch(() => caches.match(event.request).then(r =>
      r || (event.request.mode === 'navigate' ? caches.match('/Simularpesca2/index.html') : Response.error())   // index.html só como fallback de NAVEGAÇÃO (não devolve HTML pra fetch de JSON/fonte)
    ))
  );
});
